#pragma once

#include <string_view>

namespace savor::predict {

inline constexpr std::string_view kFirstBattleSoldiersProfileName =
    "first-battle-soldiers";
inline constexpr std::string_view kLegacyFirstBattleProfileName =
    "first-battle";

inline bool is_first_battle_soldiers_profile_name(std::string_view name) {
    return name == kFirstBattleSoldiersProfileName
        || name == kLegacyFirstBattleProfileName;
}

} // namespace savor::predict
