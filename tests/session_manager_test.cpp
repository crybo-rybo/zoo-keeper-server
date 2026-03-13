#include "server/session_manager.hpp"

#include <atomic>
#include <expected>
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

struct SubmittedRequest {
    zoo::RequestId id = 0;
    std::vector<zoo::Message> messages;
    std::optional<std::function<void(std::string_view)>> callback;
    std::promise<zoo::Expected<zoo::Response>> promise;
    bool cancelled = false;
};

class FakeExecutor {
  public:
    zoo::RequestHandle start(std::vector<zoo::Message> messages,
                             std::optional<std::function<void(std::string_view)>> callback) {
        auto request = std::make_shared<SubmittedRequest>();
        request->id = next_request_id_++;
        request->messages = std::move(messages);
        request->callback = std::move(callback);

        auto future = request->promise.get_future();
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(request);
        return zoo::RequestHandle{request->id, std::move(future)};
    }

    void cancel(zoo::RequestId request_id) {
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
    zoo::RequestId next_request_id_ = 1;
    std::vector<std::shared_ptr<SubmittedRequest>> requests_;
};

zks::server::ChatCompletionRequest make_chat_request(const std::string& session_id,
                                                     const std::string& content) {
    zks::server::ChatCompletionRequest request;
    request.model = "local-model";
    request.session_id = session_id;
    request.messages.push_back(zoo::Message::user(content));
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
        [&executor](std::vector<zoo::Message> messages,
                    std::optional<std::function<void(std::string_view)>> callback) {
            return executor.start(std::move(messages), std::move(callback));
        },
        [&executor](zoo::RequestId request_id) { executor.cancel(request_id); });
}

zoo::Expected<zoo::Response> finish_pending(zks::server::PendingChatCompletion& pending,
                                            const std::shared_ptr<SubmittedRequest>& submitted,
                                            zoo::Expected<zoo::Response> result) {
    submitted->promise.set_value(std::move(result));
    auto observed = pending.handle.future.get();
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

    const std::vector<zoo::Message> expected_first = {
        zoo::Message::system("Base prompt\n\nSession prompt"),
        zoo::Message::user("Hello"),
    };
    if (executor.request(0)->messages != expected_first) {
        return fail("Session completion did not submit the expected first-turn history.");
    }

    zoo::Response response;
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

    const std::vector<zoo::Message> expected_second = {
        zoo::Message::system("Base prompt\n\nSession prompt"),
        zoo::Message::user("Hello"),
        zoo::Message::assistant("Hi there"),
        zoo::Message::user("Follow up"),
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

    auto error = std::unexpected(zoo::Error{zoo::ErrorCode::InferenceFailed, "boom"});
    auto observed = finish_pending(*pending, executor.request(0), error);
    if (observed) {
        return fail("Expected failed-turn completion to surface an error.");
    }

    auto retry = manager->start_completion(make_chat_request(created->id, "Retry"), next_completion_id);
    if (!retry) {
        return fail("Expected retry completion to start after failure.");
    }

    const std::vector<zoo::Message> expected_retry = {
        zoo::Message::system("Base prompt"),
        zoo::Message::user("Retry"),
    };
    if (executor.request(1)->messages != expected_retry) {
        return fail("Failed turn was incorrectly committed to retained history.");
    }

    zoo::Response response;
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

    zoo::Response response;
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
        return fail("Expected session creation to succeed for delete test.");
    }

    auto pending = manager->start_completion(make_chat_request(created->id, "Hello"), next_completion_id);
    if (!pending) {
        return fail("Expected active request before deleting session.");
    }

    auto deleted = manager->delete_session(created->id);
    if (!deleted) {
        return fail("Expected delete_session to succeed.");
    }
    if (!executor.request(0)->cancelled) {
        return fail("Deleting an active session should cancel the shared-agent request.");
    }

    zoo::Response response;
    response.text = "late answer";
    auto observed = finish_pending(*pending, executor.request(0), response);
    if (!observed || observed->text != "late answer") {
        return fail("Expected deleted-session completion future to resolve.");
    }

    auto missing = manager->get_session(created->id);
    if (missing || missing.error().http_status != 404) {
        return fail("Deleted session should no longer be retrievable.");
    }

    return 0;
}

int test_history_trimming_preserves_exchange_boundaries() {
    FakeExecutor executor;
    auto manager = make_session_manager(executor, "Base prompt", 2);
    std::atomic<std::uint64_t> next_completion_id{1};

    auto created = manager->create_session({"local-model", std::nullopt});
    if (!created) {
        return fail("Expected session creation to succeed for trimming test.");
    }

    zoo::Response response;
    response.text = "One answer";
    auto first = manager->start_completion(make_chat_request(created->id, "One"), next_completion_id);
    if (!first) {
        return fail("Expected first trimmed-history request to start.");
    }
    if (!finish_pending(*first, executor.request(0), response)) {
        return fail("Expected first trimmed-history request to succeed.");
    }

    response.text = "Two answer";
    auto second = manager->start_completion(make_chat_request(created->id, "Two"), next_completion_id);
    if (!second) {
        return fail("Expected second trimmed-history request to start.");
    }
    if (!finish_pending(*second, executor.request(1), response)) {
        return fail("Expected second trimmed-history request to succeed.");
    }

    auto third = manager->start_completion(make_chat_request(created->id, "Three"), next_completion_id);
    if (!third) {
        return fail("Expected third trimmed-history request to start.");
    }

    const std::vector<zoo::Message> expected = {
        zoo::Message::system("Base prompt"),
        zoo::Message::user("Two"),
        zoo::Message::assistant("Two answer"),
        zoo::Message::user("Three"),
    };
    if (executor.request(2)->messages != expected) {
        return fail("History trimming did not preserve the newest full exchange.");
    }

    response.text = "Three answer";
    if (!finish_pending(*third, executor.request(2), response)) {
        return fail("Expected third trimmed-history request to succeed.");
    }

    return 0;
}

} // namespace

int main() {
    if (int rc = test_create_session_and_commit_history(); rc != 0) {
        return rc;
    }
    if (int rc = test_failed_turn_does_not_mutate_history(); rc != 0) {
        return rc;
    }
    if (int rc = test_same_session_busy_but_different_sessions_can_queue(); rc != 0) {
        return rc;
    }
    if (int rc = test_delete_active_session_cancels_request(); rc != 0) {
        return rc;
    }
    if (int rc = test_history_trimming_preserves_exchange_boundaries(); rc != 0) {
        return rc;
    }
    return 0;
}
