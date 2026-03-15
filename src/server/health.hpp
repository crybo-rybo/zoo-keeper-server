#pragma once

#include "server/runtime.hpp"

#include <memory>

#include <drogon/drogon.h>

namespace zks::server {

[[nodiscard]] drogon::HttpResponsePtr make_health_response(const HealthSnapshot& snapshot);

/// Registers health routes. Takes const ServerRuntime because health checks only
/// read configuration and readiness state — no mutable access is needed.
void register_health_routes(drogon::HttpAppFramework& app,
                            const std::shared_ptr<const ServerRuntime>& runtime);

} // namespace zks::server
