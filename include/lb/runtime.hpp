#pragma once

#include "lb/config.hpp"
#include "lb/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lb {

struct RuntimeMetrics {
    std::chrono::steady_clock::time_point started_at{std::chrono::steady_clock::now()};
    std::atomic<std::uint64_t> active_connections{0};
    std::atomic<std::uint64_t> total_connections{0};
    std::atomic<std::uint64_t> peak_connections{0};
    std::atomic<std::uint64_t> bytes_from_clients{0};
    std::atomic<std::uint64_t> bytes_from_backends{0};
    std::atomic<std::uint64_t> connection_errors{0};
    std::atomic<std::uint64_t> connect_timeouts{0};
    std::atomic<std::uint64_t> idle_timeouts{0};
};

struct BackendRuntime {
    Endpoint endpoint;
    std::atomic<std::uint64_t> bytes_from_client{0};
    std::atomic<std::uint64_t> bytes_from_backend{0};
    std::atomic<std::uint64_t> connect_latency_us{0};
};

class RuntimeState {
public:
    RuntimeState(AppConfig config, std::shared_ptr<Scheduler> scheduler);

    [[nodiscard]] const AppConfig& config() const;
    [[nodiscard]] Scheduler& scheduler();
    [[nodiscard]] const Scheduler& scheduler() const;
    [[nodiscard]] RuntimeMetrics& metrics();
    [[nodiscard]] const RuntimeMetrics& metrics() const;
    [[nodiscard]] BackendRuntime& backend(std::size_t index);
    [[nodiscard]] const BackendRuntime& backend(std::size_t index) const;
    [[nodiscard]] std::string stats_json() const;
    [[nodiscard]] std::string prometheus_metrics() const;

    void record_connection_open(std::size_t backend_index);
    void record_connection_close(std::size_t backend_index);
    void record_bytes_from_client(std::size_t backend_index, std::uint64_t count);
    void record_bytes_from_backend(std::size_t backend_index, std::uint64_t count);
    void record_backend_failure(std::size_t backend_index);
    void record_connect_latency(std::size_t backend_index, std::uint64_t latency_us);
    void set_backend_state(std::size_t backend_index, BackendState state);

private:
    AppConfig config_;
    std::shared_ptr<Scheduler> scheduler_;
    RuntimeMetrics metrics_;
    std::vector<BackendRuntime> backends_;
};

}  // namespace lb

