#include "lb/server.hpp"

#include "lb/admin.hpp"
#include "lb/logger.hpp"
#include "lb/output_buffer.hpp"
#include "lb/runtime.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lb {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kBacklog = 65535;
constexpr int kMaxEvents = 1024;
constexpr std::size_t kBufferSize = 64 * 1024;
constexpr std::size_t kHighWatermark = 1024 * 1024;
constexpr std::size_t kBufferCompactThreshold = 256 * 1024;

void throw_errno(const std::string& message) {
    throw std::runtime_error(message + ": " + std::strerror(errno));
}

std::string endpoint_label(const Endpoint& endpoint) {
    return endpoint.host + ":" + std::to_string(endpoint.port);
}

addrinfo* resolve_endpoint(const Endpoint& endpoint, int family, int socktype, int flags) {
    addrinfo hints{};
    hints.ai_family = family;
    hints.ai_socktype = socktype;
    hints.ai_flags = flags;

    addrinfo* result = nullptr;
    const auto service = std::to_string(endpoint.port);
    const int rc = getaddrinfo(endpoint.host.c_str(), service.c_str(), &hints, &result);
    if (rc != 0) {
        throw std::runtime_error("getaddrinfo(" + endpoint_label(endpoint) + ") failed: " + gai_strerror(rc));
    }
    return result;
}

void set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        throw_errno("fcntl(F_GETFL) failed");
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        throw_errno("fcntl(F_SETFL) failed");
    }
}

void set_reuse_options(int fd, bool reuse_port) {
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) == -1) {
        throw_errno("setsockopt(SO_REUSEADDR) failed");
    }
    if (reuse_port && setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) == -1) {
        throw_errno("setsockopt(SO_REUSEPORT) failed");
    }
}

void tune_socket(int fd) {
    int enabled = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
}

int make_listener(const Endpoint& endpoint, bool reuse_port) {
    addrinfo* raw = resolve_endpoint(endpoint, AF_UNSPEC, SOCK_STREAM, AI_PASSIVE);
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(raw, freeaddrinfo);

    int last_errno = 0;
    for (auto* address = addresses.get(); address != nullptr; address = address->ai_next) {
        const int fd = socket(address->ai_family, address->ai_socktype | SOCK_CLOEXEC, address->ai_protocol);
        if (fd == -1) {
            last_errno = errno;
            continue;
        }

        try {
            set_reuse_options(fd, reuse_port);
            set_nonblocking(fd);
        } catch (...) {
            close(fd);
            throw;
        }

        if (bind(fd, address->ai_addr, address->ai_addrlen) == 0 && listen(fd, kBacklog) == 0) {
            return fd;
        }

        last_errno = errno;
        close(fd);
    }

    errno = last_errno;
    throw_errno("failed to bind listener " + endpoint_label(endpoint));
}

int connect_backend(const Endpoint& endpoint) {
    addrinfo* raw = resolve_endpoint(endpoint, AF_UNSPEC, SOCK_STREAM, 0);
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(raw, freeaddrinfo);

    int last_errno = 0;
    for (auto* address = addresses.get(); address != nullptr; address = address->ai_next) {
        const int fd = socket(address->ai_family, address->ai_socktype | SOCK_CLOEXEC, address->ai_protocol);
        if (fd == -1) {
            last_errno = errno;
            continue;
        }

        try {
            set_nonblocking(fd);
            tune_socket(fd);
        } catch (...) {
            close(fd);
            throw;
        }

        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0 || errno == EINPROGRESS) {
            return fd;
        }

        last_errno = errno;
        close(fd);
    }

    errno = last_errno;
    return -1;
}

void raise_file_limit(std::uint32_t target) {
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur >= target) {
        return;
    }

    limit.rlim_cur = std::min<rlim_t>(limit.rlim_max, target);
    setrlimit(RLIMIT_NOFILE, &limit);
}

bool tcp_probe(const Endpoint& endpoint, std::uint32_t timeout_ms, std::uint64_t& latency_us) {
    const auto started = Clock::now();
    const int fd = connect_backend(endpoint);
    if (fd == -1) {
        return false;
    }

    pollfd pfd{fd, POLLOUT, 0};
    const int rc = poll(&pfd, 1, static_cast<int>(timeout_ms));
    if (rc <= 0) {
        close(fd);
        return false;
    }

    int error = 0;
    socklen_t length = sizeof(error);
    const bool ok = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
    close(fd);
    latency_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count());
    return ok;
}

std::uint32_t resolved_worker_count(std::uint32_t configured) {
    if (configured > 0) {
        return configured;
    }
    const auto detected = std::thread::hardware_concurrency();
    return std::max(1u, detected);
}

}  // namespace

struct TcpLoadBalancer::Impl {
    struct Peer {
        int fd{-1};
        int paired_fd{-1};
        std::size_t backend_index{0};
        bool backend_side{false};
        bool connecting{false};
        Clock::time_point connected_started_at{};
        Clock::time_point connect_deadline{};
        Clock::time_point last_activity{};
        OutputBuffer outbound{kBufferCompactThreshold};
    };

    struct Worker {
        std::size_t id{0};
        std::shared_ptr<RuntimeState> runtime;
        int epoll_fd{-1};
        int listener_fd{-1};
        int timer_fd{-1};
        std::unordered_map<int, Peer> peers;
        std::array<char, kBufferSize> read_buffer{};

        void run(std::atomic_bool& stop_requested) {
            listener_fd = make_listener(runtime->config().listen, true);
            epoll_fd = epoll_create1(EPOLL_CLOEXEC);
            if (epoll_fd == -1) {
                throw_errno("epoll_create1 failed");
            }

            timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
            if (timer_fd == -1) {
                throw_errno("timerfd_create failed");
            }
            itimerspec timer{};
            timer.it_interval.tv_sec = 1;
            timer.it_value.tv_sec = 1;
            if (timerfd_settime(timer_fd, 0, &timer, nullptr) == -1) {
                throw_errno("timerfd_settime failed");
            }

            add_fd(listener_fd, EPOLLIN);
            add_fd(timer_fd, EPOLLIN);

            std::vector<epoll_event> events(kMaxEvents);
            while (!stop_requested.load(std::memory_order_relaxed)) {
                const int count = epoll_wait(epoll_fd, events.data(), static_cast<int>(events.size()), 250);
                if (count == -1) {
                    if (errno == EINTR) {
                        continue;
                    }
                    log(LogLevel::Error, "worker " + std::to_string(id) + " epoll_wait failed");
                    continue;
                }

                for (int i = 0; i < count; ++i) {
                    const int fd = events[i].data.fd;
                    const auto flags = events[i].events;

                    if (fd == listener_fd) {
                        accept_clients();
                        continue;
                    }
                    if (fd == timer_fd) {
                        consume_timer();
                        reap_timeouts();
                        continue;
                    }
                    if (!peers.contains(fd)) {
                        continue;
                    }

                    if ((flags & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
                        close_pair(fd, peers[fd].backend_side);
                        continue;
                    }
                    if (peers.contains(fd) && peers[fd].connecting && (flags & EPOLLOUT) != 0) {
                        finish_connect(fd);
                    }
                    if (peers.contains(fd) && (flags & EPOLLIN) != 0) {
                        read_available(fd);
                    }
                    if (peers.contains(fd) && (flags & EPOLLOUT) != 0) {
                        flush(fd);
                    }
                }
            }
            shutdown();
        }

        void shutdown() {
            std::vector<int> open_fds;
            open_fds.reserve(peers.size());
            for (const auto& [fd, _] : peers) {
                open_fds.push_back(fd);
            }
            for (const int fd : open_fds) {
                close_pair(fd, false);
            }
            close_fd(listener_fd);
            close_fd(timer_fd);
            close_fd(epoll_fd);
        }

        void close_fd(int& fd) {
            if (fd != -1) {
                close(fd);
                fd = -1;
            }
        }

        void add_fd(int fd, std::uint32_t events) const {
            epoll_event event{};
            event.events = events;
            event.data.fd = fd;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == -1) {
                throw_errno("epoll_ctl(ADD) failed");
            }
        }

        void modify_fd(int fd) {
            auto it = peers.find(fd);
            if (it == peers.end()) {
                return;
            }

            const auto& peer = it->second;
            std::uint32_t events = EPOLLERR | EPOLLHUP | EPOLLRDHUP;
            if (peer.connecting || !peer.outbound.empty()) {
                events |= EPOLLOUT;
            }

            const auto paired = peers.find(peer.paired_fd);
            const bool pair_can_buffer = paired != peers.end() && paired->second.outbound.size() < kHighWatermark;
            if (!peer.connecting && pair_can_buffer) {
                events |= EPOLLIN;
            }

            epoll_event event{};
            event.events = events;
            event.data.fd = fd;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) == -1 && errno != EBADF && errno != ENOENT) {
                log(LogLevel::Warn, "epoll_ctl(MOD) failed");
            }
        }

        void accept_clients() {
            while (true) {
                sockaddr_storage client_address{};
                socklen_t address_length = sizeof(client_address);
                const int client_fd = accept4(listener_fd,
                                              reinterpret_cast<sockaddr*>(&client_address),
                                              &address_length,
                                              SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (client_fd == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        return;
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    log(LogLevel::Warn, "accept4 failed");
                    return;
                }

                tune_socket(client_fd);
                attach_backend(client_fd);
            }
        }

        void attach_backend(int client_fd) {
            const auto selected = runtime->scheduler().select();
            if (!selected) {
                close(client_fd);
                return;
            }

            const int backend_fd = connect_backend(selected->endpoint);
            if (backend_fd == -1) {
                runtime->record_backend_failure(id, selected->index);
                maybe_eject(selected->index);
                close(client_fd);
                return;
            }

            const auto now = Clock::now();
            const auto connect_timeout = std::chrono::milliseconds(runtime->config().connect_timeout_ms);
            runtime->record_connection_open(id, selected->index);

            peers.emplace(client_fd,
                          Peer{client_fd, backend_fd, selected->index, false, false, now, now + connect_timeout, now});
            peers.emplace(backend_fd,
                          Peer{backend_fd, client_fd, selected->index, true, true, now, now + connect_timeout, now});

            add_fd(client_fd, EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP);
            add_fd(backend_fd, EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDHUP);
        }

        void finish_connect(int fd) {
            auto it = peers.find(fd);
            if (it == peers.end()) {
                return;
            }

            int error = 0;
            socklen_t length = sizeof(error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == -1 || error != 0) {
                close_pair(fd, true);
                return;
            }

            it->second.connecting = false;
            it->second.last_activity = Clock::now();
            const auto latency_us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(it->second.last_activity -
                                                                       it->second.connected_started_at)
                    .count());
            runtime->record_connect_latency(it->second.backend_index, latency_us);
            runtime->scheduler().mark_healthy(it->second.backend_index);
            flush(fd);
            if (peers.contains(fd)) {
                modify_fd(fd);
            }
        }

        void read_available(int fd) {
            while (peers.contains(fd)) {
                const ssize_t n = read(fd, read_buffer.data(), read_buffer.size());
                if (n > 0) {
                    auto& source = peers[fd];
                    source.last_activity = Clock::now();
                    const int paired_fd = source.paired_fd;
                    const auto backend_index = source.backend_index;
                    const bool from_backend = source.backend_side;

                    auto paired = peers.find(paired_fd);
                    if (paired == peers.end()) {
                        close_pair(fd, false);
                        return;
                    }
                    paired->second.last_activity = source.last_activity;
                    paired->second.outbound.append({read_buffer.data(), static_cast<std::size_t>(n)});

                    if (from_backend) {
                        runtime->record_bytes_from_backend(id, backend_index, static_cast<std::uint64_t>(n));
                    } else {
                        runtime->record_bytes_from_client(id, backend_index, static_cast<std::uint64_t>(n));
                    }

                    flush(paired_fd);
                    if (peers.contains(fd)) {
                        modify_fd(fd);
                    }
                    if (peers.contains(paired_fd)) {
                        modify_fd(paired_fd);
                    }
                    if (peers.contains(paired_fd) && peers[paired_fd].outbound.size() >= kHighWatermark) {
                        return;
                    }
                    continue;
                }

                if (n == 0) {
                    close_pair(fd, false);
                    return;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                if (errno == EINTR) {
                    continue;
                }
                close_pair(fd, peers[fd].backend_side);
                return;
            }
        }

        void flush(int fd) {
            while (peers.contains(fd)) {
                auto& peer = peers[fd];
                if (peer.connecting || peer.outbound.empty()) {
                    return;
                }

                const auto readable = peer.outbound.readable_span();
                const ssize_t n = write(fd, readable.data(), readable.size());
                if (n > 0) {
                    peer.outbound.consume(static_cast<std::size_t>(n));
                    peer.last_activity = Clock::now();
                    if (peer.outbound.empty()) {
                        const int paired_fd = peer.paired_fd;
                        if (peers.contains(paired_fd)) {
                            modify_fd(paired_fd);
                        }
                        return;
                    }
                    continue;
                }
                if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    return;
                }
                if (n == -1 && errno == EINTR) {
                    continue;
                }
                close_pair(fd, peer.backend_side);
                return;
            }
        }

        void consume_timer() const {
            std::uint64_t expirations = 0;
            while (read(timer_fd, &expirations, sizeof(expirations)) == sizeof(expirations)) {
            }
        }

        void reap_timeouts() {
            const auto now = Clock::now();
            const auto idle_timeout = std::chrono::milliseconds(runtime->config().idle_timeout_ms);
            std::vector<std::pair<int, bool>> expired;

            for (const auto& [fd, peer] : peers) {
                if (peer.backend_side) {
                    continue;
                }
                const auto paired = peers.find(peer.paired_fd);
                const bool connecting = paired != peers.end() && paired->second.connecting;
                if (connecting && paired->second.connect_deadline <= now) {
                    expired.emplace_back(fd, true);
                } else if (!connecting && peer.last_activity + idle_timeout <= now) {
                    expired.emplace_back(fd, false);
                }
            }

            for (const auto& [fd, backend_failure] : expired) {
                if (backend_failure) {
                    runtime->record_connect_timeout(id);
                } else {
                    runtime->record_idle_timeout(id);
                }
                close_pair(fd, backend_failure);
            }
        }

        void close_pair(int fd, bool backend_failure) {
            auto first = peers.find(fd);
            if (first == peers.end()) {
                return;
            }

            const int paired_fd = first->second.paired_fd;
            const std::size_t backend_index = first->second.backend_index;

            close_one(fd);
            close_one(paired_fd);
            runtime->record_connection_close(id, backend_index);

            if (backend_failure) {
                runtime->record_backend_failure(id, backend_index);
                maybe_eject(backend_index);
            }
        }

        void close_one(int fd) {
            auto it = peers.find(fd);
            if (it == peers.end()) {
                return;
            }
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
            close(fd);
            peers.erase(it);
        }

        void maybe_eject(std::size_t backend_index) {
            if (runtime->scheduler().consecutive_failures(backend_index) >= runtime->config().passive_failure_threshold) {
                runtime->set_backend_state(backend_index, BackendState::Down);
            }
        }
    };

    explicit Impl(AppConfig app_config) {
        app_config.worker_count = resolved_worker_count(app_config.worker_count);
        auto scheduler = std::make_shared<Scheduler>(app_config.backends, app_config.policy);
        runtime = std::make_shared<RuntimeState>(std::move(app_config), scheduler);
        admin = std::make_unique<AdminServer>(runtime);
    }

    std::shared_ptr<RuntimeState> runtime;
    std::unique_ptr<AdminServer> admin;
    std::vector<std::thread> worker_threads;
    std::thread health_thread;

    void start(std::atomic_bool& stop_requested) {
        raise_file_limit(runtime->config().file_limit);
        admin->start();
        start_health_checks(stop_requested);

        log(LogLevel::Info,
            "listening on " + endpoint_label(runtime->config().listen) + " with " +
                std::to_string(runtime->config().worker_count) + " workers using " + to_string(runtime->config().policy));

        worker_threads.reserve(runtime->config().worker_count);
        for (std::uint32_t i = 0; i < runtime->config().worker_count; ++i) {
            worker_threads.emplace_back([this, &stop_requested, i] {
                Worker worker{i, runtime};
                try {
                    worker.run(stop_requested);
                } catch (const std::exception& ex) {
                    log(LogLevel::Error, "worker " + std::to_string(i) + " failed: " + ex.what());
                }
            });
        }

        for (auto& thread : worker_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        stop_requested.store(true, std::memory_order_relaxed);
        if (health_thread.joinable()) {
            health_thread.join();
        }
        admin->stop();
    }

    void shutdown() {
        admin->stop();
        if (health_thread.joinable()) {
            health_thread.join();
        }
        for (auto& thread : worker_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    void start_health_checks(std::atomic_bool& stop_requested) {
        health_thread = std::thread([this, &stop_requested] {
            while (!stop_requested.load(std::memory_order_relaxed)) {
                for (std::size_t i = 0; i < runtime->scheduler().backend_count(); ++i) {
                    if (runtime->scheduler().state(i) == BackendState::Draining) {
                        continue;
                    }
                    std::uint64_t latency_us = 0;
                    const bool ok = tcp_probe(runtime->scheduler().endpoint(i),
                                              runtime->config().health_timeout_ms,
                                              latency_us);
                    if (ok) {
                        runtime->record_connect_latency(i, latency_us);
                        runtime->scheduler().mark_healthy(i);
                    } else {
                        runtime->record_backend_failure(i);
                        if (runtime->scheduler().consecutive_failures(i) >= runtime->config().passive_failure_threshold) {
                            runtime->set_backend_state(i, BackendState::Down);
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(runtime->config().health_interval_ms));
            }
        });
    }
};

TcpLoadBalancer::TcpLoadBalancer(AppConfig config) : impl_(new Impl(std::move(config))) {}

TcpLoadBalancer::~TcpLoadBalancer() {
    if (impl_ != nullptr) {
        impl_->shutdown();
        delete impl_;
    }
}

void TcpLoadBalancer::run(std::atomic_bool& stop_requested) {
    impl_->start(stop_requested);
}

}  // namespace lb
