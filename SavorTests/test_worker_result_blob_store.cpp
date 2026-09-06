#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "Execution/WorkerResultBlobStore.h"
#include "Utils/Hash.h"
#include "Utils/FilesystemPath.h"

namespace {

std::filesystem::path ExtendedAbsolutePath(
    const std::filesystem::path& path) {
    std::filesystem::path resolved;
    EXPECT_TRUE(savor::filesystem::ResolveNativeIoPath(path, &resolved));
    return resolved;
}

class TemporaryBlobStoreDirectory final {
public:
    explicit TemporaryBlobStoreDirectory(bool make_long_root = false) {
        static std::atomic<std::uint64_t> counter = 0;
        base_ = std::filesystem::temp_directory_path()
            / ("savor-worker-result-blob-store-"
               + std::to_string(
                   std::chrono::steady_clock::now()
                       .time_since_epoch()
                       .count())
               + "-" + std::to_string(counter.fetch_add(1)));
        root_ = base_ / "object_store";
        if (make_long_root) {
            while (root_.native().size() < 300) {
                root_ /= "long-worker-result-storage-component-0123456789";
            }
        }
    }

    ~TemporaryBlobStoreDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(
            ExtendedAbsolutePath(base_),
            ignored);
    }

    TemporaryBlobStoreDirectory(
        const TemporaryBlobStoreDirectory&) = delete;
    TemporaryBlobStoreDirectory& operator=(
        const TemporaryBlobStoreDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& base() const noexcept {
        return base_;
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

private:
    std::filesystem::path base_;
    std::filesystem::path root_;
};

savor::db::execution::WorkerResultBlobStageRequest MakeStageRequest(
    std::int64_t workset_id,
    std::int64_t dispatch_attempt_id,
    std::int64_t job_id,
    std::uint64_t terminal_id,
    std::span<const std::uint8_t> envelope) {
    return savor::db::execution::WorkerResultBlobStageRequest{
        .workset_id = workset_id,
        .dispatch_attempt_id = dispatch_attempt_id,
        .job_id = job_id,
        .terminal_id = terminal_id,
        .envelope = envelope,
    };
}

TEST(
    WorkerResultBlobStore,
    UsesCompactContentAddressAndReferenceCountedPublisherPins) {
    TemporaryBlobStoreDirectory temporary;
    savor::db::execution::WorkerResultBlobStore store(temporary.root());
    ASSERT_TRUE(store.ValidateReady());

    const std::array<std::uint8_t, 7> envelope{
        0x53, 0x41, 0x56, 0x4f, 0x52, 0x01, 0x02};
    const auto expected_sha256 =
        ::hash::sha256(envelope.data(), envelope.size());
    const auto expected_relative_path =
        "worker_results/" + expected_sha256 + ".wrms-terminal-v1";

    savor::db::execution::WorkerResultBlobReference first;
    std::string error;
    ASSERT_TRUE(
        store.Stage(
            MakeStageRequest(1, 2, 3, 4, envelope),
            &first,
            &error))
        << error;
    EXPECT_EQ(first.relative_path, expected_relative_path);
    EXPECT_EQ(first.sha256, expected_sha256);

    savor::db::execution::WorkerResultBlobReference second;
    ASSERT_TRUE(
        store.Stage(
            MakeStageRequest(10, 20, 30, 40, envelope),
            &second,
            &error))
        << error;
    EXPECT_EQ(second.relative_path, first.relative_path);

    std::vector<std::uint8_t> round_trip;
    ASSERT_TRUE(store.Read(second, &round_trip, &error)) << error;
    EXPECT_EQ(
        round_trip,
        std::vector<std::uint8_t>(envelope.begin(), envelope.end()));

    store.Unpin(first.relative_path);
    EXPECT_FALSE(store.Remove(first.relative_path, &error));
    EXPECT_NE(error.find("still pinned"), std::string::npos);

    store.Unpin(second.relative_path);
    ASSERT_TRUE(store.Remove(second.relative_path, &error)) << error;
    EXPECT_TRUE(store.Remove(second.relative_path, &error))
        << "missing removal must remain idempotent: " << error;
}

TEST(
    WorkerResultBlobStore,
    PreflightAndBlobOperationsSupportExtendedLengthWindowsPaths) {
    TemporaryBlobStoreDirectory temporary(/*make_long_root=*/true);
    const auto production_staging_path =
        temporary.root() / "worker_results"
        / (std::string(64, 'f')
           + ".wrms-terminal-v1.staging");
#ifdef _WIN32
    ASSERT_GT(production_staging_path.native().size(), 260u);
#endif

    savor::db::execution::WorkerResultBlobStore store(temporary.root());
    std::string error;
    ASSERT_TRUE(store.ValidateReady(&error)) << error;

    std::vector<std::string> files;
    ASSERT_TRUE(
        store.ListFilesOlderThan(
            std::chrono::milliseconds::zero(),
            &files,
            &error))
        << error;
    EXPECT_TRUE(files.empty())
        << "readiness validation must delete its probe";

    const std::array<std::uint8_t, 5> envelope{
        0xde, 0xad, 0xbe, 0xef, 0x01};
    savor::db::execution::WorkerResultBlobReference reference;
    ASSERT_TRUE(
        store.Stage(
            MakeStageRequest(101, 202, 303, 404, envelope),
            &reference,
            &error))
        << error;

    std::vector<std::uint8_t> round_trip;
    ASSERT_TRUE(store.Read(reference, &round_trip, &error)) << error;
    EXPECT_EQ(
        round_trip,
        std::vector<std::uint8_t>(envelope.begin(), envelope.end()));

    files.clear();
    ASSERT_TRUE(
        store.ListFilesOlderThan(
            std::chrono::milliseconds::zero(),
            &files,
            &error))
        << error;
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front(), reference.relative_path);

    store.Unpin(reference.relative_path);
    ASSERT_TRUE(store.Remove(reference.relative_path, &error)) << error;
}

TEST(
    WorkerResultBlobStore,
    PreflightFailureReportsOperationPathLengthAndOperatingSystemError) {
    TemporaryBlobStoreDirectory temporary;
    std::filesystem::create_directories(temporary.root());
    {
        std::ofstream blocker(
            temporary.root() / "worker_results",
            std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(blocker);
        blocker << "not a directory";
    }

    savor::db::execution::WorkerResultBlobStore store(temporary.root());
    std::string error;
    EXPECT_FALSE(store.ValidateReady(&error));
    EXPECT_NE(
        error.find(
            "creating worker result directory during readiness validation"),
        std::string::npos)
        << error;
    EXPECT_NE(error.find("path_length="), std::string::npos) << error;
    EXPECT_NE(error.find("os_error="), std::string::npos) << error;
}

} // namespace
