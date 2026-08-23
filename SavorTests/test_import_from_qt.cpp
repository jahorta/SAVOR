#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "Boot/Boot.h"

namespace fs = std::filesystem;

static fs::path mkfile(const fs::path& p, const char* text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
    return p;
}

static fs::path mktmp(const char* name) {
    auto base = fs::temp_directory_path() / "savor_test";
    fs::create_directories(base);
    auto d = base / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

TEST(SessionFilesystemPreparer, CreatesFreshEmptyUserIfPortable)
{
    // Fake a DolphinQt base
    fs::path qt = mktmp("qtbase");
    mkfile(qt / "portable.txt", ""); // mark portable
    mkfile(qt / "Sys" / "GC" / "dsp_coef.bin", "dummy");
    mkfile(qt / "User" / "Config" / "Dolphin.ini", "[Core]\nDummy=1\n");

    const fs::path worker_root = mktmp("user_isolated");
    const fs::path user = worker_root / "User";
    const auto prepared = simboot::SessionFilesystemPreparer::Prepare({
        .worker_id = 0,
        .process_generation = 1,
        .preparation_id = "import-test-1",
        .dolphin_qt_base = qt,
        .worker_root = worker_root,
    });
    ASSERT_TRUE(prepared.ok) << prepared.error;

    EXPECT_TRUE(fs::is_directory(user));
    EXPECT_TRUE(fs::is_empty(user));
    EXPECT_EQ(prepared.file_count, 0u);
    EXPECT_EQ(prepared.directory_count, 1u);
    EXPECT_EQ(prepared.byte_count, 0u);
}
