#include "doctest.h"

#include "server/api_routes.hpp"
#include "server/config.hpp"
#include "server/health.hpp"
#include "server/runtime.hpp"

#include "fake_chat_service.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

namespace {

/// Fetches /metrics and returns the value of a specific uint64 counter field.
std::optional<int64_t> read_metric(const drogon::HttpClientPtr& client, const char* field) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setPath("/metrics");
    req->setMethod(drogon::Get);
    req->addHeader("Authorization", "Bearer test-secret");
    auto [status, resp] = client->sendRequest(req, 5.0);
    if (status != drogon::ReqResult::Ok || !resp ||
        resp->getStatusCode() != drogon::k200OK) {
        return std::nullopt;
    }
    auto json = nlohmann::json::parse(std::string(resp->body()), nullptr, false);
    if (json.is_discarded() || !json.contains(field)) {
        return std::nullopt;
    }
    return json.at(field).get<int64_t>();
}

} // namespace

TEST_CASE("HTTP integration") {
    // Build a runtime with the fake chat service
    auto chat_service = std::make_shared<FakeChatService>();
    chat_service->set_model_id("integration-test-model");

    zks::server::ServerConfig config;
    config.model_id = "integration-test-model";
    config.api_key = "test-secret";
    config.zoo_config.model_path = "/tmp/integration-test.gguf";
    config.http.client_max_body_size_bytes = 512; // intentionally small for 413 test

    auto runtime = std::make_shared<zks::server::ServerRuntime>(config, chat_service);
    auto disconnect_registry = std::make_shared<zks::server::DisconnectRegistry>();

    zks::server::register_health_routes(drogon::app(), runtime);
    zks::server::register_api_routes(drogon::app(), runtime, disconnect_registry);

    // Signal when the server loop starts
    std::mutex startup_mutex;
    std::condition_variable startup_cv;
    bool started = false;

    drogon::app().registerBeginningAdvice([&] {
        std::lock_guard<std::mutex> lock(startup_mutex);
        started = true;
        startup_cv.notify_all();
    });

    std::thread server_thread([&config, &disconnect_registry] {
        drogon::app()
            .addListener("127.0.0.1", 0)
            .setClientMaxBodySize(
                static_cast<size_t>(config.http.client_max_body_size_bytes))
            .setClientMaxMemoryBodySize(
                static_cast<size_t>(config.http.client_max_memory_body_size_bytes))
            .setConnectionCallback(
                [disconnect_registry](const trantor::TcpConnectionPtr& connection) {
                    disconnect_registry->handle_connection_event(connection);
                })
            .setLogLevel(trantor::Logger::kFatal)
            .disableSession()
            .run();
    });

    // Wait for Drogon to start
    {
        std::unique_lock<std::mutex> lock(startup_mutex);
        startup_cv.wait_for(lock, std::chrono::seconds(10), [&] { return started; });
        REQUIRE(started);
    }

    // Read the assigned port
    auto listeners = drogon::app().getListeners();
    REQUIRE_FALSE(listeners.empty());
    uint16_t port = listeners[0].toPort();

    auto client = drogon::HttpClient::newHttpClient("127.0.0.1", port, false,
                                                     drogon::app().getLoop());

    // Test 1: GET /healthz -> 200
    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath("/healthz");
        req->setMethod(drogon::Get);
        auto [status, resp] = client->sendRequest(req, 5.0);
        CHECK(status == drogon::ReqResult::Ok);
        REQUIRE(resp);
        CHECK(resp->getStatusCode() == drogon::k200OK);
    }

    // Test 2: GET /v1/models without auth -> 401
    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath("/v1/models");
        req->setMethod(drogon::Get);
        auto [status, resp] = client->sendRequest(req, 5.0);
        CHECK(status == drogon::ReqResult::Ok);
        REQUIRE(resp);
        CHECK(resp->getStatusCode() == drogon::k401Unauthorized);
    }

    // Test 3: POST /v1/chat/completions with oversized body -> 413
    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath("/v1/chat/completions");
        req->setMethod(drogon::Post);
        req->addHeader("Authorization", "Bearer test-secret");
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        std::string big_body(1024, 'x');
        req->setBody(std::move(big_body));
        auto [status, resp] = client->sendRequest(req, 5.0);
        CHECK(status == drogon::ReqResult::Ok);
        REQUIRE(resp);
        CHECK(resp->getStatusCode() == drogon::k413RequestEntityTooLarge);
    }

    // Test 4: POST /v1/chat/completions with valid JSON -> 500 (fake service returns error)
    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath("/v1/chat/completions");
        req->setMethod(drogon::Post);
        req->addHeader("Authorization", "Bearer test-secret");
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        nlohmann::json body = {
            {"model", "integration-test-model"},
            {"messages", {{{"role", "user"}, {"content", "hi"}}}},
            {"stream", false}};
        req->setBody(body.dump());
        auto [status, resp] = client->sendRequest(req, 5.0);
        CHECK(status == drogon::ReqResult::Ok);
        REQUIRE(resp);
        CHECK(static_cast<int>(resp->getStatusCode()) == 500);
    }

    // Test 5: GET /metrics -> 200 with all expected fields
    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath("/metrics");
        req->setMethod(drogon::Get);
        req->addHeader("Authorization", "Bearer test-secret");
        auto [status, resp] = client->sendRequest(req, 5.0);
        CHECK(status == drogon::ReqResult::Ok);
        REQUIRE(resp);
        CHECK(resp->getStatusCode() == drogon::k200OK);

        auto json = nlohmann::json::parse(std::string(resp->body()), nullptr, false);
        REQUIRE_FALSE(json.is_discarded());

        const char* required_fields[] = {"requests_total",
                                         "requests_errors",
                                         "requests_cancelled_total",
                                         "requests_queue_rejected_total",
                                         "stream_disconnects_total",
                                         "tool_invocations_total",
                                         "tool_failures_total",
                                         "tool_timeouts_total",
                                         "active_sessions",
                                         "model_id",
                                         "uptime_seconds"};
        for (const auto* field : required_fields) {
            CHECK(json.contains(field));
        }
    }

    // Test 6: queue_full mode increments requests_queue_rejected_total
    {
        auto before = read_metric(client, "requests_queue_rejected_total");
        REQUIRE(before.has_value());

        chat_service->set_mode(FakeCompletionMode::QueueFull);

        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath("/v1/chat/completions");
        req->setMethod(drogon::Post);
        req->addHeader("Authorization", "Bearer test-secret");
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        nlohmann::json body = {{"model", "integration-test-model"},
                               {"messages", {{{"role", "user"}, {"content", "hi"}}}},
                               {"stream", false}};
        req->setBody(body.dump());
        auto [status, resp] = client->sendRequest(req, 5.0);
        CHECK(status == drogon::ReqResult::Ok);
        REQUIRE(resp);
        CHECK(resp->getStatusCode() == drogon::k503ServiceUnavailable);

        auto after = read_metric(client, "requests_queue_rejected_total");
        REQUIRE(after.has_value());
        CHECK(*after > *before);

        chat_service->set_mode(FakeCompletionMode::ServerError);
    }

    // Test 7: cancelled completion increments requests_cancelled_total
    {
        auto before = read_metric(client, "requests_cancelled_total");
        REQUIRE(before.has_value());

        chat_service->set_mode(FakeCompletionMode::Cancelled);

        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath("/v1/chat/completions");
        req->setMethod(drogon::Post);
        req->addHeader("Authorization", "Bearer test-secret");
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        nlohmann::json body = {{"model", "integration-test-model"},
                               {"messages", {{{"role", "user"}, {"content", "hi"}}}},
                               {"stream", false}};
        req->setBody(body.dump());
        auto [status, resp] = client->sendRequest(req, 5.0);
        CHECK(status == drogon::ReqResult::Ok);
        REQUIRE(resp);

        auto after = read_metric(client, "requests_cancelled_total");
        REQUIRE(after.has_value());
        CHECK(*after > *before);

        chat_service->set_mode(FakeCompletionMode::ServerError);
    }

    // Test 8: stream_disconnects_total increment path
    {
        auto before = read_metric(client, "stream_disconnects_total");
        REQUIRE(before.has_value());

        runtime->metrics().increment_stream_disconnects();

        auto after = read_metric(client, "stream_disconnects_total");
        REQUIRE(after.has_value());
        CHECK(*after == *before + 1);
    }

    // Test 9: GET /v1/tools returns registered tool metadata
    {
        chat_service->set_tools({zks::server::ToolDefinition{
            "echo",
            "Echoes the input back",
            nlohmann::json{{"type", "object"},
                           {"properties", {{"input", {{"type", "string"}}}}},
                           {"required", {"input"}}},
        }});

        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath("/v1/tools");
        req->setMethod(drogon::Get);
        req->addHeader("Authorization", "Bearer test-secret");
        auto [status, resp] = client->sendRequest(req, 5.0);
        CHECK(status == drogon::ReqResult::Ok);
        REQUIRE(resp);
        CHECK(resp->getStatusCode() == drogon::k200OK);

        auto json = nlohmann::json::parse(std::string(resp->body()), nullptr, false);
        REQUIRE_FALSE(json.is_discarded());
        REQUIRE(json.contains("data"));
        REQUIRE(json["data"].is_array());
        REQUIRE(json["data"].size() == 1);
        CHECK(json["data"][0]["function"]["name"] == "echo");
    }

    // Test 10: successful completion with tool invocations increments tool metrics
    {
        auto before_invocations = read_metric(client, "tool_invocations_total");
        auto before_failures = read_metric(client, "tool_failures_total");
        auto before_timeouts = read_metric(client, "tool_timeouts_total");
        REQUIRE(before_invocations.has_value());
        REQUIRE(before_failures.has_value());
        REQUIRE(before_timeouts.has_value());

        zks::server::CompletionResult response;
        response.text = "tool result";

        zks::server::ToolInvocationRecord succeeded;
        succeeded.id = "call-1";
        succeeded.name = "echo";
        succeeded.arguments_json = R"({"input":"hi"})";
        succeeded.status = zks::server::ToolInvocationStatus::Succeeded;
        succeeded.result_json = R"({"output":"hi"})";
        response.tool_invocations.push_back(std::move(succeeded));

        zks::server::ToolInvocationRecord timed_out;
        timed_out.id = "call-2";
        timed_out.name = "echo";
        timed_out.arguments_json = R"({"input":"slow"})";
        timed_out.status = zks::server::ToolInvocationStatus::ExecutionFailed;
        timed_out.error = zks::server::RuntimeError{
            zks::server::RuntimeErrorCode::ToolExecutionFailed,
            "Tool command timed out after 10 ms",
            "timeout",
        };
        response.tool_invocations.push_back(std::move(timed_out));

        chat_service->set_response(std::move(response));
        chat_service->set_mode(FakeCompletionMode::Success);

        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath("/v1/chat/completions");
        req->setMethod(drogon::Post);
        req->addHeader("Authorization", "Bearer test-secret");
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        nlohmann::json body = {{"model", "integration-test-model"},
                               {"messages", {{{"role", "user"}, {"content", "hi"}}}},
                               {"stream", false}};
        req->setBody(body.dump());
        auto [status, resp] = client->sendRequest(req, 5.0);
        CHECK(status == drogon::ReqResult::Ok);
        REQUIRE(resp);
        CHECK(resp->getStatusCode() == drogon::k200OK);

        auto after_invocations = read_metric(client, "tool_invocations_total");
        auto after_failures = read_metric(client, "tool_failures_total");
        auto after_timeouts = read_metric(client, "tool_timeouts_total");
        REQUIRE(after_invocations.has_value());
        REQUIRE(after_failures.has_value());
        REQUIRE(after_timeouts.has_value());
        CHECK(*after_invocations == *before_invocations + 2);
        CHECK(*after_failures == *before_failures + 1);
        CHECK(*after_timeouts == *before_timeouts + 1);

        chat_service->set_mode(FakeCompletionMode::ServerError);
    }

    // Test 11: Streaming SSE success
    {
        chat_service->set_mode(FakeCompletionMode::StreamingSuccess);
        auto latch = chat_service->finish_latch();

        // Signal the latch after a short delay so the stream completes
        std::thread signal_thread([latch] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            latch->signal();
        });

        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath("/v1/chat/completions");
        req->setMethod(drogon::Post);
        req->addHeader("Authorization", "Bearer test-secret");
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        nlohmann::json body = {{"model", "integration-test-model"},
                               {"messages", {{{"role", "user"}, {"content", "hi"}}}},
                               {"stream", true}};
        req->setBody(body.dump());
        auto [status, resp] = client->sendRequest(req, 10.0);
        signal_thread.join();

        CHECK(status == drogon::ReqResult::Ok);
        REQUIRE(resp);

        // Verify Content-Type
        auto content_type = std::string(resp->contentTypeString());
        CHECK(content_type.find("text/event-stream") != std::string::npos);

        // Parse SSE frames from response body
        std::string resp_body(resp->body());
        CHECK(resp_body.find("\"role\":\"assistant\"") != std::string::npos);
        CHECK(resp_body.find("hello ") != std::string::npos);
        CHECK(resp_body.find("world") != std::string::npos);
        CHECK(resp_body.find("\"finish_reason\":\"stop\"") != std::string::npos);
        CHECK(resp_body.find("data: [DONE]") != std::string::npos);

        chat_service->set_mode(FakeCompletionMode::ServerError);
    }

    // Test 12: Streaming with queue_full returns 503
    {
        chat_service->set_mode(FakeCompletionMode::QueueFull);

        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath("/v1/chat/completions");
        req->setMethod(drogon::Post);
        req->addHeader("Authorization", "Bearer test-secret");
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        nlohmann::json body = {{"model", "integration-test-model"},
                               {"messages", {{{"role", "user"}, {"content", "hi"}}}},
                               {"stream", true}};
        req->setBody(body.dump());
        auto [status, resp] = client->sendRequest(req, 5.0);
        CHECK(status == drogon::ReqResult::Ok);
        REQUIRE(resp);
        CHECK(resp->getStatusCode() == drogon::k503ServiceUnavailable);

        chat_service->set_mode(FakeCompletionMode::ServerError);
    }

    // Shutdown
    client.reset();
    drogon::app().quit();
    server_thread.join();
    runtime->stop();
}
