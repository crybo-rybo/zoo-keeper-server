#include "server/runtime.hpp"

#include <utility>

namespace zks::server {

Result<std::shared_ptr<ServerRuntime>> ServerRuntime::create(ServerConfig config) {
    auto chat_service_result = ZooChatService::create(config);
    if (!chat_service_result) {
        return std::unexpected(chat_service_result.error());
    }

    return std::make_shared<ServerRuntime>(std::move(config), std::move(*chat_service_result));
}

ServerRuntime::ServerRuntime(ServerConfig config, std::shared_ptr<ChatService> chat_service)
    : config_(std::move(config)), chat_service_(std::move(chat_service)) {}

} // namespace zks::server
