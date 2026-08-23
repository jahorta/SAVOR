#include "gtest/gtest.h"
#include "Core/DolphinWrapper.h"
#include "Boot/Boot.h"
#include "scoped_wrapper.h"

#include <filesystem>

using namespace savor;

TEST(Smoke, WrapperBuilds) {
    ScopedEmu emu;
    SUCCEED(); // If we link & construct, basic wiring works.
}

TEST(DiscProbe, RejectsMissingFile) {
    ScopedEmu emu;
    EXPECT_FALSE(emu.w.loadGame("X:\\definitely_not_here\\nope.iso"));
}

TEST(DiscProbe, AcceptsValidDiscIfPresent) {
    const char* iso = std::getenv("SAVOR_TEST_ISO");
    if (!iso) GTEST_SKIP() << "Set SAVOR_TEST_ISO to a valid image";

    auto userroot = std::filesystem::temp_directory_path() / "TestUser";
    auto userdir = userroot / "User";
    auto qtdir = std::filesystem::path("D:\\SoATAS\\dolphin-2506a-x64");

    ScopedEmu emu;
    std::string err;
    const auto prepared = simboot::SessionFilesystemPreparer::Prepare({
        .worker_id = 0,
        .process_generation = 1,
        .preparation_id = "disc-probe-1",
        .dolphin_qt_base = qtdir,
        .worker_root = userroot,
    });
    ASSERT_TRUE(prepared.ok) << prepared.error;
    ASSERT_TRUE(emu.w.SetUserDirectory(userdir)) << "Error setting User base dir";
    ASSERT_TRUE(emu.w.SetDolphinQtBaseDir(qtdir, &err)) << "Error setting Qt base dir: " << err;
    ASSERT_TRUE(emu.w.loadGame(iso));

    auto info = emu.w.getDiscInfo();
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->game_id.size(), 6u) << "GC IDs are 6 chars; Wii may still be 6";
    EXPECT_FALSE(info->region.empty());
}

TEST(Savestate, CanLoadStartingPoint) {
    const char* iso = std::getenv("SAVOR_TEST_ISO");
    const char* state = std::getenv("SAVOR_TEST_STATE");

    if (!iso || !state)
        GTEST_SKIP() << "Set SAVOR_TEST_ISO and SAVOR_TEST_STATE";

    auto userroot = std::filesystem::temp_directory_path() / "TestUser";
    auto userdir = userroot / "User";
    auto qtdir = std::filesystem::path("D:\\SoATAS\\dolphin-2506a-x64");

    ScopedEmu emu;
    std::string err;
    const auto prepared = simboot::SessionFilesystemPreparer::Prepare({
        .worker_id = 0,
        .process_generation = 2,
        .preparation_id = "savestate-2",
        .dolphin_qt_base = qtdir,
        .worker_root = userroot,
    });
    ASSERT_TRUE(prepared.ok) << prepared.error;
    ASSERT_TRUE(emu.w.SetUserDirectory(userdir)) << "Error setting User base dir";
    ASSERT_TRUE(emu.w.SetDolphinQtBaseDir(qtdir, &err)) << "Error setting Qt base dir: " << err;
    ASSERT_TRUE(emu.w.loadGame(iso));
    ASSERT_TRUE(emu.w.loadSavestate(state));

    // Dummy check: emulator should be running after savestate load
    EXPECT_TRUE(emu.w.isRunning());
}
