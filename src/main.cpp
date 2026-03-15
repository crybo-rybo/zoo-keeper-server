#include "server/config.hpp"
#include "server/http_server.hpp"
#include "server/runtime.hpp"
#include "server/version.hpp"

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "zoo-keeper-server " << zks::kVersion << '\n';
        return 0;
    }

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

    zks::server::HttpServer server(*runtime_result,
                                   zks::server::HttpServerOptions{
#ifdef ZKS_ENABLE_TEST_UI
                                       .enable_test_ui = true,
#else
                                       .enable_test_ui = false,
#endif
                                   });
    return server.run();
}
