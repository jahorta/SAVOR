#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "../../../SavorCaptureFormat/CaptureFormat.h"
#include "../../../SavorProbe/ProbeProfile.h"

enum class CoreProgressFlags : std::uint32_t {
    ViDelta = 1 << 0,
    WarnViStall = 1 << 1,
    Filename = 1 << 2,
    ScriptSection = 1 << 3,
    BattleProgress = 1 << 4,
    PredicateProgress = 1 << 5,

    DontRecordHeartbeat = 1u << 31,
};

namespace savor::progress {

struct ProgressDeets {
    std::uint32_t poll_rate = 0;
    std::uint32_t flags = 0;

    void clear_flags() { flags = 0; }
    void set_flag(CoreProgressFlags flag) { flags |= static_cast<std::uint32_t>(flag); }
    void clear_flag(CoreProgressFlags flag) { flags &= ~static_cast<std::uint32_t>(flag); }
    bool has_flag(CoreProgressFlags flag) const {
        return (flags & static_cast<std::uint32_t>(flag)) != 0;
    }
};

struct FormattedProgress {
    std::string text;
    bool record_progress = true;
};

void append_battle_progress_probes(probe::Profile& profile);
std::optional<FormattedProgress> format_battle_progress(
    const capture_format::Event& event,
    bool record_progress);

} // namespace savor::progress
