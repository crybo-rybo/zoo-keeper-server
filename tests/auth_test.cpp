#include "server/auth.hpp"

#include <iostream>

#include <drogon/HttpRequest.h>

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

int main() {
    // No api_key in config — auth always passes regardless of header
    {
        zks::server::ServerConfig config;
        config.model_id = "test-model";
        config.zoo_config.model_path = "/tmp/test.gguf";

        auto req = drogon::HttpRequest::newHttpRequest();
        auto err = zks::server::check_auth(req, config);
        if (err.has_value()) {
            return fail("check_auth unexpectedly failed with no api_key configured.");
        }

        req->addHeader("Authorization", "Bearer anything");
        err = zks::server::check_auth(req, config);
        if (err.has_value()) {
            return fail("check_auth unexpectedly failed when api_key not configured (with header).");
        }
    }

    // api_key set — correct Bearer token passes
    {
        zks::server::ServerConfig config;
        config.model_id = "test-model";
        config.zoo_config.model_path = "/tmp/test.gguf";
        config.api_key = "my-secret-key";

        auto req = drogon::HttpRequest::newHttpRequest();
        req->addHeader("Authorization", "Bearer my-secret-key");
        auto err = zks::server::check_auth(req, config);
        if (err.has_value()) {
            return fail("check_auth failed with correct Bearer token.");
        }
    }

    // api_key set — wrong Bearer token returns 401
    {
        zks::server::ServerConfig config;
        config.model_id = "test-model";
        config.zoo_config.model_path = "/tmp/test.gguf";
        config.api_key = "my-secret-key";

        auto req = drogon::HttpRequest::newHttpRequest();
        req->addHeader("Authorization", "Bearer wrong-key");
        auto err = zks::server::check_auth(req, config);
        if (!err.has_value()) {
            return fail("check_auth unexpectedly passed with wrong Bearer token.");
        }
        if (err->http_status != 401 || err->type != "auth_error" ||
            err->code != std::optional<std::string>{"invalid_api_key"}) {
            return fail("check_auth returned wrong error for invalid token.");
        }
    }

    // api_key set — missing Authorization header returns 401
    {
        zks::server::ServerConfig config;
        config.model_id = "test-model";
        config.zoo_config.model_path = "/tmp/test.gguf";
        config.api_key = "my-secret-key";

        auto req = drogon::HttpRequest::newHttpRequest();
        auto err = zks::server::check_auth(req, config);
        if (!err.has_value()) {
            return fail("check_auth unexpectedly passed with missing Authorization header.");
        }
        if (err->http_status != 401) {
            return fail("check_auth returned wrong status for missing header.");
        }
    }

    // api_key set — wrong scheme (Basic) returns 401
    {
        zks::server::ServerConfig config;
        config.model_id = "test-model";
        config.zoo_config.model_path = "/tmp/test.gguf";
        config.api_key = "my-secret-key";

        auto req = drogon::HttpRequest::newHttpRequest();
        req->addHeader("Authorization", "Basic my-secret-key");
        auto err = zks::server::check_auth(req, config);
        if (!err.has_value()) {
            return fail("check_auth unexpectedly passed with Basic scheme.");
        }
        if (err->http_status != 401) {
            return fail("check_auth returned wrong status for Basic scheme.");
        }
    }

    return 0;
}
