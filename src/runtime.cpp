#include "lb/runtime.hpp"

#include <algorithm>
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

}  // namespace

RuntimeState::RuntimeState(AppConfig config, std::shared_ptr<Scheduler> scheduler)
    : config_(std::move(config)), scheduler_(std::move(scheduler)) {
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
    const auto active = metrics_.active_connections.load(std::memory_order_relaxed);
    const auto total = metrics_.total_connections.load(std::memory_order_relaxed);
    const auto bytes_in = metrics_.bytes_from_clients.load(std::memory_order_relaxed);
    const auto bytes_out = metrics_.bytes_from_backends.load(std::memory_order_relaxed);

    std::ostringstream out;
    out << "{";
    out << "\"uptime_seconds\":" << uptime << ",";
    out << "\"listener\":\"" << json_escape(endpoint_label(config_.listen)) << "\",";
    out << "\"policy\":\"" << to_string(config_.policy) << "\",";
    out << "\"worker_count\":" << config_.worker_count << ",";
    out << "\"active_connections\":" << active << ",";
    out << "\"peak_connections\":" << metrics_.peak_connections.load(std::memory_order_relaxed) << ",";
    out << "\"total_connections\":" << total << ",";
    out << "\"connections_per_sec\":" << (uptime > 0 ? static_cast<double>(total) / static_cast<double>(uptime) : 0.0)
        << ",";
    out << "\"bytes_in\":" << bytes_in << ",";
    out << "\"bytes_out\":" << bytes_out << ",";
    out << "\"errors\":" << metrics_.connection_errors.load(std::memory_order_relaxed) << ",";
    out << "\"connect_timeouts\":" << metrics_.connect_timeouts.load(std::memory_order_relaxed) << ",";
    out << "\"idle_timeouts\":" << metrics_.idle_timeouts.load(std::memory_order_relaxed) << ",";
    out << "\"latency\":{\"p50\":0,\"p99\":0,\"p999\":0},";
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
    std::ostringstream out;
    out << "# TYPE lb_active_connections gauge\n";
    out << "lb_active_connections " << metrics_.active_connections.load(std::memory_order_relaxed) << "\n";
    out << "# TYPE lb_total_connections counter\n";
    out << "lb_total_connections " << metrics_.total_connections.load(std::memory_order_relaxed) << "\n";
    out << "# TYPE lb_bytes_from_clients counter\n";
    out << "lb_bytes_from_clients " << metrics_.bytes_from_clients.load(std::memory_order_relaxed) << "\n";
    out << "# TYPE lb_bytes_from_backends counter\n";
    out << "lb_bytes_from_backends " << metrics_.bytes_from_backends.load(std::memory_order_relaxed) << "\n";
    out << "# TYPE lb_connection_errors counter\n";
    out << "lb_connection_errors " << metrics_.connection_errors.load(std::memory_order_relaxed) << "\n";
    for (std::size_t i = 0; i < backends_.size(); ++i) {
        out << "lb_backend_active_connections{backend=\"" << endpoint_label(backends_[i].endpoint) << "\"} "
            << scheduler_->active_connections(i) << "\n";
        out << "lb_backend_up{backend=\"" << endpoint_label(backends_[i].endpoint) << "\"} "
            << (scheduler_->state(i) == BackendState::Up ? 1 : 0) << "\n";
    }
    return out.str();
}

void RuntimeState::record_connection_open(std::size_t backend_index) {
    scheduler_->mark_open(backend_index);
    const auto active = metrics_.active_connections.fetch_add(1, std::memory_order_relaxed) + 1;
    metrics_.total_connections.fetch_add(1, std::memory_order_relaxed);
    update_peak(metrics_.peak_connections, active);
}

void RuntimeState::record_connection_close(std::size_t backend_index) {
    scheduler_->mark_closed(backend_index);
    auto current = metrics_.active_connections.load(std::memory_order_relaxed);
    while (current > 0 &&
           !metrics_.active_connections.compare_exchange_weak(current, current - 1, std::memory_order_relaxed)) {
    }
}

void RuntimeState::record_bytes_from_client(std::size_t backend_index, std::uint64_t count) {
    metrics_.bytes_from_clients.fetch_add(count, std::memory_order_relaxed);
    backend(backend_index).bytes_from_client.fetch_add(count, std::memory_order_relaxed);
}

void RuntimeState::record_bytes_from_backend(std::size_t backend_index, std::uint64_t count) {
    metrics_.bytes_from_backends.fetch_add(count, std::memory_order_relaxed);
    backend(backend_index).bytes_from_backend.fetch_add(count, std::memory_order_relaxed);
}

void RuntimeState::record_backend_failure(std::size_t backend_index) {
    metrics_.connection_errors.fetch_add(1, std::memory_order_relaxed);
    scheduler_->mark_failure(backend_index);
}

void RuntimeState::record_connect_latency(std::size_t backend_index, std::uint64_t latency_us) {
    backend(backend_index).connect_latency_us.store(latency_us, std::memory_order_relaxed);
}

void RuntimeState::set_backend_state(std::size_t backend_index, BackendState state) {
    scheduler_->set_state(backend_index, state);
}

}  // namespace lb
