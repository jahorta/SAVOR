#pragma once

#include <string>
#include <cstdint>

#include "../../Boot/Boot.h"

namespace simcore {

struct BootPlan {
    simboot::BootOptions boot;  // user_dir, dolphin_qt_base, force_p1_standard_pad, etc.
    std::string iso_path;       // game disc to load (no changes after start)
    bool visual = false;
    uint64_t render_widget_handle = 0;
};

} // namespace simcore
