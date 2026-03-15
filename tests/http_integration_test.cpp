#include "server/api_routes.hpp"
#include "server/config.hpp"
#include "server/health.hpp"
#include "server/runtime.hpp"

#include "fake_chat_service.hpp"

#include <gtest/gtest.h>

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
    if (status != drogon::ReqResult::Ok || !resp || resp->getStatusCode() != drogon::k200OK) {
        return std::nullopt;
    }
    auto json = nlohmann::json::parse(std::string(resp->body()), nullptr, false);
    if (json.is_discarded() || !json.contains(field)) {
        return std::nullopt;
    }
    return json.at(field).get<int64_t>();
}

// Shared state for the Drogon singleton (can only be started once per process)
struct ServerState {
    std::shared_ptr<FakeChatService> chat_service;
    std::shared_ptr<zks::server::ServerRuntime> runtime;
    std::shared_ptr<zks::server::DisconnectRegistry> disconnect_registry;
    drogon::HttpClientPtr client;
    std::thread server_thread;
    bool started = false;
};

ServerState* g_state = nullptr;

} // namespace

class HttpIntegrationTest : public ::testing::Test {
  public:
    static void SetUpTestSuite() {
        g_state = new ServerState();
        g_state->chat_service = std::make_shared<FakeChatService>();
        g_state->chat_service->set_model_id("integration-test-model");

        zks::server::ServerConfig config;
        config.model_id = "integration-test-model";
        config.api_key = "test-secret";
        config.zoo_config.model_path = "/tmp/integration-test.gguf";
        config.http.client_max_body_size_bytes = 512;

        g_state->runtime =
            zks::server::ServerRuntime::create_for_test(config, g_state->chat_service);
        g_state->disconnect_registry = std::make_shared<zks::server::DisconnectRegistry>();

        zks::server::register_health_routes(drogon::app(), g_state->runtime);
        zks::server::register_api_routes(drogon::app(), g_state->runtime,
                                         g_state->disconnect_registry);

        std::mutex startup_mutex;
        std::condition_variable startup_cv;
        bool started = false;

        drogon::app().registerBeginningAdvice([&] {
            std::lock_guard<std::mutex> lock(startup_mutex);
            started = true;
            startup_cv.notify_all();
        });

        auto disconnect_registry = g_state->disconnect_registry;
        g_state->server_thread = std::thread([&config, disconnect_registry] {
            drogon::app()
                .addListener("127.0.0.1", 0)
                .setClientMaxBodySize(static_cast<size_t>(config.http.client_max_body_size_bytes))
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

        {
            std::unique_lock<std::mutex> lock(startup_mutex);
            startup_cv.wait_for(lock, std::chrono::seconds(10), [&] { return started; });
            ASSERT_TRUE(started) << "Server failed to start within 10 seconds.";
        }

        auto listeners = drogon::app().getListeners();
        ASSERT_FALSE(listeners.empty());
        uint16_t port = listeners[0].toPort();

        g_state->client =
            drogon::HttpClient::newHttpClient("127.0.0.1", port, false, drogon::app().getLoop());
        g_state->started = true;
    }

    static void TearDownTestSuite() {
        if (g_state) {
            g_state->client.reset();
            drogon::app().quit();
            if (g_state->server_thread.joinable()) {
                g_state->server_thread.join();
            }
            g_state->runtime->stop();
            delete g_state;
            g_state = nullptr;
        }
    }

  protected:
    auto& chat_service() {
        return g_state->chat_service;
    }
    auto& runtime() {
        return g_state->runtime;
    }
    auto& client() {
        return g_state->client;
    }
};

TEST_F(HttpIntegrationTest, HealthzReturns200) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setPath("/healthz");
    req->setMethod(drogon::Get);
    auto [status, resp] = client()->sendRequest(req, 5.0);
    ASSERT_EQ(status, drogon::ReqResult::Ok);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
}

TEST_F(HttpIntegrationTest, ModelsWithoutAuthReturns401) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setPath("/v1/models");
    req->setMethod(drogon::Get);
    auto [status, resp] = client()->sendRequest(req, 5.0);
    ASSERT_EQ(status, drogon::ReqResult::Ok);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->getStatusCode(), drogon::k401Unauthorized);
}

TEST_F(HttpIntegrationTest, OversizedBodyReturns413) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setPath("/v1/chat/completions");
    req->setMethod(drogon::Post);
    req->addHeader("Authorization", "Bearer test-secret");
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    std::string big_body(1024, 'x');
    req->setBody(std::move(big_body));
    auto [status, resp] = client()->sendRequest(req, 5.0);
    ASSERT_EQ(status, drogon::ReqResult::Ok);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->getStatusCode(), drogon::k413RequestEntityTooLarge);
}

TEST_F(HttpIntegrationTest, ValidCompletionReturns500FromFake) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setPath("/v1/chat/completions");
    req->setMethod(drogon::Post);
    req->addHeader("Authorization", "Bearer test-secret");
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    nlohmann::json body = {{"model", "integration-test-model"},
                           {"messages", {{{"role", "user"}, {"content", "hi"}}}},
                           {"stream", false}};
    req->setBody(body.dump());
    auto [status, resp] = client()->sendRequest(req, 5.0);
    ASSERT_EQ(status, drogon::ReqResult::Ok);
    ASSERT_TRUE(resp);
    EXPECT_EQ(static_cast<int>(resp->getStatusCode()), 500);
}

TEST_F(HttpIntegrationTest, MetricsReturns200WithAllFields) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setPath("/metrics");
    req->setMethod(drogon::Get);
    req->addHeader("Authorization", "Bearer test-secret");
    auto [status, resp] = client()->sendRequest(req, 5.0);
    ASSERT_EQ(status, drogon::ReqResult::Ok);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    auto json = nlohmann::json::parse(std::string(resp->body()), nullptr, false);
    ASSERT_FALSE(json.is_discarded());

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
        EXPECT_TRUE(json.contains(field)) << "Missing field: " << field;
    }
}

TEST_F(HttpIntegrationTest, QueueFullIncrementsMetric) {
    auto before = read_metric(client(), "requests_queue_rejected_total");
    ASSERT_TRUE(before.has_value());

    chat_service()->set_mode(FakeCompletionMode::QueueFull);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setPath("/v1/chat/completions");
    req->setMethod(drogon::Post);
    req->addHeader("Authorization", "Bearer test-secret");
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    nlohmann::json body = {{"model", "integration-test-model"},
                           {"messages", {{{"role", "user"}, {"content", "hi"}}}},
                           {"stream", false}};
    req->setBody(body.dump());
    auto [status, resp] = client()->sendRequest(req, 5.0);
    ASSERT_EQ(status, drogon::ReqResult::Ok);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);

    auto after = read_metric(client(), "requests_queue_rejected_total");
    ASSERT_TRUE(after.has_value());
    EXPECT_GT(*after, *before);

    chat_service()->set_mode(FakeCompletionMode::ServerError);
}

TEST_F(HttpIntegrationTest, CancelledCompletionIncrementsMetric) {
    auto before = read_metric(client(), "requests_cancelled_total");
    ASSERT_TRUE(before.has_value());

    chat_service()->set_mode(FakeCompletionMode::Cancelled);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setPath("/v1/chat/completions");
    req->setMethod(drogon::Post);
    req->addHeader("Authorization", "Bearer test-secret");
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    nlohmann::json body = {{"model", "integration-test-model"},
                           {"messages", {{{"role", "user"}, {"content", "hi"}}}},
                           {"stream", false}};
    req->setBody(body.dump());
    auto [status, resp] = client()->sendRequest(req, 5.0);
    ASSERT_EQ(status, drogon::ReqResult::Ok);
    ASSERT_TRUE(resp);

    auto after = read_metric(client(), "requests_cancelled_total");
    ASSERT_TRUE(after.has_value());
    EXPECT_GT(*after, *before);

    chat_service()->set_mode(FakeCompletionMode::ServerError);
}

TEST_F(HttpIntegrationTest, StreamDisconnectsIncrementPath) {
    auto before = read_metric(client(), "stream_disconnects_total");
    ASSERT_TRUE(before.has_value());

    runtime()->metrics().increment_stream_disconnects();

    auto after = read_metric(client(), "stream_disconnects_total");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(*after, *before + 1);
}

TEST_F(HttpIntegrationTest, ToolsEndpointReturnsMetadata) {
    chat_service()->set_tools({zks::server::ToolDefinition{
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
    auto [status, resp] = client()->sendRequest(req, 5.0);
    ASSERT_EQ(status, drogon::ReqResult::Ok);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    auto json = nlohmann::json::parse(std::string(resp->body()), nullptr, false);
    ASSERT_FALSE(json.is_discarded());
    ASSERT_TRUE(json.contains("data"));
    ASSERT_TRUE(json["data"].is_array());
    ASSERT_EQ(json["data"].size(), 1u);
    EXPECT_EQ(json["data"][0]["function"]["name"], "echo");
}

TEST_F(HttpIntegrationTest, ToolInvocationsIncrementMetrics) {
    auto before_invocations = read_metric(client(), "tool_invocations_total");
    auto before_failures = read_metric(client(), "tool_failures_total");
    auto before_timeouts = read_metric(client(), "tool_timeouts_total");
    ASSERT_TRUE(before_invocations.has_value());
    ASSERT_TRUE(before_failures.has_value());
    ASSERT_TRUE(before_timeouts.has_value());

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

    chat_service()->set_response(std::move(response));
    chat_service()->set_mode(FakeCompletionMode::Success);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setPath("/v1/chat/completions");
    req->setMethod(drogon::Post);
    req->addHeader("Authorization", "Bearer test-secret");
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    nlohmann::json body = {{"model", "integration-test-model"},
                           {"messages", {{{"role", "user"}, {"content", "hi"}}}},
                           {"stream", false}};
    req->setBody(body.dump());
    auto [status, resp] = client()->sendRequest(req, 5.0);
    ASSERT_EQ(status, drogon::ReqResult::Ok);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    auto after_invocations = read_metric(client(), "tool_invocations_total");
    auto after_failures = read_metric(client(), "tool_failures_total");
    auto after_timeouts = read_metric(client(), "tool_timeouts_total");
    ASSERT_TRUE(after_invocations.has_value());
    ASSERT_TRUE(after_failures.has_value());
    ASSERT_TRUE(after_timeouts.has_value());
    EXPECT_EQ(*after_invocations, *before_invocations + 2);
    EXPECT_EQ(*after_failures, *before_failures + 1);
    EXPECT_EQ(*after_timeouts, *before_timeouts + 1);

    chat_service()->set_mode(FakeCompletionMode::ServerError);
}

TEST_F(HttpIntegrationTest, StreamingSseSuccess) {
    chat_service()->set_mode(FakeCompletionMode::StreamingSuccess);
    auto latch = chat_service()->finish_latch();

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
    auto [status, resp] = client()->sendRequest(req, 10.0);
    signal_thread.join();

    ASSERT_EQ(status, drogon::ReqResult::Ok);
    ASSERT_TRUE(resp);

    auto content_type = std::string(resp->contentTypeString());
    EXPECT_NE(content_type.find("text/event-stream"), std::string::npos);

    std::string resp_body(resp->body());
    EXPECT_NE(resp_body.find("\"role\":\"assistant\""), std::string::npos);
    EXPECT_NE(resp_body.find("hello "), std::string::npos);
    EXPECT_NE(resp_body.find("world"), std::string::npos);
    EXPECT_NE(resp_body.find("\"finish_reason\":\"stop\""), std::string::npos);
    EXPECT_NE(resp_body.find("data: [DONE]"), std::string::npos);

    chat_service()->set_mode(FakeCompletionMode::ServerError);
}

TEST_F(HttpIntegrationTest, StreamingQueueFullReturns503) {
    chat_service()->set_mode(FakeCompletionMode::QueueFull);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setPath("/v1/chat/completions");
    req->setMethod(drogon::Post);
    req->addHeader("Authorization", "Bearer test-secret");
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    nlohmann::json body = {{"model", "integration-test-model"},
                           {"messages", {{{"role", "user"}, {"content", "hi"}}}},
                           {"stream", true}};
    req->setBody(body.dump());
    auto [status, resp] = client()->sendRequest(req, 5.0);
    ASSERT_EQ(status, drogon::ReqResult::Ok);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);

    chat_service()->set_mode(FakeCompletionMode::ServerError);
}

TEST_F(HttpIntegrationTest, CorsHeadersAbsentWhenUnconfigured) {
    // The test server has no cors_allow_origins configured; no CORS headers should appear.
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setPath("/healthz");
    req->setMethod(drogon::Get);
    req->addHeader("Origin", "http://localhost:3000");
    auto [status, resp] = client()->sendRequest(req, 5.0);
    ASSERT_EQ(status, drogon::ReqResult::Ok);
    ASSERT_TRUE(resp);
    EXPECT_TRUE(resp->getHeader("Access-Control-Allow-Origin").empty());
}

TEST_F(HttpIntegrationTest, HealthzResponseIncludesVersion) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setPath("/healthz");
    req->setMethod(drogon::Get);
    auto [status, resp] = client()->sendRequest(req, 5.0);
    ASSERT_EQ(status, drogon::ReqResult::Ok);
    ASSERT_TRUE(resp);

    auto json = nlohmann::json::parse(std::string(resp->body()), nullptr, false);
    ASSERT_FALSE(json.is_discarded());
    EXPECT_TRUE(json.contains("version"));
    EXPECT_FALSE(json.at("version").get<std::string>().empty());
}
