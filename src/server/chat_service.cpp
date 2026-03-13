#include "server/chat_service.hpp"

#include <chrono>
#include <utility>

namespace zks::server {
namespace {

std::vector<zoo::tools::ToolMetadata> make_startup_tools() {
    return {};
}

std::string combine_system_prompts(const std::string& base_prompt,
                                   const std::string& request_prompt) {
    if (base_prompt.empty()) {
        return request_prompt;
    }
    if (request_prompt.empty()) {
        return base_prompt;
    }
    return base_prompt + "\n\n" + request_prompt;
}

} // namespace

Result<std::shared_ptr<ZooChatService>> ZooChatService::create(const ServerConfig& config) {
    auto agent_result = zoo::Agent::create(config.zoo_config);
    if (!agent_result) {
        return std::unexpected("Failed to create zoo::Agent: " + agent_result.error().to_string());
    }

    auto metadata = make_startup_tools();
    auto agent = std::move(*agent_result);

    std::string base_prompt = config.zoo_config.system_prompt.value_or("");
    std::string request_system_prompt =
        metadata.empty() ? base_prompt : agent->build_tool_system_prompt(base_prompt);
    if (!metadata.empty()) {
        agent->set_system_prompt(request_system_prompt);
    }

    return std::make_shared<ZooChatService>(config.model_id, std::move(request_system_prompt),
                                            std::move(metadata), std::move(agent));
}

ZooChatService::ZooChatService(std::string model_id, std::string request_system_prompt,
                               std::vector<zoo::tools::ToolMetadata> tool_metadata,
                               std::unique_ptr<zoo::Agent> agent)
    : model_id_(std::move(model_id)), request_system_prompt_(std::move(request_system_prompt)),
      tool_metadata_(std::move(tool_metadata)), agent_(std::move(agent)) {}

bool ZooChatService::is_ready() const noexcept {
    return static_cast<bool>(agent_) && agent_->is_running();
}

const std::string& ZooChatService::model_id() const noexcept {
    return model_id_;
}

const std::vector<zoo::tools::ToolMetadata>& ZooChatService::tools() const noexcept {
    return tool_metadata_;
}

ApiResult<PendingChatCompletion>
ZooChatService::start_completion(const ChatCompletionRequest& request,
                                 std::optional<std::function<void(std::string_view)>> callback) {
    if (!is_ready()) {
        return std::unexpected(
            service_unavailable_error("Server runtime is not ready", "not_ready"));
    }

    if (request.model != model_id_) {
        return std::unexpected(
            invalid_request_error("Unknown model: " + request.model, "model", "invalid_model"));
    }

    const auto created = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    const auto completion_id =
        "chatcmpl-" + std::to_string(next_completion_id_.fetch_add(1, std::memory_order_relaxed));
    auto handle = agent_->complete(prepare_messages(request), std::move(callback));

    return PendingChatCompletion{completion_id, created, model_id_, std::move(handle)};
}

void ZooChatService::cancel(zoo::RequestId id) {
    if (!agent_) {
        return;
    }
    agent_->cancel(id);
}

std::vector<zoo::Message>
ZooChatService::prepare_messages(const ChatCompletionRequest& request) const {
    auto messages = request.messages;
    if (request_system_prompt_.empty()) {
        return messages;
    }

    if (!messages.empty() && messages.front().role == zoo::Role::System) {
        messages.front() = zoo::Message::system(
            combine_system_prompts(request_system_prompt_, messages.front().content));
        return messages;
    }

    messages.insert(messages.begin(), zoo::Message::system(request_system_prompt_));
    return messages;
}

} // namespace zks::server
