#include "lb/server.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <string>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lb {
namespace {

constexpr int kBacklog = 65535;
constexpr int kMaxEvents = 1024;
constexpr std::size_t kBufferSize = 64 * 1024;
constexpr std::size_t kHighWatermark = 1024 * 1024;

void throw_errno(const std::string& message) {
    throw std::runtime_error(message + ": " + std::strerror(errno));
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

void set_reuseaddr(int fd) {
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) == -1) {
        throw_errno("setsockopt(SO_REUSEADDR) failed");
    }
}

void tune_socket(int fd) {
    int enabled = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
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

int make_listener(const Endpoint& endpoint) {
    addrinfo* raw = resolve_endpoint(endpoint, AF_UNSPEC, SOCK_STREAM, AI_PASSIVE);
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(raw, freeaddrinfo);

    int last_errno = 0;
    for (auto* address = addresses.get(); address != nullptr; address = address->ai_next) {
        const int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd == -1) {
            last_errno = errno;
            continue;
        }

        try {
            set_reuseaddr(fd);
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
        const int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
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

void raise_file_limit() {
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
        return;
    }

    const rlim_t target = 25000;
    if (limit.rlim_cur >= target) {
        return;
    }

    limit.rlim_cur = std::min(limit.rlim_max, target);
    setrlimit(RLIMIT_NOFILE, &limit);
}

}  // namespace

struct TcpLoadBalancer::Impl {
    struct Peer {
        int fd{-1};
        int paired_fd{-1};
        std::size_t backend_index{0};
        bool connecting{false};
        std::vector<char> outbound;
        std::size_t sent{0};
    };

    explicit Impl(AppConfig app_config)
        : config(std::move(app_config)), scheduler(config.backends, config.policy) {}

    AppConfig config;
    Scheduler scheduler;
    int epoll_fd{-1};
    int listener_fd{-1};
    std::unordered_map<int, Peer> peers;

    void start(std::atomic_bool& stop_requested) {
        raise_file_limit();

        listener_fd = make_listener(config.listen);
        epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd == -1) {
            throw_errno("epoll_create1 failed");
        }

        add_fd(listener_fd, EPOLLIN);

        std::cout << "listening on " << endpoint_label(config.listen)
                  << " using " << to_string(config.policy)
                  << " across " << config.backends.size() << " backends\n";

        std::vector<epoll_event> events(kMaxEvents);
        while (!stop_requested.load()) {
            const int count = epoll_wait(epoll_fd, events.data(), static_cast<int>(events.size()), 250);
            if (count == -1) {
                if (errno == EINTR) {
                    continue;
                }
                throw_errno("epoll_wait failed");
            }

            for (int i = 0; i < count; ++i) {
                const int fd = events[i].data.fd;
                const auto flags = events[i].events;

                if (fd == listener_fd) {
                    accept_clients();
                    continue;
                }

                if (!peers.contains(fd)) {
                    continue;
                }

                if ((flags & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
                    close_pair(fd);
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
    }

    void shutdown() {
        std::vector<int> open_fds;
        open_fds.reserve(peers.size());
        for (const auto& [fd, _] : peers) {
            open_fds.push_back(fd);
        }
        for (const int fd : open_fds) {
            close_pair(fd);
        }
        if (listener_fd != -1) {
            close(listener_fd);
            listener_fd = -1;
        }
        if (epoll_fd != -1) {
            close(epoll_fd);
            epoll_fd = -1;
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
        if (peer.connecting || peer.sent < peer.outbound.size()) {
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
            throw_errno("epoll_ctl(MOD) failed");
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
                throw_errno("accept4 failed");
            }

            tune_socket(client_fd);
            attach_backend(client_fd);
        }
    }

    void attach_backend(int client_fd) {
        const auto selected = scheduler.select();
        if (!selected) {
            close(client_fd);
            return;
        }

        const int backend_fd = connect_backend(selected->endpoint);
        if (backend_fd == -1) {
            close(client_fd);
            return;
        }

        scheduler.mark_open(selected->index);

        peers.emplace(client_fd, Peer{client_fd, backend_fd, selected->index, false, {}, 0});
        peers.emplace(backend_fd, Peer{backend_fd, client_fd, selected->index, true, {}, 0});

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
            close_pair(fd);
            return;
        }

        it->second.connecting = false;
        flush(fd);
        if (peers.contains(fd)) {
            modify_fd(fd);
        }
    }

    void read_available(int fd) {
        std::vector<char> buffer(kBufferSize);

        while (peers.contains(fd)) {
            const ssize_t n = read(fd, buffer.data(), buffer.size());
            if (n > 0) {
                const int paired_fd = peers[fd].paired_fd;
                auto paired = peers.find(paired_fd);
                if (paired == peers.end()) {
                    close_pair(fd);
                    return;
                }

                const auto begin = buffer.begin();
                paired->second.outbound.insert(paired->second.outbound.end(), begin, begin + n);
                flush(paired_fd);

                if (peers.contains(fd)) {
                    modify_fd(fd);
                }
                if (peers.contains(paired_fd)) {
                    modify_fd(paired_fd);
                }

                const auto still_paired = peers.find(paired_fd);
                if (still_paired != peers.end() && still_paired->second.outbound.size() >= kHighWatermark) {
                    return;
                }
                continue;
            }

            if (n == 0) {
                close_pair(fd);
                return;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == EINTR) {
                continue;
            }

            close_pair(fd);
            return;
        }
    }

    void flush(int fd) {
        while (peers.contains(fd)) {
            auto& peer = peers[fd];
            if (peer.connecting || peer.sent >= peer.outbound.size()) {
                if (peer.sent >= peer.outbound.size()) {
                    peer.outbound.clear();
                    peer.sent = 0;
                }
                return;
            }

            const char* data = peer.outbound.data() + peer.sent;
            const std::size_t remaining = peer.outbound.size() - peer.sent;
            const ssize_t n = write(fd, data, remaining);

            if (n > 0) {
                peer.sent += static_cast<std::size_t>(n);
                if (peer.sent == peer.outbound.size()) {
                    peer.outbound.clear();
                    peer.sent = 0;

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

            close_pair(fd);
            return;
        }
    }

    void close_pair(int fd) {
        auto first = peers.find(fd);
        if (first == peers.end()) {
            return;
        }

        const int paired_fd = first->second.paired_fd;
        const std::size_t backend_index = first->second.backend_index;

        close_one(fd);
        close_one(paired_fd);
        scheduler.mark_closed(backend_index);
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
