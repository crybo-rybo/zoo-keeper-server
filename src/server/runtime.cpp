#include "server/runtime.hpp"

#include <utility>

namespace zks::server {

Result<std::shared_ptr<ServerRuntime>> ServerRuntime::create(ServerConfig config) {
    auto agent_result = zoo::Agent::create(config.zoo_config);
    if (!agent_result) {
        return std::unexpected("Failed to create zoo::Agent: " + agent_result.error().to_string());
    }

    return std::make_shared<ServerRuntime>(std::move(config), std::move(*agent_result));
}

ServerRuntime::ServerRuntime(ServerConfig config, std::unique_ptr<zoo::Agent> agent)
    : config_(std::move(config)), agent_(std::move(agent)) {}

} // namespace zks::server
