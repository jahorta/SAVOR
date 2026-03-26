#pragma once
#include <filesystem>
#include <optional>
#include <string>

#include "../Core/DolphinWrapper.h"
#include "../Core/Config/SimConfig.h"  // SimConfigIO::{DefaultConfigPath,Load,Save}

namespace simboot {

    // What to boot with.
    struct BootOptions {
        std::filesystem::path user_dir;          // isolated User/ for this simulator
        std::filesystem::path dolphin_qt_base;   // MUST be portable (contains portable.txt)
        bool force_resync_from_base = false;     // recopy Sys+User even if already synced
        bool visual = false;                     // request boot with a render-surface connection
        void* render_widget_handle = nullptr;    // Qt render widget native handle (HWND on Windows)
        bool save_config_on_success = true;      // write simulator.ini so next run auto-loads
        std::filesystem::path config_path = simcore::SimConfigIO::DefaultConfigPath(); // where to save
    };

    // Boot using explicit paths.
    bool BootDolphinWrapper(simcore::DolphinWrapper& dw, const BootOptions& opts, std::string* error_out = nullptr);

    // Boot by reading simulator.ini (created by prior successful boot).
    // Optional: override config_path (defaults to SimConfigIO::DefaultConfigPath()).
    bool BootDolphinWrapperFromSavedConfig(simcore::DolphinWrapper& dw, std::string* error_out = nullptr,
            const std::filesystem::path& config_path = simcore::SimConfigIO::DefaultConfigPath());

} // namespace simcore
