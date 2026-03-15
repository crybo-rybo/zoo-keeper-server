#include "server/completion_controller.hpp"

#include "server/api_json.hpp"
#include "server/api_routes.hpp"
#include "server/auth.hpp"
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

// NOTE: with_metrics() is duplicated in api_routes.cpp. Both are in anonymous
// namespaces. A shared internal header would eliminate the duplication.
std::function<void(const drogon::HttpResponsePtr&)>
with_metrics(std::shared_ptr<ServerRuntime> runtime,
             std::function<void(const drogon::HttpResponsePtr&)> callback) {
    return [runtime = std::move(runtime), callback = std::move(callback)](
               const drogon::HttpResponsePtr& resp) {
        runtime->metrics().increment_requests();
        if (static_cast<int>(resp->getStatusCode()) >= 400) {
            runtime->metrics().increment_errors();
        }
        callback(resp);
    };
}

void release_completion(const PendingChatCompletion& pending) {
    if (pending.lease) {
        pending.lease->release();
    }
}

void finalize_completion(const PendingChatCompletion& pending,
                         const RuntimeResult<CompletionResult>& result) {
    if (pending.on_result) {
        pending.on_result(result);
    }
    release_completion(pending);
}

void log_request_start(const PendingChatCompletion& pending, const ChatCompletionRequest& request) {
    std::clog << "[request] event=start completion_id=" << pending.id << " model=" << pending.model
              << " stream=" << (request.stream ? "true" : "false");
    if (request.session_id.has_value()) {
        std::clog << " session_id=" << *request.session_id;
    }
    std::clog << '\n';
}

void log_request_result(const PendingChatCompletion& pending, const ChatCompletionRequest& request,
                        const RuntimeResult<CompletionResult>& result) {
    std::clog << "[request] event=finish completion_id=" << pending.id << " model=" << pending.model;
    if (request.session_id.has_value()) {
        std::clog << " session_id=" << *request.session_id;
    }

    if (!result) {
        std::clog << " status=error code=" << static_cast<int>(result.error().code)
                  << " message=\"" << result.error().message << "\"\n";
        return;
    }

    std::clog << " status=ok prompt_tokens=" << result->usage.prompt_tokens
              << " completion_tokens=" << result->usage.completion_tokens
              << " total_tokens=" << result->usage.total_tokens
              << " latency_ms=" << result->metrics.latency_ms.count() << '\n';
}

class StreamingSession {
  public:
    StreamingSession() = default;

    void set_metadata(std::string completion_id, std::int64_t created, std::string model_id) {
        std::vector<std::string> pending_tokens;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            completion_id_ = std::move(completion_id);
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

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!metadata_ready_) {
                pending_tokens_.emplace_back(token);
                return;
            }
        }

        push_frame(make_chat_completion_chunk(completion_id_, created_, model_id_, token,
                                              !sent_role_.load(std::memory_order_acquire),
                                              std::nullopt));
        sent_role_.store(true, std::memory_order_release);
        sent_content_.store(true, std::memory_order_release);
    }

    void finish_success(const CompletionResult& response) {
        if (!sent_role_.load(std::memory_order_acquire)) {
            if (response.text.empty()) {
                push_frame(make_chat_completion_chunk(completion_id_, created_, model_id_,
                                                      std::nullopt, true, std::nullopt));
            } else {
                push_frame(make_chat_completion_chunk(completion_id_, created_, model_id_,
                                                      response.text, true, std::nullopt));
                sent_content_.store(true, std::memory_order_release);
            }
            sent_role_.store(true, std::memory_order_release);
        } else if (!sent_content_.load(std::memory_order_acquire) && !response.text.empty()) {
            push_frame(make_chat_completion_chunk(completion_id_, created_, model_id_,
                                                  response.text, false, std::nullopt));
            sent_content_.store(true, std::memory_order_release);
        }

        push_frame(make_chat_completion_chunk(completion_id_, created_, model_id_, std::nullopt,
                                              false, "stop"));
        push_frame(make_sse_done());
        close();
    }

    void finish_error(const ApiError& error) {
        if (!sent_content_.load(std::memory_order_acquire) &&
            !sent_role_.load(std::memory_order_acquire)) {
            push_frame(make_sse_data(make_error_body(error)));
        }
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
    void push_frame(std::string frame) {
        std::function<void()> cancel_callback;
        bool should_close = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closing_) {
                return;
            }

            if (!stream_) {
                buffered_frames_.push_back(std::move(frame));
                return;
            }

            if (!stream_->send(frame)) {
                closing_ = true;
                should_close = true;
                cancel_callback = cancel_callback_;
            }
        }

        if (cancel_callback) {
            cancel_callback();
        }
        if (should_close) {
            close();
        }
    }

    std::mutex mutex_;
    std::string completion_id_;
    std::int64_t created_ = 0;
    std::string model_id_;
    bool metadata_ready_ = false;
    std::shared_ptr<drogon::ResponseStream> stream_;
    std::vector<std::string> buffered_frames_;
    std::vector<std::string> pending_tokens_;
    std::function<void()> cancel_callback_;
    bool closing_ = false;
    std::atomic<bool> sent_role_{false};
    std::atomic<bool> sent_content_{false};
};

drogon::HttpResponsePtr make_stream_response(const std::shared_ptr<StreamingSession>& session) {
    auto response = drogon::HttpResponse::newAsyncStreamResponse(
        [session](drogon::ResponseStreamPtr stream) { session->attach_stream(std::move(stream)); });
    response->setContentTypeCodeAndCustomString(drogon::CT_TEXT_PLAIN, "text/event-stream");
    response->addHeader("Cache-Control", "no-cache");
    response->addHeader("Connection", "keep-alive");
    response->addHeader("X-Accel-Buffering", "no");
    return response;
}

void increment_runtime_error_metrics(ServerMetrics& metrics, const RuntimeError& error) {
    if (error.code == RuntimeErrorCode::RequestCancelled) {
        metrics.increment_cancelled();
    } else if (error.code == RuntimeErrorCode::QueueFull) {
        metrics.increment_queue_rejected();
    }
}

void increment_tool_metrics(ServerMetrics& metrics, const CompletionResult& response) {
    for (const auto& invocation : response.tool_invocations) {
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

void start_non_stream_completion(const std::shared_ptr<ServerRuntime>& runtime,
                                 const ChatCompletionRequest& request,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    auto pending = runtime->chat_service().start_completion(request);
    if (!pending) {
        if (pending.error().code.has_value() && *pending.error().code == "queue_full") {
            runtime->metrics().increment_queue_rejected();
        }
        callback(make_error_response(pending.error()));
        return;
    }
    log_request_start(*pending, request);

    const auto result_status = pending->handle.wait_for(std::chrono::milliseconds(0));
    if (result_status == std::future_status::ready) {
        auto result = pending->handle.get();
        log_request_result(*pending, request, result);
        if (!result) {
            increment_runtime_error_metrics(runtime->metrics(), result.error());
            finalize_completion(*pending, result);
            callback(make_error_response(map_runtime_error_to_api_error(result.error())));
            return;
        }
        increment_tool_metrics(runtime->metrics(), *result);
        finalize_completion(*pending, result);
        callback(
            make_chat_completion_response(pending->id, pending->created, pending->model, *result));
        return;
    }

    // Capture only the session_id (for logging) rather than copying the entire
    // ChatCompletionRequest which includes the full message vector.
    auto submit_result = runtime->submit_background(
        [callback = std::move(callback), pending = std::move(*pending),
         session_id = request.session_id, is_stream = request.stream,
         runtime]() mutable {
            // Reconstruct a minimal request-like view for logging
            ChatCompletionRequest log_request;
            log_request.session_id = std::move(session_id);
            log_request.stream = is_stream;

            auto result = pending.handle.get();
            log_request_result(pending, log_request, result);
            if (!result) {
                increment_runtime_error_metrics(runtime->metrics(), result.error());
                finalize_completion(pending, result);
                callback(make_error_response(map_runtime_error_to_api_error(result.error())));
                return;
            }

            increment_tool_metrics(runtime->metrics(), *result);
            finalize_completion(pending, result);
            callback(
                make_chat_completion_response(pending.id, pending.created, pending.model, *result));
        });
    if (!submit_result) {
        pending->cancel();
        release_completion(*pending);
        callback(make_error_response(service_unavailable_error(
            "Server continuation executor is saturated", "server_busy")));
    }
}

void start_stream_completion(const drogon::HttpRequestPtr& request,
                             const std::shared_ptr<ServerRuntime>& runtime,
                             const std::shared_ptr<DisconnectRegistry>& disconnect_registry,
                             const ChatCompletionRequest& completion_request,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    auto session = std::make_shared<StreamingSession>();
    auto pending = runtime->chat_service().start_completion(
        completion_request, [session](std::string_view token) { session->push_token(token); });
    if (!pending) {
        if (pending.error().code.has_value() && *pending.error().code == "queue_full") {
            runtime->metrics().increment_queue_rejected();
        }
        callback(make_error_response(pending.error()));
        return;
    }
    log_request_start(*pending, completion_request);

    session->set_metadata(pending->id, pending->created, pending->model);
    session->set_cancel_callback(pending->cancel);

    const auto ready_status = pending->handle.wait_for(std::chrono::milliseconds(0));
    if (ready_status == std::future_status::ready) {
        auto result = pending->handle.get();
        log_request_result(*pending, completion_request, result);
        if (!result) {
            increment_runtime_error_metrics(runtime->metrics(), result.error());
            finalize_completion(*pending, result);
            callback(make_error_response(map_runtime_error_to_api_error(result.error())));
            return;
        }

        increment_tool_metrics(runtime->metrics(), *result);
        session->finish_success(*result);
        finalize_completion(*pending, result);
        callback(make_stream_response(session));
        return;
    }

    std::optional<DisconnectRegistry::CallbackId> callback_id;
    auto weak_connection = request->getConnectionPtr();
    if (auto connection = weak_connection.lock()) {
        callback_id = disconnect_registry->track(
            connection, [session, cancel = pending->cancel, runtime] {
                session->close();
                cancel();
                runtime->metrics().increment_stream_disconnects();
            });
    }

    // Capture only the fields needed for logging, not the full completion_request.
    auto submit_result = runtime->submit_background(
        [session, weak_connection, callback_id, disconnect_registry, pending = std::move(*pending),
         session_id = completion_request.session_id, is_stream = completion_request.stream,
         runtime]() mutable {
            ChatCompletionRequest log_request;
            log_request.session_id = std::move(session_id);
            log_request.stream = is_stream;

            auto result = pending.handle.get();
            log_request_result(pending, log_request, result);
            if (callback_id.has_value()) {
                if (auto connection = weak_connection.lock()) {
                    disconnect_registry->clear(connection, *callback_id);
                }
            }

            if (!result) {
                increment_runtime_error_metrics(runtime->metrics(), result.error());
                session->finish_error(map_runtime_error_to_api_error(result.error()));
                finalize_completion(pending, result);
                return;
            }

            increment_tool_metrics(runtime->metrics(), *result);
            session->finish_success(*result);
            finalize_completion(pending, result);
        });
    if (!submit_result) {
        if (callback_id.has_value()) {
            if (auto connection = weak_connection.lock()) {
                disconnect_registry->clear(connection, *callback_id);
            }
        }
        pending->cancel();
        release_completion(*pending);
        callback(make_error_response(service_unavailable_error(
            "Server continuation executor is saturated", "server_busy")));
        return;
    }

    callback(make_stream_response(session));
}

} // namespace

void register_chat_completion_route(
    drogon::HttpAppFramework& app, const std::shared_ptr<ServerRuntime>& runtime,
    const std::shared_ptr<DisconnectRegistry>& disconnect_registry) {
    std::weak_ptr<ServerRuntime> weak_runtime = runtime;
    app.registerHandler(
        "/v1/chat/completions",
        [weak_runtime, disconnect_registry](const drogon::HttpRequestPtr& request,
                                            std::function<void(const drogon::HttpResponsePtr&)>&&
                                                callback) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            if (auto auth_err = check_auth(request, runtime->config()); auth_err.has_value()) {
                with_metrics(runtime, std::move(callback))(make_error_response(*auth_err));
                return;
            }
            auto cb = with_metrics(runtime, std::move(callback));
            auto parsed_request = parse_chat_completion_request(request->body());
            if (!parsed_request) {
                cb(make_error_response(parsed_request.error()));
                return;
            }

            if (!parsed_request->stream) {
                start_non_stream_completion(runtime, *parsed_request, std::move(cb));
                return;
            }

            start_stream_completion(request, runtime, disconnect_registry, *parsed_request,
                                    std::move(cb));
        },
        {drogon::Post});
}

} // namespace zks::server
