#include "server/session_manager.hpp"

#include <atomic>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

class PromiseCompletionSource final : public zks::server::CompletionSource {
  public:
    explicit PromiseCompletionSource(
        std::future<zks::server::RuntimeResult<zks::server::CompletionResult>> future)
        : future_(std::move(future)) {}

    [[nodiscard]] std::future_status
    wait_for(std::chrono::milliseconds timeout) const override {
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
    zks::server::CompletionHandle start(
        std::vector<zks::server::ChatMessage> messages,
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

int test_create_session_and_commit_history() {
    FakeExecutor executor;
    auto manager = make_session_manager(executor);
    std::atomic<std::uint64_t> next_completion_id{1};

    zks::server::SessionCreateRequest create_request{"local-model", "Session prompt"};
    auto created = manager->create_session(create_request);
    if (!created || created->id != "sess-1") {
        return fail("Expected session creation to succeed.");
    }
    if (executor.request_count() != 0) {
        return fail("Creating a session should not start inference.");
    }

    auto pending = manager->start_completion(make_chat_request(created->id, "Hello"), next_completion_id);
    if (!pending) {
        return fail("Expected first session completion to start successfully.");
    }

    const std::vector<zks::server::ChatMessage> expected_first = {
        zks::server::ChatMessage::system("Base prompt\n\nSession prompt"),
        zks::server::ChatMessage::user("Hello"),
    };
    if (executor.request(0)->messages != expected_first) {
        return fail("Session completion did not submit the expected first-turn history.");
    }

    zks::server::CompletionResult response;
    response.text = "Hi there";
    auto observed = finish_pending(*pending, executor.request(0), response);
    if (!observed || observed->text != "Hi there") {
        return fail("Expected the first session completion to succeed.");
    }

    auto second = manager->start_completion(make_chat_request(created->id, "Follow up"),
                                            next_completion_id);
    if (!second) {
        return fail("Expected second session completion to start successfully.");
    }

    const std::vector<zks::server::ChatMessage> expected_second = {
        zks::server::ChatMessage::system("Base prompt\n\nSession prompt"),
        zks::server::ChatMessage::user("Hello"),
        zks::server::ChatMessage::assistant("Hi there"),
        zks::server::ChatMessage::user("Follow up"),
    };
    if (executor.request(1)->messages != expected_second) {
        return fail("Successful completion was not committed to retained session history.");
    }

    response.text = "Second answer";
    observed = finish_pending(*second, executor.request(1), response);
    if (!observed || observed->text != "Second answer") {
        return fail("Expected the follow-up session completion to succeed.");
    }
    return 0;
}

int test_failed_turn_does_not_mutate_history() {
    FakeExecutor executor;
    auto manager = make_session_manager(executor);
    std::atomic<std::uint64_t> next_completion_id{1};

    auto created = manager->create_session({"local-model", std::nullopt});
    if (!created) {
        return fail("Expected session creation to succeed for failed-turn test.");
    }

    auto pending = manager->start_completion(make_chat_request(created->id, "Hello"), next_completion_id);
    if (!pending) {
        return fail("Expected first session completion to start in failed-turn test.");
    }

    auto error = std::unexpected(zks::server::RuntimeError{
        zks::server::RuntimeErrorCode::InferenceFailed,
        "boom",
    });
    auto observed = finish_pending(*pending, executor.request(0), error);
    if (observed) {
        return fail("Expected failed-turn completion to surface an error.");
    }

    auto retry = manager->start_completion(make_chat_request(created->id, "Retry"), next_completion_id);
    if (!retry) {
        return fail("Expected retry completion to start after failure.");
    }

    const std::vector<zks::server::ChatMessage> expected_retry = {
        zks::server::ChatMessage::system("Base prompt"),
        zks::server::ChatMessage::user("Retry"),
    };
    if (executor.request(1)->messages != expected_retry) {
        return fail("Failed turn was incorrectly committed to retained history.");
    }

    zks::server::CompletionResult response;
    response.text = "Recovered";
    observed = finish_pending(*retry, executor.request(1), response);
    if (!observed || observed->text != "Recovered") {
        return fail("Expected retry completion to succeed.");
    }

    return 0;
}

int test_same_session_busy_but_different_sessions_can_queue() {
    FakeExecutor executor;
    auto manager = make_session_manager(executor);
    std::atomic<std::uint64_t> next_completion_id{1};

    auto session_a = manager->create_session({"local-model", std::nullopt});
    auto session_b = manager->create_session({"local-model", "Other prompt"});
    if (!session_a || !session_b) {
        return fail("Expected both sessions to be created.");
    }

    auto first = manager->start_completion(make_chat_request(session_a->id, "A1"), next_completion_id);
    if (!first) {
        return fail("Expected first request for session A to start.");
    }

    auto busy = manager->start_completion(make_chat_request(session_a->id, "A2"), next_completion_id);
    if (busy || busy.error().http_status != 409) {
        return fail("Expected same-session concurrent request to return session_busy.");
    }

    auto second = manager->start_completion(make_chat_request(session_b->id, "B1"), next_completion_id);
    if (!second) {
        return fail("Expected different-session request to queue while session A is active.");
    }

    if (executor.request_count() != 2) {
        return fail("Expected both active sessions to submit requests to the shared executor.");
    }

    zks::server::CompletionResult response;
    response.text = "ok";
    auto observed = finish_pending(*first, executor.request(0), response);
    if (!observed) {
        return fail("Expected session A completion to finish successfully.");
    }
    observed = finish_pending(*second, executor.request(1), response);
    if (!observed) {
        return fail("Expected session B completion to finish successfully.");
    }

    return 0;
}

int test_delete_active_session_cancels_request() {
    FakeExecutor executor;
    auto manager = make_session_manager(executor, "Base prompt", 64, 1);
    std::atomic<std::uint64_t> next_completion_id{1};

    auto created = manager->create_session({"local-model", std::nullopt});
    if (!created) {
        return fail("Expected session creation to succeed.");
    }

    auto pending = manager->start_completion(make_chat_request(created->id, "Hello"), next_completion_id);
    if (!pending) {
        return fail("Expected session completion to start before deletion.");
    }
    auto submitted = executor.request(0);

    auto deleted = manager->delete_session(created->id);
    if (!deleted) {
        return fail("Expected session deletion to succeed.");
    }
    if (!submitted->cancelled) {
        return fail("Deleting an active session should cancel the executor request.");
    }

    zks::server::CompletionResult response;
    response.text = "late answer";
    auto observed = finish_pending(*pending, submitted, response);
    if (!observed || observed->text != "late answer") {
        return fail("Expected deleted-session completion future to resolve.");
    }

    auto missing = manager->get_session(created->id);
    if (missing || missing.error().http_status != 404) {
        return fail("Deleted session should no longer be retrievable.");
    }
    return 0;
}

int test_session_capacity_is_enforced() {
    FakeExecutor executor;
    auto manager = make_session_manager(executor, "Base prompt", 64, 1);

    auto first = manager->create_session({"local-model", std::nullopt});
    if (!first) {
        return fail("Expected first session creation to succeed.");
    }

    auto second = manager->create_session({"local-model", std::nullopt});
    if (second || second.error().http_status != 503 ||
        second.error().code != std::optional<std::string>{"session_capacity_reached"}) {
        return fail("Expected session capacity to reject the second session.");
    }

    return 0;
}

int test_history_trims_oldest_turns() {
    FakeExecutor executor;
    auto manager = make_session_manager(executor, "Base prompt", 2);
    std::atomic<std::uint64_t> next_completion_id{1};

    auto created = manager->create_session({"local-model", std::nullopt});
    if (!created) {
        return fail("Expected session creation to succeed for history trimming test.");
    }

    auto first = manager->start_completion(make_chat_request(created->id, "One"), next_completion_id);
    if (!first) {
        return fail("Expected first history request to start.");
    }
    zks::server::CompletionResult response;
    response.text = "One answer";
    finish_pending(*first, executor.request(0), response);

    auto second = manager->start_completion(make_chat_request(created->id, "Two"), next_completion_id);
    if (!second) {
        return fail("Expected second history request to start.");
    }
    response.text = "Two answer";
    finish_pending(*second, executor.request(1), response);

    auto third = manager->start_completion(make_chat_request(created->id, "Three"), next_completion_id);
    if (!third) {
        return fail("Expected third history request to start.");
    }

    const std::vector<zks::server::ChatMessage> expected = {
        zks::server::ChatMessage::system("Base prompt"),
        zks::server::ChatMessage::user("Two"),
        zks::server::ChatMessage::assistant("Two answer"),
        zks::server::ChatMessage::user("Three"),
    };
    if (executor.request(2)->messages != expected) {
        return fail("Session history did not trim the oldest turn as expected.");
    }

    response.text = "Three answer";
    finish_pending(*third, executor.request(2), response);
    return 0;
}

} // namespace

int main() {
    if (auto status = test_create_session_and_commit_history(); status != 0) {
        return status;
    }
    if (auto status = test_failed_turn_does_not_mutate_history(); status != 0) {
        return status;
    }
    if (auto status = test_same_session_busy_but_different_sessions_can_queue(); status != 0) {
        return status;
    }
    if (auto status = test_delete_active_session_cancels_request(); status != 0) {
        return status;
    }
    if (auto status = test_session_capacity_is_enforced(); status != 0) {
        return status;
    }
    if (auto status = test_history_trims_oldest_turns(); status != 0) {
        return status;
    }

    return 0;
}
