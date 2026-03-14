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

    // New counters initial state
    {
        zks::server::ServerMetrics metrics;
        if (metrics.requests_cancelled_total() != 0) {
            return fail("Initial requests_cancelled_total should be 0.");
        }
        if (metrics.requests_queue_rejected_total() != 0) {
            return fail("Initial requests_queue_rejected_total should be 0.");
        }
        if (metrics.stream_disconnects_total() != 0) {
            return fail("Initial stream_disconnects_total should be 0.");
        }
    }

    // Counter increments
    {
        zks::server::ServerMetrics metrics;
        metrics.increment_requests();
        metrics.increment_requests();
        metrics.increment_requests();
        metrics.increment_errors();
        metrics.increment_cancelled();
        metrics.increment_cancelled();
        metrics.increment_queue_rejected();
        metrics.increment_stream_disconnects();
        metrics.increment_stream_disconnects();
        metrics.increment_stream_disconnects();

        if (metrics.requests_total() != 3) {
            return fail("requests_total should be 3 after 3 increments.");
        }
        if (metrics.requests_errors() != 1) {
            return fail("requests_errors should be 1 after 1 increment.");
        }
        if (metrics.requests_cancelled_total() != 2) {
            return fail("requests_cancelled_total should be 2 after 2 increments.");
        }
        if (metrics.requests_queue_rejected_total() != 1) {
            return fail("requests_queue_rejected_total should be 1 after 1 increment.");
        }
        if (metrics.stream_disconnects_total() != 3) {
            return fail("stream_disconnects_total should be 3 after 3 increments.");
        }
    }

    // MetricsSnapshot fields are populated
    {
        zks::server::MetricsSnapshot snapshot;
        snapshot.requests_total = 10;
        snapshot.requests_errors = 2;
        snapshot.requests_cancelled_total = 5;
        snapshot.requests_queue_rejected_total = 3;
        snapshot.stream_disconnects_total = 7;
        snapshot.active_sessions = 3;
        snapshot.model_id = "test-model";
        snapshot.uptime_seconds = 42;

        if (snapshot.requests_total != 10 || snapshot.requests_errors != 2 ||
            snapshot.requests_cancelled_total != 5 ||
            snapshot.requests_queue_rejected_total != 3 ||
            snapshot.stream_disconnects_total != 7 || snapshot.active_sessions != 3 ||
            snapshot.model_id != "test-model" || snapshot.uptime_seconds != 42) {
            return fail("MetricsSnapshot fields do not match expected values.");
        }
    }

    return 0;
}
