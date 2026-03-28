#include "server/chat_service.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace zks::server {

class ZooChatServiceTest : public ::testing::Test {
  protected:
    ZooChatService make_test_service(std::string system_prompt = "Base prompt") {
        zoo::ModelConfig model_config;
        model_config.model_path = "/tmp/test.gguf";
        model_config.context_size = 128;

        return ZooChatService("test-model", std::move(model_config), zoo::AgentConfig{},
                              std::move(system_prompt), {}, std::shared_ptr<zoo::Agent>{},
                              std::unique_ptr<SessionStore>{}, zoo::GenerationOptions{});
    }

    void expect_history_matches_prompt(const ZooChatService& service) {
        std::scoped_lock lock(service.system_prompt_mutex_, service.agent_history_mutex_);
        if (service.request_system_prompt_.empty()) {
            EXPECT_TRUE(service.agent_history_.messages.empty());
            return;
        }

        ASSERT_FALSE(service.agent_history_.messages.empty());
        EXPECT_EQ(service.agent_history_.messages.front().role, MessageRole::System);
        EXPECT_EQ(service.agent_history_.messages.front().content, service.request_system_prompt_);
    }

    std::string update_system_prompt_state(ZooChatService& service, std::string prompt) {
        return service.update_system_prompt_state(std::move(prompt));
    }

    void reset_agent_history_to_system_prompt(ZooChatService& service) {
        service.reset_agent_history_to_system_prompt();
    }

    void append_history_message(ZooChatService& service, const ChatMessage& message) {
        service.agent_history_.messages.push_back(message);
    }

    bool history_matches_prompt(const ZooChatService& service) {
        std::scoped_lock lock(service.system_prompt_mutex_, service.agent_history_mutex_);
        if (service.request_system_prompt_.empty()) {
            return service.agent_history_.messages.empty();
        }

        return !service.agent_history_.messages.empty() &&
               service.agent_history_.messages.front().role == MessageRole::System &&
               service.agent_history_.messages.front().content == service.request_system_prompt_;
    }

    size_t history_size(const ZooChatService& service) {
        std::lock_guard<std::mutex> lock(service.agent_history_mutex_);
        return service.agent_history_.messages.size();
    }
};

TEST_F(ZooChatServiceTest, UpdateSystemPromptStateKeepsRetainedHistoryAligned) {
    auto service = make_test_service("Base prompt");

    auto applied_prompt = update_system_prompt_state(service, "Updated prompt");

    EXPECT_EQ(applied_prompt, "Updated prompt");
    expect_history_matches_prompt(service);
}

TEST_F(ZooChatServiceTest, ResetAgentHistoryPreservesCurrentSystemPrompt) {
    auto service = make_test_service("Base prompt");
    append_history_message(service, ChatMessage::user("hello"));
    append_history_message(service, ChatMessage::assistant("world"));

    reset_agent_history_to_system_prompt(service);

    ASSERT_EQ(history_size(service), 1u);
    expect_history_matches_prompt(service);
}

TEST_F(ZooChatServiceTest, ConcurrentPromptUpdatesAndHistoryResetsStayConsistent) {
    auto service = make_test_service("Base prompt");
    std::atomic<bool> stop{false};
    std::atomic<bool> mismatch{false};

    std::thread updater([&] {
        for (int i = 0; i < 2000; ++i) {
            service.set_system_prompt((i % 2 == 0) ? "Prompt A" : "Prompt B");
        }
        stop.store(true, std::memory_order_release);
    });

    std::thread clearer([&] {
        while (!stop.load(std::memory_order_acquire)) {
            reset_agent_history_to_system_prompt(service);
        }
    });

    std::thread observer([&] {
        while (!stop.load(std::memory_order_acquire) && !mismatch.load(std::memory_order_relaxed)) {
            if (!history_matches_prompt(service)) {
                mismatch.store(true, std::memory_order_relaxed);
            }
        }
    });

    updater.join();
    clearer.join();
    observer.join();

    EXPECT_FALSE(mismatch.load(std::memory_order_relaxed));
    expect_history_matches_prompt(service);
}

} // namespace zks::server
