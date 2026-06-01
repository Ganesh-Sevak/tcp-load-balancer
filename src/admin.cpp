#include "lb/admin.hpp"

#include "lb/logger.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <netdb.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

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

bool send_all(int fd, const std::string& payload) {
    const char* data = payload.data();
    std::size_t remaining = payload.size();
    while (remaining > 0) {
        const ssize_t written = send(fd, data, remaining, MSG_NOSIGNAL);
        if (written > 0) {
            data += written;
            remaining -= static_cast<std::size_t>(written);
            continue;
        }
        if (written == -1 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
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

void stream_events(int fd, const std::shared_ptr<RuntimeState>& runtime) {
    std::ostringstream headers;
    headers << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: text/event-stream\r\n"
            << "Cache-Control: no-cache\r\n"
            << "Connection: keep-alive\r\n"
            << "Access-Control-Allow-Origin: *\r\n\r\n";
    if (!send_all(fd, headers.str())) {
        return;
    }

    for (int i = 0; i < 7200; ++i) {
        const auto payload = "event: stats\ndata: " + runtime->stats_json() + "\n\n";
        if (!send_all(fd, payload)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
}

void handle_client(int fd, std::shared_ptr<RuntimeState> runtime) {
    std::string request(4096, '\0');
    const ssize_t n = recv(fd, request.data(), request.size() - 1, 0);
    if (n <= 0) {
        close(fd);
        return;
    }
    request.resize(static_cast<std::size_t>(n));

    std::istringstream lines(request);
    std::string method;
    std::string path;
    std::string version;
    lines >> method >> path >> version;

    if (method == "OPTIONS") {
        send_all(fd, response("204 No Content", "text/plain", ""));
        close(fd);
        return;
    }

    if (method == "GET" && path == "/stats") {
        send_all(fd, response("200 OK", "application/json", runtime->stats_json()));
    } else if (method == "GET" && path == "/metrics") {
        send_all(fd, response("200 OK", "text/plain; version=0.0.4", runtime->prometheus_metrics()));
    } else if (method == "GET" && path == "/events") {
        stream_events(fd, runtime);
    } else if (method == "POST" && path.starts_with("/backends/")) {
        const auto tail = path.substr(std::string("/backends/").size());
        const auto slash = tail.find('/');
        if (slash == std::string::npos) {
            send_all(fd, response("404 Not Found", "application/json", "{\"error\":\"not found\"}"));
        } else {
            const auto id = static_cast<std::size_t>(std::stoul(tail.substr(0, slash)));
            const auto action = tail.substr(slash + 1);
            if (action == "drain") {
                runtime->set_backend_state(id, BackendState::Draining);
                send_all(fd, response("200 OK", "application/json", runtime->stats_json()));
            } else if (action == "enable") {
                runtime->set_backend_state(id, BackendState::Up);
                send_all(fd, response("200 OK", "application/json", runtime->stats_json()));
            } else {
                send_all(fd, response("404 Not Found", "application/json", "{\"error\":\"not found\"}"));
            }
        }
    } else if (method == "GET") {
        std::string file_path = "web/dist";
        file_path += path == "/" ? "/index.html" : path;
        auto body = read_file(file_path);
        if (body.empty() && path != "/") {
            body = read_file("web/dist/index.html");
        }
        if (body.empty()) {
            send_all(fd, response("404 Not Found", "text/plain", "dashboard build not found\n"));
        } else {
            send_all(fd, response("200 OK", content_type_for(file_path), body));
        }
    } else {
        send_all(fd, response("405 Method Not Allowed", "application/json", "{\"error\":\"method not allowed\"}"));
    }

    close(fd);
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

        while (!stop_requested_.load(std::memory_order_relaxed)) {
            sockaddr_storage address{};
            socklen_t address_length = sizeof(address);
            const int fd = accept(listener_fd_, reinterpret_cast<sockaddr*>(&address), &address_length);
            if (fd == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    continue;
                }
                break;
            }
            std::thread(handle_client, fd, runtime_).detach();
        }
    } catch (const std::exception& ex) {
        log(LogLevel::Error, std::string("admin server failed: ") + ex.what());
    }
}

}  // namespace lb

