#include "lb/runtime.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace lb {
namespace {

std::string endpoint_label(const Endpoint& endpoint) {
    return endpoint.host + ":" + std::to_string(endpoint.port);
}

void update_peak(std::atomic<std::uint64_t>& peak, std::uint64_t value) {
    auto current = peak.load(std::memory_order_relaxed);
    while (value > current && !peak.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

std::string json_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char c : value) {
        if (c == '"' || c == '\\') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    return escaped;
}

struct AggregatedMetrics {
    std::uint64_t active_connections{0};
    std::uint64_t total_connections{0};
    std::uint64_t peak_connections{0};
    std::uint64_t bytes_from_clients{0};
    std::uint64_t bytes_from_backends{0};
    std::uint64_t connection_errors{0};
    std::uint64_t connect_timeouts{0};
    std::uint64_t idle_timeouts{0};
};

AggregatedMetrics aggregate_workers(const std::vector<WorkerRuntimeMetrics>& workers) {
    AggregatedMetrics aggregate;
    for (const auto& worker : workers) {
        aggregate.active_connections += worker.active_connections.load(std::memory_order_relaxed);
        aggregate.total_connections += worker.total_connections.load(std::memory_order_relaxed);
        aggregate.peak_connections += worker.peak_connections.load(std::memory_order_relaxed);
        aggregate.bytes_from_clients += worker.bytes_from_clients.load(std::memory_order_relaxed);
        aggregate.bytes_from_backends += worker.bytes_from_backends.load(std::memory_order_relaxed);
        aggregate.connection_errors += worker.connection_errors.load(std::memory_order_relaxed);
        aggregate.connect_timeouts += worker.connect_timeouts.load(std::memory_order_relaxed);
        aggregate.idle_timeouts += worker.idle_timeouts.load(std::memory_order_relaxed);
    }
    return aggregate;
}

std::uint64_t percentile_bucket_us(const std::array<std::uint64_t, kConnectLatencyBucketUpperBoundsUs.size()>& buckets,
                                   std::uint64_t overflow,
                                   double percentile) {
    std::uint64_t total = overflow;
    for (const auto count : buckets) {
        total += count;
    }
    if (total == 0) {
        return 0;
    }

    const auto rank = static_cast<std::uint64_t>(std::ceil((percentile / 100.0) * static_cast<double>(total)));
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < buckets.size(); ++i) {
        seen += buckets[i];
        if (seen >= rank) {
            return kConnectLatencyBucketUpperBoundsUs[i];
        }
    }
    return kConnectLatencyBucketUpperBoundsUs.back();
}

}  // namespace

RuntimeState::RuntimeState(AppConfig config, std::shared_ptr<Scheduler> scheduler)
    : config_(std::move(config)), scheduler_(std::move(scheduler)) {
    workers_ = std::vector<WorkerRuntimeMetrics>(std::max<std::uint32_t>(1, config_.worker_count));
    backends_ = std::vector<BackendRuntime>(config_.backends.size());
    for (std::size_t i = 0; i < config_.backends.size(); ++i) {
        backends_[i].endpoint = config_.backends[i];
    }
}

const AppConfig& RuntimeState::config() const {
    return config_;
}

Scheduler& RuntimeState::scheduler() {
    return *scheduler_;
}

const Scheduler& RuntimeState::scheduler() const {
    return *scheduler_;
}

RuntimeMetrics& RuntimeState::metrics() {
    return metrics_;
}

const RuntimeMetrics& RuntimeState::metrics() const {
    return metrics_;
}

WorkerRuntimeMetrics& RuntimeState::worker_metrics(std::size_t worker_id) {
    if (worker_id >= workers_.size()) {
        throw std::out_of_range("worker index out of range");
    }
    return workers_[worker_id];
}

const WorkerRuntimeMetrics& RuntimeState::worker_metrics(std::size_t worker_id) const {
    if (worker_id >= workers_.size()) {
        throw std::out_of_range("worker index out of range");
    }
    return workers_[worker_id];
}

BackendRuntime& RuntimeState::backend(std::size_t index) {
    if (index >= backends_.size()) {
        throw std::out_of_range("backend index out of range");
    }
    return backends_[index];
}

const BackendRuntime& RuntimeState::backend(std::size_t index) const {
    if (index >= backends_.size()) {
        throw std::out_of_range("backend index out of range");
    }
    return backends_[index];
}

std::string RuntimeState::stats_json() const {
    const auto now = std::chrono::steady_clock::now();
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - metrics_.started_at).count();
    auto aggregate = aggregate_workers(workers_);
    aggregate.connection_errors += metrics_.control_errors.load(std::memory_order_relaxed);

    std::array<std::uint64_t, kConnectLatencyBucketUpperBoundsUs.size()> latency_buckets{};
    for (std::size_t i = 0; i < latency_buckets.size(); ++i) {
        latency_buckets[i] = connect_latency_.buckets[i].load(std::memory_order_relaxed);
    }
    const auto latency_overflow = connect_latency_.overflow.load(std::memory_order_relaxed);
    const auto p50_us = percentile_bucket_us(latency_buckets, latency_overflow, 50.0);
    const auto p99_us = percentile_bucket_us(latency_buckets, latency_overflow, 99.0);
    const auto p999_us = percentile_bucket_us(latency_buckets, latency_overflow, 99.9);

    std::ostringstream out;
    out << "{";
    out << "\"uptime_seconds\":" << uptime << ",";
    out << "\"listener\":\"" << json_escape(endpoint_label(config_.listen)) << "\",";
    out << "\"policy\":\"" << to_string(config_.policy) << "\",";
    out << "\"worker_count\":" << config_.worker_count << ",";
    out << "\"active_connections\":" << aggregate.active_connections << ",";
    out << "\"peak_connections\":" << aggregate.peak_connections << ",";
    out << "\"total_connections\":" << aggregate.total_connections << ",";
    out << "\"connections_per_sec\":"
        << (uptime > 0 ? static_cast<double>(aggregate.total_connections) / static_cast<double>(uptime) : 0.0)
        << ",";
    out << "\"bytes_in\":" << aggregate.bytes_from_clients << ",";
    out << "\"bytes_out\":" << aggregate.bytes_from_backends << ",";
    out << "\"errors\":" << aggregate.connection_errors << ",";
    out << "\"connect_timeouts\":" << aggregate.connect_timeouts << ",";
    out << "\"idle_timeouts\":" << aggregate.idle_timeouts << ",";
    out << "\"latency\":{\"kind\":\"backend_connect\",";
    out << "\"p50\":" << static_cast<double>(p50_us) / 1000.0 << ",";
    out << "\"p99\":" << static_cast<double>(p99_us) / 1000.0 << ",";
    out << "\"p999\":" << static_cast<double>(p999_us) / 1000.0 << ",";
    out << "\"count\":" << connect_latency_.count.load(std::memory_order_relaxed) << ",";
    out << "\"sum_us\":" << connect_latency_.sum_us.load(std::memory_order_relaxed) << ",";
    out << "\"buckets\":[";
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < latency_buckets.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        cumulative += latency_buckets[i];
        out << "{\"le_us\":" << kConnectLatencyBucketUpperBoundsUs[i] << ",\"count\":" << cumulative << "}";
    }
    out << "]},";
    out << "\"workers\":[";
    for (std::size_t i = 0; i < workers_.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        const auto& worker = workers_[i];
        out << "{";
        out << "\"id\":" << i << ",";
        out << "\"active_connections\":" << worker.active_connections.load(std::memory_order_relaxed) << ",";
        out << "\"total_connections\":" << worker.total_connections.load(std::memory_order_relaxed) << ",";
        out << "\"peak_connections\":" << worker.peak_connections.load(std::memory_order_relaxed) << ",";
        out << "\"bytes_in\":" << worker.bytes_from_clients.load(std::memory_order_relaxed) << ",";
        out << "\"bytes_out\":" << worker.bytes_from_backends.load(std::memory_order_relaxed) << ",";
        out << "\"errors\":" << worker.connection_errors.load(std::memory_order_relaxed);
        out << "}";
    }
    out << "],";
    out << "\"backends\":[";
    for (std::size_t i = 0; i < backends_.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        const auto& backend = backends_[i];
        out << "{";
        out << "\"id\":" << i << ",";
        out << "\"address\":\"" << json_escape(endpoint_label(backend.endpoint)) << "\",";
        out << "\"state\":\"" << to_string(scheduler_->state(i)) << "\",";
        out << "\"active_connections\":" << scheduler_->active_connections(i) << ",";
        out << "\"total_connections\":" << scheduler_->total_connections(i) << ",";
        out << "\"bytes_in\":" << backend.bytes_from_client.load(std::memory_order_relaxed) << ",";
        out << "\"bytes_out\":" << backend.bytes_from_backend.load(std::memory_order_relaxed) << ",";
        out << "\"errors\":" << scheduler_->error_count(i) << ",";
        out << "\"connect_latency_us\":" << backend.connect_latency_us.load(std::memory_order_relaxed);
        out << "}";
    }
    out << "]}";
    return out.str();
}

std::string RuntimeState::prometheus_metrics() const {
    auto aggregate = aggregate_workers(workers_);
    aggregate.connection_errors += metrics_.control_errors.load(std::memory_order_relaxed);

    std::ostringstream out;
    out << "# TYPE lb_active_connections gauge\n";
    out << "lb_active_connections " << aggregate.active_connections << "\n";
    out << "# TYPE lb_total_connections counter\n";
    out << "lb_total_connections " << aggregate.total_connections << "\n";
    out << "# TYPE lb_bytes_from_clients counter\n";
    out << "lb_bytes_from_clients " << aggregate.bytes_from_clients << "\n";
    out << "# TYPE lb_bytes_from_backends counter\n";
    out << "lb_bytes_from_backends " << aggregate.bytes_from_backends << "\n";
    out << "# TYPE lb_connection_errors counter\n";
    out << "lb_connection_errors " << aggregate.connection_errors << "\n";
    out << "# TYPE lb_connect_timeouts counter\n";
    out << "lb_connect_timeouts " << aggregate.connect_timeouts << "\n";
    out << "# TYPE lb_idle_timeouts counter\n";
    out << "lb_idle_timeouts " << aggregate.idle_timeouts << "\n";
    out << "# TYPE lb_backend_connect_latency_us histogram\n";
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < kConnectLatencyBucketUpperBoundsUs.size(); ++i) {
        cumulative += connect_latency_.buckets[i].load(std::memory_order_relaxed);
        out << "lb_backend_connect_latency_us_bucket{le=\"" << kConnectLatencyBucketUpperBoundsUs[i] << "\"} "
            << cumulative << "\n";
    }
    out << "lb_backend_connect_latency_us_bucket{le=\"+Inf\"} "
        << connect_latency_.count.load(std::memory_order_relaxed) << "\n";
    out << "lb_backend_connect_latency_us_sum " << connect_latency_.sum_us.load(std::memory_order_relaxed) << "\n";
    out << "lb_backend_connect_latency_us_count " << connect_latency_.count.load(std::memory_order_relaxed) << "\n";
    for (std::size_t i = 0; i < backends_.size(); ++i) {
        out << "lb_backend_active_connections{backend=\"" << endpoint_label(backends_[i].endpoint) << "\"} "
            << scheduler_->active_connections(i) << "\n";
        out << "lb_backend_up{backend=\"" << endpoint_label(backends_[i].endpoint) << "\"} "
            << (scheduler_->state(i) == BackendState::Up ? 1 : 0) << "\n";
    }
    return out.str();
}

void RuntimeState::record_connection_open(std::size_t worker_id, std::size_t backend_index) {
    scheduler_->mark_open(backend_index);
    auto& worker = worker_metrics(worker_id);
    const auto active = worker.active_connections.fetch_add(1, std::memory_order_relaxed) + 1;
    worker.total_connections.fetch_add(1, std::memory_order_relaxed);
    update_peak(worker.peak_connections, active);
}

void RuntimeState::record_connection_close(std::size_t worker_id, std::size_t backend_index) {
    scheduler_->mark_closed(backend_index);
    auto& worker = worker_metrics(worker_id);
    auto current = worker.active_connections.load(std::memory_order_relaxed);
    while (current > 0 &&
           !worker.active_connections.compare_exchange_weak(current, current - 1, std::memory_order_relaxed)) {
    }
}

void RuntimeState::record_bytes_from_client(std::size_t worker_id, std::size_t backend_index, std::uint64_t count) {
    worker_metrics(worker_id).bytes_from_clients.fetch_add(count, std::memory_order_relaxed);
    backend(backend_index).bytes_from_client.fetch_add(count, std::memory_order_relaxed);
}

void RuntimeState::record_bytes_from_backend(std::size_t worker_id, std::size_t backend_index, std::uint64_t count) {
    worker_metrics(worker_id).bytes_from_backends.fetch_add(count, std::memory_order_relaxed);
    backend(backend_index).bytes_from_backend.fetch_add(count, std::memory_order_relaxed);
}

void RuntimeState::record_backend_failure(std::size_t worker_id, std::size_t backend_index) {
    worker_metrics(worker_id).connection_errors.fetch_add(1, std::memory_order_relaxed);
    scheduler_->mark_failure(backend_index);
}

void RuntimeState::record_backend_failure(std::size_t backend_index) {
    metrics_.control_errors.fetch_add(1, std::memory_order_relaxed);
    scheduler_->mark_failure(backend_index);
}

void RuntimeState::record_connect_latency(std::size_t backend_index, std::uint64_t latency_us) {
    backend(backend_index).connect_latency_us.store(latency_us, std::memory_order_relaxed);
    connect_latency_.count.fetch_add(1, std::memory_order_relaxed);
    connect_latency_.sum_us.fetch_add(latency_us, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kConnectLatencyBucketUpperBoundsUs.size(); ++i) {
        if (latency_us <= kConnectLatencyBucketUpperBoundsUs[i]) {
            connect_latency_.buckets[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    connect_latency_.overflow.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeState::record_connect_timeout(std::size_t worker_id) {
    worker_metrics(worker_id).connect_timeouts.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeState::record_idle_timeout(std::size_t worker_id) {
    worker_metrics(worker_id).idle_timeouts.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeState::set_backend_state(std::size_t backend_index, BackendState state) {
    scheduler_->set_state(backend_index, state);
}

}  // namespace lb
