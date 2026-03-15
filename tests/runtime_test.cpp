#include "server/runtime.hpp"

#include "fake_chat_service.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace {

struct Probe {
    explicit Probe(std::shared_ptr<std::atomic<int>> destroyed) : destroyed_(std::move(destroyed)) {}

    ~Probe() {
        destroyed_->fetch_add(1, std::memory_order_relaxed);
    }

    std::shared_ptr<std::atomic<int>> destroyed_;
};

} // namespace

TEST(RuntimeTest, StopDrainsBackgroundTasks) {
    auto chat_service = std::make_shared<FakeChatService>();
    chat_service->set_model_id("runtime-test-model");
    zks::server::ServerConfig config;
    config.model_id = "runtime-test-model";
    config.zoo_config.model_path = "/tmp/runtime-test-model.gguf";

    auto runtime = zks::server::ServerRuntime::create_for_test(config, chat_service);

    auto destroyed = std::make_shared<std::atomic<int>>(0);
    auto started = std::make_shared<std::atomic<bool>>(false);

    auto submitted =
        runtime->submit_background([probe = std::make_shared<Probe>(destroyed), started]() mutable {
        started->store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    });
    ASSERT_TRUE(submitted);

    while (!started->load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    runtime->stop();

    EXPECT_EQ(chat_service->stop_calls(), 1);
    EXPECT_EQ(destroyed->load(std::memory_order_relaxed), 1);

    runtime->stop();
    EXPECT_EQ(chat_service->stop_calls(), 1);
}
