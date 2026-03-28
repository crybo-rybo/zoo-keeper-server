#include "server/route_utils.hpp"

namespace zks::server {

void increment_runtime_error_metrics(ServerMetrics& metrics, const RuntimeError& error) {
    if (error.code == RuntimeErrorCode::RequestCancelled) {
        metrics.increment_cancelled();
    } else if (error.code == RuntimeErrorCode::QueueFull) {
        metrics.increment_queue_rejected();
    }
}

void increment_tool_metrics(ServerMetrics& metrics,
                            const std::vector<ToolInvocationRecord>& tool_invocations) {
    for (const auto& invocation : tool_invocations) {
        metrics.increment_tool_invocations();
        if (invocation.status == ToolInvocationStatus::Succeeded) {
            continue;
        }

        metrics.increment_tool_failures();
        if (invocation.error.has_value() &&
            invocation.error->context == std::optional<std::string>{"timeout"}) {
            metrics.increment_tool_timeouts();
        }
    }
}

} // namespace zks::server
