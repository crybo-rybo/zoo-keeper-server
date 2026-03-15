#include "server/executor.hpp"

#include <gtest/gtest.h>

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
        ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "Timed out waiting for flag";
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace

TEST(ExecutorTest, SubmitRunsTaskOnWorkerThread) {
    zks::server::BoundedExecutor executor(1, 4);
    std::atomic<bool> ran{false};

    auto result = executor.submit([&ran] { ran.store(true, std::memory_order_release); });
    ASSERT_TRUE(result.has_value());

    busy_wait(ran);
    EXPECT_TRUE(ran.load());
}

TEST(ExecutorTest, SubmitReturnsErrorAfterStop) {
    zks::server::BoundedExecutor executor(1, 4);
    executor.stop();

    auto result = executor.submit([] {});
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("stop"), std::string::npos);
}

TEST(ExecutorTest, SubmitReturnsErrorWhenQueueFull) {
    zks::server::BoundedExecutor executor(1, 1);
    std::latch blocker(1);

    auto r1 = executor.submit([&blocker] { blocker.wait(); });
    ASSERT_TRUE(r1.has_value());

    // Give worker time to pick up the task
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto r2 = executor.submit([] {});
    ASSERT_TRUE(r2.has_value());

    auto r3 = executor.submit([] {});
    EXPECT_FALSE(r3.has_value());

    blocker.count_down();
}

TEST(ExecutorTest, StopWaitsForRunningTasks) {
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
    EXPECT_EQ(counter.load(), 4);
}

TEST(ExecutorTest, StopIsIdempotent) {
    zks::server::BoundedExecutor executor(1, 1);
    executor.stop();
    executor.stop(); // should not crash
}

TEST(ExecutorTest, DestructorCallsStop) {
    std::atomic<bool> ran{false};
    {
        zks::server::BoundedExecutor executor(1, 4);
        executor.submit([&ran] { ran.store(true, std::memory_order_release); });
    }
    EXPECT_TRUE(ran.load());
}

TEST(ExecutorTest, WorkerCountZeroNormalizesToOne) {
    zks::server::BoundedExecutor executor(0, 4);
    std::atomic<bool> ran{false};

    executor.submit([&ran] { ran.store(true, std::memory_order_release); });
    busy_wait(ran);
    EXPECT_TRUE(ran.load());
}

TEST(ExecutorTest, MaxPendingTasksZeroNormalizesToOne) {
    zks::server::BoundedExecutor executor(1, 0);
    std::atomic<bool> ran{false};

    executor.submit([&ran] { ran.store(true, std::memory_order_release); });
    busy_wait(ran);
    EXPECT_TRUE(ran.load());
}

TEST(ExecutorTest, ExceptionsInTasksDoNotKillWorkers) {
    zks::server::BoundedExecutor executor(1, 4);

    executor.submit([] { throw std::runtime_error("intentional"); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::atomic<bool> ran{false};
    executor.submit([&ran] { ran.store(true, std::memory_order_release); });
    busy_wait(ran);
    EXPECT_TRUE(ran.load());
}
