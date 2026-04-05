#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

#include "Boot/Boot.h"            // BootDolphinWrapper, BootOptions, BootDolphinWrapperFromSavedConfig
#include "Core/Config/SimConfig.h"   // SimConfigIO::Load
#include "Core/HW/SI/SI_Device.h"    // SerialInterface::SIDevices
#include "Core/Config/MainSettings.h"
#include "Common/Config/Config.h"
#include "seh_guard.h"
#include "serial_guard.h"

namespace fs = std::filesystem;

static fs::path mkfile(const fs::path& p, const char* text) {
    fs::create_directories(p.parent_path());
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    ofs << text;
    return p;
}

static fs::path mktmpdir(const char* name) {
    auto base = fs::temp_directory_path() / "soasim_tests";
    fs::create_directories(base);
    auto d = base / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

TEST(Boot, BootDolphinWrapper_SyncsPortableBaseAndSavesConfig)
{
    tests::SerialGuard guard;
    
    // 0) Fake a *portable* DolphinQt base
    const fs::path qt = mktmpdir("qt_portable_base");
    mkfile(qt / "portable.txt", "");
    // Minimal Sys + User contents
    mkfile(qt / "Sys" / "GC" / "dsp_coef.bin", "dummy");
    mkfile(qt / "User" / "Config" / "Dolphin.ini", "[Core]\nDummy=1\n");

    // 1) Our isolated user dir + config path
    const fs::path user = mktmpdir("user_isolated");
    const fs::path cfg_path = mktmpdir("cfg") / "simulator.ini";

    // 2) Boot with options
    simboot::BootOptions opts;
    opts.user_dir = user;
    opts.dolphin_qt_base = qt;
    opts.force_resync_from_base = true;     // make the copy explicit for the test
    opts.save_config_on_success = true;
    opts.config_path = cfg_path;

    std::string err;
    simcore::DolphinWrapper dw;

    ASSERT_TRUE(simboot::BootDolphinWrapper(dw, opts, &err)) << "Boot failed: " << err;

    // 3) Validate the wrapper paths
    EXPECT_TRUE(fs::exists(dw.GetUserDirectory() / "Sys" / "GC" / "dsp_coef.bin"));
    EXPECT_TRUE(fs::exists(dw.GetUserDirectory() / "Config" / "Dolphin.ini"));
    EXPECT_EQ(fs::weakly_canonical(dw.GetDolphinQtBaseDir()),
        fs::weakly_canonical(qt));

    // 4) Port 1 must be Standard Controller when forcing that option
    using SerialInterface::SIDevices;
    const int sid0 = Config::Get(Config::GetInfoForSIDevice(0));
    EXPECT_EQ(sid0, static_cast<int>(SIDevices::SIDEVICE_GC_CONTROLLER));

    // 5) Config file should have been written and loadable
    EXPECT_TRUE(fs::exists(cfg_path));
    auto loaded = simcore::SimConfigIO::Load(cfg_path, &err);
    ASSERT_TRUE(loaded.has_value()) << "Load config failed: " << err;
    EXPECT_EQ(fs::weakly_canonical(loaded->user_dir), fs::weakly_canonical(user));
    EXPECT_EQ(fs::weakly_canonical(loaded->dolphin_base_dir), fs::weakly_canonical(qt));
}

TEST(Boot, BootDolphinWrapperFromSavedConfig_Reloads)
{
    tests::SerialGuard guard;
    
    // 0) Prepare a saved config first
    const fs::path qt = mktmpdir("qt_portable_base2");
    mkfile(qt / "portable.txt", "");
    mkfile(qt / "Sys" / "GC" / "dsp_coef.bin", "dummy");
    mkfile(qt / "User" / "Config" / "Dolphin.ini", "[Core]\nDummy=1\n");

    const fs::path user = mktmpdir("user_isolated2");
    const fs::path cfg_path = mktmpdir("cfg2") / "simulator.ini";

    // Save a config
    simcore::SimConfig to_save{ user, qt };
    std::string err;
    ASSERT_TRUE(simcore::SimConfigIO::Save(to_save, cfg_path, &err)) << err;

    simcore::DolphinWrapper dw;

    // 1) Boot from that saved config
    ASSERT_TRUE(simboot::BootDolphinWrapperFromSavedConfig(dw, &err, cfg_path)) << "Boot-from-config failed: " << err;

    // 2) Validate copied files exist in our user dir
    EXPECT_TRUE(fs::exists(dw.GetUserDirectory() / "Sys" / "GC" / "dsp_coef.bin"));
    EXPECT_TRUE(fs::exists(dw.GetUserDirectory() / "Config" / "Dolphin.ini"));
}
