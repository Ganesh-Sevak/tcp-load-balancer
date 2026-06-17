#include "lb/config.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string input(reinterpret_cast<const char*>(data), size);

    try {
        (void)lb::parse_policy(input);
    } catch (...) {
    }

    try {
        (void)lb::parse_endpoint(input);
    } catch (...) {
    }

    return 0;
}
