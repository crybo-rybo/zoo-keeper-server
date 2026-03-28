#pragma once

#include "server/api_types.hpp"
#include "server/config.hpp"
#include "server/result.hpp"
#include "server/session_manager.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

/// Abstract interface for the chat completion service.
///
/// All methods are safe to call from Drogon I/O threads.
/// One shared agent backs all requests; sessions are managed via SessionStore.
class ChatService {
  public:
    virtual ~ChatService() = default;

    [[nodiscard]] virtual bool is_ready() const noexcept = 0;
    [[nodiscard]] virtual const std::string& model_id() const noexcept = 0;
    [[nodiscard]] virtual const std::vector<ToolDefinition>& tools() const noexcept = 0;
    [[nodiscard]] virtual SessionHealth session_health() const noexcept = 0;
    /// Returns the static model configuration loaded at startup.
    [[nodiscard]] virtual const zoo::ModelConfig& model_config() const noexcept = 0;
    /// Returns the shared-agent configuration loaded at startup.
    [[nodiscard]] virtual const zoo::AgentConfig& agent_config() const noexcept = 0;
    /// Returns the default generation options applied to requests.
    [[nodiscard]] virtual const zoo::GenerationOptions& default_generation() const noexcept = 0;

    /// Begins a chat completion. Returns `ApiError` with `queue_full` code when the
    /// bounded continuation executor is saturated.
    virtual ApiResult<PendingChatCompletion>
    start_completion(const ChatCompletionRequest& request,
                     std::optional<TokenCallback> callback = std::nullopt) = 0;
    /// Begins a retained-agent chat request against the shared agent history.
    virtual ApiResult<PendingChatCompletion>
    start_agent_chat(const AgentChatRequest& request,
                     std::optional<TokenCallback> callback = std::nullopt) = 0;
    /// Begins a structured extraction request.
    virtual ApiResult<PendingExtraction>
    start_extraction(const ExtractionRequest& request,
                     std::optional<TokenCallback> callback = std::nullopt) = 0;
    /// Cancels an in-flight request by its public request identifier.
    virtual ApiResult<void> cancel_request(std::string_view request_id) = 0;

    virtual ApiResult<SessionSummary> create_session(const SessionCreateRequest& request) = 0;
    virtual ApiResult<SessionSummary> get_session(std::string_view session_id) = 0;
    virtual ApiResult<void> delete_session(std::string_view session_id) = 0;
    /// Returns a snapshot of the retained shared-agent history.
    virtual ApiResult<AgentHistorySnapshot> get_agent_history() = 0;
    /// Clears the retained shared-agent history while preserving the base system prompt.
    virtual ApiResult<void> clear_agent_history() = 0;
    /// Replaces the retained shared-agent history with the provided message list.
    virtual ApiResult<AgentHistorySnapshot>
    replace_agent_history(const AgentHistoryRequest& request) = 0;
    /// Replaces the retained shared-agent history and returns the previous snapshot.
    virtual ApiResult<AgentHistorySnapshot>
    swap_agent_history(const AgentHistoryRequest& request) = 0;
    /// Appends a single validated message to the retained shared-agent history.
    virtual ApiResult<AgentHistorySnapshot>
    append_agent_history_message(const AgentHistoryMessageRequest& request) = 0;
    /// Returns the current effective system prompt for the shared agent.
    virtual ApiResult<std::string> get_system_prompt() = 0;
    /// Updates the effective system prompt for future retained-agent and new-session requests.
    virtual ApiResult<std::string> set_system_prompt(std::string prompt) = 0;

    virtual void reap_sessions() noexcept {}

    /// Stops any background resources owned by the service.
    virtual void stop() = 0;
};

/// Concrete ChatService backed by a single shared `zoo::Agent`.
///
/// Owns both the agent and the session store. Handles completion orchestration
/// for both session and non-session paths directly.
class ZooChatService final : public ChatService {
  public:
    static Result<std::shared_ptr<ZooChatService>> create(const ServerConfig& config);
    static Result<std::shared_ptr<ZooChatService>> create(const ServerConfig& config,
                                                          ToolProvider tools);

    ZooChatService(std::string model_id, zoo::ModelConfig model_config,
                   zoo::AgentConfig agent_config, std::string request_system_prompt,
                   std::vector<ToolDefinition> tool_metadata,
                   std::shared_ptr<zoo::Agent> shared_agent,
                   std::unique_ptr<SessionStore> session_store,
                   zoo::GenerationOptions default_generation);
    ~ZooChatService() override;

    [[nodiscard]] bool is_ready() const noexcept override;
    [[nodiscard]] const std::string& model_id() const noexcept override;
    [[nodiscard]] const std::vector<ToolDefinition>& tools() const noexcept override;
    [[nodiscard]] SessionHealth session_health() const noexcept override;
    [[nodiscard]] const zoo::ModelConfig& model_config() const noexcept override;
    [[nodiscard]] const zoo::AgentConfig& agent_config() const noexcept override;
    [[nodiscard]] const zoo::GenerationOptions& default_generation() const noexcept override;

    ApiResult<PendingChatCompletion>
    start_completion(const ChatCompletionRequest& request,
                     std::optional<TokenCallback> callback = std::nullopt) override;
    ApiResult<PendingChatCompletion>
    start_agent_chat(const AgentChatRequest& request,
                     std::optional<TokenCallback> callback = std::nullopt) override;
    ApiResult<PendingExtraction>
    start_extraction(const ExtractionRequest& request,
                     std::optional<TokenCallback> callback = std::nullopt) override;
    ApiResult<void> cancel_request(std::string_view request_id) override;

    ApiResult<SessionSummary> create_session(const SessionCreateRequest& request) override;
    ApiResult<SessionSummary> get_session(std::string_view session_id) override;
    ApiResult<void> delete_session(std::string_view session_id) override;
    ApiResult<AgentHistorySnapshot> get_agent_history() override;
    ApiResult<void> clear_agent_history() override;
    ApiResult<AgentHistorySnapshot>
    replace_agent_history(const AgentHistoryRequest& request) override;
    ApiResult<AgentHistorySnapshot> swap_agent_history(const AgentHistoryRequest& request) override;
    ApiResult<AgentHistorySnapshot>
    append_agent_history_message(const AgentHistoryMessageRequest& request) override;
    ApiResult<std::string> get_system_prompt() override;
    ApiResult<std::string> set_system_prompt(std::string prompt) override;

    void reap_sessions() noexcept override;

    void stop() override;

  private:
    [[nodiscard]] std::vector<ChatMessage>
    prepare_messages(const ChatCompletionRequest& request) const;
    [[nodiscard]] std::vector<ChatMessage> prepare_messages(const ExtractionRequest& request) const;
    [[nodiscard]] zoo::HistorySnapshot
    make_history_snapshot(const AgentHistoryRequest& request) const;
    [[nodiscard]] AgentHistorySnapshot make_agent_history_snapshot() const;

    ApiResult<PendingChatCompletion>
    start_session_completion(const ChatCompletionRequest& request,
                             std::optional<TokenCallback> callback);
    ApiResult<PendingExtraction> start_session_extraction(const ExtractionRequest& request,
                                                          std::optional<TokenCallback> callback);

    void track_session_request(const std::string& session_id, std::uint64_t zoo_request_id);
    void untrack_session_request(const std::string& session_id);
    void register_public_request(std::string request_id, std::function<void()> cancel);
    void unregister_public_request(std::string_view request_id);
    static int estimate_history_tokens(const zoo::HistorySnapshot& history) noexcept;
    std::string update_system_prompt_state(std::string prompt);
    void reset_agent_history_to_system_prompt();

    std::string model_id_;
    zoo::ModelConfig model_config_;
    zoo::AgentConfig agent_config_;
    std::string request_system_prompt_;
    std::vector<ToolDefinition> tool_metadata_;
    std::shared_ptr<zoo::Agent> agent_;
    zoo::GenerationOptions default_generation_;
    std::unique_ptr<SessionStore> session_store_;
    std::atomic<std::uint64_t> next_completion_id_{1};

    /// Maps session_id → zoo::RequestId for active session requests.
    /// Used by delete_session to cancel in-flight inference.
    std::mutex active_requests_mutex_;
    std::unordered_map<std::string, std::uint64_t> active_session_requests_;
    std::mutex public_requests_mutex_;
    std::unordered_map<std::string, std::function<void()>, TransparentStringHash,
                       TransparentStringEqual>
        public_request_cancellers_;
    mutable std::mutex agent_history_mutex_;
    zoo::HistorySnapshot agent_history_;
    mutable std::mutex system_prompt_mutex_;

    friend class ZooChatServiceTest;
    friend class ZooChatServiceTest_UpdateSystemPromptStateKeepsRetainedHistoryAligned_Test;
    friend class ZooChatServiceTest_ResetAgentHistoryPreservesCurrentSystemPrompt_Test;
    friend class ZooChatServiceTest_ConcurrentPromptUpdatesAndHistoryResetsStayConsistent_Test;
};

} // namespace zks::server
