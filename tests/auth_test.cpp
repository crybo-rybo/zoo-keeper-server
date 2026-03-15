#include "server/auth.hpp"

#include <gtest/gtest.h>

#include <drogon/HttpRequest.h>

namespace {

zks::server::ServerConfig make_config(std::optional<std::string> api_key = std::nullopt) {
    zks::server::ServerConfig config;
    config.model_id = "test-model";
    config.zoo_config.model_path = "/tmp/test.gguf";
    config.api_key = std::move(api_key);
    return config;
}

} // namespace

TEST(AuthTest, NoApiKeyConfiguredAlwaysPasses) {
    auto config = make_config();

    auto req = drogon::HttpRequest::newHttpRequest();
    EXPECT_FALSE(zks::server::check_auth(req, config).has_value());

    req->addHeader("Authorization", "Bearer anything");
    EXPECT_FALSE(zks::server::check_auth(req, config).has_value());
}

TEST(AuthTest, CorrectBearerTokenPasses) {
    auto config = make_config("my-secret-key");

    auto req = drogon::HttpRequest::newHttpRequest();
    req->addHeader("Authorization", "Bearer my-secret-key");
    EXPECT_FALSE(zks::server::check_auth(req, config).has_value());
}

TEST(AuthTest, WrongBearerTokenReturns401) {
    auto config = make_config("my-secret-key");

    auto req = drogon::HttpRequest::newHttpRequest();
    req->addHeader("Authorization", "Bearer wrong-key");
    auto err = zks::server::check_auth(req, config);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->http_status, 401);
    EXPECT_EQ(err->type, "auth_error");
    EXPECT_EQ(err->code, std::optional<std::string>{"invalid_api_key"});
}

TEST(AuthTest, MissingAuthorizationHeaderReturns401) {
    auto config = make_config("my-secret-key");

    auto req = drogon::HttpRequest::newHttpRequest();
    auto err = zks::server::check_auth(req, config);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->http_status, 401);
}

TEST(AuthTest, WrongSchemeReturns401) {
    auto config = make_config("my-secret-key");

    auto req = drogon::HttpRequest::newHttpRequest();
    req->addHeader("Authorization", "Basic my-secret-key");
    auto err = zks::server::check_auth(req, config);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->http_status, 401);
}
