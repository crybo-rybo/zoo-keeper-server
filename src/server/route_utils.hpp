#pragma once

#include "server/runtime.hpp"

#include <functional>
#include <memory>
#include <utility>

#include <drogon/HttpResponse.h>

namespace zks::server {

inline std::function<void(const drogon::HttpResponsePtr&)>
with_metrics(std::shared_ptr<ServerRuntime> runtime,
             std::function<void(const drogon::HttpResponsePtr&)> callback) {
    return [runtime = std::move(runtime), callback = std::move(callback)](
               const drogon::HttpResponsePtr& resp) {
        runtime->metrics().increment_requests();
        if (static_cast<int>(resp->getStatusCode()) >= 400) {
            runtime->metrics().increment_errors();
        }
        callback(resp);
    };
}

} // namespace zks::server
