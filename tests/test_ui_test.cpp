#include "server/test_ui.hpp"

#include <array>
#include <iostream>
#include <string>

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

int main() {
    const auto response = zks::server::make_test_ui_response(
        {true, "demo-model", {true, 1u, 4u, 900u}});

    if (response->getStatusCode() != drogon::k200OK) {
        return fail("Expected 200 for test UI response.");
    }

    if (response->contentType() != drogon::CT_TEXT_HTML ||
        response->contentTypeString().find("text/html") != 0) {
        return fail("Expected text/html content type for test UI response.");
    }

    if (response->getHeader("Cache-Control") != "no-store") {
        return fail("Expected Cache-Control: no-store on test UI response.");
    }

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
        if (body.find(fragment) == std::string::npos) {
            return fail("Test UI response body is missing expected content.");
        }
    }

    if (body.find("demo-model") == std::string::npos) {
        return fail("Test UI response body is missing the bootstrapped model id.");
    }

    return 0;
}
