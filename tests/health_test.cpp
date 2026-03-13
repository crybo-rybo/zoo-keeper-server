#include "server/health.hpp"

#include <iostream>

#include <nlohmann/json.hpp>

int main() {
    const auto ready = zks::server::make_health_response(
        {true, "demo-model", {true, 2u, 4u, 900u}});
    if (ready->getStatusCode() != drogon::k200OK) {
        std::cerr << "Expected 200 for ready health response." << '\n';
        return 1;
    }

    const auto ready_json = nlohmann::json::parse(std::string(ready->getBody()));
    if (ready_json.at("status") != "ok" || !ready_json.at("ready").get<bool>() ||
        ready_json.at("model").at("id") != "demo-model" ||
        !ready_json.at("sessions").at("enabled").get<bool>() ||
        ready_json.at("sessions").at("active") != 2 ||
        ready_json.at("sessions").at("max_sessions") != 4 ||
        ready_json.at("sessions").at("idle_ttl_seconds") != 900) {
        std::cerr << "Ready health response body mismatch." << '\n';
        return 1;
    }

    const auto not_ready = zks::server::make_health_response(
        {false, "demo-model", {false, 0u, 0u, 900u}});
    if (not_ready->getStatusCode() != drogon::k503ServiceUnavailable) {
        std::cerr << "Expected 503 for non-ready health response." << '\n';
        return 1;
    }

    const auto not_ready_json = nlohmann::json::parse(std::string(not_ready->getBody()));
    if (not_ready_json.at("status") != "starting" || not_ready_json.at("ready").get<bool>() ||
        not_ready_json.at("model").at("id") != "demo-model" ||
        not_ready_json.at("sessions").at("enabled").get<bool>() ||
        not_ready_json.at("sessions").at("active") != 0) {
        std::cerr << "Non-ready health response body mismatch." << '\n';
        return 1;
    }

    return 0;
}
