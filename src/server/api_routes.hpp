#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <trantor/net/TcpConnection.h>

namespace zks::server {

class ServerRuntime;

class DisconnectRegistry {
  public:
    void track(const trantor::TcpConnectionPtr& connection, std::function<void()> on_disconnect);
    void clear(const trantor::TcpConnectionPtr& connection);
    void handle_connection_event(const trantor::TcpConnectionPtr& connection);

  private:
    std::mutex mutex_;
    std::unordered_map<trantor::TcpConnectionPtr, std::function<void()>> callbacks_;
};

void register_api_routes(const std::shared_ptr<ServerRuntime>& runtime,
                         const std::shared_ptr<DisconnectRegistry>& disconnect_registry);

} // namespace zks::server
