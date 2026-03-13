#pragma once

#include "server/chat_service.hpp"
#include "server/config.hpp"
#include "server/result.hpp"

#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace zks::server {

struct HealthSnapshot {
    bool ready = false;
    std::string model_id;
    SessionHealth sessions;
};

class ServerRuntime {
  public:
    static Result<std::shared_ptr<ServerRuntime>> create(ServerConfig config);

    ServerRuntime(ServerConfig config, std::shared_ptr<ChatService> chat_service);
    ~ServerRuntime();

    [[nodiscard]] const ServerConfig& config() const noexcept {
        return config_;
    }

    [[nodiscard]] bool is_ready() const noexcept {
        return static_cast<bool>(chat_service_) && chat_service_->is_ready();
    }

    [[nodiscard]] HealthSnapshot health_snapshot() const {
        return HealthSnapshot{is_ready(), config_.model_id, chat_service_->session_health()};
    }

    [[nodiscard]] ChatService& chat_service() noexcept {
        return *chat_service_;
    }

    [[nodiscard]] const ChatService& chat_service() const noexcept {
        return *chat_service_;
    }

    void stop();

    template <typename Func> void spawn_background(Func&& task) {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        prune_background_tasks_locked();
        if (stopping_) {
            return;
        }

        background_tasks_.push_back(std::async(std::launch::async,
                                               [task = std::forward<Func>(task)]() mutable {
                                                   try {
                                                       task();
                                                   } catch (const std::exception& error) {
                                                       std::cerr
                                                           << "Background task failed: "
                                                           << error.what() << '\n';
                                                   } catch (...) {
                                                       std::cerr
                                                           << "Background task failed with an unknown exception"
                                                           << '\n';
                                                   }
                                               }));
    }

  private:
    void prune_background_tasks_locked();

    ServerConfig config_;
    std::shared_ptr<ChatService> chat_service_;
    std::mutex tasks_mutex_;
    std::vector<std::future<void>> background_tasks_;
    bool stopping_ = false;
};

} // namespace zks::server
