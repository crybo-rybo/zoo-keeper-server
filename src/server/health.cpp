#include "server/health.hpp"

#include <json/value.h>

namespace zks::server {

drogon::HttpResponsePtr make_health_response(const HealthSnapshot& snapshot) {
    Json::Value body;
    body["status"] = snapshot.ready ? "ok" : "starting";
    body["ready"] = snapshot.ready;
    body["model"]["id"] = snapshot.model_id;

    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(snapshot.ready ? drogon::k200OK
                                           : drogon::k503ServiceUnavailable);
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
