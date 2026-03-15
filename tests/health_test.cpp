#include "server/health.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

TEST(HealthTest, ReadyReturns200) {
    const auto ready = zks::server::make_health_response(
        {true, "demo-model", {true, 2u, 4u, 900u}, "1.2.3"});
    EXPECT_EQ(ready->getStatusCode(), drogon::k200OK);

    const auto json = nlohmann::json::parse(std::string(ready->getBody()));
    EXPECT_EQ(json.at("status"), "ok");
    EXPECT_TRUE(json.at("ready").get<bool>());
    EXPECT_EQ(json.at("model").at("id"), "demo-model");
    EXPECT_TRUE(json.at("sessions").at("enabled").get<bool>());
    EXPECT_EQ(json.at("sessions").at("active"), 2);
    EXPECT_EQ(json.at("sessions").at("max_sessions"), 4);
    EXPECT_EQ(json.at("sessions").at("idle_ttl_seconds"), 900);
    EXPECT_EQ(json.at("version"), "1.2.3");
}

TEST(HealthTest, NotReadyReturns503) {
    const auto not_ready = zks::server::make_health_response(
        {false, "demo-model", {false, 0u, 0u, 900u}, "0.0.1"});
    EXPECT_EQ(not_ready->getStatusCode(), drogon::k503ServiceUnavailable);

    const auto json = nlohmann::json::parse(std::string(not_ready->getBody()));
    EXPECT_EQ(json.at("status"), "starting");
    EXPECT_FALSE(json.at("ready").get<bool>());
    EXPECT_EQ(json.at("model").at("id"), "demo-model");
    EXPECT_FALSE(json.at("sessions").at("enabled").get<bool>());
    EXPECT_EQ(json.at("sessions").at("active"), 0);
    EXPECT_EQ(json.at("version"), "0.0.1");
}
