#include "SoldierAiModel.h"

namespace savor::predict {

int pre_ai_draws_for_fake_attacks(int fake_attacks_this_turn) {
    if (fake_attacks_this_turn <= 0) {
        return 3;
    }
    return fake_attacks_this_turn + 2;
}

int soldier_attack_param_from_rand(std::uint16_t value) {
    return (value % 10) > 3 ? 0 : 1;
}

SoldierAiDecision resolve_soldier_ai(int soldier_slot, std::uint32_t& state) {
    SoldierAiDecision decision;
    decision.slot = soldier_slot;

    const auto action = draw_rand15(state);
    state = action.next_state;
    decision.action_state = action.next_state;
    decision.action_rand = action.value;
    decision.attacks = (action.value % 100) >= 11;

    if (!decision.attacks) {
        return decision;
    }

    const auto target = draw_rand15(state);
    state = target.next_state;
    decision.target_state = target.next_state;
    decision.target_rand = target.value;
    decision.target_pc_slot = ((target.value % 2) == 0) ? 0 : 1;

    const auto attack_param = draw_rand15(state);
    state = attack_param.next_state;
    decision.attack_param_state = attack_param.next_state;
    decision.attack_param_rand = attack_param.value;

    return decision;
}

} // namespace savor::predict
