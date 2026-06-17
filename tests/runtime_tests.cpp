#include "lb/runtime.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "test failed: " << message << "\n";
        std::exit(1);
    }
}

lb::AppConfig config() {
    lb::AppConfig app_config;
    app_config.worker_count = 2;
    app_config.backends = {{"127.0.0.1", 9101}, {"127.0.0.1", 9102}};
    return app_config;
}

std::shared_ptr<lb::RuntimeState> runtime() {
    auto app_config = config();
    auto scheduler = std::make_shared<lb::Scheduler>(app_config.backends, app_config.policy);
    return std::make_shared<lb::RuntimeState>(app_config, scheduler);
}

void stats_json_includes_latency_histogram() {
    auto state = runtime();
    state->record_connect_latency(0, 900);
    state->record_connect_latency(1, 4200);

    const auto json = state->stats_json();
    require(json.find("\"kind\":\"backend_connect\"") != std::string::npos, "latency kind included");
    require(json.find("\"p50\":1") != std::string::npos, "p50 latency bucket included");
    require(json.find("\"p99\":5") != std::string::npos, "p99 latency bucket included");
    require(json.find("\"buckets\":[") != std::string::npos, "latency buckets included");
    require(json.find("\"le_us\":1000") != std::string::npos, "latency bucket upper bound included");
}

void worker_metrics_are_aggregated() {
    auto state = runtime();
    state->record_connection_open(0, 0);
    state->record_connection_open(1, 1);
    state->record_bytes_from_client(0, 0, 12);
    state->record_bytes_from_backend(1, 1, 34);
    state->record_backend_failure(1, 1);
    state->record_connect_timeout(0);
    state->record_idle_timeout(1);

    const auto json = state->stats_json();
    require(json.find("\"active_connections\":2") != std::string::npos, "active connections aggregated");
    require(json.find("\"total_connections\":2") != std::string::npos, "total connections aggregated");
    require(json.find("\"bytes_in\":12") != std::string::npos, "client bytes aggregated");
    require(json.find("\"bytes_out\":34") != std::string::npos, "backend bytes aggregated");
    require(json.find("\"errors\":1") != std::string::npos, "errors aggregated");
    require(json.find("\"connect_timeouts\":1") != std::string::npos, "connect timeouts aggregated");
    require(json.find("\"idle_timeouts\":1") != std::string::npos, "idle timeouts aggregated");
    require(json.find("\"workers\":[") != std::string::npos, "worker summaries included");
}

void prometheus_includes_connect_latency_histogram() {
    auto state = runtime();
    state->record_connect_latency(0, 900);

    const auto metrics = state->prometheus_metrics();
    require(metrics.find("lb_backend_connect_latency_us_bucket{le=\"1000\"} 1") != std::string::npos,
            "prometheus finite latency bucket included");
    require(metrics.find("lb_backend_connect_latency_us_bucket{le=\"+Inf\"} 1") != std::string::npos,
            "prometheus infinite latency bucket included");
    require(metrics.find("lb_backend_connect_latency_us_count 1") != std::string::npos,
            "prometheus latency count included");
}

}  // namespace

int main() {
    stats_json_includes_latency_histogram();
    worker_metrics_are_aggregated();
    prometheus_includes_connect_latency_histogram();

    std::cout << "runtime tests passed\n";
    return 0;
}
