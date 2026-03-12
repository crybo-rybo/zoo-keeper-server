#pragma once

#include "server/runtime.hpp"

#include <memory>

#include <drogon/drogon.h>

namespace zks::server {

[[nodiscard]] drogon::HttpResponsePtr make_health_response(const HealthSnapshot& snapshot);

void register_health_routes(const std::shared_ptr<const ServerRuntime>& runtime);

} // namespace zks::server
