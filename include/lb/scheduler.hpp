#pragma once

#include "lb/config.hpp"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

namespace lb {

struct SelectedBackend {
    std::size_t index{};
    Endpoint endpoint;
};

class Scheduler {
public:
    Scheduler(std::vector<Endpoint> backends, Policy policy);

    [[nodiscard]] std::optional<SelectedBackend> select();
    void mark_open(std::size_t index);
    void mark_closed(std::size_t index);

    [[nodiscard]] std::size_t backend_count() const;
    [[nodiscard]] std::size_t active_connections(std::size_t index) const;
    [[nodiscard]] Policy policy() const;

private:
    std::vector<Endpoint> backends_;
    Policy policy_;
    std::atomic<std::size_t> next_{0};

    mutable std::mutex active_mutex_;
    std::vector<std::size_t> active_;
};

}  // namespace lb

