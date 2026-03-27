#include "server/test_ui.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

// These symbols are generated at build time by xxd from the bundled test UI assets.
// NOLINTBEGIN(modernize-avoid-c-arrays)
extern "C" {
extern unsigned char kTestUiTemplateData[];
extern unsigned int kTestUiTemplateData_len;
extern unsigned char kTestUiCssData[];
extern unsigned int kTestUiCssData_len;
extern unsigned char kTestUiJsData[];
extern unsigned int kTestUiJsData_len;
}
// NOLINTEND(modernize-avoid-c-arrays)

namespace zks::server {
namespace {

constexpr std::string_view kBootPlaceholder = "__ZKS_TEST_UI_BOOT__";
constexpr std::string_view kCssPlaceholder = "__ZKS_TEST_UI_CSS__";
constexpr std::string_view kJsPlaceholder = "__ZKS_TEST_UI_JS__";

std::string escape_inline_script(std::string text) {
    size_t pos = 0;
    while ((pos = text.find("</", pos)) != std::string::npos) {
        text.replace(pos, 2, "<\\/");
        pos += 3;
    }
    return text;
}

void replace_placeholder(std::string& body, std::string_view placeholder,
                         std::string_view replacement) {
    const auto pos = body.find(placeholder);
    if (pos == std::string::npos) {
        return;
    }
    body.replace(pos, placeholder.size(), replacement);
}

} // namespace

drogon::HttpResponsePtr make_test_ui_response(const HealthSnapshot& snapshot) {
    const std::string_view html_template(reinterpret_cast<const char*>(kTestUiTemplateData),
                                         kTestUiTemplateData_len);
    const std::string_view css_bundle(reinterpret_cast<const char*>(kTestUiCssData),
                                      kTestUiCssData_len);
    const std::string_view js_bundle(reinterpret_cast<const char*>(kTestUiJsData),
                                     kTestUiJsData_len);

    const auto boot_payload = escape_inline_script(nlohmann::json{
        {"health",
         {{"ready", snapshot.ready},
          {"model", {{"id", snapshot.model_id}}},
          {"version", snapshot.version},
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
    std::string body(html_template);
    replace_placeholder(body, kBootPlaceholder, boot_payload);
    replace_placeholder(body, kCssPlaceholder, css_bundle);
    replace_placeholder(body, kJsPlaceholder, escape_inline_script(std::string(js_bundle)));
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
