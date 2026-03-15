#include "doctest.h"

#include "server/metrics.hpp"

TEST_CASE("initial state — all counters at zero") {
    zks::server::ServerMetrics metrics;
    CHECK(metrics.requests_total() == 0);
    CHECK(metrics.requests_errors() == 0);
    CHECK(metrics.uptime_seconds() >= 0);
}

TEST_CASE("new counters initial state") {
    zks::server::ServerMetrics metrics;
    CHECK(metrics.requests_cancelled_total() == 0);
    CHECK(metrics.requests_queue_rejected_total() == 0);
    CHECK(metrics.stream_disconnects_total() == 0);
    CHECK(metrics.tool_invocations_total() == 0);
    CHECK(metrics.tool_failures_total() == 0);
    CHECK(metrics.tool_timeouts_total() == 0);
}

TEST_CASE("counter increments") {
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
    metrics.increment_tool_invocations();
    metrics.increment_tool_invocations();
    metrics.increment_tool_failures();
    metrics.increment_tool_timeouts();

    CHECK(metrics.requests_total() == 3);
    CHECK(metrics.requests_errors() == 1);
    CHECK(metrics.requests_cancelled_total() == 2);
    CHECK(metrics.requests_queue_rejected_total() == 1);
    CHECK(metrics.stream_disconnects_total() == 3);
    CHECK(metrics.tool_invocations_total() == 2);
    CHECK(metrics.tool_failures_total() == 1);
    CHECK(metrics.tool_timeouts_total() == 1);
}

TEST_CASE("MetricsSnapshot fields are populated") {
    zks::server::MetricsSnapshot snapshot;
    snapshot.requests_total = 10;
    snapshot.requests_errors = 2;
    snapshot.requests_cancelled_total = 5;
    snapshot.requests_queue_rejected_total = 3;
    snapshot.stream_disconnects_total = 7;
    snapshot.tool_invocations_total = 8;
    snapshot.tool_failures_total = 4;
    snapshot.tool_timeouts_total = 2;
    snapshot.active_sessions = 3;
    snapshot.model_id = "test-model";
    snapshot.uptime_seconds = 42;

    CHECK(snapshot.requests_total == 10);
    CHECK(snapshot.requests_errors == 2);
    CHECK(snapshot.requests_cancelled_total == 5);
    CHECK(snapshot.requests_queue_rejected_total == 3);
    CHECK(snapshot.stream_disconnects_total == 7);
    CHECK(snapshot.tool_invocations_total == 8);
    CHECK(snapshot.tool_failures_total == 4);
    CHECK(snapshot.tool_timeouts_total == 2);
    CHECK(snapshot.active_sessions == 3);
    CHECK(snapshot.model_id == "test-model");
    CHECK(snapshot.uptime_seconds == 42);
}
