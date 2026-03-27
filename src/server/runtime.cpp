#include "server/runtime.hpp"

#include "server/command_tools.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace zks::server {

namespace {
constexpr std::chrono::seconds kReaperInterval{60};

size_t continuation_worker_count() {
    const auto hardware_threads = std::thread::hardware_concurrency();
    if (hardware_threads == 0) {
        return 2;
    }
    return std::min<size_t>(4, std::max<size_t>(2, hardware_threads));
}

size_t continuation_queue_depth(const ServerConfig& config) {
    const size_t request_capacity =
        std::max<size_t>(1, static_cast<size_t>(config.agent_config.request_queue_capacity));
    return std::max<size_t>(32, request_capacity * 2);
}
} // namespace

Result<std::shared_ptr<ServerRuntime>> ServerRuntime::create(ServerConfig config) {
    auto tool_provider_result = make_command_tool_provider(config.tools);
    if (!tool_provider_result) {
        return std::unexpected(tool_provider_result.error());
    }

    auto chat_service_result = ZooChatService::create(config, std::move(*tool_provider_result));
    if (!chat_service_result) {
        return std::unexpected(chat_service_result.error());
    }

    return std::shared_ptr<ServerRuntime>(
        new ServerRuntime(std::move(config), std::move(*chat_service_result)));
}

std::shared_ptr<ServerRuntime>
ServerRuntime::create_for_test(ServerConfig config, std::shared_ptr<ChatService> chat_service) {
    return std::shared_ptr<ServerRuntime>(
        new ServerRuntime(std::move(config), std::move(chat_service)));
}

ServerRuntime::ServerRuntime(ServerConfig config, std::shared_ptr<ChatService> chat_service)
    : config_(std::move(config)), chat_service_(std::move(chat_service)),
      continuation_executor_(continuation_worker_count(), continuation_queue_depth(config_)) {
    if (config_.sessions.enabled()) {
        reaper_thread_ = std::thread([this]() { run_session_reaper(); });
    }
}

ServerRuntime::~ServerRuntime() {
    stop();
}

void ServerRuntime::stop() {
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
    }

    reaper_cv_.notify_all();
    if (reaper_thread_.joinable()) {
        reaper_thread_.join();
    }

    continuation_executor_.stop();

    if (chat_service_) {
        chat_service_->stop();
    }
}

MetricsSnapshot ServerRuntime::metrics_snapshot() const {
    MetricsSnapshot snapshot;
    metrics_.populate_snapshot(snapshot);
    snapshot.active_sessions = chat_service_->session_health().active;
    snapshot.model_id = config_.model_id;
    snapshot.uptime_seconds = metrics_.uptime_seconds();
    return snapshot;
}

void ServerRuntime::run_session_reaper() {
    while (true) {
        std::unique_lock<std::mutex> lock(tasks_mutex_);
        reaper_cv_.wait_for(lock, kReaperInterval, [this] { return stopping_; });
        if (stopping_) {
            break;
        }
        lock.unlock();
        chat_service_->reap_sessions();
    }
}

} // namespace zks::server
