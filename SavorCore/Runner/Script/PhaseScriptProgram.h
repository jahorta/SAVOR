#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../Breakpoints/BpRegistry.h"
#include "PhaseScriptOpcodes.h"
#include "PSContext.h"

namespace savor {

struct PhaseScript {
    std::vector<BPKey> canonical_bp_keys;
    std::vector<BPKey> gated_bp_keys;
    std::vector<PSOp> ops;
};

enum DBuf : uint8_t {
    DK_None = 0,
    DK_Battle = 1,
    DK_Explore = 2,
};

struct PSInit {
    std::string savestate_path;
    DBuf derived_buffer_type{ DBuf::DK_None };
};

struct PSJob {
    std::vector<uint8_t> payload;
    PSContext ctx;
};

struct PSResult {
    bool ok{ false };
    uint8_t w_err{ 0 };
    PSContext ctx;
};

enum class RunToBpOutcome : uint32_t {
    Hit = 0,
    ReservedLegacyTimeout = 1,
    ReservedLegacyViStall = 2,
    MovieEnded = 3,
    Aborted = 4,
    InputPlaybackFailed = 5,
    Unknown = 0xffffffff,
};

inline const char* RunToBpOutcomeToString(uint32_t outcome)
{
    switch (static_cast<RunToBpOutcome>(outcome)) {
    case RunToBpOutcome::Hit: return "Finished";
    case RunToBpOutcome::ReservedLegacyTimeout: return "ReservedLegacyTimeout";
    case RunToBpOutcome::ReservedLegacyViStall: return "ReservedLegacyViStall";
    case RunToBpOutcome::MovieEnded: return "MovieEnded";
    case RunToBpOutcome::Aborted: return "Aborted";
    case RunToBpOutcome::InputPlaybackFailed: return "InputPlaybackFailed";
    case RunToBpOutcome::Unknown: return "Unknown";
    default: return "UnrecognizedRunOutcome";
    }
}

} // namespace savor
