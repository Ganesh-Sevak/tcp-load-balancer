#include "lb/scheduler.hpp"

#include <algorithm>
#include <stdexcept>

namespace lb {

Scheduler::Scheduler(std::vector<Endpoint> backends, Policy policy)
    : backends_(std::move(backends)), policy_(policy), active_(backends_.size(), 0) {}

std::optional<SelectedBackend> Scheduler::select() {
    if (backends_.empty()) {
        return std::nullopt;
    }

    if (policy_ == Policy::RoundRobin) {
        const auto index = next_.fetch_add(1, std::memory_order_relaxed) % backends_.size();
        return SelectedBackend{index, backends_[index]};
    }

    std::lock_guard<std::mutex> lock(active_mutex_);
    std::size_t selected = 0;
    for (std::size_t i = 1; i < active_.size(); ++i) {
        if (active_[i] < active_[selected]) {
            selected = i;
        }
    }
    return SelectedBackend{selected, backends_[selected]};
}

void Scheduler::mark_open(std::size_t index) {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (index >= active_.size()) {
        throw std::out_of_range("backend index out of range");
    }
    ++active_[index];
}

void Scheduler::mark_closed(std::size_t index) {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (index >= active_.size()) {
        throw std::out_of_range("backend index out of range");
    }
    if (active_[index] > 0) {
        --active_[index];
    }
}

std::size_t Scheduler::backend_count() const {
    return backends_.size();
}

std::size_t Scheduler::active_connections(std::size_t index) const {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (index >= active_.size()) {
        throw std::out_of_range("backend index out of range");
    }
    return active_[index];
}

Policy Scheduler::policy() const {
    return policy_;
}

}  // namespace lb

