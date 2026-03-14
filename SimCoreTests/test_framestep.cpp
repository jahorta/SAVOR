#include "gtest/gtest.h"

#include "Core/DolphinWrapper.h"
#include "scoped_wrapper.h"

#include "Core/Core.h"   // for pausing the core

#include <optional>
#include <string>
#include <cstdlib>
#include "serial_guard.h"

using namespace simcore;

// --- small helpers ----------------------------------------------------------

static void force_paused(DolphinWrapper& w)
{
    // Put the core in Paused state so DoFrameStep produces exactly one Running->Paused cycle.
    Core::SetState(*w.system(), Core::State::Paused);

    // Give the emu thread a moment to obey; no busy-wait.
    for (int i = 0; i < 10; ++i)
    {
        if (Core::GetState(*w.system()) == Core::State::Paused) break;
        Core::HostDispatchJobs(*w.system());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

static std::optional<std::string> env_opt(const char* name)
{
    if (const char* v = std::getenv(name); v && *v) return std::string(v);
    return std::nullopt;
}

// --- tests ------------------------------------------------------------------

TEST(FrameStep, ReturnsFalseWhenCoreNotRunning)
{
    ScopedEmu emu;
    EXPECT_FALSE(emu.w.stepOneFrameBlocking(100)); // nothing booted -> should be false
}

TEST(FrameStep, BootGameThenStepOnce)
{
    tests::SerialGuard guard;
    
    auto iso = env_opt("SOASIM_TEST_ISO");
    if (!iso) GTEST_SKIP() << "Set SOASIM_TEST_ISO";

    auto userdir = std::filesystem::temp_directory_path() / "TestUser";
    auto qtdir = std::filesystem::path("D:\\SoATAS\\dolphin-2506a-x64");

    ScopedEmu emu;
    std::string err;
    ASSERT_TRUE(emu.w.SetUserDirectory(userdir)) << "Error setting User base dir";
    ASSERT_TRUE(emu.w.SetDolphinQtBaseDir(qtdir, &err)) << "Error setting Qt base dir: " << err;
    ASSERT_TRUE(emu.w.SyncFromDolphinQtBase(false, &err)) << "Error syncing Qt base dir: " << err;
    ASSERT_TRUE(emu.w.loadGame(iso->c_str()));        // boots emu core
    ASSERT_TRUE(emu.w.isRunning());

    force_paused(emu.w);

    const uint64_t before = emu.w.getFrameCountApprox();

    ASSERT_TRUE(emu.w.stepOneFrameBlocking(100000));  // should complete exactly one frame

    const uint64_t after = emu.w.getFrameCountApprox();
    EXPECT_GT(after, before) << "VI counter should advance";
}

TEST(FrameStep, LoadSavestateThenStepOnce)
{
    tests::SerialGuard guard;
    
    auto iso = env_opt("SOASIM_TEST_ISO");
    auto state = env_opt("SOASIM_TEST_STATE");
    if (!iso || !state) GTEST_SKIP() << "Set SOASIM_TEST_ISO and SOASIM_TEST_STATE";

    auto userdir = std::filesystem::temp_directory_path() / "TestUser";
    auto qtdir = std::filesystem::path("D:\\SoATAS\\dolphin-2506a-x64");

    ScopedEmu emu;
    std::string err;
    ASSERT_TRUE(emu.w.SetUserDirectory(userdir)) << "Error setting User base dir";
    ASSERT_TRUE(emu.w.SetDolphinQtBaseDir(qtdir, &err)) << "Error setting Qt base dir: " << err;
    ASSERT_TRUE(emu.w.SyncFromDolphinQtBase(false, &err)) << "Error syncing Qt base dir: " << err;
    ASSERT_TRUE(emu.w.loadGame(iso->c_str()));
    ASSERT_TRUE(emu.w.loadSavestate(state->c_str()));
    ASSERT_TRUE(emu.w.isRunning());

    force_paused(emu.w);

    const uint64_t before = emu.w.getFrameCountApprox();

    ASSERT_TRUE(emu.w.stepOneFrameBlocking(5000));

    const uint64_t after = emu.w.getFrameCountApprox();
    EXPECT_GT(after, before) << "VI counter should advance";
}
