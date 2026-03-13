#include "server/health.hpp"

#include <nlohmann/json.hpp>

namespace zks::server {

drogon::HttpResponsePtr make_health_response(const HealthSnapshot& snapshot) {
    const auto body = nlohmann::json{
        {"status", snapshot.ready ? "ok" : "starting"},
        {"ready", snapshot.ready},
        {"model", {{"id", snapshot.model_id}}},
        {"sessions",
         {{"enabled", snapshot.sessions.enabled},
          {"active", snapshot.sessions.active},
          {"max_sessions", snapshot.sessions.max_sessions},
          {"idle_ttl_seconds", snapshot.sessions.idle_ttl_seconds}}},
    };

    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(snapshot.ready ? drogon::k200OK
                                           : drogon::k503ServiceUnavailable);
    response->setContentTypeCodeAndCustomString(drogon::CT_APPLICATION_JSON, "application/json");
    response->setBody(body.dump());
    return response;
}

void register_health_routes(const std::shared_ptr<const ServerRuntime>& runtime) {
    drogon::app().registerHandler(
        "/healthz",
        [runtime](const drogon::HttpRequestPtr&,
                  std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(make_health_response(runtime->health_snapshot()));
        },
        {drogon::Get});
}

} // namespace zks::server
