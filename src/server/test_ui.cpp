#include "server/test_ui.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

// These symbols are generated at build time by xxd from test_ui.html and test_ui.js.
// NOLINTBEGIN(modernize-avoid-c-arrays)
extern "C" {
extern unsigned char kTestUiHtmlData[];
extern unsigned int kTestUiHtmlData_len;
extern unsigned char kTestUiJsData[];
extern unsigned int kTestUiJsData_len;
}
// NOLINTEND(modernize-avoid-c-arrays)

namespace zks::server {
namespace {

std::string escape_script_json(std::string json) {
    size_t pos = 0;
    while ((pos = json.find("</", pos)) != std::string::npos) {
        json.replace(pos, 2, "<\\/");
        pos += 3;
    }
    return json;
}

} // namespace

drogon::HttpResponsePtr make_test_ui_response(const HealthSnapshot& snapshot) {
    const std::string_view html_prefix(reinterpret_cast<const char*>(kTestUiHtmlData),
                                       kTestUiHtmlData_len);
    const std::string_view html_suffix(reinterpret_cast<const char*>(kTestUiJsData),
                                       kTestUiJsData_len);

    const auto boot_payload = escape_script_json(nlohmann::json{
        {"health",
         {{"ready", snapshot.ready},
          {"model", {{"id", snapshot.model_id}}},
          {"sessions",
           {{"enabled", snapshot.sessions.enabled},
            {"active", snapshot.sessions.active},
            {"max_sessions", snapshot.sessions.max_sessions},
            {"idle_ttl_seconds",
             snapshot.sessions.idle_ttl_seconds}}}}}}.dump());

    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k200OK);
    response->setContentTypeCodeAndCustomString(drogon::CT_TEXT_HTML, "text/html; charset=utf-8");
    response->addHeader("Cache-Control", "no-store");
    std::string body;
    body.reserve(html_prefix.size() + boot_payload.size() + html_suffix.size());
    body.append(html_prefix);
    body.append(boot_payload);
    body.append(html_suffix);
    response->setBody(std::move(body));
    return response;
}

void register_test_ui_routes(drogon::HttpAppFramework& app,
                             const std::shared_ptr<const ServerRuntime>& runtime) {
    std::weak_ptr<const ServerRuntime> weak_runtime = runtime;
    app.registerHandler(
        "/_test",
        [weak_runtime](const drogon::HttpRequestPtr&,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_test_ui_response({false, "", {}}));
                return;
            }

            callback(make_test_ui_response(runtime->health_snapshot()));
        },
        {drogon::Get});
}

} // namespace zks::server
