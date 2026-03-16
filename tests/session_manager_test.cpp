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

struct SubmittedRequest {
    std::uint64_t id = 0;
    std::vector<zks::server::ChatMessage> messages;
    std::promise<zks::server::RuntimeResult<zks::server::CompletionResult>> promise;
    bool cancelled = false;
};

class FakeAgent {
  public:
    std::pair<zks::server::CompletionHandle, std::shared_ptr<SubmittedRequest>>
    complete(std::vector<zks::server::ChatMessage> messages) {
        auto request = std::make_shared<SubmittedRequest>();
        request->id = next_request_id_++;
        request->messages = std::move(messages);

        auto future = request->promise.get_future();

        // Wrap in RuntimeResult future
        auto result_future = std::async(
            std::launch::deferred,
            [f = std::move(future)]() mutable -> zks::server::RuntimeResult<zks::server::CompletionResult> {
                return f.get();
            });

        std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(request);
        return {zks::server::make_completion_handle(request->id, std::move(result_future)), request};
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

std::unique_ptr<zks::server::SessionStore>
make_session_store(std::string base_prompt = "Base prompt",
                   size_t max_history_messages = 64, size_t max_sessions = 4) {
    zks::server::SessionConfig config;
    config.max_sessions = max_sessions;
    config.idle_ttl_seconds = 600;

    return std::make_unique<zks::server::SessionStore>(
        "local-model", config, std::move(base_prompt), max_history_messages);
}

// Helper: start a request via begin_request + fake agent, returning the handle + submitted request
struct StartedRequest {
    zks::server::CompletionHandle handle;
    std::shared_ptr<SubmittedRequest> submitted;
    std::uint64_t tracking_id;
    zks::server::ChatMessage user_message;
    std::string session_id;
};

StartedRequest start_request(zks::server::SessionStore& store, FakeAgent& agent,
                              std::atomic<std::uint64_t>& next_id,
                              const std::string& session_id, const std::string& content) {
    auto request = make_chat_request(session_id, content);
    auto tracking_id = next_id.fetch_add(1, std::memory_order_relaxed);
    auto begin = store.begin_request(request, tracking_id);
    EXPECT_TRUE(begin.has_value());

    auto [handle, submitted] = agent.complete(std::move(begin->messages));

    return StartedRequest{std::move(handle), submitted, tracking_id,
                          begin->user_message, session_id};
}

void finish_and_commit(zks::server::SessionStore& store, StartedRequest& started,
                       zks::server::RuntimeResult<zks::server::CompletionResult> result) {
    started.submitted->promise.set_value(std::move(result));
    auto observed = started.handle.get();
    store.commit_result(started.session_id, started.tracking_id, started.user_message, observed);
    store.release_request(started.session_id, started.tracking_id);
}

} // namespace

TEST(SessionStoreTest, CreateSessionAndCommitHistory) {
    FakeAgent agent;
    auto store = make_session_store();
    std::atomic<std::uint64_t> next_id{1};

    zks::server::SessionCreateRequest create_request{"local-model", "Session prompt"};
    auto created = store->create_session(create_request);
    ASSERT_TRUE(created.has_value());
    EXPECT_EQ(created->id, "sess-1");

    auto started = start_request(*store, agent, next_id, created->id, "Hello");

    const std::vector<zks::server::ChatMessage> expected_first = {
        zks::server::ChatMessage::system("Base prompt\n\nSession prompt"),
        zks::server::ChatMessage::user("Hello"),
    };
    EXPECT_EQ(started.submitted->messages, expected_first);

    zks::server::CompletionResult response;
    response.text = "Hi there";
    finish_and_commit(*store, started, response);

    auto started2 = start_request(*store, agent, next_id, created->id, "Follow up");

    const std::vector<zks::server::ChatMessage> expected_second = {
        zks::server::ChatMessage::system("Base prompt\n\nSession prompt"),
        zks::server::ChatMessage::user("Hello"),
        zks::server::ChatMessage::assistant("Hi there"),
        zks::server::ChatMessage::user("Follow up"),
    };
    EXPECT_EQ(started2.submitted->messages, expected_second);

    response.text = "Second answer";
    finish_and_commit(*store, started2, response);
}

TEST(SessionStoreTest, FailedTurnDoesNotMutateHistory) {
    FakeAgent agent;
    auto store = make_session_store();
    std::atomic<std::uint64_t> next_id{1};

    auto created = store->create_session({"local-model", std::nullopt});
    ASSERT_TRUE(created.has_value());

    auto started = start_request(*store, agent, next_id, created->id, "Hello");

    auto error = std::unexpected(zks::server::RuntimeError{
        zks::server::RuntimeErrorCode::InferenceFailed,
        "boom",
    });
    finish_and_commit(*store, started, error);

    auto started2 = start_request(*store, agent, next_id, created->id, "Retry");

    const std::vector<zks::server::ChatMessage> expected_retry = {
        zks::server::ChatMessage::system("Base prompt"),
        zks::server::ChatMessage::user("Retry"),
    };
    EXPECT_EQ(started2.submitted->messages, expected_retry);

    zks::server::CompletionResult response;
    response.text = "Recovered";
    finish_and_commit(*store, started2, response);
}

TEST(SessionStoreTest, SameSessionBusyDifferentSessionsCanQueue) {
    FakeAgent agent;
    auto store = make_session_store();
    std::atomic<std::uint64_t> next_id{1};

    auto session_a = store->create_session({"local-model", std::nullopt});
    auto session_b = store->create_session({"local-model", "Other prompt"});
    ASSERT_TRUE(session_a.has_value());
    ASSERT_TRUE(session_b.has_value());

    auto started_a = start_request(*store, agent, next_id, session_a->id, "A1");

    // Same session should be busy
    auto busy_request = make_chat_request(session_a->id, "A2");
    auto busy = store->begin_request(busy_request, next_id.fetch_add(1));
    ASSERT_FALSE(busy.has_value());
    EXPECT_EQ(busy.error().http_status, 409);

    // Different session should work
    auto started_b = start_request(*store, agent, next_id, session_b->id, "B1");
    EXPECT_EQ(agent.request_count(), 2u);

    zks::server::CompletionResult response;
    response.text = "ok";
    finish_and_commit(*store, started_a, response);
    finish_and_commit(*store, started_b, response);
}

TEST(SessionStoreTest, DeleteSessionMarksAsClosed) {
    FakeAgent agent;
    auto store = make_session_store("Base prompt", 64, 1);
    std::atomic<std::uint64_t> next_id{1};

    auto created = store->create_session({"local-model", std::nullopt});
    ASSERT_TRUE(created.has_value());

    auto started = start_request(*store, agent, next_id, created->id, "Hello");

    auto deleted = store->delete_session(created->id);
    ASSERT_TRUE(deleted.has_value());

    // Complete the request — result should be discarded (session closed)
    zks::server::CompletionResult response;
    response.text = "late answer";
    finish_and_commit(*store, started, response);

    auto missing = store->get_session(created->id);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().http_status, 404);
}

TEST(SessionStoreTest, SessionCapacityEnforced) {
    auto store = make_session_store("Base prompt", 64, 1);

    auto first = store->create_session({"local-model", std::nullopt});
    ASSERT_TRUE(first.has_value());

    auto second = store->create_session({"local-model", std::nullopt});
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().http_status, 503);
    EXPECT_EQ(second.error().code, std::optional<std::string>{"session_capacity_reached"});
}

TEST(SessionStoreTest, HistoryTrimsOldestTurns) {
    FakeAgent agent;
    auto store = make_session_store("Base prompt", 2);
    std::atomic<std::uint64_t> next_id{1};

    auto created = store->create_session({"local-model", std::nullopt});
    ASSERT_TRUE(created.has_value());

    auto s1 = start_request(*store, agent, next_id, created->id, "One");
    zks::server::CompletionResult response;
    response.text = "One answer";
    finish_and_commit(*store, s1, response);

    auto s2 = start_request(*store, agent, next_id, created->id, "Two");
    response.text = "Two answer";
    finish_and_commit(*store, s2, response);

    auto s3 = start_request(*store, agent, next_id, created->id, "Three");

    const std::vector<zks::server::ChatMessage> expected = {
        zks::server::ChatMessage::system("Base prompt"),
        zks::server::ChatMessage::user("Two"),
        zks::server::ChatMessage::assistant("Two answer"),
        zks::server::ChatMessage::user("Three"),
    };
    EXPECT_EQ(s3.submitted->messages, expected);

    response.text = "Three answer";
    finish_and_commit(*store, s3, response);
}
