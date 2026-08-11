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
    PK_TasMovieCheckpointSterilize = 11,
};

} // namespace savor
