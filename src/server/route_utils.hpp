#pragma once

#include "server/api_types.hpp"
#include "server/metrics.hpp"

#include <functional>
#include <utility>
#include <vector>

#include <drogon/HttpResponse.h>

namespace zks::server {

/// Wraps a Drogon response callback to increment request/error metrics.
/// Takes a reference to ServerMetrics — the metrics object outlives all requests
/// (owned by ServerRuntime), so a reference is safe and avoids a shared_ptr copy per request.
inline std::function<void(const drogon::HttpResponsePtr&)>
with_metrics(ServerMetrics& metrics, std::function<void(const drogon::HttpResponsePtr&)> callback) {
    return [&metrics, callback = std::move(callback)](const drogon::HttpResponsePtr& resp) {
        metrics.increment_requests();
        if (static_cast<int>(resp->getStatusCode()) >= 400) {
            metrics.increment_errors();
        }
        callback(resp);
    };
}

void increment_runtime_error_metrics(ServerMetrics& metrics, const RuntimeError& error);

void increment_tool_metrics(ServerMetrics& metrics,
                            const std::vector<ToolInvocationRecord>& tool_invocations);

} // namespace zks::server
