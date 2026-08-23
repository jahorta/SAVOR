#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "Execution/ProgramDB/ResultStagingCleanupService.h"
#include "Utils/Hash.h"
#include "common/RecordingExecutionDb.h"

namespace {

using namespace savor::db;
using namespace savor::db::execution::programdb;

class CleanupExecutionDb final : public RecordingExecutionDb {
public:
    explicit CleanupExecutionDb(ResultStagingCleanupRecord record)
        : record_(std::move(record)) {}

    std::optional<ClaimedResultStagingCleanup>
    ClaimNextResultStagingCleanup(
        const ClaimResultStagingCleanupCommand& command,
        std::string* error_out) override {
        std::lock_guard lock(mutex_);
        if (claimed_) {
            if (error_out) error_out->clear();
            return std::nullopt;
        }
        claimed_ = true;
        return ClaimedResultStagingCleanup{
            .cleanup = record_,
            .cleanup_token = command.cleanup_token,
            .cleanup_lease_expires_at_utc = command.lease_duration_ms,
        };
    }

    bool CompleteResultStagingCleanup(
        const CompleteResultStagingCleanupCommand& command,
        ExecutionDbOperationDisposition* disposition_out,
        std::string* error_out) override {
        {
            std::lock_guard lock(mutex_);
            completion_ = command;
        }
        if (disposition_out)
            *disposition_out = ExecutionDbOperationDisposition::Applied;
        if (error_out) error_out->clear();
        cv_.notify_all();
        return true;
    }

    std::optional<CompleteResultStagingCleanupCommand> WaitForCompletion() {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::seconds(2), [this]() {
            return completion_.has_value();
        });
        return completion_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    ResultStagingCleanupRecord record_;
    bool claimed_ = false;
    std::optional<CompleteResultStagingCleanupCommand> completion_;
};

class ResultStagingCleanupTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path()
            / ("savor-result-staging-test-"
                + std::to_string(std::chrono::steady_clock::now()
                    .time_since_epoch().count()));
        std::filesystem::create_directories(root_);
        ProgramKindDescriptor descriptor{};
        descriptor.program_kind = 9001;
        descriptor.program_name = "cleanup-test";
        descriptor.result_staging_root = root_;
        descriptor.default_progress_library_ids = std::vector<std::string>{};
        descriptor.default_derived_state_block_ids = std::vector<std::string>{};
        ASSERT_TRUE(registry_.Register(descriptor));
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    ResultStagingCleanupRecord Record(
        std::string relative,
        std::string sha,
        std::uint64_t size) const {
        return {
            .cleanup_id = 1,
            .job_id = 2,
            .terminal_sha256 = std::string(64, 'a'),
            .program_kind = 9001,
            .relative_path = std::move(relative),
            .expected_sha256 = std::move(sha),
            .expected_size_bytes = size,
            .cleanup_state = "PENDING",
        };
    }

    std::filesystem::path root_;
    ProgramKindRegistry registry_;
};

TEST_F(ResultStagingCleanupTest, DeletesMatchingFileAndEmptyParents) {
    const auto path = root_ / "request-1" / "capture" / "result.sav";
    std::filesystem::create_directories(path.parent_path());
    const std::string bytes = "durable-result-bytes";
    std::ofstream(path, std::ios::binary).write(bytes.data(), bytes.size());
    CleanupExecutionDb db(Record(
        "request-1/capture/result.sav",
        hash::sha256(bytes.data(), bytes.size()),
        bytes.size()));
    ResultStagingCleanupService service(&db, &registry_, {
        .poll_interval = std::chrono::milliseconds(5),
        .lease_duration = std::chrono::seconds(1),
    });
    ASSERT_TRUE(service.Start());
    const auto completion = db.WaitForCompletion();
    service.Stop();
    ASSERT_TRUE(completion.has_value());
    EXPECT_EQ(completion->completion, ResultStagingCleanupCompletion::Deleted);
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_FALSE(std::filesystem::exists(root_ / "request-1"));
}

TEST_F(ResultStagingCleanupTest, MissingFileCompletesSuccessfully) {
    CleanupExecutionDb db(Record(
        "request-2/capture/result.sav", std::string(64, 'b'), 12));
    ResultStagingCleanupService service(&db, &registry_, {
        .poll_interval = std::chrono::milliseconds(5),
        .lease_duration = std::chrono::seconds(1),
    });
    ASSERT_TRUE(service.Start());
    const auto completion = db.WaitForCompletion();
    service.Stop();
    ASSERT_TRUE(completion.has_value());
    EXPECT_EQ(completion->completion, ResultStagingCleanupCompletion::Deleted);
}

TEST_F(ResultStagingCleanupTest, HashMismatchBlocksAndPreservesFile) {
    const auto path = root_ / "request-3" / "capture" / "result.sav";
    std::filesystem::create_directories(path.parent_path());
    const std::string bytes = "unexpected-bytes";
    std::ofstream(path, std::ios::binary).write(bytes.data(), bytes.size());
    CleanupExecutionDb db(Record(
        "request-3/capture/result.sav", std::string(64, 'c'), bytes.size()));
    ResultStagingCleanupService service(&db, &registry_, {
        .poll_interval = std::chrono::milliseconds(5),
        .lease_duration = std::chrono::seconds(1),
    });
    ASSERT_TRUE(service.Start());
    const auto completion = db.WaitForCompletion();
    service.Stop();
    ASSERT_TRUE(completion.has_value());
    EXPECT_EQ(completion->completion, ResultStagingCleanupCompletion::Blocked);
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_TRUE(completion->cleanup_error.has_value());
}

} // namespace
