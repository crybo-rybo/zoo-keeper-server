#include "doctest.h"

#include "server/executor.hpp"

#include <atomic>
#include <chrono>
#include <latch>
#include <stdexcept>
#include <thread>

namespace {

constexpr auto kTimeout = std::chrono::seconds(5);

void busy_wait(const std::atomic<bool>& flag) {
    auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline) {
            FAIL("Timed out waiting for flag");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace

TEST_CASE("submit runs task on worker thread") {
    zks::server::BoundedExecutor executor(1, 4);
    std::atomic<bool> ran{false};

    auto result = executor.submit([&ran] {
        ran.store(true, std::memory_order_release);
    });
    CHECK(result.has_value());

    busy_wait(ran);
    CHECK(ran.load());
}

TEST_CASE("submit returns error after stop") {
    zks::server::BoundedExecutor executor(1, 4);
    executor.stop();

    auto result = executor.submit([] {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("stop") != std::string::npos);
}

TEST_CASE("submit returns error when queue is full") {
    zks::server::BoundedExecutor executor(1, 1);
    std::latch blocker(1);

    // Fill the worker with a blocking task
    auto r1 = executor.submit([&blocker] { blocker.wait(); });
    CHECK(r1.has_value());

    // Give worker time to pick up the task
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Fill the single pending slot
    auto r2 = executor.submit([] {});
    CHECK(r2.has_value());

    // Queue should now be full
    auto r3 = executor.submit([] {});
    CHECK_FALSE(r3.has_value());

    blocker.count_down();
}

TEST_CASE("stop waits for running tasks to complete") {
    std::atomic<int> counter{0};
    {
        zks::server::BoundedExecutor executor(4, 8);
        for (int i = 0; i < 4; ++i) {
            executor.submit([&counter] {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }
        executor.stop();
    }
    CHECK(counter.load() == 4);
}

TEST_CASE("stop is idempotent") {
    zks::server::BoundedExecutor executor(1, 1);
    executor.stop();
    executor.stop(); // should not crash
}

TEST_CASE("destructor calls stop") {
    std::atomic<bool> ran{false};
    {
        zks::server::BoundedExecutor executor(1, 4);
        executor.submit([&ran] {
            ran.store(true, std::memory_order_release);
        });
    } // destructor runs here
    CHECK(ran.load());
}

TEST_CASE("worker_count=0 normalizes to 1") {
    zks::server::BoundedExecutor executor(0, 4);
    std::atomic<bool> ran{false};

    executor.submit([&ran] {
        ran.store(true, std::memory_order_release);
    });
    busy_wait(ran);
    CHECK(ran.load());
}

TEST_CASE("max_pending_tasks=0 normalizes to 1") {
    zks::server::BoundedExecutor executor(1, 0);
    std::atomic<bool> ran{false};

    executor.submit([&ran] {
        ran.store(true, std::memory_order_release);
    });
    busy_wait(ran);
    CHECK(ran.load());
}

TEST_CASE("exceptions in tasks do not kill workers") {
    zks::server::BoundedExecutor executor(1, 4);

    executor.submit([] { throw std::runtime_error("intentional"); });

    // Give worker time to process the throwing task
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::atomic<bool> ran{false};
    executor.submit([&ran] {
        ran.store(true, std::memory_order_release);
    });
    busy_wait(ran);
    CHECK(ran.load());
}
