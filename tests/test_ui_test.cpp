#include "server/test_ui.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>

TEST(TestUiTest, ResponseContent) {
    const auto response =
        zks::server::make_test_ui_response({true, "demo-model", {true, 1u, 4u, 900u}});

    EXPECT_EQ(response->getStatusCode(), drogon::k200OK);
    EXPECT_EQ(response->contentType(), drogon::CT_TEXT_HTML);
    ASSERT_EQ(response->contentTypeString().find("text/html"), 0u);
    EXPECT_EQ(response->getHeader("Cache-Control"), "no-store");

    const std::string body(response->getBody());
    const std::array<std::string_view, 13> expected_fragments = {
        "Capability Atlas", "/healthz",     "/v1/models",
        "/v1/tools",        "/v1/sessions", "/v1/chat/completions",
        "/metrics",         "Overview",     "Inference",
        "Memory",           "Tools",        "Operations",
        "Enter live demos",
    };

    for (const auto fragment : expected_fragments) {
        EXPECT_NE(body.find(fragment), std::string::npos) << "Missing: " << fragment;
    }
    EXPECT_NE(body.find("demo-model"), std::string::npos);
}
