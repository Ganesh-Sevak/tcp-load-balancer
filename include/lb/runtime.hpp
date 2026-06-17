#pragma once

#include "lb/config.hpp"
#include "lb/scheduler.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lb {

inline constexpr std::array<std::uint64_t, 13> kConnectLatencyBucketUpperBoundsUs{
    100, 250, 500, 1000, 2500, 5000, 10000, 25000, 50000, 100000, 250000, 500000, 1000000};

struct RuntimeMetrics {
    std::chrono::steady_clock::time_point started_at{std::chrono::steady_clock::now()};
    std::atomic<std::uint64_t> control_errors{0};
};

struct alignas(64) WorkerRuntimeMetrics {
    std::atomic<std::uint64_t> active_connections{0};
    std::atomic<std::uint64_t> total_connections{0};
    std::atomic<std::uint64_t> peak_connections{0};
    std::atomic<std::uint64_t> bytes_from_clients{0};
    std::atomic<std::uint64_t> bytes_from_backends{0};
    std::atomic<std::uint64_t> connection_errors{0};
    std::atomic<std::uint64_t> connect_timeouts{0};
    std::atomic<std::uint64_t> idle_timeouts{0};
};

struct ConnectLatencyHistogram {
    std::array<std::atomic<std::uint64_t>, kConnectLatencyBucketUpperBoundsUs.size()> buckets{};
    std::atomic<std::uint64_t> overflow{0};
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> sum_us{0};
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
    [[nodiscard]] const WorkerRuntimeMetrics& worker_metrics(std::size_t worker_id) const;
    [[nodiscard]] BackendRuntime& backend(std::size_t index);
    [[nodiscard]] const BackendRuntime& backend(std::size_t index) const;
    [[nodiscard]] std::string stats_json() const;
    [[nodiscard]] std::string prometheus_metrics() const;

    void record_connection_open(std::size_t worker_id, std::size_t backend_index);
    void record_connection_close(std::size_t worker_id, std::size_t backend_index);
    void record_bytes_from_client(std::size_t worker_id, std::size_t backend_index, std::uint64_t count);
    void record_bytes_from_backend(std::size_t worker_id, std::size_t backend_index, std::uint64_t count);
    void record_backend_failure(std::size_t worker_id, std::size_t backend_index);
    void record_backend_failure(std::size_t backend_index);
    void record_connect_latency(std::size_t backend_index, std::uint64_t latency_us);
    void record_connect_timeout(std::size_t worker_id);
    void record_idle_timeout(std::size_t worker_id);
    void set_backend_state(std::size_t backend_index, BackendState state);

private:
    [[nodiscard]] WorkerRuntimeMetrics& worker_metrics(std::size_t worker_id);

    AppConfig config_;
    std::shared_ptr<Scheduler> scheduler_;
    RuntimeMetrics metrics_;
    std::vector<WorkerRuntimeMetrics> workers_;
    std::vector<BackendRuntime> backends_;
    ConnectLatencyHistogram connect_latency_;
};

}  // namespace lb
