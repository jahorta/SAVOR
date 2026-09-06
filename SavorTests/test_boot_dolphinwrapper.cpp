#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

#include "Boot/Boot.h"
#include "Core/HW/SI/SI_Device.h"    // SerialInterface::SIDevices
#include "Core/Config/MainSettings.h"
#include "Common/Config/Config.h"
#include "serial_guard.h"

namespace fs = std::filesystem;

static fs::path mkfile(const fs::path& p, const char* text) {
    fs::create_directories(p.parent_path());
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    ofs << text;
    return p;
}

static fs::path mktmpdir(const char* name) {
    auto base = fs::temp_directory_path() / "savor_tests";
    fs::create_directories(base);
    auto d = base / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

TEST(Boot, BootDolphinWrapper_UsesFreshUser)
{
    tests::SerialGuard guard;
    
    // 0) Fake a *portable* DolphinQt base
    const fs::path qt = mktmpdir("qt_portable_base");
    mkfile(qt / "portable.txt", "");
    // Minimal Sys plus deliberately contaminated base User contents.
    mkfile(qt / "Sys" / "GC" / "dsp_coef.bin", "dummy");
    mkfile(qt / "User" / "Config" / "import-me.ini", "contaminated");

    // 1) Our isolated user dir
    const fs::path user_root = mktmpdir("user_isolated");
    const fs::path user = user_root / "User";

    const auto prepared = simboot::SessionFilesystemPreparer::Prepare({
        .worker_id = 0,
        .process_generation = 1,
        .preparation_id = "boot-test-1",
        .dolphin_qt_base = qt,
        .worker_root = user_root,
    });
    ASSERT_TRUE(prepared.ok) << prepared.error;

    // 2) Boot with options
    simboot::BootOptions opts;
    opts.user_dir = user;
    opts.dolphin_qt_base = qt;
    opts.session_filesystem_preparation_id = "boot-test-1";
    opts.process_generation = 1;

    std::string err;
    savor::DolphinWrapper dw;

    ASSERT_TRUE(simboot::BootDolphinWrapper(dw, opts, &err)) << "Boot failed: " << err;

    // 3) Dolphin creates its own directory skeleton without importing base User.
    EXPECT_TRUE(fs::is_directory(dw.GetUserDirectory() / "Config"));
    EXPECT_FALSE(fs::exists(dw.GetUserDirectory() / "Config" / "import-me.ini"));
    EXPECT_EQ(fs::weakly_canonical(dw.GetDolphinQtBaseDir()),
        fs::weakly_canonical(qt));

    // 4) Port 1 must be Standard Controller when forcing that option
    using SerialInterface::SIDevices;
    const int sid0 = Config::Get(Config::GetInfoForSIDevice(0));
    EXPECT_EQ(sid0, static_cast<int>(SIDevices::SIDEVICE_GC_CONTROLLER));

}
