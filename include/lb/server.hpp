#pragma once

#include "lb/config.hpp"
#include "lb/scheduler.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace lb {

class TcpLoadBalancer {
public:
    explicit TcpLoadBalancer(AppConfig config);
    ~TcpLoadBalancer();

    TcpLoadBalancer(const TcpLoadBalancer&) = delete;
    TcpLoadBalancer& operator=(const TcpLoadBalancer&) = delete;

    void run(std::atomic_bool& stop_requested);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace lb

