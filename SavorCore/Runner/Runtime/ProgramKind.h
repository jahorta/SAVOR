#pragma once

#include <cstdint>

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

} // namespace savor
