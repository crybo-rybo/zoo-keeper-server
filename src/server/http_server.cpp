#include "server/http_server.hpp"

#include "server/api_routes.hpp"
#include "server/health.hpp"
#include "server/runtime.hpp"
#ifdef ZKS_ENABLE_TEST_UI
#include "server/test_ui.hpp"
#endif

#include <drogon/drogon.h>

#include <iostream>
#include <memory>

namespace zks::server {

HttpServer::HttpServer(std::shared_ptr<ServerRuntime> runtime, HttpServerOptions options)
    : runtime_(std::move(runtime)), options_(options) {}

int HttpServer::run() {
    if (!runtime_) {
        std::cerr << "Server runtime is not initialized." << '\n';
        return 1;
    }

    auto disconnect_registry = std::make_shared<DisconnectRegistry>();
    auto& app = drogon::app();

    register_health_routes(app, runtime_);
    register_api_routes(app, runtime_, disconnect_registry);
#ifdef ZKS_ENABLE_TEST_UI
    if (options_.enable_test_ui) {
        register_test_ui_routes(app, runtime_);
    }
#endif

    const auto& config = runtime_->config();
    std::clog << "zoo-keeper-server listening on " << config.bind_address << ":" << config.port
              << '\n';

    if (!config.http.cors_allow_origins.empty()) {
        const auto cors_origins = config.http.cors_allow_origins;
        app.registerPostHandlingAdvice(
            [cors_origins](const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp) {
                const auto origin = req->getHeader("Origin");
                if (origin.empty()) {
                    return;
                }
                for (const auto& allowed : cors_origins) {
                    if (allowed == "*") {
                        resp->addHeader("Access-Control-Allow-Origin", "*");
                        return;
                    }
                    if (allowed == origin) {
                        resp->addHeader("Access-Control-Allow-Origin", origin);
                        return;
                    }
                }
            });
    }

    app.addListener(config.bind_address, config.port)
        .setClientMaxBodySize(static_cast<size_t>(config.http.client_max_body_size_bytes))
        .setClientMaxMemoryBodySize(
            static_cast<size_t>(config.http.client_max_memory_body_size_bytes))
        .setIdleConnectionTimeout(config.http.idle_connection_timeout_seconds)
        .setConnectionCallback([disconnect_registry](const trantor::TcpConnectionPtr& connection) {
            disconnect_registry->handle_connection_event(connection);
        })
        .setTermSignalHandler([] {
            std::clog << "[shutdown] Signal received, stopping server..." << '\n';
            drogon::app().quit();
        })
        .setIntSignalHandler([] {
            std::clog << "[shutdown] Signal received, stopping server..." << '\n';
            drogon::app().quit();
        })
        .setLogLevel(trantor::Logger::kWarn)
        .disableSession()
        .run();

    runtime_->stop();
    disconnect_registry.reset();

    std::clog << "[shutdown] Server stopped." << '\n';
    return 0;
}

} // namespace zks::server
