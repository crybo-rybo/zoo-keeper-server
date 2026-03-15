#pragma once

#include "server/chat_service.hpp"
#include "server/config.hpp"
#include "server/executor.hpp"
#include "server/metrics.hpp"
#include "server/result.hpp"
#include "server/version.hpp"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace zks::server {

/// Point-in-time health snapshot returned by `/healthz`.
struct HealthSnapshot {
    bool ready = false;
    std::string model_id;
    SessionHealth sessions;
    std::string version = kVersion;
};

/// Singleton lifecycle owner for the server process.
///
/// Owns the `ChatService`, background continuation executor, and session reaper thread.
/// Private constructor — use `create()` or `create_for_test()`.
/// `stop()` is idempotent; it may safely be called multiple times.
class ServerRuntime {
  public:
    static Result<std::shared_ptr<ServerRuntime>> create(ServerConfig config);

    /// Creates a ServerRuntime for tests, bypassing model loading and tool validation.
    static std::shared_ptr<ServerRuntime>
    create_for_test(ServerConfig config, std::shared_ptr<ChatService> chat_service);

    ~ServerRuntime();

    [[nodiscard]] const ServerConfig& config() const noexcept {
        return config_;
    }

    [[nodiscard]] bool is_ready() const noexcept {
        return static_cast<bool>(chat_service_) && chat_service_->is_ready();
    }

    /// Returns a const, lock-free snapshot of the current health state.
    [[nodiscard]] HealthSnapshot health_snapshot() const {
        return HealthSnapshot{is_ready(), config_.model_id, chat_service_->session_health(),
                              kVersion};
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

    /// Signals all background threads to stop and joins them. Safe to call multiple times.
    void stop();

    template <typename Func> Result<void> submit_background(Func&& task) {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        if (stopping_) {
            return std::unexpected("Server runtime is stopping");
        }
        return continuation_executor_.submit(std::forward<Func>(task));
    }

  private:
    ServerRuntime(ServerConfig config, std::shared_ptr<ChatService> chat_service);

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
