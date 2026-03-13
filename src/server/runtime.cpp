#include "server/runtime.hpp"

#include <chrono>
#include <future>
#include <utility>

namespace zks::server {

Result<std::shared_ptr<ServerRuntime>> ServerRuntime::create(ServerConfig config) {
    auto chat_service_result = ZooChatService::create(config);
    if (!chat_service_result) {
        return std::unexpected(chat_service_result.error());
    }

    return std::make_shared<ServerRuntime>(std::move(config), std::move(*chat_service_result));
}

ServerRuntime::ServerRuntime(ServerConfig config, std::shared_ptr<ChatService> chat_service)
    : config_(std::move(config)), chat_service_(std::move(chat_service)) {}

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

    if (chat_service_) {
        chat_service_->stop();
    }

    for (auto& task : tasks) {
        task.wait();
    }
}

} // namespace zks::server
