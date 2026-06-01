#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lb {

enum class Policy {
    RoundRobin,
    LeastConnections,
    PowerOfTwoChoices,
};

struct Endpoint {
    std::string host;
    std::uint16_t port{};
};

struct AppConfig {
    Endpoint listen{"0.0.0.0", 9000};
    Endpoint admin{"127.0.0.1", 9100};
    Policy policy{Policy::RoundRobin};
    std::vector<Endpoint> backends;
    std::uint32_t worker_count{0};
    std::uint32_t file_limit{25000};
    std::uint32_t connect_timeout_ms{3000};
    std::uint32_t idle_timeout_ms{60000};
    std::uint32_t health_interval_ms{2000};
    std::uint32_t health_timeout_ms{500};
    std::uint32_t passive_failure_threshold{3};
};

Policy parse_policy(const std::string& value);
std::string to_string(Policy policy);
Endpoint parse_endpoint(const std::string& value);
AppConfig load_config_file(const std::string& path);

}  // namespace lb
