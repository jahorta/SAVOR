#pragma once

#include <cstdint>
#include <string_view>

namespace savor {

// Program kinds admitted by the homogeneous Full Phase runtime. Numeric
// identities are durable database identities, not worker capabilities.
enum : std::uint8_t {
    PK_None = 0,
    PK_SeedProbe = 1,
    PK_TasMovie = 2,
    PK_BattleContext = 4,
    PK_BattleSingleTurnRunner = 5,
    PK_BattleCompletion = 9,
    PK_BattleRecord = 10,
    PK_TasMovieCheckpointSterilize = 11,
    PK_BattleReplay = 12,
    PK_TasMovieAnnotate = 13,
    PK_TasMovieRevise = 14,
    PK_TasMovieCutscene = 15,
    // Reserved high-value diagnostic range. Do not renumber with production kinds.
    PK_TasMovieInputEpochBreakpointDiagnostic = 100,
};

[[nodiscard]] inline constexpr std::string_view ProgramKindDisplayName(
    const std::int32_t program_kind) noexcept
{
    switch (program_kind)
    {
    case PK_SeedProbe:
        return "SeedProbe";
    case PK_TasMovie:
        return "TAS Movie Complete Validation";
    case PK_BattleContext:
        return "Battle Context";
    case PK_BattleSingleTurnRunner:
        return "Battle Single Turn";
    case PK_BattleCompletion:
        return "Battle Completion";
    case PK_BattleRecord:
        return "Battle Recording";
    case PK_TasMovieCheckpointSterilize:
        return "TAS Movie Checkpoint Sterilization";
    case PK_BattleReplay:
        return "Battle Replay";
    case PK_TasMovieAnnotate:
        return "TAS Movie Input Epoch Annotation";
    case PK_TasMovieRevise:
        return "TAS Movie Input Epoch Rewrite";
    case PK_TasMovieCutscene:
        return "TAS Movie Cutscene";
    case PK_TasMovieInputEpochBreakpointDiagnostic:
        return "TAS Movie Input Epoch Breakpoint Diagnostic";
    default:
        return {};
    }
}

} // namespace savor
