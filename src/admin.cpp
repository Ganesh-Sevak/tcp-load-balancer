#include "lb/admin.hpp"

#include "lb/logger.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <netdb.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace lb {
namespace {

std::string endpoint_label(const Endpoint& endpoint) {
    return endpoint.host + ":" + std::to_string(endpoint.port);
}

void set_reuseaddr(int fd) {
    int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
}

void set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

int make_listener(const Endpoint& endpoint) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* raw = nullptr;
    const auto service = std::to_string(endpoint.port);
    const int rc = getaddrinfo(endpoint.host.c_str(), service.c_str(), &hints, &raw);
    if (rc != 0) {
        throw std::runtime_error("admin getaddrinfo failed: " + std::string(gai_strerror(rc)));
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(raw, freeaddrinfo);

    for (auto* address = addresses.get(); address != nullptr; address = address->ai_next) {
        const int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd == -1) {
            continue;
        }
        set_reuseaddr(fd);
        if (bind(fd, address->ai_addr, address->ai_addrlen) == 0 && listen(fd, 128) == 0) {
            set_nonblocking(fd);
            return fd;
        }
        close(fd);
    }

    throw std::runtime_error("failed to bind admin listener " + endpoint_label(endpoint));
}

std::string response(const std::string& status,
                     const std::string& content_type,
                     const std::string& body,
                     const std::string& extra_headers = "") {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << "\r\n";
    out << "Content-Type: " << content_type << "\r\n";
    out << "Content-Length: " << body.size() << "\r\n";
    out << "Access-Control-Allow-Origin: *\r\n";
    out << "Access-Control-Allow-Methods: GET,POST,OPTIONS\r\n";
    out << "Access-Control-Allow-Headers: content-type\r\n";
    out << extra_headers;
    out << "Connection: close\r\n\r\n";
    out << body;
    return out.str();
}

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream out;
    out << input.rdbuf();
    return out.str();
}

std::string content_type_for(const std::string& path) {
    if (path.ends_with(".css")) {
        return "text/css";
    }
    if (path.ends_with(".js")) {
        return "application/javascript";
    }
    if (path.ends_with(".svg")) {
        return "image/svg+xml";
    }
    return "text/html";
}

struct AdminClient {
    int fd{-1};
    std::string input;
    std::string output;
    bool sse{false};
    bool close_after_write{false};
    std::chrono::steady_clock::time_point next_stats{};
    std::chrono::steady_clock::time_point next_heartbeat{};
};

constexpr std::size_t kMaxAdminClients = 128;
constexpr std::size_t kMaxRequestBytes = 16 * 1024;
constexpr std::size_t kMaxQueuedBytes = 1 * 1024 * 1024;
constexpr auto kSseStatsInterval = std::chrono::milliseconds(400);
constexpr auto kSseHeartbeatInterval = std::chrono::seconds(5);

std::string json_error(const std::string& message) {
    return "{\"error\":\"" + message + "\"}";
}

std::string sse_headers() {
    return "HTTP/1.1 200 OK\r\n"
           "Content-Type: text/event-stream\r\n"
           "Cache-Control: no-cache\r\n"
           "Connection: keep-alive\r\n"
           "Access-Control-Allow-Origin: *\r\n\r\n";
}

void enqueue_sse(AdminClient& client, const std::shared_ptr<RuntimeState>& runtime) {
    const auto now = std::chrono::steady_clock::now();
    if (client.output.size() >= kMaxQueuedBytes) {
        client.close_after_write = true;
        return;
    }
    if (now >= client.next_stats) {
        client.output += "event: stats\ndata: " + runtime->stats_json() + "\n\n";
        client.next_stats = now + kSseStatsInterval;
    }
    if (now >= client.next_heartbeat) {
        client.output += ": heartbeat\n\n";
        client.next_heartbeat = now + kSseHeartbeatInterval;
    }
}

bool flush_client(AdminClient& client) {
    while (!client.output.empty()) {
        const ssize_t written = send(client.fd, client.output.data(), client.output.size(), MSG_NOSIGNAL);
        if (written > 0) {
            client.output.erase(0, static_cast<std::size_t>(written));
            continue;
        }
        if (written == -1 && errno == EINTR) {
            continue;
        }
        if (written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        return false;
    }
    return !client.close_after_write;
}

void queue_response(AdminClient& client,
                    const std::string& status,
                    const std::string& content_type,
                    const std::string& body) {
    client.output += response(status, content_type, body);
    client.close_after_write = true;
}

void process_request(AdminClient& client, const std::shared_ptr<RuntimeState>& runtime) {
    std::istringstream lines(client.input);
    std::string method;
    std::string path;
    std::string version;
    lines >> method >> path >> version;

    if (method.empty() || path.empty() || version.empty()) {
        queue_response(client, "400 Bad Request", "application/json", json_error("malformed request line"));
        return;
    }

    if (method == "OPTIONS") {
        queue_response(client, "204 No Content", "text/plain", "");
    } else if (method == "GET" && path == "/stats") {
        queue_response(client, "200 OK", "application/json", runtime->stats_json());
    } else if (method == "GET" && path == "/metrics") {
        queue_response(client, "200 OK", "text/plain; version=0.0.4", runtime->prometheus_metrics());
    } else if (method == "GET" && path == "/events") {
        const auto now = std::chrono::steady_clock::now();
        client.sse = true;
        client.output += sse_headers();
        client.next_stats = now;
        client.next_heartbeat = now + kSseHeartbeatInterval;
        enqueue_sse(client, runtime);
    } else if (method == "POST" && path.starts_with("/backends/")) {
        std::string error;
        const auto command = parse_admin_backend_command(path, error);
        if (!command) {
            queue_response(client, "400 Bad Request", "application/json", json_error(error));
            return;
        }
        if (command->backend_id >= runtime->scheduler().backend_count()) {
            queue_response(client, "404 Not Found", "application/json", json_error("backend id not found"));
            return;
        }

        if (command->action == AdminBackendAction::Drain) {
            runtime->set_backend_state(command->backend_id, BackendState::Draining);
        } else {
            runtime->set_backend_state(command->backend_id, BackendState::Up);
        }
        queue_response(client, "200 OK", "application/json", runtime->stats_json());
    } else if (method == "GET") {
        if (path.find("..") != std::string::npos) {
            queue_response(client, "400 Bad Request", "application/json", json_error("invalid static path"));
            return;
        }
        std::string file_path = "web/dist";
        file_path += path == "/" ? "/index.html" : path;
        auto body = read_file(file_path);
        if (body.empty() && path != "/") {
            body = read_file("web/dist/index.html");
        }
        if (body.empty()) {
            queue_response(client, "404 Not Found", "text/plain", "dashboard build not found\n");
        } else {
            queue_response(client, "200 OK", content_type_for(file_path), body);
        }
    } else {
        queue_response(client, "405 Method Not Allowed", "application/json", json_error("method not allowed"));
    }
}

bool read_client(AdminClient& client, const std::shared_ptr<RuntimeState>& runtime) {
    std::array<char, 4096> buffer{};
    while (!client.sse) {
        const ssize_t n = recv(client.fd, buffer.data(), buffer.size(), 0);
        if (n > 0) {
            client.input.append(buffer.data(), static_cast<std::size_t>(n));
            if (client.input.size() > kMaxRequestBytes) {
                queue_response(client, "413 Payload Too Large", "application/json", json_error("request too large"));
                return true;
            }
            if (client.input.find("\r\n\r\n") != std::string::npos || client.input.find("\n\n") != std::string::npos) {
                process_request(client, runtime);
                return true;
            }
            continue;
        }
        if (n == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        return false;
    }

    const ssize_t n = recv(client.fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
    return n != 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

}  // namespace

AdminServer::AdminServer(std::shared_ptr<RuntimeState> runtime) : runtime_(std::move(runtime)) {}

AdminServer::~AdminServer() {
    stop();
}

void AdminServer::start() {
    stop_requested_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this] { run(); });
}

void AdminServer::stop() {
    stop_requested_.store(true, std::memory_order_relaxed);
    if (listener_fd_ != -1) {
        close(listener_fd_);
        listener_fd_ = -1;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void AdminServer::run() {
    try {
        listener_fd_ = make_listener(runtime_->config().admin);
        log(LogLevel::Info, "admin listening on " + endpoint_label(runtime_->config().admin));

        std::vector<AdminClient> clients;
        clients.reserve(kMaxAdminClients);

        while (!stop_requested_.load(std::memory_order_relaxed)) {
            for (auto& client : clients) {
                if (client.sse) {
                    enqueue_sse(client, runtime_);
                }
            }

            std::vector<pollfd> fds;
            fds.reserve(clients.size() + 1);
            fds.push_back(pollfd{listener_fd_, POLLIN, 0});
            for (const auto& client : clients) {
                short events = POLLIN;
                if (!client.output.empty()) {
                    events |= POLLOUT;
                }
                fds.push_back(pollfd{client.fd, events, 0});
            }

            const int ready = poll(fds.data(), fds.size(), 100);
            if (ready == -1) {
                if (errno == EINTR) {
                    continue;
                }
                log(LogLevel::Warn, "admin poll failed");
                break;
            }

            for (std::size_t i = 0; i < clients.size();) {
                bool keep = true;
                const auto revents = fds[i + 1].revents;
                if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                    keep = false;
                }
                if (keep && (revents & POLLIN) != 0) {
                    keep = read_client(clients[i], runtime_);
                }
                if (keep && (revents & POLLOUT) != 0) {
                    keep = flush_client(clients[i]);
                }
                if (!keep) {
                    close(clients[i].fd);
                    clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i));
                    continue;
                }
                ++i;
            }

            if ((fds[0].revents & POLLIN) != 0) {
                while (clients.size() < kMaxAdminClients) {
                    sockaddr_storage address{};
                    socklen_t address_length = sizeof(address);
                    const int fd = accept(listener_fd_, reinterpret_cast<sockaddr*>(&address), &address_length);
                    if (fd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        if (errno == EINTR) {
                            continue;
                        }
                        log(LogLevel::Warn, "admin accept failed");
                        break;
                    }
                    set_nonblocking(fd);
                    AdminClient client;
                    client.fd = fd;
                    clients.push_back(std::move(client));
                }

                if (clients.size() >= kMaxAdminClients) {
                    sockaddr_storage address{};
                    socklen_t address_length = sizeof(address);
                    const int fd = accept(listener_fd_, reinterpret_cast<sockaddr*>(&address), &address_length);
                    if (fd != -1) {
                        const auto busy = response("503 Service Unavailable",
                                                   "application/json",
                                                   json_error("too many admin clients"));
                        send(fd, busy.data(), busy.size(), MSG_NOSIGNAL);
                        close(fd);
                    }
                }
            }
        }

        for (auto& client : clients) {
            close(client.fd);
        }
    } catch (const std::exception& ex) {
        log(LogLevel::Error, std::string("admin server failed: ") + ex.what());
    }
}

}  // namespace lb
