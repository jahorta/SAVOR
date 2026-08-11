#pragma once

#include "ActionTypes.h"
#include "../../Memory/Soa/Battle/BattleContext.h"

#include <cstdint>
#include <string>

namespace soa::battle::actions {

enum class BattlePlanValidationError : std::uint8_t
{
    None = 0,
    EmptyPlan,
    InvalidActor,
    DuplicateActor,
    InvalidTarget,
    TargetNotAlive,
    ItemUnavailable,
    UnsupportedCommand,
    FakeAttackCountOutOfRange,
};

struct BattlePlanValidationResult
{
    bool valid = false;
    BattlePlanValidationError error = BattlePlanValidationError::None;
    std::uint32_t command_ordinal = 0;
    std::string diagnostic;
    explicit operator bool() const noexcept { return valid; }
};

[[nodiscard]] BattlePlanValidationResult ValidateBattleTurnPlan(
    const soa::battle::ctx::BattleContext& context,
    const BattleTurnExecutionSpec& plan);

} // namespace soa::battle::actions
