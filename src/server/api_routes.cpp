#include "server/api_routes.hpp"

#include "server/api_json.hpp"
#include "server/runtime.hpp"
#include "server/streaming.hpp"

#include <chrono>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <drogon/drogon.h>

namespace zks::server {
namespace {

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

void start_non_stream_completion(ChatService& service, const ChatCompletionRequest& request,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    auto pending = service.start_completion(request);
    if (!pending) {
        callback(make_error_response(pending.error()));
        return;
    }

    auto result_status = pending->handle.future.wait_for(std::chrono::seconds(0));
    if (result_status == std::future_status::ready) {
        auto result = pending->handle.future.get();
        if (!result) {
            callback(make_error_response(map_zoo_error_to_api_error(result.error())));
            return;
        }
        callback(
            make_chat_completion_response(pending->id, pending->created, pending->model, *result));
        return;
    }

    std::thread([callback = std::move(callback), pending = std::move(*pending)]() mutable {
        auto result = pending.handle.future.get();
        if (!result) {
            callback(make_error_response(map_zoo_error_to_api_error(result.error())));
            return;
        }

        callback(
            make_chat_completion_response(pending.id, pending.created, pending.model, *result));
    }).detach();
}

void start_stream_completion(const drogon::HttpRequestPtr& request,
                             const std::shared_ptr<ServerRuntime>& runtime,
                             const std::shared_ptr<DisconnectRegistry>& disconnect_registry,
                             const ChatCompletionRequest& completion_request,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    auto& service = runtime->chat_service();

    auto session = std::make_shared<StreamingSession>();
    auto pending = service.start_completion(
        completion_request, [session](std::string_view token) { session->push_token(token); });
    if (!pending) {
        callback(make_error_response(pending.error()));
        return;
    }

    session->set_metadata(pending->id, pending->created, pending->model);
    session->set_cancel_callback(
        [runtime, request_id = pending->handle.id] { runtime->chat_service().cancel(request_id); });

    auto ready_status = pending->handle.future.wait_for(std::chrono::seconds(0));
    if (ready_status == std::future_status::ready) {
        auto result = pending->handle.future.get();
        if (!result) {
            callback(make_error_response(map_zoo_error_to_api_error(result.error())));
            return;
        }

        session->finish_success(*result);
        callback(make_stream_response(session));
        return;
    }

    auto weak_connection = request->getConnectionPtr();
    if (auto connection = weak_connection.lock()) {
        disconnect_registry->track(connection, [session, runtime, request_id = pending->handle.id] {
            session->close();
            runtime->chat_service().cancel(request_id);
        });
    }

    callback(make_stream_response(session));

    std::thread([session, weak_connection, disconnect_registry,
                 pending = std::move(*pending)]() mutable {
        auto result = pending.handle.future.get();
        if (auto connection = weak_connection.lock()) {
            disconnect_registry->clear(connection);
        }

        if (!result) {
            session->finish_error(map_zoo_error_to_api_error(result.error()));
            return;
        }

        session->finish_success(*result);
    }).detach();
}

} // namespace

void DisconnectRegistry::track(const trantor::TcpConnectionPtr& connection,
                               std::function<void()> on_disconnect) {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_[connection] = std::move(on_disconnect);
}

void DisconnectRegistry::clear(const trantor::TcpConnectionPtr& connection) {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.erase(connection);
}

void DisconnectRegistry::handle_connection_event(const trantor::TcpConnectionPtr& connection) {
    if (!connection || !connection->disconnected()) {
        return;
    }

    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = callbacks_.find(connection);
        if (it == callbacks_.end()) {
            return;
        }
        callback = std::move(it->second);
        callbacks_.erase(it);
    }

    if (callback) {
        callback();
    }
}

void register_api_routes(const std::shared_ptr<ServerRuntime>& runtime,
                         const std::shared_ptr<DisconnectRegistry>& disconnect_registry) {
    drogon::app().registerHandler(
        "/v1/models",
        [runtime](const drogon::HttpRequestPtr&,
                  std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(make_models_response(runtime->chat_service().model_id()));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/v1/tools",
        [runtime](const drogon::HttpRequestPtr&,
                  std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(make_tools_response(runtime->chat_service().tools()));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/v1/chat/completions",
        [runtime,
         disconnect_registry](const drogon::HttpRequestPtr& request,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto parsed_request = parse_chat_completion_request(request->body());
            if (!parsed_request) {
                callback(make_error_response(parsed_request.error()));
                return;
            }

            if (!parsed_request->stream) {
                start_non_stream_completion(runtime->chat_service(), *parsed_request,
                                            std::move(callback));
                return;
            }

            start_stream_completion(request, runtime, disconnect_registry, *parsed_request,
                                    std::move(callback));
        },
        {drogon::Post});
}

} // namespace zks::server
