#include "lb/config.hpp"
#include "lb/server.hpp"

#include <atomic>
#include <csignal>
#include <exception>
#include <iostream>
#include <string>

namespace {

std::atomic_bool stop_requested{false};

void handle_signal(int) {
    stop_requested.store(true);
}

void usage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " --config <path>\n"
              << "\n"
              << "Options:\n"
              << "  --config <path>  Load balancer config file. Default: config/backends.conf\n"
              << "  --help           Show this help text\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path = "config/backends.conf";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        }
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
            continue;
        }

        std::cerr << "unknown or incomplete argument: " << arg << "\n";
        usage(argv[0]);
        return 2;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    try {
        auto config = lb::load_config_file(config_path);
        lb::TcpLoadBalancer server(std::move(config));
        server.run(stop_requested);
    } catch (const std::exception& ex) {
        std::cerr << "fatal: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}

