#include "server/metrics.hpp"

#include <nlohmann/json.hpp>

namespace zks::server {

nlohmann::json MetricsSnapshot::to_json() const {
    return nlohmann::json{
        {"requests_total", requests_total},
        {"requests_errors", requests_errors},
        {"requests_cancelled_total", requests_cancelled_total},
        {"requests_queue_rejected_total", requests_queue_rejected_total},
        {"stream_disconnects_total", stream_disconnects_total},
        {"tool_invocations_total", tool_invocations_total},
        {"tool_failures_total", tool_failures_total},
        {"tool_timeouts_total", tool_timeouts_total},
        {"active_sessions", active_sessions},
        {"model_id", model_id},
        {"uptime_seconds", uptime_seconds},
    };
}

} // namespace zks::server
