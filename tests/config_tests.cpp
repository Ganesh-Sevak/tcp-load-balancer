#include "lb/config.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "test failed: " << message << "\n";
        std::exit(1);
    }
}

void endpoint_parser_accepts_host_port() {
    const auto endpoint = lb::parse_endpoint("127.0.0.1:9000");
    require(endpoint.host == "127.0.0.1", "endpoint host parsed");
    require(endpoint.port == 9000, "endpoint port parsed");
}

void policy_parser_accepts_aliases() {
    require(lb::parse_policy("round-robin") == lb::Policy::RoundRobin, "round-robin parsed");
    require(lb::parse_policy("rr") == lb::Policy::RoundRobin, "rr alias parsed");
    require(lb::parse_policy("least-connections") == lb::Policy::LeastConnections, "least-connections parsed");
    require(lb::parse_policy("lc") == lb::Policy::LeastConnections, "lc alias parsed");
}

void endpoint_parser_rejects_invalid_ports() {
    bool rejected = false;
    try {
        (void)lb::parse_endpoint("127.0.0.1:70000");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "invalid port rejected");
}

}  // namespace

int main() {
    endpoint_parser_accepts_host_port();
    policy_parser_accepts_aliases();
    endpoint_parser_rejects_invalid_ports();

    std::cout << "config tests passed\n";
    return 0;
}
