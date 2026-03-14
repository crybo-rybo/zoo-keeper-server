#include "server/api_routes.hpp"
#include "server/config.hpp"
#include "server/health.hpp"
#include "server/runtime.hpp"
#ifdef ZKS_ENABLE_TEST_UI
#include "server/test_ui.hpp"
#endif

#include <drogon/drogon.h>

#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: zoo_keeper_server <config.json>" << '\n';
        return 1;
    }

    auto config_result = zks::server::load_config(argv[1]);
    if (!config_result) {
        std::cerr << "Config load failed: " << config_result.error() << '\n';
        return 1;
    }

    if (auto warning = zks::server::startup_warning(*config_result); warning.has_value()) {
        std::clog << *warning << '\n';
    }

    auto runtime_result = zks::server::ServerRuntime::create(std::move(*config_result));
    if (!runtime_result) {
        std::cerr << "Config bootstrap failed: " << runtime_result.error() << '\n';
        return 1;
    }

    const auto& config = (*runtime_result)->config();
    auto disconnect_registry = std::make_shared<zks::server::DisconnectRegistry>();
    zks::server::register_health_routes(*runtime_result);
    zks::server::register_api_routes(*runtime_result, disconnect_registry);
#ifdef ZKS_ENABLE_TEST_UI
    zks::server::register_test_ui_routes(*runtime_result);
#endif

    std::clog << "zoo-keeper-server listening on " << config.bind_address << ":" << config.port
              << '\n';

    drogon::app()
        .addListener(config.bind_address, config.port)
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

    runtime_result->reset();
    disconnect_registry.reset();

    std::clog << "[shutdown] Server stopped." << '\n';

    return 0;
}
