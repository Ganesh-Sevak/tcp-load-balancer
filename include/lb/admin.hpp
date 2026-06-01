#pragma once

#include "lb/runtime.hpp"

#include <atomic>
#include <memory>
#include <thread>

namespace lb {

class AdminServer {
public:
    explicit AdminServer(std::shared_ptr<RuntimeState> runtime);
    ~AdminServer();

    AdminServer(const AdminServer&) = delete;
    AdminServer& operator=(const AdminServer&) = delete;

    void start();
    void stop();

private:
    void run();

    std::shared_ptr<RuntimeState> runtime_;
    std::atomic_bool stop_requested_{false};
    std::thread thread_;
    int listener_fd_{-1};
};

}  // namespace lb

