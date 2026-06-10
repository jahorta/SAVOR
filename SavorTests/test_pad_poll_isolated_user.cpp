#include <gtest/gtest.h>
#include <filesystem>
#include <chrono>
#include <thread>

#include "serial_guard.h"
#include "Core/DolphinWrapper.h"
#include "InputCommon/GCPadStatus.h"
#include "Core/Input/InputPlan.h"

using namespace std::chrono_literals;
namespace fs = std::filesystem;

static fs::path MakeTempUserDir(const char* name) {
    auto base = fs::temp_directory_path() / "savor_user";
    fs::create_directories(base);
    auto ud = base / name;
    fs::remove_all(ud);
    fs::create_directories(ud / "Config");
    return ud;
}

TEST(Dolphin, PollsPad_UsesIsolatedUserDir)
{
    tests::SerialGuard guard;

    // 1) Point wrapper at an isolated User dir
    savor::DolphinWrapper dw;
    const fs::path userdir = MakeTempUserDir("xinput0_isolated");
    auto qtdir = std::filesystem::path("D:\\SoATAS\\dolphin-2506a-x64");

    std::string err;
    ASSERT_TRUE(dw.SetUserDirectory(userdir)) << "Error setting User base dir";
    ASSERT_TRUE(dw.SetDolphinQtBaseDir(qtdir, &err)) << "Error setting Qt base dir: " << err;
    ASSERT_TRUE(dw.SyncFromDolphinQtBase(false, &err)) << "Error syncing Qt base dir: " << err;

    const char* iso = std::getenv("SAVOR_TEST_ISO");
    if (!iso) GTEST_SKIP() << "Set SAVOR_TEST_ISO to a valid image";
    ASSERT_TRUE(dw.loadGame(iso));
    ASSERT_TRUE(dw.isRunning());

    dw.ConfigurePortsStandardPadP1();

    // 5) Verify neutral is �A up�
    GCPadStatus st{};
    dw.stepOneFrameBlocking(5000);
    dw.stepOneFrameBlocking(5000);
    ASSERT_TRUE(dw.QueryPadStatus(0, &st));
    EXPECT_EQ((st.button & 0x0100), 0u);  // PAD_BUTTON_A bit

    // 6) Press A via GCPadOverride and confirm Dolphin reads it
    dw.setInputPlan( savor::InputPlan{
        savor::GCInputFrame(/*buttons=*/0x0100, /*lt=*/0, /*rt=*/0, /*lx=*/0, /*ly=*/0, /*rx=*/0, /*ry=*/0)
        } );
    dw.applyNextInputFrame();
    dw.stepOneFrameBlocking(5000);
    dw.stepOneFrameBlocking(5000);
    ASSERT_TRUE(dw.QueryPadStatus(0, &st));
    EXPECT_NE((st.button & 0x0100), 0u);

}