#include "doctest.h"

#include "server/test_ui.hpp"

#include <array>
#include <string>

TEST_CASE("test UI response") {
    const auto response = zks::server::make_test_ui_response(
        {true, "demo-model", {true, 1u, 4u, 900u}});

    CHECK(response->getStatusCode() == drogon::k200OK);
    CHECK(response->contentType() == drogon::CT_TEXT_HTML);
    REQUIRE(response->contentTypeString().find("text/html") == 0);
    CHECK(response->getHeader("Cache-Control") == "no-store");

    const std::string body(response->getBody());
    const std::array<std::string_view, 8> expected_fragments = {
        "GET /_test",
        "/healthz",
        "/v1/models",
        "/v1/tools",
        "/v1/sessions",
        "/v1/chat/completions",
        "Send message",
        "Create session",
    };

    for (const auto fragment : expected_fragments) {
        CHECK(body.find(fragment) != std::string::npos);
    }
    CHECK(body.find("demo-model") != std::string::npos);
}
