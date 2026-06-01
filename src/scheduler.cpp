#include "lb/scheduler.hpp"

#include <algorithm>
#include <random>
#include <stdexcept>
#include <utility>

namespace lb {

namespace {

std::vector<std::atomic<std::uint64_t>> make_u64_slots(std::size_t count) {
    return std::vector<std::atomic<std::uint64_t>>(count);
}

std::vector<std::atomic<std::uint32_t>> make_u32_slots(std::size_t count) {
    return std::vector<std::atomic<std::uint32_t>>(count);
}

std::vector<std::atomic<int>> make_state_slots(std::size_t count) {
    std::vector<std::atomic<int>> states(count);
    for (auto& state : states) {
        state.store(static_cast<int>(BackendState::Up), std::memory_order_relaxed);
    }
    return states;
}

}  // namespace

Scheduler::Scheduler(std::vector<Endpoint> backends, Policy policy)
    : backends_(std::move(backends)),
      policy_(policy),
      active_(make_u64_slots(backends_.size())),
      total_(make_u64_slots(backends_.size())),
      errors_(make_u64_slots(backends_.size())),
      consecutive_failures_(make_u32_slots(backends_.size())),
      states_(make_state_slots(backends_.size())) {}

std::string to_string(BackendState state) {
    switch (state) {
        case BackendState::Up:
            return "UP";
        case BackendState::Down:
            return "DOWN";
        case BackendState::Draining:
            return "DRAINING";
    }
    return "UNKNOWN";
}

std::optional<SelectedBackend> Scheduler::select() {
    if (backends_.empty()) {
        return std::nullopt;
    }

    if (policy_ == Policy::RoundRobin) {
        const auto start = next_.fetch_add(1, std::memory_order_relaxed);
        for (std::size_t offset = 0; offset < backends_.size(); ++offset) {
            const auto index = (start + offset) % backends_.size();
            if (routable(index)) {
                return SelectedBackend{index, backends_[index]};
            }
        }
        return std::nullopt;
    }

    if (policy_ == Policy::PowerOfTwoChoices && backends_.size() > 1) {
        thread_local std::mt19937_64 rng(std::random_device{}());
        std::uniform_int_distribution<std::size_t> distribution(0, backends_.size() - 1);
        for (std::size_t attempts = 0; attempts < backends_.size() * 2; ++attempts) {
            const auto first = distribution(rng);
            const auto second = distribution(rng);
            if (!routable(first) && !routable(second)) {
                continue;
            }
            if (routable(first) && !routable(second)) {
                return SelectedBackend{first, backends_[first]};
            }
            if (!routable(first) && routable(second)) {
                return SelectedBackend{second, backends_[second]};
            }

            const auto first_active = active_[first].load(std::memory_order_relaxed);
            const auto second_active = active_[second].load(std::memory_order_relaxed);
            const auto selected = first_active <= second_active ? first : second;
            return SelectedBackend{selected, backends_[selected]};
        }
    }

    std::optional<std::size_t> selected;
    for (std::size_t i = 0; i < active_.size(); ++i) {
        if (!routable(i)) {
            continue;
        }
        if (!selected.has_value() ||
            active_[i].load(std::memory_order_relaxed) < active_[*selected].load(std::memory_order_relaxed)) {
            selected = i;
        }
    }

    if (!selected.has_value()) {
        return std::nullopt;
    }
    return SelectedBackend{*selected, backends_[*selected]};
}

void Scheduler::mark_open(std::size_t index) {
    if (index >= active_.size()) {
        throw std::out_of_range("backend index out of range");
    }
    active_[index].fetch_add(1, std::memory_order_relaxed);
    total_[index].fetch_add(1, std::memory_order_relaxed);
}

void Scheduler::mark_closed(std::size_t index) {
    if (index >= active_.size()) {
        throw std::out_of_range("backend index out of range");
    }
    auto current = active_[index].load(std::memory_order_relaxed);
    while (current > 0 &&
           !active_[index].compare_exchange_weak(current, current - 1, std::memory_order_relaxed)) {
    }
}

void Scheduler::mark_failure(std::size_t index) {
    if (index >= errors_.size()) {
        throw std::out_of_range("backend index out of range");
    }
    errors_[index].fetch_add(1, std::memory_order_relaxed);
    consecutive_failures_[index].fetch_add(1, std::memory_order_relaxed);
}

void Scheduler::mark_healthy(std::size_t index) {
    if (index >= states_.size()) {
        throw std::out_of_range("backend index out of range");
    }
    consecutive_failures_[index].store(0, std::memory_order_relaxed);
    if (state(index) == BackendState::Down) {
        set_state(index, BackendState::Up);
    }
}

void Scheduler::set_state(std::size_t index, BackendState state) {
    if (index >= states_.size()) {
        throw std::out_of_range("backend index out of range");
    }
    states_[index].store(static_cast<int>(state), std::memory_order_release);
}

std::size_t Scheduler::backend_count() const {
    return backends_.size();
}

std::size_t Scheduler::active_connections(std::size_t index) const {
    if (index >= active_.size()) {
        throw std::out_of_range("backend index out of range");
    }
    return active_[index].load(std::memory_order_relaxed);
}

std::uint64_t Scheduler::total_connections(std::size_t index) const {
    if (index >= total_.size()) {
        throw std::out_of_range("backend index out of range");
    }
    return total_[index].load(std::memory_order_relaxed);
}

std::uint64_t Scheduler::error_count(std::size_t index) const {
    if (index >= errors_.size()) {
        throw std::out_of_range("backend index out of range");
    }
    return errors_[index].load(std::memory_order_relaxed);
}

std::uint32_t Scheduler::consecutive_failures(std::size_t index) const {
    if (index >= consecutive_failures_.size()) {
        throw std::out_of_range("backend index out of range");
    }
    return consecutive_failures_[index].load(std::memory_order_relaxed);
}

BackendState Scheduler::state(std::size_t index) const {
    if (index >= states_.size()) {
        throw std::out_of_range("backend index out of range");
    }
    return static_cast<BackendState>(states_[index].load(std::memory_order_acquire));
}

Endpoint Scheduler::endpoint(std::size_t index) const {
    if (index >= backends_.size()) {
        throw std::out_of_range("backend index out of range");
    }
    return backends_[index];
}

Policy Scheduler::policy() const {
    return policy_;
}

bool Scheduler::routable(std::size_t index) const {
    return state(index) == BackendState::Up;
}

}  // namespace lb
