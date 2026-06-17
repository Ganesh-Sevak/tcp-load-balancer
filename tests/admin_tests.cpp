#include "lb/admin.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "test failed: " << message << "\n";
        std::exit(1);
    }
}

void parses_valid_backend_commands() {
    std::string error;
    const auto drain = lb::parse_admin_backend_command("/backends/12/drain", error);
    require(drain.has_value(), "drain command parsed");
    require(drain->backend_id == 12, "drain backend id parsed");
    require(drain->action == lb::AdminBackendAction::Drain, "drain action parsed");

    const auto enable = lb::parse_admin_backend_command("/backends/7/enable", error);
    require(enable.has_value(), "enable command parsed");
    require(enable->backend_id == 7, "enable backend id parsed");
    require(enable->action == lb::AdminBackendAction::Enable, "enable action parsed");
}

void rejects_malformed_backend_commands_without_throwing() {
    std::string error;
    require(!lb::parse_admin_backend_command("/backends/not-a-number/drain", error).has_value(),
            "non-numeric backend id rejected");
    require(!error.empty(), "non-numeric backend id reports error");

    error.clear();
    require(!lb::parse_admin_backend_command("/backends/1", error).has_value(), "missing action rejected");
    require(!error.empty(), "missing action reports error");

    error.clear();
    require(!lb::parse_admin_backend_command("/backends/1/delete", error).has_value(),
            "unsupported action rejected");
    require(!error.empty(), "unsupported action reports error");
}

}  // namespace

int main() {
    parses_valid_backend_commands();
    rejects_malformed_backend_commands_without_throwing();

    std::cout << "admin tests passed\n";
    return 0;
}
