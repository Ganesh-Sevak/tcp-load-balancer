#include "lb/scheduler.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "test failed: " << message << "\n";
        std::exit(1);
    }
}

std::vector<lb::Endpoint> backends() {
    return {
        {"127.0.0.1", 9101},
        {"127.0.0.1", 9102},
        {"127.0.0.1", 9103},
    };
}

void round_robin_rotates_across_backends() {
    lb::Scheduler scheduler(backends(), lb::Policy::RoundRobin);

    require(scheduler.select()->index == 0, "round robin first backend");
    require(scheduler.select()->index == 1, "round robin second backend");
    require(scheduler.select()->index == 2, "round robin third backend");
    require(scheduler.select()->index == 0, "round robin wraps");
}

void least_connections_picks_lowest_active_count() {
    lb::Scheduler scheduler(backends(), lb::Policy::LeastConnections);

    auto first = scheduler.select();
    require(first.has_value(), "least connections returns backend");
    require(first->index == 0, "least connections starts at first backend");

    scheduler.mark_open(0);
    require(scheduler.select()->index == 1, "least connections chooses backend 1");

    scheduler.mark_open(1);
    require(scheduler.select()->index == 2, "least connections chooses backend 2");

    scheduler.mark_closed(0);
    require(scheduler.select()->index == 0, "least connections returns to lowered backend");
}

void scheduler_skips_unroutable_backends() {
    lb::Scheduler scheduler(backends(), lb::Policy::RoundRobin);

    scheduler.set_state(0, lb::BackendState::Down);
    require(scheduler.select()->index == 1, "round robin skips down backend");

    scheduler.set_state(1, lb::BackendState::Draining);
    require(scheduler.select()->index == 2, "round robin skips draining backend");

    scheduler.set_state(2, lb::BackendState::Down);
    require(!scheduler.select().has_value(), "scheduler returns null when all backends are unroutable");
}

void power_of_two_choices_routes_to_available_backend() {
    lb::Scheduler scheduler(backends(), lb::Policy::PowerOfTwoChoices);
    scheduler.set_state(0, lb::BackendState::Down);
    scheduler.set_state(1, lb::BackendState::Down);

    for (int i = 0; i < 8; ++i) {
        const auto selected = scheduler.select();
        require(selected.has_value(), "p2c returns available backend");
        require(selected->index == 2, "p2c skips down backends");
    }
}

void closed_connection_never_underflows() {
    lb::Scheduler scheduler(backends(), lb::Policy::LeastConnections);
    scheduler.mark_closed(0);
    require(scheduler.active_connections(0) == 0, "closed connection does not underflow");
}

}  // namespace

int main() {
    round_robin_rotates_across_backends();
    least_connections_picks_lowest_active_count();
    scheduler_skips_unroutable_backends();
    power_of_two_choices_routes_to_available_backend();
    closed_connection_never_underflows();

    std::cout << "scheduler tests passed\n";
    return 0;
}
