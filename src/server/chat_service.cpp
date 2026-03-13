#include "server/chat_service.hpp"

#include <chrono>
#include <iostream>
#include <utility>

namespace zks::server {
namespace {

struct StartupTools {
    std::vector<zoo::tools::ToolMetadata> metadata;
    std::function<Result<void>(zoo::Agent&)> install = [](zoo::Agent&) -> Result<void> {
        return {};
    };
};

struct ConfiguredAgent {
    std::unique_ptr<zoo::Agent> agent;
    std::string request_system_prompt;
};

StartupTools make_startup_tools() {
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

Result<ConfiguredAgent> create_configured_agent(const zoo::Config& base_config,
                                                const StartupTools& tools,
                                                const std::optional<std::string>& extra_prompt) {
    zoo::Config config = base_config;
    const std::string effective_prompt =
        combine_system_prompts(base_config.system_prompt.value_or(""), extra_prompt.value_or(""));
    config.system_prompt = effective_prompt.empty() ? std::nullopt
                                                    : std::optional<std::string>(effective_prompt);

    auto agent_result = zoo::Agent::create(config);
    if (!agent_result) {
        return std::unexpected("Failed to create zoo::Agent: " + agent_result.error().to_string());
    }

    auto agent = std::move(*agent_result);
    if (auto install_result = tools.install(*agent); !install_result) {
        return std::unexpected(install_result.error());
    }

    std::string request_system_prompt = effective_prompt;
    if (!tools.metadata.empty()) {
        request_system_prompt = agent->build_tool_system_prompt(effective_prompt);
        agent->set_system_prompt(request_system_prompt);
    }

    return ConfiguredAgent{std::move(agent), std::move(request_system_prompt)};
}

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

Result<std::shared_ptr<ZooChatService>> ZooChatService::create(const ServerConfig& config) {
    const auto startup_tools = make_startup_tools();
    auto shared_result = create_configured_agent(config.zoo_config, startup_tools, std::nullopt);
    if (!shared_result) {
        return std::unexpected(shared_result.error());
    }

    auto shared_agent = std::move(shared_result->agent);
    auto* agent = shared_agent.get();
    auto session_manager = std::make_unique<SessionManager>(
        config.model_id, config.sessions, shared_result->request_system_prompt,
        config.zoo_config.max_history_messages,
        [agent](std::vector<zoo::Message> messages,
                std::optional<std::function<void(std::string_view)>> callback) {
            return agent->complete(std::move(messages), std::move(callback));
        },
        [agent](zoo::RequestId request_id) {
            if (agent) {
                agent->cancel(request_id);
            }
        });

    return std::make_shared<ZooChatService>(
        config.model_id, std::move(shared_result->request_system_prompt), startup_tools.metadata,
        std::move(shared_agent), std::move(session_manager));
}

ZooChatService::ZooChatService(std::string model_id, std::string request_system_prompt,
                               std::vector<zoo::tools::ToolMetadata> tool_metadata,
                               std::unique_ptr<zoo::Agent> shared_agent,
                               std::unique_ptr<SessionManager> session_manager)
    : model_id_(std::move(model_id)), request_system_prompt_(std::move(request_system_prompt)),
      tool_metadata_(std::move(tool_metadata)), agent_(std::move(shared_agent)),
      session_manager_(std::move(session_manager)) {}

bool ZooChatService::is_ready() const noexcept {
    return static_cast<bool>(agent_) && agent_->is_running();
}

const std::string& ZooChatService::model_id() const noexcept {
    return model_id_;
}

const std::vector<zoo::tools::ToolMetadata>& ZooChatService::tools() const noexcept {
    return tool_metadata_;
}

SessionHealth ZooChatService::session_health() const noexcept {
    return session_manager_ ? session_manager_->health() : SessionHealth{};
}

ApiResult<PendingChatCompletion>
ZooChatService::start_completion(const ChatCompletionRequest& request,
                                 std::optional<std::function<void(std::string_view)>> callback) {
    if (request.session_id.has_value()) {
        return session_manager_->start_completion(request, next_completion_id_, std::move(callback));
    }

    if (!is_ready()) {
        return std::unexpected(
            service_unavailable_error("Server runtime is not ready", "not_ready"));
    }
    if (request.model != model_id_) {
        return std::unexpected(
            invalid_request_error("Unknown model: " + request.model, "model", "invalid_model"));
    }

    auto handle = agent_->complete(prepare_messages(request), std::move(callback));
    const auto request_id = handle.id;
    const auto created = now_seconds();
    const auto completion_id =
        "chatcmpl-" + std::to_string(next_completion_id_.fetch_add(1, std::memory_order_relaxed));

    return PendingChatCompletion{
        completion_id,
        created,
        model_id_,
        std::move(handle),
        {},
        [agent = agent_.get(), request_id] {
            if (agent) {
                agent->cancel(request_id);
            }
        },
        std::make_shared<CompletionLease>(),
    };
}

ApiResult<SessionSummary> ZooChatService::create_session(const SessionCreateRequest& request) {
    if (!is_ready()) {
        return std::unexpected(
            service_unavailable_error("Server runtime is not ready", "agent_not_ready"));
    }
    return session_manager_->create_session(request);
}

ApiResult<SessionSummary> ZooChatService::get_session(std::string_view session_id) {
    return session_manager_->get_session(session_id);
}

ApiResult<void> ZooChatService::delete_session(std::string_view session_id) {
    return session_manager_->delete_session(session_id);
}

void ZooChatService::stop() {
    if (session_manager_) {
        session_manager_->stop();
    }
    if (agent_) {
        agent_->stop();
    }
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
