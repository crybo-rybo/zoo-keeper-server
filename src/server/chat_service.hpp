#pragma once

#include "server/api_types.hpp"
#include "server/config.hpp"
#include "server/result.hpp"
#include "server/session_manager.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zoo {
class Agent;
}

namespace zks::server {

struct RegisteredTool {
    ToolDefinition definition;
    std::function<RuntimeResult<nlohmann::json>(const nlohmann::json&)> invoke;
};

struct ToolProvider {
    std::vector<RegisteredTool> tools;
};

class ChatService {
  public:
    virtual ~ChatService() = default;

    [[nodiscard]] virtual bool is_ready() const noexcept = 0;
    [[nodiscard]] virtual const std::string& model_id() const noexcept = 0;
    [[nodiscard]] virtual const std::vector<ToolDefinition>& tools() const noexcept = 0;
    [[nodiscard]] virtual SessionHealth session_health() const noexcept = 0;

    virtual ApiResult<PendingChatCompletion> start_completion(
        const ChatCompletionRequest& request,
        std::optional<TokenCallback> callback = std::nullopt) = 0;

    virtual ApiResult<SessionSummary> create_session(const SessionCreateRequest& request) = 0;
    virtual ApiResult<SessionSummary> get_session(std::string_view session_id) = 0;
    virtual ApiResult<void> delete_session(std::string_view session_id) = 0;

    virtual void reap_sessions() noexcept {}

    virtual void stop() = 0;
};

class ZooChatService final : public ChatService {
  public:
    static Result<std::shared_ptr<ZooChatService>> create(const ServerConfig& config);
    static Result<std::shared_ptr<ZooChatService>> create(const ServerConfig& config,
                                                           ToolProvider tools);

    ZooChatService(std::string model_id, std::string request_system_prompt,
                   std::vector<ToolDefinition> tool_metadata,
                   std::unique_ptr<zoo::Agent> shared_agent,
                   std::unique_ptr<SessionManager> session_manager);
    ~ZooChatService() override;

    [[nodiscard]] bool is_ready() const noexcept override;
    [[nodiscard]] const std::string& model_id() const noexcept override;
    [[nodiscard]] const std::vector<ToolDefinition>& tools() const noexcept override;
    [[nodiscard]] SessionHealth session_health() const noexcept override;

    ApiResult<PendingChatCompletion> start_completion(
        const ChatCompletionRequest& request,
        std::optional<TokenCallback> callback = std::nullopt) override;

    ApiResult<SessionSummary> create_session(const SessionCreateRequest& request) override;
    ApiResult<SessionSummary> get_session(std::string_view session_id) override;
    ApiResult<void> delete_session(std::string_view session_id) override;

    void reap_sessions() noexcept override;

    void stop() override;

  private:
    [[nodiscard]] std::vector<ChatMessage> prepare_messages(const ChatCompletionRequest& request) const;

    std::string model_id_;
    std::string request_system_prompt_;
    std::vector<ToolDefinition> tool_metadata_;
    std::unique_ptr<zoo::Agent> agent_;
    std::unique_ptr<SessionManager> session_manager_;
    std::atomic<std::uint64_t> next_completion_id_{1};
};

} // namespace zks::server
