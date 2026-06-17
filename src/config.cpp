#include "lb/config.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <limits>
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

std::uint32_t parse_u32(const std::string& value, std::uint32_t max_value, const std::string& field) {
    const auto input = trim(value);
    std::uint32_t parsed = 0;
    const auto* begin = input.data();
    const auto* end = input.data() + input.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (input.empty() || ec != std::errc{} || ptr != end || parsed > max_value) {
        throw std::invalid_argument(field + " out of range: " + value);
    }
    return parsed;
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
    if (normalized == "power-of-two-choices" || normalized == "power_of_two_choices" || normalized == "p2c") {
        return Policy::PowerOfTwoChoices;
    }
    throw std::invalid_argument("unknown scheduling policy: " + value);
}

std::string to_string(Policy policy) {
    switch (policy) {
        case Policy::RoundRobin:
            return "round-robin";
        case Policy::LeastConnections:
            return "least-connections";
        case Policy::PowerOfTwoChoices:
            return "power-of-two-choices";
    }
    return "unknown";
}

Endpoint parse_endpoint(const std::string& value) {
    const auto input = trim(value);
    const auto pos = input.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= input.size()) {
        throw std::invalid_argument("endpoint must be host:port: " + value);
    }

    const auto port_number = parse_u32(input.substr(pos + 1), std::numeric_limits<std::uint16_t>::max(), "port");
    if (port_number == 0) {
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
        } else if (key == "admin") {
            config.admin = parse_endpoint(value);
        } else if (key == "policy") {
            config.policy = parse_policy(value);
        } else if (key == "backend") {
            config.backends.push_back(parse_endpoint(value));
        } else if (key == "workers") {
            config.worker_count = parse_u32(value, std::numeric_limits<std::uint32_t>::max(), key);
        } else if (key == "file_limit") {
            config.file_limit = parse_u32(value, std::numeric_limits<std::uint32_t>::max(), key);
        } else if (key == "connect_timeout_ms") {
            config.connect_timeout_ms = parse_u32(value, std::numeric_limits<std::uint32_t>::max(), key);
        } else if (key == "idle_timeout_ms") {
            config.idle_timeout_ms = parse_u32(value, std::numeric_limits<std::uint32_t>::max(), key);
        } else if (key == "health_interval_ms") {
            config.health_interval_ms = parse_u32(value, std::numeric_limits<std::uint32_t>::max(), key);
        } else if (key == "health_timeout_ms") {
            config.health_timeout_ms = parse_u32(value, std::numeric_limits<std::uint32_t>::max(), key);
        } else if (key == "passive_failure_threshold") {
            config.passive_failure_threshold = parse_u32(value, std::numeric_limits<std::uint32_t>::max(), key);
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
