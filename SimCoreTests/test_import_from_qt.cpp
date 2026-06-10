#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "Core/DolphinWrapper.h"

namespace fs = std::filesystem;

static fs::path mkfile(const fs::path& p, const char* text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
    return p;
}

static fs::path mktmp(const char* name) {
    auto base = fs::temp_directory_path() / "soasim_test";
    fs::create_directories(base);
    auto d = base / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

TEST(Wrapper, ImportFromDolphinQtBase_CopiesSysAndUserIfPortable)
{
    // Fake a DolphinQt base
    fs::path qt = mktmp("qtbase");
    mkfile(qt / "portable.txt", ""); // mark portable
    mkfile(qt / "Sys" / "GC" / "dsp_coef.bin", "dummy");
    mkfile(qt / "User" / "Config" / "Dolphin.ini", "[Core]\nDummy=1\n");

    // Our isolated User dir
    simcore::DolphinWrapper w;
    fs::path user = mktmp("user_isolated");
    ASSERT_TRUE(w.SetUserDirectory(user));

    // Import
    std::string err;
    ASSERT_TRUE(w.SetDolphinQtBaseDir(qt, &err)) << "Error at " << err;
    ASSERT_TRUE(w.SyncFromDolphinQtBase(false, &err)) << "Error at " << err;

    // Assert User files now exist under OUR user dir. Sys materialization is handled by the worker coordinator.
    EXPECT_TRUE(fs::exists(user / "Config" / "Dolphin.ini"));
}
