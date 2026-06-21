#include "DropModel.h"

namespace savor::predict {

FirstBattleSoldierDropSimulation simulate_first_battle_soldier_drop(std::uint32_t state) {
    FirstBattleSoldierDropSimulation result;

    const auto first = draw_rand15(state);
    state = first.next_state;
    result.first_roll = first.value;
    result.draws_consumed = 1;

    if ((first.value % 100) < 1) {
        result.end_state = state;
        result.drop = FirstBattleSoldierDropKind::ElectriBox;
        result.item_id = 273;
        result.amount = 1;
        return result;
    }

    const auto second = draw_rand15(state);
    state = second.next_state;
    result.second_roll = second.value;
    result.draws_consumed = 2;

    if ((second.value % 100) < 1) {
        result.drop = FirstBattleSoldierDropKind::Moonberry;
        result.item_id = 258;
        result.amount = 1;
    }

    result.end_state = state;
    return result;
}

const char* first_battle_soldier_drop_name(FirstBattleSoldierDropKind drop) {
    switch (drop) {
    case FirstBattleSoldierDropKind::ElectriBox:
        return "Electri Box";
    case FirstBattleSoldierDropKind::Moonberry:
        return "Moonberry";
    case FirstBattleSoldierDropKind::None:
        return "None";
    }
    return "None";
}

} // namespace savor::predict
