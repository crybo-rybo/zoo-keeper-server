#include "server/metrics.hpp"

#include <iostream>
#include <thread>

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

int main() {
    // Initial state — all counters at zero
    {
        zks::server::ServerMetrics metrics;
        if (metrics.requests_total() != 0) {
            return fail("Initial requests_total should be 0.");
        }
        if (metrics.requests_errors() != 0) {
            return fail("Initial requests_errors should be 0.");
        }
        if (metrics.uptime_seconds() < 0) {
            return fail("uptime_seconds must be non-negative.");
        }
    }

    // Counter increments
    {
        zks::server::ServerMetrics metrics;
        metrics.increment_requests();
        metrics.increment_requests();
        metrics.increment_requests();
        metrics.increment_errors();

        if (metrics.requests_total() != 3) {
            return fail("requests_total should be 3 after 3 increments.");
        }
        if (metrics.requests_errors() != 1) {
            return fail("requests_errors should be 1 after 1 increment.");
        }
    }

    // MetricsSnapshot fields are populated
    {
        zks::server::MetricsSnapshot snapshot;
        snapshot.requests_total = 10;
        snapshot.requests_errors = 2;
        snapshot.active_sessions = 3;
        snapshot.model_id = "test-model";
        snapshot.uptime_seconds = 42;

        if (snapshot.requests_total != 10 || snapshot.requests_errors != 2 ||
            snapshot.active_sessions != 3 || snapshot.model_id != "test-model" ||
            snapshot.uptime_seconds != 42) {
            return fail("MetricsSnapshot fields do not match expected values.");
        }
    }

    return 0;
}
