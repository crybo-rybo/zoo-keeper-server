#pragma once

#include "server/api_types.hpp"

#include <memory>

namespace drogon {
class HttpAppFramework;
}

namespace zks::server {

class DisconnectRegistry;
class ServerRuntime;

void register_chat_completion_route(drogon::HttpAppFramework& app,
                                    const std::shared_ptr<ServerRuntime>& runtime,
                                    const std::shared_ptr<DisconnectRegistry>& disconnect_registry);

void release_completion(const PendingChatCompletion& pending);

void finalize_completion(const PendingChatCompletion& pending,
                         const RuntimeResult<CompletionResult>& result);

} // namespace zks::server
