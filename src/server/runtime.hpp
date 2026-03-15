#pragma once

#include "server/chat_service.hpp"
#include "server/config.hpp"
#include "server/executor.hpp"
#include "server/metrics.hpp"
#include "server/result.hpp"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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

    [[nodiscard]] ServerMetrics& metrics() noexcept {
        return metrics_;
    }

    [[nodiscard]] MetricsSnapshot metrics_snapshot() const;

    void stop();

    template <typename Func> Result<void> submit_background(Func&& task) {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        if (stopping_) {
            return std::unexpected("Server runtime is stopping");
        }
        return continuation_executor_.submit(std::forward<Func>(task));
    }

  private:
    void run_session_reaper();

    ServerConfig config_;
    std::shared_ptr<ChatService> chat_service_;
    ServerMetrics metrics_;
    BoundedExecutor continuation_executor_;
    std::mutex tasks_mutex_;
    std::condition_variable reaper_cv_;
    std::thread reaper_thread_;
    bool stopping_ = false;
};

} // namespace zks::server
