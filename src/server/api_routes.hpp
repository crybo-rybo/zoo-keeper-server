#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include <trantor/net/TcpConnection.h>

namespace zks::server {

class ServerRuntime;

class DisconnectRegistry {
  public:
    using CallbackId = size_t;

    CallbackId track(const trantor::TcpConnectionPtr& connection,
                     std::function<void()> on_disconnect);
    void clear(const trantor::TcpConnectionPtr& connection, CallbackId callback_id);
    void handle_connection_event(const trantor::TcpConnectionPtr& connection);

  private:
    std::mutex mutex_;
    CallbackId next_callback_id_ = 1;
    std::unordered_map<trantor::TcpConnectionPtr,
                       std::unordered_map<CallbackId, std::function<void()>>>
        callbacks_;
};

void register_api_routes(const std::shared_ptr<ServerRuntime>& runtime,
                         const std::shared_ptr<DisconnectRegistry>& disconnect_registry);

} // namespace zks::server
