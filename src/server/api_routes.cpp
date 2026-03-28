#include "server/api_routes.hpp"

#include "server/api_json.hpp"
#include "server/auth.hpp"
#include "server/completion_controller.hpp"
#include "server/route_utils.hpp"
#include "server/runtime.hpp"
#include "server/streaming.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <drogon/drogon.h>

namespace zks::server {
namespace {

nlohmann::json make_tool_invocation_body(const ToolInvocationRecord& invocation) {
    nlohmann::json body = {{"id", invocation.id},
                           {"name", invocation.name},
                           {"status", zoo::to_string(invocation.status)}};
    try {
        body["arguments"] = nlohmann::json::parse(invocation.arguments_json);
    } catch (...) {
        body["arguments"] = invocation.arguments_json;
    }
    if (invocation.result_json.has_value()) {
        try {
            body["result"] = nlohmann::json::parse(*invocation.result_json);
        } catch (...) {
            body["result"] = *invocation.result_json;
        }
    }
    if (invocation.error.has_value()) {
        body["error"] = invocation.error->message;
    }
    return body;
}

nlohmann::json make_message_body(const ChatMessage& message) {
    nlohmann::json body{{"role", to_string(message.role)}, {"content", message.content}};
    if (!message.tool_call_id.empty()) {
        body["tool_call_id"] = message.tool_call_id;
    }
    return body;
}

nlohmann::json make_agent_history_body(const AgentHistorySnapshot& history) {
    nlohmann::json messages = nlohmann::json::array();
    for (const auto& message : history.messages) {
        messages.push_back(make_message_body(message));
    }
    return {{"messages", std::move(messages)},
            {"estimated_tokens", history.estimated_tokens},
            {"context_size", history.context_size},
            {"context_exceeded", history.context_exceeded}};
}

drogon::HttpResponsePtr make_no_content_response() {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k204NoContent);
    return response;
}

drogon::HttpResponsePtr make_accepted_response(const nlohmann::json& body = {}) {
    return make_json_response(body, drogon::k202Accepted);
}

nlohmann::json make_extraction_body(std::string_view extraction_id, std::int64_t created,
                                    std::string_view model_id, const ExtractionResult& response) {
    nlohmann::json tool_invocations = nlohmann::json::array();
    for (const auto& invocation : response.tool_invocations) {
        tool_invocations.push_back(make_tool_invocation_body(invocation));
    }

    return {{"id", extraction_id},
            {"object", "extraction"},
            {"created", created},
            {"model", model_id},
            {"text", response.text},
            {"data", response.data},
            {"usage",
             {{"prompt_tokens", response.usage.prompt_tokens},
              {"completion_tokens", response.usage.completion_tokens},
              {"total_tokens", response.usage.total_tokens}}},
            {"zoo_metrics",
             {{"latency_ms", response.metrics.latency_ms.count()},
              {"time_to_first_token_ms", response.metrics.time_to_first_token_ms.count()},
              {"tokens_per_second", response.metrics.tokens_per_second}}},
            {"tool_invocations", std::move(tool_invocations)}};
}

nlohmann::json make_runtime_body(const ServerRuntime& runtime) {
    const auto& chat_service = runtime.chat_service();
    nlohmann::json tools = nlohmann::json::array();
    for (const auto& tool : chat_service.tools()) {
        tools.push_back({{"type", "function"},
                         {"function",
                          {{"name", tool.name},
                           {"description", tool.description},
                           {"parameters", tool.parameters_schema}}}});
    }

    nlohmann::json body;
    body["ready"] = runtime.is_ready();
    body["model_id"] = chat_service.model_id();
    body["model_config"] = {{"model_path", chat_service.model_config().model_path},
                            {"context_size", chat_service.model_config().context_size},
                            {"n_gpu_layers", chat_service.model_config().n_gpu_layers},
                            {"use_mmap", chat_service.model_config().use_mmap},
                            {"use_mlock", chat_service.model_config().use_mlock}};
    body["agent_config"] = {
        {"max_history_messages", chat_service.agent_config().max_history_messages},
        {"request_queue_capacity", chat_service.agent_config().request_queue_capacity},
        {"max_tool_iterations", chat_service.agent_config().max_tool_iterations},
        {"max_tool_retries", chat_service.agent_config().max_tool_retries}};
    body["default_generation"] = {
        {"max_tokens", chat_service.default_generation().max_tokens},
        {"stop_sequences", chat_service.default_generation().stop_sequences},
        {"record_tool_trace", chat_service.default_generation().record_tool_trace},
        {"sampling",
         {{"temperature", chat_service.default_generation().sampling.temperature},
          {"top_p", chat_service.default_generation().sampling.top_p},
          {"top_k", chat_service.default_generation().sampling.top_k},
          {"repeat_penalty", chat_service.default_generation().sampling.repeat_penalty},
          {"repeat_last_n", chat_service.default_generation().sampling.repeat_last_n},
          {"seed", chat_service.default_generation().sampling.seed}}}};
    body["tool_count"] = chat_service.tools().size();
    body["tools"] = std::move(tools);
    body["sessions"] = {{"enabled", runtime.config().sessions.enabled()},
                        {"max_sessions", runtime.config().sessions.max_sessions},
                        {"idle_ttl_seconds", runtime.config().sessions.idle_ttl_seconds},
                        {"active", runtime.chat_service().session_health().active}};
    return body;
}

void release_extraction(const PendingExtraction& pending) {
    if (pending.lease) {
        pending.lease->release();
    }
}

void finalize_extraction(const PendingExtraction& pending,
                         const RuntimeResult<ExtractionResult>& result) {
    if (pending.on_result) {
        pending.on_result(result);
    }
    release_extraction(pending);
}

void increment_runtime_error_metrics(ServerMetrics& metrics, const RuntimeError& error) {
    if (error.code == RuntimeErrorCode::RequestCancelled) {
        metrics.increment_cancelled();
    } else if (error.code == RuntimeErrorCode::QueueFull) {
        metrics.increment_queue_rejected();
    }
}

void increment_tool_metrics(ServerMetrics& metrics,
                            const std::vector<ToolInvocationRecord>& tool_invocations) {
    for (const auto& invocation : tool_invocations) {
        metrics.increment_tool_invocations();
        if (invocation.status == ToolInvocationStatus::Succeeded) {
            continue;
        }

        metrics.increment_tool_failures();
        if (invocation.error.has_value() &&
            invocation.error->context == std::optional<std::string>{"timeout"}) {
            metrics.increment_tool_timeouts();
        }
    }
}

class ExtractionStreamingSession {
  public:
    void set_metadata(std::string extraction_id, std::int64_t created, std::string model_id) {
        std::vector<std::string> pending_tokens;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            extraction_id_ = std::move(extraction_id);
            created_ = created;
            model_id_ = std::move(model_id);
            metadata_ready_ = true;
            pending_tokens.swap(pending_tokens_);
        }

        for (const auto& token : pending_tokens) {
            push_token(token);
        }
    }

    void set_cancel_callback(std::function<void()> callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        cancel_callback_ = std::move(callback);
    }

    void attach_stream(drogon::ResponseStreamPtr stream) {
        auto shared_stream = std::shared_ptr<drogon::ResponseStream>(std::move(stream));
        std::function<void()> cancel_callback;
        bool should_close = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            stream_ = shared_stream;
            for (const auto& frame : buffered_frames_) {
                if (!stream_->send(frame)) {
                    closing_ = true;
                    should_close = true;
                    cancel_callback = cancel_callback_;
                    break;
                }
            }
            buffered_frames_.clear();
            should_close = should_close || closing_;
        }

        if (cancel_callback) {
            cancel_callback();
        }
        if (should_close) {
            shared_stream->close();
        }
    }

    void push_token(std::string_view token) {
        if (token.empty()) {
            return;
        }

        DeferredActions deferred;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!metadata_ready_) {
                pending_tokens_.emplace_back(token);
                return;
            }
            deferred = push_frame_locked(make_sse_data({{"delta", token}}));
        }
        execute_deferred(deferred);
        if (deferred.should_close) {
            close();
        }
    }

    void finish_success(const ExtractionResult& response) {
        std::vector<DeferredActions> deferred_actions;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            deferred_actions.push_back(push_frame_locked(make_sse_data(
                make_extraction_body(extraction_id_, created_, model_id_, response))));
            deferred_actions.push_back(push_frame_locked(make_sse_done()));
        }

        for (const auto& deferred : deferred_actions) {
            execute_deferred(deferred);
        }
        close();
    }

    void finish_error(const ApiError& error) {
        DeferredActions deferred;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            deferred = push_frame_locked(make_sse_data(make_error_body(error)));
        }
        execute_deferred(deferred);
        close();
    }

    void close() {
        std::shared_ptr<drogon::ResponseStream> stream;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closing_ && !stream_) {
                return;
            }
            closing_ = true;
            stream = std::move(stream_);
            if (stream) {
                buffered_frames_.clear();
            }
        }

        if (stream) {
            stream->close();
        }
    }

  private:
    struct DeferredActions {
        bool should_close = false;
        std::function<void()> cancel_cb;
    };

    DeferredActions push_frame_locked(std::string frame) {
        DeferredActions actions;
        if (closing_) {
            return actions;
        }
        if (!stream_) {
            buffered_frames_.push_back(std::move(frame));
            return actions;
        }
        if (!stream_->send(frame)) {
            closing_ = true;
            actions.should_close = true;
            actions.cancel_cb = cancel_callback_;
        }
        return actions;
    }

    static void execute_deferred(const DeferredActions& actions) {
        if (actions.cancel_cb) {
            actions.cancel_cb();
        }
    }

    std::mutex mutex_;
    std::string extraction_id_;
    std::int64_t created_ = 0;
    std::string model_id_;
    bool metadata_ready_ = false;
    std::shared_ptr<drogon::ResponseStream> stream_;
    std::vector<std::string> buffered_frames_;
    std::vector<std::string> pending_tokens_;
    std::function<void()> cancel_callback_;
    bool closing_ = false;
};

drogon::HttpResponsePtr
make_stream_response(const std::shared_ptr<ExtractionStreamingSession>& session) {
    auto response = drogon::HttpResponse::newAsyncStreamResponse(
        [session](drogon::ResponseStreamPtr stream) { session->attach_stream(std::move(stream)); });
    response->setContentTypeCodeAndCustomString(drogon::CT_TEXT_PLAIN, "text/event-stream");
    response->addHeader("Cache-Control", "no-cache");
    response->addHeader("Connection", "keep-alive");
    response->addHeader("X-Accel-Buffering", "no");
    return response;
}

void start_non_stream_extraction(const std::shared_ptr<ServerRuntime>& runtime,
                                 const ExtractionRequest& request,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    auto pending = runtime->chat_service().start_extraction(request);
    if (!pending) {
        if (pending.error().code.has_value() && *pending.error().code == "queue_full") {
            runtime->metrics().increment_queue_rejected();
        }
        callback(make_error_response(pending.error()));
        return;
    }

    const auto result_status = pending->handle.wait_for(std::chrono::milliseconds(0));
    if (result_status == std::future_status::ready) {
        auto result = pending->handle.get();
        if (!result) {
            increment_runtime_error_metrics(runtime->metrics(), result.error());
            finalize_extraction(*pending, result);
            callback(make_error_response(map_runtime_error_to_api_error(result.error())));
            return;
        }
        increment_tool_metrics(runtime->metrics(), result->tool_invocations);
        finalize_extraction(*pending, result);
        callback(make_extraction_response(pending->id, pending->created, pending->model, *result));
        return;
    }

    auto submit_result = runtime->submit_background(
        [callback = std::move(callback), pending = std::move(*pending), runtime]() mutable {
            auto result = pending.handle.get();
            if (!result) {
                increment_runtime_error_metrics(runtime->metrics(), result.error());
                finalize_extraction(pending, result);
                callback(make_error_response(map_runtime_error_to_api_error(result.error())));
                return;
            }

            increment_tool_metrics(runtime->metrics(), result->tool_invocations);
            finalize_extraction(pending, result);
            callback(make_extraction_response(pending.id, pending.created, pending.model, *result));
        });
    if (!submit_result) {
        pending->cancel();
        release_extraction(*pending);
        callback(make_error_response(
            service_unavailable_error("Server continuation executor is saturated", "server_busy")));
    }
}

void start_stream_extraction(const drogon::HttpRequestPtr& request,
                             const std::shared_ptr<ServerRuntime>& runtime,
                             const std::shared_ptr<DisconnectRegistry>& disconnect_registry,
                             const ExtractionRequest& extraction_request,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    auto session = std::make_shared<ExtractionStreamingSession>();
    auto pending = runtime->chat_service().start_extraction(
        extraction_request, [session](std::string_view token) { session->push_token(token); });
    if (!pending) {
        if (pending.error().code.has_value() && *pending.error().code == "queue_full") {
            runtime->metrics().increment_queue_rejected();
        }
        callback(make_error_response(pending.error()));
        return;
    }

    session->set_metadata(pending->id, pending->created, pending->model);
    session->set_cancel_callback(pending->cancel);

    const auto ready_status = pending->handle.wait_for(std::chrono::milliseconds(0));
    if (ready_status == std::future_status::ready) {
        auto result = pending->handle.get();
        if (!result) {
            increment_runtime_error_metrics(runtime->metrics(), result.error());
            finalize_extraction(*pending, result);
            callback(make_error_response(map_runtime_error_to_api_error(result.error())));
            return;
        }

        increment_tool_metrics(runtime->metrics(), result->tool_invocations);
        session->finish_success(*result);
        finalize_extraction(*pending, result);
        callback(make_stream_response(session));
        return;
    }

    std::optional<DisconnectRegistry::CallbackId> callback_id;
    auto weak_connection = request->getConnectionPtr();
    if (auto connection = weak_connection.lock()) {
        callback_id =
            disconnect_registry->track(connection, [session, cancel = pending->cancel, runtime] {
                session->close();
                cancel();
                runtime->metrics().increment_stream_disconnects();
            });
    }

    auto submit_result =
        runtime->submit_background([session, weak_connection, callback_id, disconnect_registry,
                                    pending = std::move(*pending), runtime]() mutable {
            auto result = pending.handle.get();
            if (callback_id.has_value()) {
                if (auto connection = weak_connection.lock()) {
                    disconnect_registry->clear(connection, *callback_id);
                }
            }

            if (!result) {
                increment_runtime_error_metrics(runtime->metrics(), result.error());
                session->finish_error(map_runtime_error_to_api_error(result.error()));
                finalize_extraction(pending, result);
                return;
            }

            increment_tool_metrics(runtime->metrics(), result->tool_invocations);
            session->finish_success(*result);
            finalize_extraction(pending, result);
        });
    if (!submit_result) {
        if (callback_id.has_value()) {
            if (auto connection = weak_connection.lock()) {
                disconnect_registry->clear(connection, *callback_id);
            }
        }
        pending->cancel();
        release_extraction(*pending);
        callback(make_error_response(
            service_unavailable_error("Server continuation executor is saturated", "server_busy")));
        return;
    }

    callback(make_stream_response(session));
}

void start_agent_chat(const std::shared_ptr<ServerRuntime>& runtime,
                      const AgentChatRequest& request,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    auto pending = runtime->chat_service().start_agent_chat(request);
    if (!pending) {
        if (pending.error().code.has_value() && *pending.error().code == "queue_full") {
            runtime->metrics().increment_queue_rejected();
        }
        callback(make_error_response(pending.error()));
        return;
    }

    const auto result_status = pending->handle.wait_for(std::chrono::milliseconds(0));
    if (result_status == std::future_status::ready) {
        auto result = pending->handle.get();
        if (!result) {
            increment_runtime_error_metrics(runtime->metrics(), result.error());
            if (pending->on_result) {
                pending->on_result(result);
            }
            if (pending->lease) {
                pending->lease->release();
            }
            callback(make_error_response(map_runtime_error_to_api_error(result.error())));
            return;
        }
        increment_tool_metrics(runtime->metrics(), result->tool_invocations);
        if (pending->on_result) {
            pending->on_result(result);
        }
        if (pending->lease) {
            pending->lease->release();
        }
        callback(
            make_chat_completion_response(pending->id, pending->created, pending->model, *result));
        return;
    }

    auto submit_result = runtime->submit_background(
        [callback = std::move(callback), pending = std::move(*pending), runtime]() mutable {
            auto result = pending.handle.get();
            if (!result) {
                increment_runtime_error_metrics(runtime->metrics(), result.error());
                if (pending.on_result) {
                    pending.on_result(result);
                }
                if (pending.lease) {
                    pending.lease->release();
                }
                callback(make_error_response(map_runtime_error_to_api_error(result.error())));
                return;
            }

            increment_tool_metrics(runtime->metrics(), result->tool_invocations);
            if (pending.on_result) {
                pending.on_result(result);
            }
            if (pending.lease) {
                pending.lease->release();
            }
            callback(
                make_chat_completion_response(pending.id, pending.created, pending.model, *result));
        });
    if (!submit_result) {
        pending->cancel();
        if (pending->lease) {
            pending->lease->release();
        }
        callback(make_error_response(
            service_unavailable_error("Server continuation executor is saturated", "server_busy")));
    }
}

} // namespace

DisconnectRegistry::CallbackId
DisconnectRegistry::track(const trantor::TcpConnectionPtr& connection,
                          std::function<void()> on_disconnect) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto callback_id = next_callback_id_++;
    callbacks_[connection].emplace(callback_id, std::move(on_disconnect));
    return callback_id;
}

void DisconnectRegistry::clear(const trantor::TcpConnectionPtr& connection,
                               CallbackId callback_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = callbacks_.find(connection);
    if (it == callbacks_.end()) {
        return;
    }

    it->second.erase(callback_id);
    if (it->second.empty()) {
        callbacks_.erase(it);
    }
}

void DisconnectRegistry::handle_connection_event(const trantor::TcpConnectionPtr& connection) {
    if (!connection || !connection->disconnected()) {
        return;
    }

    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = callbacks_.find(connection);
        if (it == callbacks_.end()) {
            return;
        }

        callbacks.reserve(it->second.size());
        for (auto& [callback_id, callback] : it->second) {
            callbacks.push_back(std::move(callback));
        }
        callbacks_.erase(it);
    }

    for (auto& callback : callbacks) {
        if (callback) {
            callback();
        }
    }
}

void register_api_routes(drogon::HttpAppFramework& app,
                         const std::shared_ptr<ServerRuntime>& runtime,
                         const std::shared_ptr<DisconnectRegistry>& disconnect_registry) {
    std::weak_ptr<ServerRuntime> weak_runtime = runtime;

    if (!runtime->config().http.cors_allow_origins.empty()) {
        app.registerHandler(
            "/{path}",
            [](const drogon::HttpRequestPtr&,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string&) {
                auto response = make_no_content_response();
                response->addHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
                response->addHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
                response->addHeader("Access-Control-Max-Age", "86400");
                callback(response);
            },
            {drogon::Options});
    }
    app.registerHandler(
        "/v1/models",
        [weak_runtime](const drogon::HttpRequestPtr& request,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            if (auto auth_err = check_auth(request, runtime->config()); auth_err.has_value()) {
                with_metrics(runtime->metrics(),
                             std::move(callback))(make_error_response(*auth_err));
                return;
            }
            with_metrics(runtime->metrics(), std::move(callback))(
                make_models_response(runtime->chat_service().model_id()));
        },
        {drogon::Get});

    app.registerHandler(
        "/v1/tools",
        [weak_runtime](const drogon::HttpRequestPtr& request,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            if (auto auth_err = check_auth(request, runtime->config()); auth_err.has_value()) {
                with_metrics(runtime->metrics(),
                             std::move(callback))(make_error_response(*auth_err));
                return;
            }
            with_metrics(runtime->metrics(),
                         std::move(callback))(make_tools_response(runtime->chat_service().tools()));
        },
        {drogon::Get});

    app.registerHandler(
        "/v1/sessions",
        [weak_runtime](const drogon::HttpRequestPtr& request,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            if (auto auth_err = check_auth(request, runtime->config()); auth_err.has_value()) {
                with_metrics(runtime->metrics(),
                             std::move(callback))(make_error_response(*auth_err));
                return;
            }
            auto cb = with_metrics(runtime->metrics(), std::move(callback));
            auto parsed_request = parse_session_create_request(request->body());
            if (!parsed_request) {
                cb(make_error_response(parsed_request.error()));
                return;
            }

            auto session = runtime->chat_service().create_session(*parsed_request);
            if (!session) {
                cb(make_error_response(session.error()));
                return;
            }

            cb(make_session_response(*session, drogon::k201Created));
        },
        {drogon::Post});

    app.registerHandler(
        "/v1/sessions/{session-id}",
        [weak_runtime](const drogon::HttpRequestPtr& request,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                       const std::string& session_id) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            if (auto auth_err = check_auth(request, runtime->config()); auth_err.has_value()) {
                with_metrics(runtime->metrics(),
                             std::move(callback))(make_error_response(*auth_err));
                return;
            }
            auto cb = with_metrics(runtime->metrics(), std::move(callback));
            auto session = runtime->chat_service().get_session(session_id);
            if (!session) {
                cb(make_error_response(session.error()));
                return;
            }

            cb(make_session_response(*session));
        },
        {drogon::Get});

    app.registerHandler(
        "/v1/sessions/{session-id}",
        [weak_runtime](const drogon::HttpRequestPtr& request,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                       const std::string& session_id) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            if (auto auth_err = check_auth(request, runtime->config()); auth_err.has_value()) {
                with_metrics(runtime->metrics(),
                             std::move(callback))(make_error_response(*auth_err));
                return;
            }
            auto cb = with_metrics(runtime->metrics(), std::move(callback));
            auto deleted = runtime->chat_service().delete_session(session_id);
            if (!deleted) {
                cb(make_error_response(deleted.error()));
                return;
            }

            cb(make_no_content_response());
        },
        {drogon::Delete});

    register_chat_completion_route(app, runtime, disconnect_registry);

    app.registerHandler(
        "/v1/extractions",
        [weak_runtime,
         disconnect_registry](const drogon::HttpRequestPtr& request,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            if (auto auth_err = check_auth(request, runtime->config()); auth_err.has_value()) {
                with_metrics(runtime->metrics(),
                             std::move(callback))(make_error_response(*auth_err));
                return;
            }
            auto cb = with_metrics(runtime->metrics(), std::move(callback));
            auto parsed_request = parse_extraction_request(request->body());
            if (!parsed_request) {
                cb(make_error_response(parsed_request.error()));
                return;
            }
            if (!parsed_request->stream) {
                start_non_stream_extraction(runtime, *parsed_request, std::move(cb));
                return;
            }

            start_stream_extraction(request, runtime, disconnect_registry, *parsed_request,
                                    std::move(cb));
        },
        {drogon::Post});

    app.registerHandler(
        "/v1/agent/chat",
        [weak_runtime](const drogon::HttpRequestPtr& request,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            if (auto auth_err = check_auth(request, runtime->config()); auth_err.has_value()) {
                with_metrics(runtime->metrics(),
                             std::move(callback))(make_error_response(*auth_err));
                return;
            }
            auto cb = with_metrics(runtime->metrics(), std::move(callback));
            auto parsed_request = parse_agent_chat_request(request->body());
            if (!parsed_request) {
                cb(make_error_response(parsed_request.error()));
                return;
            }
            start_agent_chat(runtime, *parsed_request, std::move(cb));
        },
        {drogon::Post});

    app.registerHandler(
        "/v1/requests/{request-id}/cancel",
        [weak_runtime](const drogon::HttpRequestPtr& request,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                       const std::string& request_id) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            if (auto auth_err = check_auth(request, runtime->config()); auth_err.has_value()) {
                with_metrics(runtime->metrics(),
                             std::move(callback))(make_error_response(*auth_err));
                return;
            }
            auto cb = with_metrics(runtime->metrics(), std::move(callback));
            auto cancelled = runtime->chat_service().cancel_request(request_id);
            if (!cancelled) {
                cb(make_error_response(cancelled.error()));
                return;
            }
            cb(make_accepted_response({{"id", request_id}, {"cancelled", true}}));
        },
        {drogon::Post});

    app.registerHandler(
        "/v1/agent/history",
        [weak_runtime](const drogon::HttpRequestPtr& request,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            if (auto auth_err = check_auth(request, runtime->config()); auth_err.has_value()) {
                with_metrics(runtime->metrics(),
                             std::move(callback))(make_error_response(*auth_err));
                return;
            }
            auto cb = with_metrics(runtime->metrics(), std::move(callback));
            auto history = runtime->chat_service().get_agent_history();
            if (!history) {
                cb(make_error_response(history.error()));
                return;
            }
            cb(make_json_response(make_agent_history_body(*history)));
        },
        {drogon::Get});

    app.registerHandler(
        "/v1/agent/history",
        [weak_runtime](const drogon::HttpRequestPtr& request,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            if (auto auth_err = check_auth(request, runtime->config()); auth_err.has_value()) {
                with_metrics(runtime->metrics(),
                             std::move(callback))(make_error_response(*auth_err));
                return;
            }
            auto cb = with_metrics(runtime->metrics(), std::move(callback));
            auto parsed_request = parse_agent_history_request(request->body());
            if (!parsed_request) {
                cb(make_error_response(parsed_request.error()));
                return;
            }
            auto history = runtime->chat_service().replace_agent_history(*parsed_request);
            if (!history) {
                cb(make_error_response(history.error()));
                return;
            }
            cb(make_json_response(make_agent_history_body(*history)));
        },
        {drogon::Put});

    app.registerHandler(
        "/v1/agent/history",
        [weak_runtime](const drogon::HttpRequestPtr& request,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            if (auto auth_err = check_auth(request, runtime->config()); auth_err.has_value()) {
                with_metrics(runtime->metrics(),
                             std::move(callback))(make_error_response(*auth_err));
                return;
            }
            auto cb = with_metrics(runtime->metrics(), std::move(callback));
            auto cleared = runtime->chat_service().clear_agent_history();
            if (!cleared) {
                cb(make_error_response(cleared.error()));
                return;
            }
            cb(make_no_content_response());
        },
        {drogon::Delete});

    app.registerHandler(
        "/v1/agent/history:swap",
        [weak_runtime](const drogon::HttpRequestPtr& request,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            if (auto auth_err = check_auth(request, runtime->config()); auth_err.has_value()) {
                with_metrics(runtime->metrics(),
                             std::move(callback))(make_error_response(*auth_err));
                return;
            }
            auto cb = with_metrics(runtime->metrics(), std::move(callback));
            auto parsed_request = parse_agent_history_request(request->body());
            if (!parsed_request) {
                cb(make_error_response(parsed_request.error()));
                return;
            }
            auto history = runtime->chat_service().swap_agent_history(*parsed_request);
            if (!history) {
                cb(make_error_response(history.error()));
                return;
            }
            cb(make_json_response(make_agent_history_body(*history)));
        },
        {drogon::Post});

    app.registerHandler(
        "/v1/agent/history/messages",
        [weak_runtime](const drogon::HttpRequestPtr& request,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            if (auto auth_err = check_auth(request, runtime->config()); auth_err.has_value()) {
                with_metrics(runtime->metrics(),
                             std::move(callback))(make_error_response(*auth_err));
                return;
            }
            auto cb = with_metrics(runtime->metrics(), std::move(callback));
            auto parsed_request = parse_agent_history_message_request(request->body());
            if (!parsed_request) {
                cb(make_error_response(parsed_request.error()));
                return;
            }
            auto history = runtime->chat_service().append_agent_history_message(*parsed_request);
            if (!history) {
                cb(make_error_response(history.error()));
                return;
            }
            cb(make_json_response(make_agent_history_body(*history)));
        },
        {drogon::Post});

    app.registerHandler(
        "/v1/agent/system-prompt",
        [weak_runtime](const drogon::HttpRequestPtr& request,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            if (auto auth_err = check_auth(request, runtime->config()); auth_err.has_value()) {
                with_metrics(runtime->metrics(),
                             std::move(callback))(make_error_response(*auth_err));
                return;
            }
            auto cb = with_metrics(runtime->metrics(), std::move(callback));
            auto prompt = runtime->chat_service().get_system_prompt();
            if (!prompt) {
                cb(make_error_response(prompt.error()));
                return;
            }
            cb(make_json_response({{"system_prompt", *prompt}}));
        },
        {drogon::Get});

    app.registerHandler(
        "/v1/agent/system-prompt",
        [weak_runtime](const drogon::HttpRequestPtr& request,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            if (auto auth_err = check_auth(request, runtime->config()); auth_err.has_value()) {
                with_metrics(runtime->metrics(),
                             std::move(callback))(make_error_response(*auth_err));
                return;
            }
            auto cb = with_metrics(runtime->metrics(), std::move(callback));
            auto parsed_request = parse_system_prompt_request(request->body());
            if (!parsed_request) {
                cb(make_error_response(parsed_request.error()));
                return;
            }
            auto prompt = runtime->chat_service().set_system_prompt(parsed_request->system_prompt);
            if (!prompt) {
                cb(make_error_response(prompt.error()));
                return;
            }
            cb(make_json_response({{"system_prompt", *prompt}}));
        },
        {drogon::Put});

    app.registerHandler(
        "/v1/runtime",
        [weak_runtime](const drogon::HttpRequestPtr& request,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            if (auto auth_err = check_auth(request, runtime->config()); auth_err.has_value()) {
                with_metrics(runtime->metrics(),
                             std::move(callback))(make_error_response(*auth_err));
                return;
            }
            with_metrics(runtime->metrics(),
                         std::move(callback))(make_json_response(make_runtime_body(*runtime)));
        },
        {drogon::Get});

    app.registerHandler(
        "/metrics",
        [weak_runtime](const drogon::HttpRequestPtr& request,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            if (auto auth_err = check_auth(request, runtime->config()); auth_err.has_value()) {
                with_metrics(runtime->metrics(),
                             std::move(callback))(make_error_response(*auth_err));
                return;
            }
            with_metrics(runtime->metrics(), std::move(callback))(
                make_json_response(runtime->metrics_snapshot().to_json()));
        },
        {drogon::Get});
}

} // namespace zks::server
