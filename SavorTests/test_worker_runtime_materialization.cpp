#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "Execution/DBWorkflowWorkerCoordinator.h"

namespace fs = std::filesystem;
namespace coordinator = savor::runner::parallel::savordb;

namespace {

fs::path WriteFile(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
    return path;
}

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>()};
}

fs::path FreshTempDir(const char* name) {
    const auto root = fs::temp_directory_path() / "savor_tests";
    const auto path = root / name;
    fs::remove_all(path);
    fs::create_directories(path);
    return path;
}

struct RuntimeFixture {
    fs::path root;
    fs::path worker_exe;
    fs::path dolphin_base;
    coordinator::DBWorkflowWorkerCoordinatorConfig config;
};

RuntimeFixture MakeRuntimeFixture(const char* name) {
    RuntimeFixture fixture;
    fixture.root = FreshTempDir(name);
    fixture.worker_exe = WriteFile(
        fixture.root / "source" / "SavorWorker.exe",
        "dummy worker executable");
    fixture.dolphin_base = fixture.root / "dolphin_base";
    WriteFile(fixture.dolphin_base / "portable.txt", "");
    WriteFile(
        fixture.dolphin_base / "Sys" / "GC" / "dsp_coef.bin",
        "dummy dsp coefficients");
    WriteFile(
        fixture.dolphin_base / "Sys" / "Resources" / "runtime-marker.dat",
        "runtime marker v1");

    // A populated source User tree must never seed the shared runtime template.
    WriteFile(
        fixture.dolphin_base / "User" / "Config" / "Dolphin.ini",
        "[Core]\nBackgroundInput=True\n");
    WriteFile(
        fixture.dolphin_base / "User" / "Wii" / "fst.bin",
        "stale source fst");

    fixture.config.worker_exe_path = fixture.worker_exe.string();
    fixture.config.dolphin_base_dir = fixture.dolphin_base.string();
    fixture.config.worker_binary_runtime_root =
        (fixture.root / "runtime-cache").string();
    return fixture;
}

struct RuntimeMaterializationResult {
    bool ok = false;
    fs::path worker_exe;
    std::string error;
};

RuntimeMaterializationResult Materialize(
    size_t worker_idx,
    const coordinator::DBWorkflowWorkerCoordinatorConfig& config) {
    RuntimeMaterializationResult result;
    result.ok = coordinator::DBWorkflowWorkerCoordinator::MaterializeWorkerRuntimeForTest(
        worker_idx,
        config,
        &result.worker_exe,
        &result.error);
    return result;
}

void ExpectCompleteRuntime(const fs::path& runtime_worker_exe) {
    const auto runtime_root = runtime_worker_exe.parent_path();
    EXPECT_EQ(runtime_worker_exe.filename(), "SavorWorker.exe");
    EXPECT_TRUE(fs::is_regular_file(runtime_worker_exe));
    EXPECT_TRUE(fs::is_regular_file(runtime_root / "Sys" / "GC" / "dsp_coef.bin"));
    EXPECT_TRUE(fs::is_regular_file(
        runtime_root / "Sys" / "Resources" / "runtime-marker.dat"));
    EXPECT_TRUE(fs::is_regular_file(runtime_root / "portable.txt"));
    EXPECT_TRUE(fs::is_regular_file(runtime_root / "worker-runtime.manifest"));
    EXPECT_TRUE(fs::is_directory(runtime_root / "User"));
}

} // namespace

TEST(WorkflowWorkerRuntime, AllWorkerIndicesUseOneCommonRuntimeAndEmptyUserTemplate) {
    const auto fixture = MakeRuntimeFixture("worker_runtime_common_image");

    const auto first = Materialize(0, fixture.config);
    ASSERT_TRUE(first.ok) << first.error;
    const auto last = Materialize(19, fixture.config);
    ASSERT_TRUE(last.ok) << last.error;

    EXPECT_EQ(last.worker_exe, first.worker_exe);
    ExpectCompleteRuntime(first.worker_exe);
    EXPECT_EQ(first.worker_exe.parent_path().filename(), "runtime");
    const auto fingerprint_root = first.worker_exe.parent_path().parent_path();
    EXPECT_FALSE(fs::exists(fingerprint_root / "slot-0"));
    EXPECT_FALSE(fs::exists(fingerprint_root / "slot-19"));

    const auto manifest = ReadFile(
        first.worker_exe.parent_path() / "worker-runtime.manifest");
    EXPECT_NE(
        manifest.find("savor_worker_runtime_manifest_version=3\n"),
        std::string::npos);
    EXPECT_EQ(manifest.find("slot_id="), std::string::npos);

    const auto runtime_user = first.worker_exe.parent_path() / "User";
    EXPECT_TRUE(fs::is_empty(runtime_user));
    EXPECT_FALSE(fs::exists(runtime_user / "Config" / "Dolphin.ini"));
    EXPECT_FALSE(fs::exists(runtime_user / "Wii" / "fst.bin"));
}

TEST(WorkflowWorkerRuntime, RepeatedMaterializationReusesTheSameCompleteRuntime) {
    const auto fixture = MakeRuntimeFixture("worker_runtime_repeated");

    const auto initial = Materialize(7, fixture.config);
    ASSERT_TRUE(initial.ok) << initial.error;

    for (size_t attempt = 0; attempt < 5; ++attempt) {
        const auto repeated = Materialize(attempt, fixture.config);
        ASSERT_TRUE(repeated.ok) << repeated.error;
        EXPECT_EQ(repeated.worker_exe, initial.worker_exe);
        ExpectCompleteRuntime(repeated.worker_exe);
        EXPECT_TRUE(fs::is_empty(repeated.worker_exe.parent_path() / "User"));
    }
}

TEST(WorkflowWorkerRuntime, ConcurrentMaterializationPublishesOneCompleteRuntime) {
    const auto fixture = MakeRuntimeFixture("worker_runtime_concurrent");
    constexpr size_t kWorkerCount = 20;

    std::atomic<bool> start{false};
    std::vector<std::future<RuntimeMaterializationResult>> futures;
    futures.reserve(kWorkerCount);
    for (size_t worker_idx = 0; worker_idx < kWorkerCount; ++worker_idx) {
        futures.push_back(std::async(
            std::launch::async,
            [worker_idx, &fixture, &start]() {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                return Materialize(worker_idx, fixture.config);
            }));
    }
    start.store(true, std::memory_order_release);

    std::vector<RuntimeMaterializationResult> results;
    results.reserve(kWorkerCount);
    for (auto& future : futures) {
        results.push_back(future.get());
    }

    ASSERT_FALSE(results.empty());
    for (const auto& result : results) {
        ASSERT_TRUE(result.ok) << result.error;
        EXPECT_EQ(result.worker_exe, results.front().worker_exe);
        ExpectCompleteRuntime(result.worker_exe);
    }
    EXPECT_TRUE(fs::is_empty(results.front().worker_exe.parent_path() / "User"));
}

TEST(WorkflowWorkerRuntime, SysSourceChangeSelectsANewRuntimeFingerprint) {
    auto fixture = MakeRuntimeFixture("worker_runtime_source_change");

    const auto initial = Materialize(0, fixture.config);
    ASSERT_TRUE(initial.ok) << initial.error;

    WriteFile(
        fixture.dolphin_base / "Sys" / "Resources" / "runtime-marker.dat",
        "runtime marker v2 with different content and size");

    const auto updated = Materialize(0, fixture.config);
    ASSERT_TRUE(updated.ok) << updated.error;
    EXPECT_NE(updated.worker_exe, initial.worker_exe);
    ExpectCompleteRuntime(updated.worker_exe);

    const auto marker = ReadFile(
        updated.worker_exe.parent_path()
        / "Sys" / "Resources" / "runtime-marker.dat");
    EXPECT_EQ(marker, "runtime marker v2 with different content and size");
}

TEST(WorkflowWorkerRuntime, WorkerExecutableChangePublishesANewCopiedSnapshot) {
    auto fixture = MakeRuntimeFixture("worker_runtime_exe_change");

    const auto initial = Materialize(0, fixture.config);
    ASSERT_TRUE(initial.ok) << initial.error;
    EXPECT_EQ(ReadFile(initial.worker_exe), "dummy worker executable");

    WriteFile(fixture.worker_exe, "replacement worker executable with a new build");

    const auto updated = Materialize(19, fixture.config);
    ASSERT_TRUE(updated.ok) << updated.error;
    EXPECT_NE(updated.worker_exe, initial.worker_exe);
    EXPECT_EQ(ReadFile(updated.worker_exe), "replacement worker executable with a new build");
    EXPECT_EQ(ReadFile(initial.worker_exe), "dummy worker executable");
}

TEST(WorkflowWorkerRuntime, InvalidPublishedRuntimeFailsClosedWithoutDeletingIt) {
    const auto fixture = MakeRuntimeFixture("worker_runtime_invalid_published");

    const auto initial = Materialize(0, fixture.config);
    ASSERT_TRUE(initial.ok) << initial.error;
    const auto runtime_root = initial.worker_exe.parent_path();
    WriteFile(runtime_root / "User" / "unexpected-state.bin", "must not be shared");

    const auto repeated = Materialize(1, fixture.config);
    EXPECT_FALSE(repeated.ok);
    EXPECT_NE(repeated.error.find("left untouched"), std::string::npos);
    EXPECT_TRUE(fs::is_regular_file(initial.worker_exe));
    EXPECT_EQ(
        ReadFile(runtime_root / "User" / "unexpected-state.bin"),
        "must not be shared");
}
