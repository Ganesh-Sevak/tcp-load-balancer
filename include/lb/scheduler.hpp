#pragma once

#include "lb/config.hpp"

#include <atomic>
#include <cstddef>
#include <optional>
#include <vector>

namespace lb {

struct SelectedBackend {
    std::size_t index{};
    Endpoint endpoint;
};

enum class BackendState {
    Up = 0,
    Down = 1,
    Draining = 2,
};

std::string to_string(BackendState state);

class Scheduler {
public:
    Scheduler(std::vector<Endpoint> backends, Policy policy);

    [[nodiscard]] std::optional<SelectedBackend> select();
    void mark_open(std::size_t index);
    void mark_closed(std::size_t index);
    void mark_failure(std::size_t index);
    void mark_healthy(std::size_t index);
    void set_state(std::size_t index, BackendState state);

    [[nodiscard]] std::size_t backend_count() const;
    [[nodiscard]] std::size_t active_connections(std::size_t index) const;
    [[nodiscard]] std::uint64_t total_connections(std::size_t index) const;
    [[nodiscard]] std::uint64_t error_count(std::size_t index) const;
    [[nodiscard]] std::uint32_t consecutive_failures(std::size_t index) const;
    [[nodiscard]] BackendState state(std::size_t index) const;
    [[nodiscard]] Endpoint endpoint(std::size_t index) const;
    [[nodiscard]] Policy policy() const;

private:
    [[nodiscard]] bool routable(std::size_t index) const;

    std::vector<Endpoint> backends_;
    Policy policy_;
    std::atomic<std::size_t> next_{0};
    std::vector<std::atomic<std::uint64_t>> active_;
    std::vector<std::atomic<std::uint64_t>> total_;
    std::vector<std::atomic<std::uint64_t>> errors_;
    std::vector<std::atomic<std::uint32_t>> consecutive_failures_;
    std::vector<std::atomic<int>> states_;
};

}  // namespace lb
