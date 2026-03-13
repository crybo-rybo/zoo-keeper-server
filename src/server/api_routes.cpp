#include "server/api_routes.hpp"

#include "server/api_json.hpp"
#include "server/runtime.hpp"
#include "server/streaming.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include <drogon/drogon.h>

namespace zks::server {
namespace {

void release_completion(const PendingChatCompletion& pending) {
    if (pending.lease) {
        pending.lease->release();
    }
}

void finalize_completion(const PendingChatCompletion& pending,
                         const zoo::Expected<zoo::Response>& result) {
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
                        const zoo::Expected<zoo::Response>& result) {
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

    void finish_success(const zoo::Response& response) {
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

drogon::HttpResponsePtr make_no_content_response() {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k204NoContent);
    return response;
}

void start_non_stream_completion(const std::shared_ptr<ServerRuntime>& runtime,
                                 const ChatCompletionRequest& request,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    auto pending = runtime->chat_service().start_completion(request);
    if (!pending) {
        callback(make_error_response(pending.error()));
        return;
    }
    log_request_start(*pending, request);

    auto result_status = pending->handle.future.wait_for(std::chrono::seconds(0));
    if (result_status == std::future_status::ready) {
        auto result = pending->handle.future.get();
        log_request_result(*pending, request, result);
        if (!result) {
            finalize_completion(*pending, result);
            callback(make_error_response(map_zoo_error_to_api_error(result.error())));
            return;
        }
        finalize_completion(*pending, result);
        callback(
            make_chat_completion_response(pending->id, pending->created, pending->model, *result));
        return;
    }

    runtime->spawn_background(
        [callback = std::move(callback), pending = std::move(*pending), request]() mutable {
            auto result = pending.handle.future.get();
            log_request_result(pending, request, result);
            if (!result) {
                finalize_completion(pending, result);
                callback(make_error_response(map_zoo_error_to_api_error(result.error())));
                return;
            }

            finalize_completion(pending, result);
            callback(
                make_chat_completion_response(pending.id, pending.created, pending.model, *result));
        });
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
        callback(make_error_response(pending.error()));
        return;
    }
    log_request_start(*pending, completion_request);

    session->set_metadata(pending->id, pending->created, pending->model);
    session->set_cancel_callback(pending->cancel);

    auto ready_status = pending->handle.future.wait_for(std::chrono::seconds(0));
    if (ready_status == std::future_status::ready) {
        auto result = pending->handle.future.get();
        log_request_result(*pending, completion_request, result);
        if (!result) {
            finalize_completion(*pending, result);
            callback(make_error_response(map_zoo_error_to_api_error(result.error())));
            return;
        }

        session->finish_success(*result);
        finalize_completion(*pending, result);
        callback(make_stream_response(session));
        return;
    }

    std::optional<DisconnectRegistry::CallbackId> callback_id;
    auto weak_connection = request->getConnectionPtr();
    if (auto connection = weak_connection.lock()) {
        callback_id = disconnect_registry->track(connection, [session, cancel = pending->cancel] {
            session->close();
            cancel();
        });
    }

    callback(make_stream_response(session));

    runtime->spawn_background([session, weak_connection, callback_id, disconnect_registry,
                               pending = std::move(*pending),
                               completion_request]() mutable {
        auto result = pending.handle.future.get();
        log_request_result(pending, completion_request, result);
        if (callback_id.has_value()) {
            if (auto connection = weak_connection.lock()) {
                disconnect_registry->clear(connection, *callback_id);
            }
        }

        if (!result) {
            session->finish_error(map_zoo_error_to_api_error(result.error()));
            finalize_completion(pending, result);
            return;
        }

        session->finish_success(*result);
        finalize_completion(pending, result);
    });
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

void DisconnectRegistry::clear(const trantor::TcpConnectionPtr& connection, CallbackId callback_id) {
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

void register_api_routes(const std::shared_ptr<ServerRuntime>& runtime,
                         const std::shared_ptr<DisconnectRegistry>& disconnect_registry) {
    std::weak_ptr<ServerRuntime> weak_runtime = runtime;
    drogon::app().registerHandler(
        "/v1/models",
        [weak_runtime](const drogon::HttpRequestPtr&,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            callback(make_models_response(runtime->chat_service().model_id()));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/v1/tools",
        [weak_runtime](const drogon::HttpRequestPtr&,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            callback(make_tools_response(runtime->chat_service().tools()));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/v1/sessions",
        [weak_runtime](const drogon::HttpRequestPtr& request,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            auto parsed_request = parse_session_create_request(request->body());
            if (!parsed_request) {
                callback(make_error_response(parsed_request.error()));
                return;
            }

            auto session = runtime->chat_service().create_session(*parsed_request);
            if (!session) {
                callback(make_error_response(session.error()));
                return;
            }

            callback(make_session_response(*session, drogon::k201Created));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/v1/sessions/{session-id}",
        [weak_runtime](const drogon::HttpRequestPtr&,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                       const std::string& session_id) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            auto session = runtime->chat_service().get_session(session_id);
            if (!session) {
                callback(make_error_response(session.error()));
                return;
            }

            callback(make_session_response(*session));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/v1/sessions/{session-id}",
        [weak_runtime](const drogon::HttpRequestPtr&,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                       const std::string& session_id) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            auto deleted = runtime->chat_service().delete_session(session_id);
            if (!deleted) {
                callback(make_error_response(deleted.error()));
                return;
            }

            callback(make_no_content_response());
        },
        {drogon::Delete});

    drogon::app().registerHandler(
        "/v1/chat/completions",
        [weak_runtime,
         disconnect_registry](const drogon::HttpRequestPtr& request,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_error_response(
                    service_unavailable_error("Server runtime is not ready", "not_ready")));
                return;
            }
            auto parsed_request = parse_chat_completion_request(request->body());
            if (!parsed_request) {
                callback(make_error_response(parsed_request.error()));
                return;
            }

            if (!parsed_request->stream) {
                start_non_stream_completion(runtime, *parsed_request, std::move(callback));
                return;
            }

            start_stream_completion(request, runtime, disconnect_registry, *parsed_request,
                                    std::move(callback));
        },
        {drogon::Post});
}

} // namespace zks::server
