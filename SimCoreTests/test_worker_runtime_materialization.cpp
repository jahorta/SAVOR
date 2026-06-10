#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "Runner/Parallel/SimCoreDB/DBWorkflowWorkerCoordinator.h"

namespace fs = std::filesystem;
namespace coordinator = simcore::runner::parallel::simcoredb;

namespace {

fs::path WriteFile(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
    return path;
}

fs::path FreshTempDir(const char* name) {
    const auto root = fs::temp_directory_path() / "soasim_tests";
    const auto path = root / name;
    fs::remove_all(path);
    fs::create_directories(path);
    return path;
}

} // namespace

TEST(WorkflowWorkerRuntime, MaterializesSysPortableAndWorkerExeIntoRuntimeSlot) {
    const auto root = FreshTempDir("worker_runtime_materialization");
    const auto worker_exe = WriteFile(root / "source" / "SimCoreWorker.exe", "dummy worker exe");
    const auto dolphin_base = root / "dolphin_base";
    WriteFile(dolphin_base / "portable.txt", "");
    WriteFile(dolphin_base / "Sys" / "GC" / "dsp_coef.bin", "dummy dsp");
    WriteFile(dolphin_base / "User" / "Config" / "Dolphin.ini", "[Core]\nDummy=1\n");

    coordinator::DBWorkflowWorkerCoordinatorConfig cfg;
    cfg.worker_exe_path = worker_exe.string();
    cfg.dolphin_base_dir = dolphin_base.string();

    fs::path runtime_worker_exe;
    std::string err;
    ASSERT_TRUE(coordinator::DBWorkflowWorkerCoordinator::MaterializeWorkerRuntimeForTest(
        7,
        cfg,
        &runtime_worker_exe,
        &err)) << err;

    const auto slot_root = runtime_worker_exe.parent_path();
    EXPECT_EQ(runtime_worker_exe.filename(), "SimCoreWorker.exe");
    EXPECT_EQ(slot_root.filename(), "slot-7");
    EXPECT_TRUE(fs::is_regular_file(runtime_worker_exe));
    EXPECT_TRUE(fs::is_regular_file(slot_root / "Sys" / "GC" / "dsp_coef.bin"));
    EXPECT_TRUE(fs::is_regular_file(slot_root / "portable.txt"));
    EXPECT_TRUE(fs::is_directory(slot_root / "User"));
    EXPECT_TRUE(fs::is_regular_file(slot_root / "worker-runtime.manifest"));
}
