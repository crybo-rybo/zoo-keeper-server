#include "server/runtime.hpp"

#include "fake_chat_service.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

struct Probe {
    explicit Probe(std::shared_ptr<std::atomic<int>> destroyed) : destroyed_(std::move(destroyed)) {}

    ~Probe() {
        destroyed_->fetch_add(1, std::memory_order_relaxed);
    }

    std::shared_ptr<std::atomic<int>> destroyed_;
};

} // namespace

int main() {
    auto chat_service = std::make_shared<FakeChatService>();
    chat_service->set_model_id("runtime-test-model");
    zks::server::ServerConfig config;
    config.model_id = "runtime-test-model";
    config.zoo_config.model_path = "/tmp/runtime-test-model.gguf";

    auto runtime = std::make_shared<zks::server::ServerRuntime>(config, chat_service);

    auto destroyed = std::make_shared<std::atomic<int>>(0);
    auto started = std::make_shared<std::atomic<bool>>(false);

    runtime->spawn_background([probe = std::make_shared<Probe>(destroyed), started]() mutable {
        started->store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    });

    while (!started->load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    runtime->stop();

    if (chat_service->stop_calls() != 1) {
        return fail("Expected ServerRuntime::stop() to stop the chat service exactly once.");
    }

    if (destroyed->load(std::memory_order_relaxed) != 1) {
        return fail("Expected background task captures to be destroyed during stop().");
    }

    runtime->stop();
    if (chat_service->stop_calls() != 1) {
        return fail("Expected ServerRuntime::stop() to remain idempotent.");
    }

    return 0;
}
