#include "server/session_manager.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

class PromiseCompletionSource final : public zks::server::CompletionSource {
  public:
    explicit PromiseCompletionSource(
        std::future<zks::server::RuntimeResult<zks::server::CompletionResult>> future)
        : future_(std::move(future)) {}

    [[nodiscard]] std::future_status wait_for(std::chrono::milliseconds timeout) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (consumed_) {
            return std::future_status::ready;
        }
        return future_.wait_for(timeout);
    }

    zks::server::RuntimeResult<zks::server::CompletionResult> get() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (consumed_) {
            return std::unexpected(zks::server::RuntimeError{
                zks::server::RuntimeErrorCode::RuntimeFailure,
                "Completion already consumed",
            });
        }
        consumed_ = true;
        return future_.get();
    }

  private:
    mutable std::mutex mutex_;
    mutable std::future<zks::server::RuntimeResult<zks::server::CompletionResult>> future_;
    bool consumed_ = false;
};

struct SubmittedRequest {
    std::uint64_t id = 0;
    std::vector<zks::server::ChatMessage> messages;
    std::optional<zks::server::TokenCallback> callback;
    std::promise<zks::server::RuntimeResult<zks::server::CompletionResult>> promise;
    bool cancelled = false;
};

class FakeExecutor {
  public:
    zks::server::CompletionHandle start(std::vector<zks::server::ChatMessage> messages,
                                        std::optional<zks::server::TokenCallback> callback) {
        auto request = std::make_shared<SubmittedRequest>();
        request->id = next_request_id_++;
        request->messages = std::move(messages);
        request->callback = std::move(callback);

        auto future = request->promise.get_future();
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(request);
        return zks::server::CompletionHandle{
            request->id,
            std::make_shared<PromiseCompletionSource>(std::move(future)),
        };
    }

    void cancel(std::uint64_t request_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& request : requests_) {
            if (request->id == request_id) {
                request->cancelled = true;
                return;
            }
        }
    }

    size_t request_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_.size();
    }

    std::shared_ptr<SubmittedRequest> request(size_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_.at(index);
    }

  private:
    mutable std::mutex mutex_;
    std::uint64_t next_request_id_ = 1;
    std::vector<std::shared_ptr<SubmittedRequest>> requests_;
};

zks::server::ChatCompletionRequest make_chat_request(const std::string& session_id,
                                                     const std::string& content) {
    zks::server::ChatCompletionRequest request;
    request.model = "local-model";
    request.session_id = session_id;
    request.messages.push_back(zks::server::ChatMessage::user(content));
    return request;
}

std::unique_ptr<zks::server::SessionManager>
make_session_manager(FakeExecutor& executor, std::string base_prompt = "Base prompt",
                     size_t max_history_messages = 64, size_t max_sessions = 4) {
    zks::server::SessionConfig config;
    config.max_sessions = max_sessions;
    config.idle_ttl_seconds = 600;

    return std::make_unique<zks::server::SessionManager>(
        "local-model", config, std::move(base_prompt), max_history_messages,
        [&executor](std::vector<zks::server::ChatMessage> messages,
                    std::optional<zks::server::TokenCallback> callback) {
            return executor.start(std::move(messages), std::move(callback));
        },
        [&executor](std::uint64_t request_id) { executor.cancel(request_id); });
}

zks::server::RuntimeResult<zks::server::CompletionResult>
finish_pending(zks::server::PendingChatCompletion& pending,
               const std::shared_ptr<SubmittedRequest>& submitted,
               zks::server::RuntimeResult<zks::server::CompletionResult> result) {
    submitted->promise.set_value(std::move(result));
    auto observed = pending.handle.get();
    if (pending.on_result) {
        pending.on_result(observed);
    }
    if (pending.lease) {
        pending.lease->release();
    }
    return observed;
}

} // namespace

TEST(SessionManagerTest, CreateSessionAndCommitHistory) {
    FakeExecutor executor;
    auto manager = make_session_manager(executor);
    std::atomic<std::uint64_t> next_completion_id{1};

    zks::server::SessionCreateRequest create_request{"local-model", "Session prompt"};
    auto created = manager->create_session(create_request);
    ASSERT_TRUE(created.has_value());
    EXPECT_EQ(created->id, "sess-1");
    EXPECT_EQ(executor.request_count(), 0u);

    auto pending =
        manager->start_completion(make_chat_request(created->id, "Hello"), next_completion_id);
    ASSERT_TRUE(pending.has_value());

    const std::vector<zks::server::ChatMessage> expected_first = {
        zks::server::ChatMessage::system("Base prompt\n\nSession prompt"),
        zks::server::ChatMessage::user("Hello"),
    };
    EXPECT_EQ(executor.request(0)->messages, expected_first);

    zks::server::CompletionResult response;
    response.text = "Hi there";
    auto observed = finish_pending(*pending, executor.request(0), response);
    ASSERT_TRUE(observed.has_value());
    EXPECT_EQ(observed->text, "Hi there");

    auto second =
        manager->start_completion(make_chat_request(created->id, "Follow up"), next_completion_id);
    ASSERT_TRUE(second.has_value());

    const std::vector<zks::server::ChatMessage> expected_second = {
        zks::server::ChatMessage::system("Base prompt\n\nSession prompt"),
        zks::server::ChatMessage::user("Hello"),
        zks::server::ChatMessage::assistant("Hi there"),
        zks::server::ChatMessage::user("Follow up"),
    };
    EXPECT_EQ(executor.request(1)->messages, expected_second);

    response.text = "Second answer";
    observed = finish_pending(*second, executor.request(1), response);
    ASSERT_TRUE(observed.has_value());
    EXPECT_EQ(observed->text, "Second answer");
}

TEST(SessionManagerTest, FailedTurnDoesNotMutateHistory) {
    FakeExecutor executor;
    auto manager = make_session_manager(executor);
    std::atomic<std::uint64_t> next_completion_id{1};

    auto created = manager->create_session({"local-model", std::nullopt});
    ASSERT_TRUE(created.has_value());

    auto pending =
        manager->start_completion(make_chat_request(created->id, "Hello"), next_completion_id);
    ASSERT_TRUE(pending.has_value());

    auto error = std::unexpected(zks::server::RuntimeError{
        zks::server::RuntimeErrorCode::InferenceFailed,
        "boom",
    });
    auto observed = finish_pending(*pending, executor.request(0), error);
    EXPECT_FALSE(observed.has_value());

    auto retry =
        manager->start_completion(make_chat_request(created->id, "Retry"), next_completion_id);
    ASSERT_TRUE(retry.has_value());

    const std::vector<zks::server::ChatMessage> expected_retry = {
        zks::server::ChatMessage::system("Base prompt"),
        zks::server::ChatMessage::user("Retry"),
    };
    EXPECT_EQ(executor.request(1)->messages, expected_retry);

    zks::server::CompletionResult response;
    response.text = "Recovered";
    observed = finish_pending(*retry, executor.request(1), response);
    ASSERT_TRUE(observed.has_value());
    EXPECT_EQ(observed->text, "Recovered");
}

TEST(SessionManagerTest, SameSessionBusyDifferentSessionsCanQueue) {
    FakeExecutor executor;
    auto manager = make_session_manager(executor);
    std::atomic<std::uint64_t> next_completion_id{1};

    auto session_a = manager->create_session({"local-model", std::nullopt});
    auto session_b = manager->create_session({"local-model", "Other prompt"});
    ASSERT_TRUE(session_a.has_value());
    ASSERT_TRUE(session_b.has_value());

    auto first =
        manager->start_completion(make_chat_request(session_a->id, "A1"), next_completion_id);
    ASSERT_TRUE(first.has_value());

    auto busy =
        manager->start_completion(make_chat_request(session_a->id, "A2"), next_completion_id);
    ASSERT_FALSE(busy.has_value());
    EXPECT_EQ(busy.error().http_status, 409);

    auto second =
        manager->start_completion(make_chat_request(session_b->id, "B1"), next_completion_id);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(executor.request_count(), 2u);

    zks::server::CompletionResult response;
    response.text = "ok";
    auto observed = finish_pending(*first, executor.request(0), response);
    ASSERT_TRUE(observed.has_value());
    observed = finish_pending(*second, executor.request(1), response);
    ASSERT_TRUE(observed.has_value());
}

TEST(SessionManagerTest, DeleteActiveSessionCancelsRequest) {
    FakeExecutor executor;
    auto manager = make_session_manager(executor, "Base prompt", 64, 1);
    std::atomic<std::uint64_t> next_completion_id{1};

    auto created = manager->create_session({"local-model", std::nullopt});
    ASSERT_TRUE(created.has_value());

    auto pending =
        manager->start_completion(make_chat_request(created->id, "Hello"), next_completion_id);
    ASSERT_TRUE(pending.has_value());
    auto submitted = executor.request(0);

    auto deleted = manager->delete_session(created->id);
    ASSERT_TRUE(deleted.has_value());
    EXPECT_TRUE(submitted->cancelled);

    zks::server::CompletionResult response;
    response.text = "late answer";
    auto observed = finish_pending(*pending, submitted, response);
    ASSERT_TRUE(observed.has_value());
    EXPECT_EQ(observed->text, "late answer");

    auto missing = manager->get_session(created->id);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().http_status, 404);
}

TEST(SessionManagerTest, SessionCapacityEnforced) {
    FakeExecutor executor;
    auto manager = make_session_manager(executor, "Base prompt", 64, 1);

    auto first = manager->create_session({"local-model", std::nullopt});
    ASSERT_TRUE(first.has_value());

    auto second = manager->create_session({"local-model", std::nullopt});
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().http_status, 503);
    EXPECT_EQ(second.error().code, std::optional<std::string>{"session_capacity_reached"});
}

TEST(SessionManagerTest, HistoryTrimsOldestTurns) {
    FakeExecutor executor;
    auto manager = make_session_manager(executor, "Base prompt", 2);
    std::atomic<std::uint64_t> next_completion_id{1};

    auto created = manager->create_session({"local-model", std::nullopt});
    ASSERT_TRUE(created.has_value());

    auto first =
        manager->start_completion(make_chat_request(created->id, "One"), next_completion_id);
    ASSERT_TRUE(first.has_value());
    zks::server::CompletionResult response;
    response.text = "One answer";
    finish_pending(*first, executor.request(0), response);

    auto second =
        manager->start_completion(make_chat_request(created->id, "Two"), next_completion_id);
    ASSERT_TRUE(second.has_value());
    response.text = "Two answer";
    finish_pending(*second, executor.request(1), response);

    auto third =
        manager->start_completion(make_chat_request(created->id, "Three"), next_completion_id);
    ASSERT_TRUE(third.has_value());

    const std::vector<zks::server::ChatMessage> expected = {
        zks::server::ChatMessage::system("Base prompt"),
        zks::server::ChatMessage::user("Two"),
        zks::server::ChatMessage::assistant("Two answer"),
        zks::server::ChatMessage::user("Three"),
    };
    EXPECT_EQ(executor.request(2)->messages, expected);

    response.text = "Three answer";
    finish_pending(*third, executor.request(2), response);
}
