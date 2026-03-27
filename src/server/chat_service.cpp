#include "server/chat_service.hpp"

#include "server/internal_utils.hpp"
#include "server/zoo_adapter.hpp"

#include <future>
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

Result<ConfiguredAgent> configure_tools(ConfiguredAgent configured, const ToolProvider& tools) {
    configured.tool_definitions.reserve(tools.tools.size());

    for (const auto& tool : tools.tools) {
        auto register_result = configured.agent->register_tool(
            tool.definition.name, tool.definition.description, tool.definition.parameters_schema,
            [invoke =
                 tool.invoke](const nlohmann::json& arguments) -> zoo::Expected<nlohmann::json> {
                auto result = invoke(arguments);
                if (!result) {
                    return std::unexpected(result.error());
                }
                return *result;
            });
        if (!register_result) {
            return std::unexpected("Failed to register tool '" + tool.definition.name +
                                   "': " + register_result.error().to_string());
        }

        configured.tool_definitions.push_back(tool.definition);
    }

    return configured;
}

Result<ConfiguredAgent> create_configured_agent(const zoo::ModelConfig& model_config,
                                                const zoo::AgentConfig& agent_config,
                                                const zoo::GenerationOptions& gen_options,
                                                const std::string& system_prompt,
                                                const ToolProvider& tools) {
    auto agent_result = zoo::Agent::create(model_config, agent_config, gen_options);
    if (!agent_result) {
        return std::unexpected("Failed to create zoo::Agent: " + agent_result.error().to_string());
    }

    if (!system_prompt.empty()) {
        (*agent_result)->set_system_prompt(system_prompt);
    }

    ConfiguredAgent configured{
        .agent = std::move(*agent_result),
        .request_system_prompt = system_prompt,
        .tool_definitions = {},
    };

    return configure_tools(std::move(configured), tools);
}

CompletionHandle wrap_zoo_handle(zoo::RequestHandle<zoo::TextResponse> handle) {
    const auto request_id = static_cast<std::uint64_t>(handle.id());
    auto future = std::async(std::launch::async, [h = std::move(handle)]() mutable {
        auto result = h.await_result();
        if (!result) {
            return RuntimeResult<CompletionResult>{std::unexpected(result.error())};
        }
        return RuntimeResult<CompletionResult>{from_zoo_response(*result)};
    });
    return make_completion_handle(request_id, std::move(future));
}

} // namespace

Result<std::shared_ptr<ZooChatService>> ZooChatService::create(const ServerConfig& config) {
    return create(config, ToolProvider{});
}

Result<std::shared_ptr<ZooChatService>> ZooChatService::create(const ServerConfig& config,
                                                               ToolProvider tools) {
    auto gen_options = config.default_generation;
    if (!tools.tools.empty()) {
        gen_options.record_tool_trace = true;
    }

    auto shared_result = create_configured_agent(config.model_config, config.agent_config,
                                                 gen_options, config.system_prompt, tools);
    if (!shared_result) {
        return std::unexpected(shared_result.error());
    }

    auto shared_agent = std::move(shared_result->agent);
    auto session_store = std::make_unique<SessionStore>(config.model_id, config.sessions,
                                                        shared_result->request_system_prompt,
                                                        config.agent_config.max_history_messages);

    return std::make_shared<ZooChatService>(
        config.model_id, std::move(shared_result->request_system_prompt),
        std::move(shared_result->tool_definitions), std::move(shared_agent),
        std::move(session_store), gen_options);
}

ZooChatService::ZooChatService(std::string model_id, std::string request_system_prompt,
                               std::vector<ToolDefinition> tool_metadata,
                               std::shared_ptr<zoo::Agent> shared_agent,
                               std::unique_ptr<SessionStore> session_store,
                               zoo::GenerationOptions default_generation)
    : model_id_(std::move(model_id)), request_system_prompt_(std::move(request_system_prompt)),
      tool_metadata_(std::move(tool_metadata)), agent_(std::move(shared_agent)),
      default_generation_(std::move(default_generation)), session_store_(std::move(session_store)) {
}

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
    return session_store_ ? session_store_->health() : SessionHealth{};
}

ApiResult<PendingChatCompletion>
ZooChatService::start_completion(const ChatCompletionRequest& request,
                                 std::optional<TokenCallback> callback) {
    if (request.session_id.has_value()) {
        return start_session_completion(request, std::move(callback));
    }

    if (!is_ready()) {
        return std::unexpected(
            service_unavailable_error("Server runtime is not ready", "not_ready"));
    }
    if (request.model != model_id_) {
        return std::unexpected(
            invalid_request_error("Unknown model: " + request.model, "model", "invalid_model"));
    }

    auto gen_options = merge_request_overrides(default_generation_, request);
    auto messages = prepare_messages(request);
    auto view = zoo::ConversationView(std::span<const ChatMessage>(messages));
    zoo::AsyncTextCallback async_cb;
    if (callback) {
        async_cb = [cb = std::move(*callback)](std::string_view token) { cb(token); };
    }
    auto handle = wrap_zoo_handle(agent_->complete(view, gen_options, std::move(async_cb)));
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

ApiResult<PendingChatCompletion>
ZooChatService::start_session_completion(const ChatCompletionRequest& request,
                                         std::optional<TokenCallback> callback) {
    if (!is_ready()) {
        return std::unexpected(
            service_unavailable_error("Server runtime is not ready", "not_ready"));
    }

    // Use completion counter as the session request tracking ID.
    const auto tracking_id = next_completion_id_.fetch_add(1, std::memory_order_relaxed);

    auto begin = session_store_->begin_request(request, tracking_id);
    if (!begin) {
        return std::unexpected(begin.error());
    }

    auto gen_options = merge_request_overrides(default_generation_, request);
    zoo::ConversationView view(std::span<const ChatMessage>(begin->messages));
    zoo::AsyncTextCallback async_cb;
    if (callback) {
        async_cb = [cb = std::move(*callback)](std::string_view token) { cb(token); };
    }
    auto handle = wrap_zoo_handle(agent_->complete(view, gen_options, std::move(async_cb)));
    const auto zoo_request_id = handle.id;
    const auto completion_id = "chatcmpl-" + std::to_string(tracking_id);
    const auto session_id = *request.session_id;

    track_session_request(session_id, zoo_request_id);

    auto lease = std::make_shared<CompletionLease>([this, session_id, tracking_id] {
        untrack_session_request(session_id);
        session_store_->release_request(session_id, tracking_id);
    });

    return PendingChatCompletion{
        completion_id,
        begin->started_at,
        model_id_,
        std::move(handle),
        [this, session_id, tracking_id,
         user_message = begin->user_message](const RuntimeResult<CompletionResult>& result) {
            untrack_session_request(session_id);
            session_store_->commit_result(session_id, tracking_id, user_message, result);
        },
        [agent = agent_, zoo_request_id] {
            if (agent) {
                agent->cancel(static_cast<zoo::RequestId>(zoo_request_id));
            }
        },
        std::move(lease),
    };
}

ApiResult<SessionSummary> ZooChatService::create_session(const SessionCreateRequest& request) {
    if (!is_ready()) {
        return std::unexpected(
            service_unavailable_error("Server runtime is not ready", "agent_not_ready"));
    }
    return session_store_->create_session(request);
}

ApiResult<SessionSummary> ZooChatService::get_session(std::string_view session_id) {
    return session_store_->get_session(session_id);
}

ApiResult<void> ZooChatService::delete_session(std::string_view session_id) {
    auto result = session_store_->delete_session(session_id);
    if (!result) {
        return result;
    }

    // Cancel any in-flight inference for this session.
    std::uint64_t zoo_request_id = 0;
    {
        std::lock_guard<std::mutex> lock(active_requests_mutex_);
        auto it = active_session_requests_.find(std::string(session_id));
        if (it != active_session_requests_.end()) {
            zoo_request_id = it->second;
            active_session_requests_.erase(it);
        }
    }
    if (zoo_request_id != 0 && agent_) {
        agent_->cancel(static_cast<zoo::RequestId>(zoo_request_id));
    }

    return result;
}

void ZooChatService::reap_sessions() noexcept {
    if (session_store_) {
        session_store_->reap_expired_sessions();
    }
}

void ZooChatService::stop() {
    if (session_store_) {
        session_store_->stop();
    }
    if (agent_) {
        agent_->stop();
    }
}

std::vector<ChatMessage>
ZooChatService::prepare_messages(const ChatCompletionRequest& request) const {
    if (request_system_prompt_.empty()) {
        return request.messages;
    }

    std::vector<ChatMessage> messages;
    if (!request.messages.empty() && request.messages.front().role == MessageRole::System) {
        messages.reserve(request.messages.size());
        messages.push_back(ChatMessage::system(
            combine_system_prompts(request_system_prompt_, request.messages.front().content)));
        messages.insert(messages.end(), request.messages.begin() + 1, request.messages.end());
    } else {
        messages.reserve(request.messages.size() + 1);
        messages.push_back(ChatMessage::system(request_system_prompt_));
        messages.insert(messages.end(), request.messages.begin(), request.messages.end());
    }
    return messages;
}

void ZooChatService::track_session_request(const std::string& session_id,
                                           std::uint64_t zoo_request_id) {
    std::lock_guard<std::mutex> lock(active_requests_mutex_);
    active_session_requests_[session_id] = zoo_request_id;
}

void ZooChatService::untrack_session_request(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(active_requests_mutex_);
    active_session_requests_.erase(session_id);
}

} // namespace zks::server
