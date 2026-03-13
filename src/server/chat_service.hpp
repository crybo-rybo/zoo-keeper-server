#pragma once

#include "server/api_types.hpp"
#include "server/config.hpp"
#include "server/result.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <zoo/agent.hpp>
#include <zoo/tools/types.hpp>

namespace zks::server {

struct PendingChatCompletion {
    std::string id;
    std::int64_t created = 0;
    std::string model;
    zoo::RequestHandle handle;
};

class ChatService {
  public:
    virtual ~ChatService() = default;

    [[nodiscard]] virtual bool is_ready() const noexcept = 0;
    [[nodiscard]] virtual const std::string& model_id() const noexcept = 0;
    [[nodiscard]] virtual const std::vector<zoo::tools::ToolMetadata>& tools() const noexcept = 0;

    virtual ApiResult<PendingChatCompletion> start_completion(
        const ChatCompletionRequest& request,
        std::optional<std::function<void(std::string_view)>> callback = std::nullopt) = 0;

    virtual void cancel(zoo::RequestId id) = 0;
};

class ZooChatService final : public ChatService {
  public:
    static Result<std::shared_ptr<ZooChatService>> create(const ServerConfig& config);

    ZooChatService(std::string model_id, std::string request_system_prompt,
                   std::vector<zoo::tools::ToolMetadata> tool_metadata,
                   std::unique_ptr<zoo::Agent> agent);

    [[nodiscard]] bool is_ready() const noexcept override;
    [[nodiscard]] const std::string& model_id() const noexcept override;
    [[nodiscard]] const std::vector<zoo::tools::ToolMetadata>& tools() const noexcept override;

    ApiResult<PendingChatCompletion> start_completion(
        const ChatCompletionRequest& request,
        std::optional<std::function<void(std::string_view)>> callback = std::nullopt) override;

    void cancel(zoo::RequestId id) override;

  private:
    [[nodiscard]] std::vector<zoo::Message>
    prepare_messages(const ChatCompletionRequest& request) const;

    std::string model_id_;
    std::string request_system_prompt_;
    std::vector<zoo::tools::ToolMetadata> tool_metadata_;
    std::unique_ptr<zoo::Agent> agent_;
    std::atomic<std::uint64_t> next_completion_id_{1};
};

} // namespace zks::server
