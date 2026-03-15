#include "server/chat_service.hpp"

#include "server/zoo_adapter.hpp"

#include <chrono>
#include <mutex>
#include <utility>

#include <zoo/agent.hpp>

namespace zks::server {
namespace {

struct ConfiguredAgent {
    std::shared_ptr<zoo::Agent> agent;
    std::string request_system_prompt;
    std::vector<ToolDefinition> tool_definitions;
};

class ZooCompletionSource final : public CompletionSource {
  public:
    explicit ZooCompletionSource(std::future<zoo::Expected<zoo::Response>> future)
        : future_(std::move(future)) {}

    [[nodiscard]] std::future_status
    wait_for(std::chrono::milliseconds timeout) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (consumed_) {
            return std::future_status::ready;
        }
        return future_.wait_for(timeout);
    }

    RuntimeResult<CompletionResult> get() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (consumed_) {
            return std::unexpected(RuntimeError{
                RuntimeErrorCode::RuntimeFailure,
                "Completion result was already consumed",
            });
        }

        consumed_ = true;
        auto result = future_.get();
        if (!result) {
            return std::unexpected(from_zoo_error(result.error()));
        }
        return from_zoo_response(*result);
    }

  private:
    mutable std::mutex mutex_;
    mutable std::future<zoo::Expected<zoo::Response>> future_;
    bool consumed_ = false;
};

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

ConfiguredAgent configure_tools(ConfiguredAgent configured, const ToolProvider& tools) {
    configured.tool_definitions.reserve(tools.tools.size());

    for (const auto& tool : tools.tools) {
        auto register_result = configured.agent->register_tool(
            tool.definition.name, tool.definition.description, tool.definition.parameters_schema,
            [invoke = tool.invoke](const nlohmann::json& arguments) -> zoo::Expected<nlohmann::json> {
                auto result = invoke(arguments);
                if (!result) {
                    return std::unexpected(to_zoo_error(result.error()));
                }
                return *result;
            });
        if (!register_result) {
            throw std::runtime_error("Failed to register tool '" + tool.definition.name + "': " +
                                     register_result.error().to_string());
        }

        configured.tool_definitions.push_back(tool.definition);
    }

    if (!configured.tool_definitions.empty()) {
        configured.request_system_prompt =
            configured.agent->build_tool_system_prompt(configured.request_system_prompt);
        configured.agent->set_system_prompt(configured.request_system_prompt);
    }

    return configured;
}

Result<ConfiguredAgent> create_configured_agent(const zoo::Config& base_config,
                                                const ToolProvider& tools,
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

    ConfiguredAgent configured{
        .agent = std::move(*agent_result),
        .request_system_prompt = effective_prompt,
        .tool_definitions = {},
    };

    try {
        return configure_tools(std::move(configured), tools);
    } catch (const std::exception& error) {
        return std::unexpected(error.what());
    }
}

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

CompletionHandle wrap_zoo_handle(zoo::RequestHandle handle) {
    const auto request_id = static_cast<std::uint64_t>(handle.id);
    return CompletionHandle{
        request_id,
        std::make_shared<ZooCompletionSource>(std::move(handle.future)),
    };
}

} // namespace

Result<std::shared_ptr<ZooChatService>> ZooChatService::create(const ServerConfig& config) {
    return create(config, ToolProvider{});
}

Result<std::shared_ptr<ZooChatService>> ZooChatService::create(const ServerConfig& config,
                                                               ToolProvider tools) {
    auto shared_result = create_configured_agent(config.zoo_config, tools, std::nullopt);
    if (!shared_result) {
        return std::unexpected(shared_result.error());
    }

    std::shared_ptr<zoo::Agent> shared_agent = std::move(shared_result->agent);
    auto session_manager = std::make_unique<SessionManager>(
        config.model_id, config.sessions, shared_result->request_system_prompt,
        config.zoo_config.max_history_messages,
        [agent = shared_agent](std::vector<ChatMessage> messages, std::optional<TokenCallback> callback) {
            return wrap_zoo_handle(agent->complete(to_zoo_messages(messages), std::move(callback)));
        },
        [agent = shared_agent](std::uint64_t request_id) {
            agent->cancel(static_cast<zoo::RequestId>(request_id));
        });

    return std::make_shared<ZooChatService>(
        config.model_id, std::move(shared_result->request_system_prompt),
        std::move(shared_result->tool_definitions), std::move(shared_agent),
        std::move(session_manager));
}

ZooChatService::ZooChatService(std::string model_id, std::string request_system_prompt,
                               std::vector<ToolDefinition> tool_metadata,
                               std::shared_ptr<zoo::Agent> shared_agent,
                               std::unique_ptr<SessionManager> session_manager)
    : model_id_(std::move(model_id)), request_system_prompt_(std::move(request_system_prompt)),
      tool_metadata_(std::move(tool_metadata)), agent_(std::move(shared_agent)),
      session_manager_(std::move(session_manager)) {}

ZooChatService::~ZooChatService() = default;

bool ZooChatService::is_ready() const noexcept {
    return static_cast<bool>(agent_) && agent_->is_running();
}

const std::string& ZooChatService::model_id() const noexcept {
    return model_id_;
}

const std::vector<ToolDefinition>& ZooChatService::tools() const noexcept {
    return tool_metadata_;
}

SessionHealth ZooChatService::session_health() const noexcept {
    return session_manager_ ? session_manager_->health() : SessionHealth{};
}

ApiResult<PendingChatCompletion>
ZooChatService::start_completion(const ChatCompletionRequest& request,
                                 std::optional<TokenCallback> callback) {
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

    auto handle = wrap_zoo_handle(agent_->complete(to_zoo_messages(prepare_messages(request)),
                                                   std::move(callback)));
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
        [agent = agent_, request_id] {
            if (agent) {
                agent->cancel(static_cast<zoo::RequestId>(request_id));
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

void ZooChatService::reap_sessions() noexcept {
    if (session_manager_) {
        session_manager_->reap_expired_sessions();
    }
}

void ZooChatService::stop() {
    if (session_manager_) {
        session_manager_->stop();
    }
    if (agent_) {
        agent_->stop();
    }
}

std::vector<ChatMessage>
ZooChatService::prepare_messages(const ChatCompletionRequest& request) const {
    if (request_system_prompt_.empty()) {
        return request.messages; // copy only when needed (RVO applies)
    }

    std::vector<ChatMessage> messages;
    if (!request.messages.empty() && request.messages.front().role == MessageRole::System) {
        messages.reserve(request.messages.size());
        messages.push_back(ChatMessage::system(combine_system_prompts(
            request_system_prompt_, request.messages.front().content)));
        messages.insert(messages.end(), request.messages.begin() + 1, request.messages.end());
    } else {
        messages.reserve(request.messages.size() + 1);
        messages.push_back(ChatMessage::system(request_system_prompt_));
        messages.insert(messages.end(), request.messages.begin(), request.messages.end());
    }
    return messages;
}

} // namespace zks::server
