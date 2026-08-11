#pragma once

#include <cstdint>
#include <string>

namespace soa::battle::actions {

enum class BattleCommandPreparationError : std::uint32_t
{
    None = 0,
    NoValidTarget = 1,
    NotEnoughResource = 2,
    InvalidNavigation = 3,
    OutOfTurns = 4,
    BadBlob = 5,
    InvalidTurnIndexZero = 6,
};

inline std::string GetBattleCommandPreparationErrorString(
    BattleCommandPreparationError error)
{
    switch (error) {
    case BattleCommandPreparationError::None: return "No Error";
    case BattleCommandPreparationError::NoValidTarget: return "No Valid Target";
    case BattleCommandPreparationError::NotEnoughResource: return "Not Enough Resources";
    case BattleCommandPreparationError::InvalidNavigation: return "Invalid Menu Navigation";
    case BattleCommandPreparationError::OutOfTurns: return "Out of Turns";
    case BattleCommandPreparationError::BadBlob: return "Bad BattlePath Blob";
    case BattleCommandPreparationError::InvalidTurnIndexZero: return "Invalid Turn Number (0)";
    }
    return "Unknown Error";
}

} // namespace soa::battle::actions
