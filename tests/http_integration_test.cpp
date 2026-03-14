#include "server/api_routes.hpp"
#include "server/config.hpp"
#include "server/health.hpp"
#include "server/runtime.hpp"

#include "fake_chat_service.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

namespace {

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

/// Fetches /metrics and returns the value of a specific uint64 counter field.
/// Returns -1 on transport or parse failure.
int64_t read_metric(const drogon::HttpClientPtr& client, const char* field) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setPath("/metrics");
    req->setMethod(drogon::Get);
    req->addHeader("Authorization", "Bearer test-secret");
    auto [status, resp] = client->sendRequest(req, 5.0);
    if (status != drogon::ReqResult::Ok || !resp ||
        resp->getStatusCode() != drogon::k200OK) {
        return -1;
    }
    auto json = nlohmann::json::parse(std::string(resp->body()), nullptr, false);
    if (json.is_discarded() || !json.contains(field)) {
        return -1;
    }
    return json.at(field).get<int64_t>();
}

} // namespace

int main() {
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

    zks::server::register_health_routes(runtime);
    zks::server::register_api_routes(runtime, disconnect_registry);

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
        if (!started) {
            std::cerr << "Server failed to start within 10 seconds." << '\n';
            drogon::app().quit();
            server_thread.join();
            return 1;
        }
    }

    // Read the assigned port
    auto listeners = drogon::app().getListeners();
    if (listeners.empty()) {
        std::cerr << "No listeners found after startup." << '\n';
        drogon::app().quit();
        server_thread.join();
        return 1;
    }
    uint16_t port = listeners[0].toPort();

    // Create a client using the Drogon event loop (sync sendRequest is called from main thread)
    auto client = drogon::HttpClient::newHttpClient("127.0.0.1", port, false,
                                                     drogon::app().getLoop());

    int result = 0;

    // Test 1: GET /healthz → 200
    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath("/healthz");
        req->setMethod(drogon::Get);
        auto [status, resp] = client->sendRequest(req, 5.0);
        if (status != drogon::ReqResult::Ok || !resp ||
            resp->getStatusCode() != drogon::k200OK) {
            result = fail("Test 1 failed: GET /healthz should return 200.");
        }
    }

    // Test 2: GET /v1/models without auth → 401
    if (result == 0) {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath("/v1/models");
        req->setMethod(drogon::Get);
        auto [status, resp] = client->sendRequest(req, 5.0);
        if (status != drogon::ReqResult::Ok || !resp ||
            resp->getStatusCode() != drogon::k401Unauthorized) {
            result = fail("Test 2 failed: GET /v1/models without auth should return 401.");
        }
    }

    // Test 3: POST /v1/chat/completions with oversized body → 413
    if (result == 0) {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath("/v1/chat/completions");
        req->setMethod(drogon::Post);
        req->addHeader("Authorization", "Bearer test-secret");
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        // Create a body larger than the 512-byte limit
        std::string big_body(1024, 'x');
        req->setBody(std::move(big_body));
        auto [status, resp] = client->sendRequest(req, 5.0);
        if (status != drogon::ReqResult::Ok || !resp ||
            resp->getStatusCode() != drogon::k413RequestEntityTooLarge) {
            auto actual_code = resp ? static_cast<int>(resp->getStatusCode()) : -1;
            std::cerr << "Test 3 failed: oversized POST should return 413, got " << actual_code
                      << '\n';
            result = 1;
        }
    }

    // Test 4: POST /v1/chat/completions with valid JSON → 500 (fake service returns error)
    if (result == 0) {
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
        if (status != drogon::ReqResult::Ok || !resp) {
            result = fail("Test 4 failed: valid completion request should get a response.");
        } else {
            // Expect 500 because FakeChatService returns server_error
            auto code = static_cast<int>(resp->getStatusCode());
            if (code != 500) {
                std::cerr << "Test 4 failed: expected 500 from fake service, got " << code << '\n';
                result = 1;
            }
        }
    }

    // Test 5: GET /metrics → 200 with all expected fields
    if (result == 0) {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath("/metrics");
        req->setMethod(drogon::Get);
        req->addHeader("Authorization", "Bearer test-secret");
        auto [status, resp] = client->sendRequest(req, 5.0);
        if (status != drogon::ReqResult::Ok || !resp ||
            resp->getStatusCode() != drogon::k200OK) {
            result = fail("Test 5 failed: GET /metrics should return 200.");
        } else {
            auto json = nlohmann::json::parse(std::string(resp->body()), nullptr, false);
            if (json.is_discarded()) {
                result = fail("Test 5 failed: /metrics response is not valid JSON.");
            } else {
                // Check all expected fields exist
                const char* required_fields[] = {"requests_total",
                                                 "requests_errors",
                                                 "requests_cancelled_total",
                                                 "requests_queue_rejected_total",
                                                 "stream_disconnects_total",
                                                 "active_sessions",
                                                 "model_id",
                                                 "uptime_seconds"};
                for (const auto* field : required_fields) {
                    if (!json.contains(field)) {
                        std::cerr << "Test 5 failed: /metrics missing field: " << field << '\n';
                        result = 1;
                        break;
                    }
                }
            }
        }
    }

    // Test 6: queue_full mode increments requests_queue_rejected_total
    if (result == 0) {
        auto before = read_metric(client, "requests_queue_rejected_total");
        chat_service->set_mode(FakeCompletionMode::QueueFull);

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
        if (status != drogon::ReqResult::Ok || !resp ||
            resp->getStatusCode() != drogon::k503ServiceUnavailable) {
            auto actual_code = resp ? static_cast<int>(resp->getStatusCode()) : -1;
            std::cerr << "Test 6 failed: queue_full should return 503, got " << actual_code << '\n';
            result = 1;
        } else {
            auto after = read_metric(client, "requests_queue_rejected_total");
            if (after <= before) {
                std::cerr << "Test 6 failed: requests_queue_rejected_total did not increment ("
                          << before << " -> " << after << ")\n";
                result = 1;
            }
        }
        chat_service->set_mode(FakeCompletionMode::ServerError);
    }

    // Test 7: cancelled completion increments requests_cancelled_total
    if (result == 0) {
        auto before = read_metric(client, "requests_cancelled_total");
        chat_service->set_mode(FakeCompletionMode::Cancelled);

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
        if (status != drogon::ReqResult::Ok || !resp) {
            result = fail("Test 7 failed: cancelled completion request should get a response.");
        } else {
            auto after = read_metric(client, "requests_cancelled_total");
            if (after <= before) {
                std::cerr << "Test 7 failed: requests_cancelled_total did not increment ("
                          << before << " -> " << after << ")\n";
                result = 1;
            }
        }
        chat_service->set_mode(FakeCompletionMode::ServerError);
    }

    // Test 8: stream_disconnects_total increment path
    //
    // On non-Linux platforms, Drogon's ListenerManager does not propagate
    // setConnectionCallback to HttpServer instances, so TCP-level disconnect
    // detection does not work. We verify the counter path directly: the
    // DisconnectRegistry callback (when fired) increments the metric via
    // runtime->metrics().increment_stream_disconnects(). We test this by
    // calling increment_stream_disconnects() and verifying /metrics reflects it.
    if (result == 0) {
        auto before = read_metric(client, "stream_disconnects_total");
        runtime->metrics().increment_stream_disconnects();
        auto after = read_metric(client, "stream_disconnects_total");
        if (after != before + 1) {
            std::cerr << "Test 8 failed: stream_disconnects_total did not increment ("
                      << before << " -> " << after << ")\n";
            result = 1;
        }
    }

    // Shutdown
    client.reset();
    drogon::app().quit();
    server_thread.join();
    runtime->stop();

    return result;
}
