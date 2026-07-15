#pragma once

#include <Core/Memory/Soa/SoaConstants.h>

#include <cstdint>
#include <optional>
#include <string>

namespace savor::predict {

enum class BattleInitialFacingStatus {
    Exact,
    Skipped,
    MissingInput,
};

struct BattleInitialFacingInput {
    int slot = -1;
    bool present = false;
    bool is_player = false;
    std::optional<soa::battle::TurnType> turn_type;
};

struct BattleInitialFacingResult {
    BattleInitialFacingStatus status = BattleInitialFacingStatus::MissingInput;
    std::optional<std::uint32_t> facing_angle_0x2c;
    std::string provenance;
    std::string detail;
};

BattleInitialFacingResult model_initial_battle_facing(
    const BattleInitialFacingInput& input);

const char* battle_initial_facing_status_name(BattleInitialFacingStatus status);

} // namespace savor::predict
