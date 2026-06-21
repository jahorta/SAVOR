#pragma once

#include "RngCore.h"

#include <cstdint>
#include <optional>

namespace savor::predict {

enum class FirstBattleSoldierDropKind {
    None,
    ElectriBox,
    Moonberry,
};

struct FirstBattleSoldierDropSimulation {
    std::uint32_t end_state = 0;
    int draws_consumed = 0;
    std::uint16_t first_roll = 0;
    std::optional<std::uint16_t> second_roll;
    FirstBattleSoldierDropKind drop = FirstBattleSoldierDropKind::None;
    int item_id = -1;
    int amount = 0;
};

FirstBattleSoldierDropSimulation simulate_first_battle_soldier_drop(std::uint32_t state);
const char* first_battle_soldier_drop_name(FirstBattleSoldierDropKind drop);

} // namespace savor::predict
