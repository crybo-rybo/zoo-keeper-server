#pragma once

#include "server/runtime.hpp"

#include <drogon/drogon.h>

#include <memory>

namespace zks::server {

[[nodiscard]] drogon::HttpResponsePtr make_test_ui_response(const HealthSnapshot& snapshot);

void register_test_ui_routes(drogon::HttpAppFramework& app,
                             const std::shared_ptr<const ServerRuntime>& runtime);

} // namespace zks::server
