#include "lb/config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string>

namespace lb {
namespace {

std::string trim(std::string value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

}  // namespace

Policy parse_policy(const std::string& value) {
    const auto normalized = lower(trim(value));
    if (normalized == "round-robin" || normalized == "round_robin" || normalized == "rr") {
        return Policy::RoundRobin;
    }
    if (normalized == "least-connections" || normalized == "least_connections" || normalized == "lc") {
        return Policy::LeastConnections;
    }
    throw std::invalid_argument("unknown scheduling policy: " + value);
}

std::string to_string(Policy policy) {
    switch (policy) {
        case Policy::RoundRobin:
            return "round-robin";
        case Policy::LeastConnections:
            return "least-connections";
    }
    return "unknown";
}

Endpoint parse_endpoint(const std::string& value) {
    const auto input = trim(value);
    const auto pos = input.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= input.size()) {
        throw std::invalid_argument("endpoint must be host:port: " + value);
    }

    const auto port_number = std::stoul(input.substr(pos + 1));
    if (port_number == 0 || port_number > 65535) {
        throw std::invalid_argument("port out of range in endpoint: " + value);
    }

    return Endpoint{input.substr(0, pos), static_cast<std::uint16_t>(port_number)};
}

AppConfig load_config_file(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("failed to open config file: " + path);
    }

    AppConfig config;
    std::string line;
    std::size_t line_number = 0;

    while (std::getline(input, line)) {
        ++line_number;
        const auto comment = line.find('#');
        if (comment != std::string::npos) {
            line.erase(comment);
        }

        line = trim(line);
        if (line.empty()) {
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            throw std::runtime_error("invalid config line " + std::to_string(line_number) + ": " + line);
        }

        const auto key = lower(trim(line.substr(0, equals)));
        const auto value = trim(line.substr(equals + 1));

        if (key == "listen") {
            config.listen = parse_endpoint(value);
        } else if (key == "policy") {
            config.policy = parse_policy(value);
        } else if (key == "backend") {
            config.backends.push_back(parse_endpoint(value));
        } else {
            throw std::runtime_error("unknown config key on line " + std::to_string(line_number) + ": " + key);
        }
    }

    if (config.backends.empty()) {
        throw std::runtime_error("config must define at least one backend");
    }

    return config;
}

}  // namespace lb

