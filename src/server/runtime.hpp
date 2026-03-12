#pragma once

#include "server/config.hpp"
#include "server/result.hpp"

#include <memory>
#include <string>

#include <zoo/agent.hpp>

namespace zks::server {

struct HealthSnapshot {
    bool ready = false;
    std::string model_id;
};

class ServerRuntime {
  public:
    static Result<std::shared_ptr<ServerRuntime>> create(ServerConfig config);

    ServerRuntime(ServerConfig config, std::unique_ptr<zoo::Agent> agent);

    [[nodiscard]] const ServerConfig& config() const noexcept {
        return config_;
    }

    [[nodiscard]] bool is_ready() const noexcept {
        return static_cast<bool>(agent_);
    }

    [[nodiscard]] HealthSnapshot health_snapshot() const {
        return HealthSnapshot{is_ready(), config_.model_id};
    }

    [[nodiscard]] zoo::Agent& agent() noexcept {
        return *agent_;
    }

  private:
    ServerConfig config_;
    std::unique_ptr<zoo::Agent> agent_;
};

} // namespace zks::server
