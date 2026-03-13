#pragma once

#include "server/chat_service.hpp"
#include "server/config.hpp"
#include "server/result.hpp"

#include <memory>
#include <string>

namespace zks::server {

struct HealthSnapshot {
    bool ready = false;
    std::string model_id;
};

class ServerRuntime {
  public:
    static Result<std::shared_ptr<ServerRuntime>> create(ServerConfig config);

    ServerRuntime(ServerConfig config, std::shared_ptr<ChatService> chat_service);

    [[nodiscard]] const ServerConfig& config() const noexcept {
        return config_;
    }

    [[nodiscard]] bool is_ready() const noexcept {
        return static_cast<bool>(chat_service_) && chat_service_->is_ready();
    }

    [[nodiscard]] HealthSnapshot health_snapshot() const {
        return HealthSnapshot{is_ready(), config_.model_id};
    }

    [[nodiscard]] ChatService& chat_service() noexcept {
        return *chat_service_;
    }

    [[nodiscard]] const ChatService& chat_service() const noexcept {
        return *chat_service_;
    }

  private:
    ServerConfig config_;
    std::shared_ptr<ChatService> chat_service_;
};

} // namespace zks::server
