#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lb {

enum class Policy {
    RoundRobin,
    LeastConnections,
};

struct Endpoint {
    std::string host;
    std::uint16_t port{};
};

struct AppConfig {
    Endpoint listen{"0.0.0.0", 9000};
    Policy policy{Policy::RoundRobin};
    std::vector<Endpoint> backends;
};

Policy parse_policy(const std::string& value);
std::string to_string(Policy policy);
Endpoint parse_endpoint(const std::string& value);
AppConfig load_config_file(const std::string& path);

}  // namespace lb

