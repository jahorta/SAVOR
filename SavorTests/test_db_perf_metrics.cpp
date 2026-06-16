#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <thread>

#include <gtest/gtest.h>

#include "Common/QueuedDb.h"

namespace {

bool WaitForCondition(
    const std::function<bool()>& condition,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{ 1000 }) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{ 5 });
    }
    return condition();
}

const savor::db::core::DbOperationTelemetrySnapshot* FindOperation(
    const savor::db::core::QueuedDbLaneTelemetrySnapshot& snapshot,
    const std::string& operation_name) {
    for (const auto& operation : snapshot.operations) {
        if (operation.operation_name == operation_name) {
            return &operation;
        }
    }
    return nullptr;
}

} // namespace

TEST(DbPerfMetrics, QueuedLaneCapturesLatencyDepthAndRejectionsByOperation) {
    savor::db::core::QueuedDbLane lane("test-write", 1);
    std::string error;
    ASSERT_TRUE(lane.Start(&error)) << error;

    std::promise<void> release_first;
    auto release_future = release_first.get_future().share();
    std::atomic<bool> first_started{ false };
    std::atomic<int> completed{ 0 };

    ASSERT_TRUE(lane.Enqueue("Test.BlockingWrite", [&]() {
        first_started = true;
        release_future.wait();
        ++completed;
    }));
    ASSERT_TRUE(WaitForCondition([&]() { return first_started.load(); }));

    ASSERT_TRUE(lane.Enqueue("Test.QueuedWrite", [&]() {
        ++completed;
    }));
    EXPECT_FALSE(lane.Enqueue("Test.RejectedWrite", []() {}));

    auto queued_snapshot = lane.GetTelemetrySnapshot();
    EXPECT_EQ(queued_snapshot.capacity, 1u);
    EXPECT_EQ(queued_snapshot.depth, 1u);
    EXPECT_EQ(queued_snapshot.high_water_depth, 1u);
    EXPECT_EQ(queued_snapshot.rejected, 1u);
    EXPECT_EQ(queued_snapshot.at_capacity_rejections, 1u);
    ASSERT_NE(FindOperation(queued_snapshot, "Test.RejectedWrite"), nullptr);
    EXPECT_EQ(FindOperation(queued_snapshot, "Test.RejectedWrite")->rejected, 1u);

    release_first.set_value();
    ASSERT_TRUE(WaitForCondition([&]() { return completed.load() == 2; }));

    auto final_snapshot = lane.GetTelemetrySnapshot();
    EXPECT_EQ(final_snapshot.completed, 2u);
    EXPECT_EQ(final_snapshot.failed, 0u);

    const auto* blocking = FindOperation(final_snapshot, "Test.BlockingWrite");
    ASSERT_NE(blocking, nullptr);
    EXPECT_EQ(blocking->completed, 1u);
    EXPECT_EQ(blocking->end_to_end.count, 1u);

    const auto* queued = FindOperation(final_snapshot, "Test.QueuedWrite");
    ASSERT_NE(queued, nullptr);
    EXPECT_EQ(queued->completed, 1u);
    EXPECT_EQ(queued->queue_wait.count, 1u);

    lane.Stop();
}

TEST(DbPerfMetrics, QueuedTelemetryCombinesReadAndWriteLaneSnapshots) {
    savor::db::core::QueuedDbLane write_lane("test-write", 4);
    savor::db::core::QueuedDbLane read_lane("test-read", 4);
    std::string error;
    ASSERT_TRUE(write_lane.Start(&error)) << error;
    ASSERT_TRUE(read_lane.Start(&error)) << error;

    std::atomic<int> completed{ 0 };
    ASSERT_TRUE(write_lane.Enqueue("Test.Write", [&]() { ++completed; }));
    ASSERT_TRUE(read_lane.Enqueue("Test.Read", [&]() { ++completed; }));
    ASSERT_TRUE(WaitForCondition([&]() { return completed.load() == 2; }));

    const auto snapshot = savor::db::core::BuildQueuedDbTelemetrySnapshot(&write_lane, &read_lane);
    EXPECT_EQ(snapshot.write_completed, 1u);
    EXPECT_EQ(snapshot.read_completed, 1u);
    EXPECT_EQ(snapshot.write_capacity, 4u);
    EXPECT_EQ(snapshot.read_capacity, 4u);
    ASSERT_EQ(snapshot.write_lane.operations.size(), 1u);
    ASSERT_EQ(snapshot.read_lane.operations.size(), 1u);
    EXPECT_EQ(snapshot.write_lane.operations.front().operation_name, "Test.Write");
    EXPECT_EQ(snapshot.read_lane.operations.front().operation_name, "Test.Read");

    write_lane.Stop();
    read_lane.Stop();
}
