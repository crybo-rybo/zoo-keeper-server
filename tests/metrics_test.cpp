#include "server/metrics.hpp"

#include <gtest/gtest.h>

TEST(MetricsTest, InitialStateAllZero) {
    zks::server::ServerMetrics metrics;
    EXPECT_EQ(metrics.requests_total(), 0);
    EXPECT_EQ(metrics.requests_errors(), 0);
    EXPECT_GE(metrics.uptime_seconds(), 0);
}

TEST(MetricsTest, NewCountersInitialState) {
    zks::server::ServerMetrics metrics;
    EXPECT_EQ(metrics.requests_cancelled_total(), 0);
    EXPECT_EQ(metrics.requests_queue_rejected_total(), 0);
    EXPECT_EQ(metrics.stream_disconnects_total(), 0);
    EXPECT_EQ(metrics.tool_invocations_total(), 0);
    EXPECT_EQ(metrics.tool_failures_total(), 0);
    EXPECT_EQ(metrics.tool_timeouts_total(), 0);
}

TEST(MetricsTest, CounterIncrements) {
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

    EXPECT_EQ(metrics.requests_total(), 3);
    EXPECT_EQ(metrics.requests_errors(), 1);
    EXPECT_EQ(metrics.requests_cancelled_total(), 2);
    EXPECT_EQ(metrics.requests_queue_rejected_total(), 1);
    EXPECT_EQ(metrics.stream_disconnects_total(), 3);
    EXPECT_EQ(metrics.tool_invocations_total(), 2);
    EXPECT_EQ(metrics.tool_failures_total(), 1);
    EXPECT_EQ(metrics.tool_timeouts_total(), 1);
}

TEST(MetricsTest, SnapshotFieldsPopulated) {
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

    EXPECT_EQ(snapshot.requests_total, 10);
    EXPECT_EQ(snapshot.requests_errors, 2);
    EXPECT_EQ(snapshot.requests_cancelled_total, 5);
    EXPECT_EQ(snapshot.requests_queue_rejected_total, 3);
    EXPECT_EQ(snapshot.stream_disconnects_total, 7);
    EXPECT_EQ(snapshot.tool_invocations_total, 8);
    EXPECT_EQ(snapshot.tool_failures_total, 4);
    EXPECT_EQ(snapshot.tool_timeouts_total, 2);
    EXPECT_EQ(snapshot.active_sessions, 3);
    EXPECT_EQ(snapshot.model_id, "test-model");
    EXPECT_EQ(snapshot.uptime_seconds, 42);
}
