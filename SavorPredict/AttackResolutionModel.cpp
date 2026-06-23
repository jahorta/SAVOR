#include "AttackResolutionModel.h"

namespace savor::predict {

namespace {
constexpr double kGuaranteedDamageMultiplier = 0.95;
constexpr double kPotentialDamageMultiplier = 0.05;
constexpr double kRandUnitMultiplier = 1.0 / 32767.0;

void append_damage_draws(
    BasicAttackSimulation& result,
    std::uint32_t& state,
    const BasicAttackInputs& inputs) {
    auto spread = draw_rand15(state);
    state = spread.next_state;
    auto bonus = draw_rand15(state);
    state = bonus.next_state;
    result.damage_spread_rand = spread.value;
    result.damage_bonus_rand = bonus.value;
    result.damage_draws_spent = true;
    result.draws_consumed += 2;

    const int target_defense = (result.attack_result == 2) ? 0 : inputs.target_defense;
    result.base_damage = (inputs.attacker_attack * 2) - target_defense;
    result.damage = roll_damage_from_draws(
        result.base_damage,
        inputs.attacker_element,
        inputs.target_element_effectiveness_tenths,
        spread.value,
        bonus.value,
        (inputs.target_status_flags & 1) != 0);
    if (result.attack_result == 2 && result.damage == 0) {
        result.damage = 1;
    }
}

void apply_target_hit_check_overrides(BasicAttackSimulation& result, const BasicAttackInputs& inputs) {
    result.hit_check = result.attack_result;
    if (result.attack_result != 0) {
        if ((inputs.target_status_flags & 4) != 0) {
            result.hit_check = 4;
        } else if ((inputs.target_status_flags & 1) != 0) {
            result.hit_check = 3;
        }
    }
}
}

int attack_hit_threshold(int attacker_hit, int target_dodge) {
    return (100 - attacker_hit + target_dodge) / 2;
}

int attack_result_from_draw(
    std::uint16_t hit_rand,
    std::optional<std::uint16_t> crit_rand,
    const BasicAttackInputs& inputs) {
    if ((hit_rand % 101) < attack_hit_threshold(inputs.attacker_hit, inputs.target_dodge)) {
        return 0;
    }

    if (inputs.instr_param_0x6 == 0 && crit_rand.has_value()) {
        if ((*crit_rand % 101) <= inputs.attacker_agile) {
            return 2;
        }
    }

    return 1;
}

int roll_damage_from_draws(
    int base_damage,
    int attacker_element,
    int target_element_effectiveness_tenths,
    std::uint16_t spread_rand,
    std::uint16_t bonus_rand,
    bool halve_damage) {
    double base = static_cast<double>(base_damage);
    if (base < 0.0) {
        base = 0.0;
    }

    const double guaranteed_damage = base * kGuaranteedDamageMultiplier;
    const double extra_damage = base * kPotentialDamageMultiplier;
    double damage_temp = extra_damage * (static_cast<double>(spread_rand) * kRandUnitMultiplier)
        + guaranteed_damage;

    if ((bonus_rand & 7) == 0) {
        damage_temp += 1.0;
    }

    if (attacker_element >= 0 && attacker_element < 6) {
        damage_temp = damage_temp * static_cast<double>(target_element_effectiveness_tenths) / 10.0;
    }

    int damage = halve_damage
        ? static_cast<int>(damage_temp * 0.5)
        : static_cast<int>(damage_temp);
    if (damage > 9999) {
        damage = 9999;
    }
    return damage;
}

BasicAttackSimulation simulate_basic_attack_burst(std::uint32_t state, const BasicAttackInputs& inputs) {
    BasicAttackSimulation result;

    auto hit = draw_rand15(state);
    state = hit.next_state;
    result.hit_rand = hit.value;
    result.hit_draw_spent = true;
    ++result.draws_consumed;

    const bool hit_succeeds = (hit.value % 101) >= attack_hit_threshold(inputs.attacker_hit, inputs.target_dodge);
    if (hit_succeeds && inputs.instr_param_0x6 == 0) {
        auto crit = draw_rand15(state);
        state = crit.next_state;
        result.crit_rand = crit.value;
        result.crit_draw_spent = true;
        ++result.draws_consumed;
    }

    result.attack_result = attack_result_from_draw(result.hit_rand, result.crit_rand, inputs);
    apply_target_hit_check_overrides(result, inputs);

    if (result.hit_check != 0 && result.hit_check != 4) {
        append_damage_draws(result, state, inputs);
    }

    result.end_state = state;
    return result;
}

BasicAttackSimulation simulate_forced_basic_attack_damage_burst(
    std::uint32_t state,
    const BasicAttackInputs& inputs) {
    BasicAttackSimulation result;
    result.attack_result = 1;
    apply_target_hit_check_overrides(result, inputs);
    if (result.hit_check != 0 && result.hit_check != 4) {
        append_damage_draws(result, state, inputs);
    }
    result.end_state = state;
    return result;
}

} // namespace savor::predict
