#include "doctest.h"

#include "server/auth.hpp"

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

TEST_CASE("no api_key configured — auth always passes") {
    auto config = make_config();

    auto req = drogon::HttpRequest::newHttpRequest();
    CHECK_FALSE(zks::server::check_auth(req, config).has_value());

    req->addHeader("Authorization", "Bearer anything");
    CHECK_FALSE(zks::server::check_auth(req, config).has_value());
}

TEST_CASE("correct Bearer token passes") {
    auto config = make_config("my-secret-key");

    auto req = drogon::HttpRequest::newHttpRequest();
    req->addHeader("Authorization", "Bearer my-secret-key");
    CHECK_FALSE(zks::server::check_auth(req, config).has_value());
}

TEST_CASE("wrong Bearer token returns 401") {
    auto config = make_config("my-secret-key");

    auto req = drogon::HttpRequest::newHttpRequest();
    req->addHeader("Authorization", "Bearer wrong-key");
    auto err = zks::server::check_auth(req, config);
    REQUIRE(err.has_value());
    CHECK(err->http_status == 401);
    CHECK(err->type == "auth_error");
    CHECK(err->code == std::optional<std::string>{"invalid_api_key"});
}

TEST_CASE("missing Authorization header returns 401") {
    auto config = make_config("my-secret-key");

    auto req = drogon::HttpRequest::newHttpRequest();
    auto err = zks::server::check_auth(req, config);
    REQUIRE(err.has_value());
    CHECK(err->http_status == 401);
}

TEST_CASE("wrong scheme (Basic) returns 401") {
    auto config = make_config("my-secret-key");

    auto req = drogon::HttpRequest::newHttpRequest();
    req->addHeader("Authorization", "Basic my-secret-key");
    auto err = zks::server::check_auth(req, config);
    REQUIRE(err.has_value());
    CHECK(err->http_status == 401);
}
