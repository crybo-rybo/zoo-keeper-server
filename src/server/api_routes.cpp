#include "server/api_routes.hpp"

#include "server/api_json.hpp"
#include "server/auth.hpp"
#include "server/completion_controller.hpp"
#include "server/route_utils.hpp"
#include "server/runtime.hpp"

#include <memory>
#include <utility>
#include <vector>

#include <drogon/drogon.h>

namespace zks::server {
namespace {

drogon::HttpResponsePtr make_no_content_response() {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k204NoContent);
    return response;
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
               std::function<void(const drogon::HttpResponsePtr&)>&& callback,
               const std::string&) {
                auto response = make_no_content_response();
                response->addHeader("Access-Control-Allow-Methods",
                                    "GET, POST, DELETE, OPTIONS");
                response->addHeader("Access-Control-Allow-Headers",
                                    "Authorization, Content-Type");
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
                with_metrics(runtime, std::move(callback))(make_error_response(*auth_err));
                return;
            }
            with_metrics(runtime, std::move(callback))(
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
                with_metrics(runtime, std::move(callback))(make_error_response(*auth_err));
                return;
            }
            with_metrics(runtime, std::move(callback))(
                make_tools_response(runtime->chat_service().tools()));
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
                with_metrics(runtime, std::move(callback))(make_error_response(*auth_err));
                return;
            }
            auto cb = with_metrics(runtime, std::move(callback));
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
                with_metrics(runtime, std::move(callback))(make_error_response(*auth_err));
                return;
            }
            auto cb = with_metrics(runtime, std::move(callback));
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
                with_metrics(runtime, std::move(callback))(make_error_response(*auth_err));
                return;
            }
            auto cb = with_metrics(runtime, std::move(callback));
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
                with_metrics(runtime, std::move(callback))(make_error_response(*auth_err));
                return;
            }
            with_metrics(runtime, std::move(callback))(
                make_json_response(runtime->metrics_snapshot().to_json()));
        },
        {drogon::Get});
}

} // namespace zks::server
