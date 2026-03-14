#include "server/runtime.hpp"

#include <chrono>
#include <future>
#include <utility>

namespace zks::server {

namespace {
constexpr std::chrono::seconds kReaperInterval{60};
} // namespace

Result<std::shared_ptr<ServerRuntime>> ServerRuntime::create(ServerConfig config) {
    auto chat_service_result = ZooChatService::create(config);
    if (!chat_service_result) {
        return std::unexpected(chat_service_result.error());
    }

    return std::make_shared<ServerRuntime>(std::move(config), std::move(*chat_service_result));
}

ServerRuntime::ServerRuntime(ServerConfig config, std::shared_ptr<ChatService> chat_service)
    : config_(std::move(config)), chat_service_(std::move(chat_service)) {
    if (config_.sessions.enabled()) {
        reaper_thread_ = std::thread([this]() { run_session_reaper(); });
    }
}

ServerRuntime::~ServerRuntime() {
    stop();
}

void ServerRuntime::prune_background_tasks_locked() {
    auto it = background_tasks_.begin();
    while (it != background_tasks_.end()) {
        if (it->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }

        it->get();
        it = background_tasks_.erase(it);
    }
}

void ServerRuntime::stop() {
    std::vector<std::future<void>> tasks;
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
        tasks.swap(background_tasks_);
    }

    reaper_cv_.notify_all();
    if (reaper_thread_.joinable()) {
        reaper_thread_.join();
    }

    if (chat_service_) {
        chat_service_->stop();
    }

    for (auto& task : tasks) {
        task.get();
    }
}

MetricsSnapshot ServerRuntime::metrics_snapshot() const {
    return MetricsSnapshot{
        .requests_total = metrics_.requests_total(),
        .requests_errors = metrics_.requests_errors(),
        .requests_cancelled_total = metrics_.requests_cancelled_total(),
        .requests_queue_rejected_total = metrics_.requests_queue_rejected_total(),
        .stream_disconnects_total = metrics_.stream_disconnects_total(),
        .active_sessions = chat_service_->session_health().active,
        .model_id = config_.model_id,
        .uptime_seconds = metrics_.uptime_seconds(),
    };
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
