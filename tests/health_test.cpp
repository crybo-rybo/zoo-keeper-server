#include "doctest.h"

#include "server/health.hpp"

#include <nlohmann/json.hpp>

TEST_CASE("ready health response returns 200") {
    const auto ready = zks::server::make_health_response(
        {true, "demo-model", {true, 2u, 4u, 900u}});
    CHECK(ready->getStatusCode() == drogon::k200OK);

    const auto json = nlohmann::json::parse(std::string(ready->getBody()));
    CHECK(json.at("status") == "ok");
    CHECK(json.at("ready").get<bool>());
    CHECK(json.at("model").at("id") == "demo-model");
    CHECK(json.at("sessions").at("enabled").get<bool>());
    CHECK(json.at("sessions").at("active") == 2);
    CHECK(json.at("sessions").at("max_sessions") == 4);
    CHECK(json.at("sessions").at("idle_ttl_seconds") == 900);
}

TEST_CASE("not-ready health response returns 503") {
    const auto not_ready = zks::server::make_health_response(
        {false, "demo-model", {false, 0u, 0u, 900u}});
    CHECK(not_ready->getStatusCode() == drogon::k503ServiceUnavailable);

    const auto json = nlohmann::json::parse(std::string(not_ready->getBody()));
    CHECK(json.at("status") == "starting");
    CHECK_FALSE(json.at("ready").get<bool>());
    CHECK(json.at("model").at("id") == "demo-model");
    CHECK_FALSE(json.at("sessions").at("enabled").get<bool>());
    CHECK(json.at("sessions").at("active") == 0);
}
