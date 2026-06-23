#pragma once

#include "RngCore.h"

#include <cstdint>
#include <optional>

namespace savor::predict {

struct BasicAttackInputs {
    int attacker_attack = 0;
    int attacker_hit = 0;
    int attacker_agile = 0;
    int attacker_element = 0;
    int target_defense = 0;
    int target_dodge = 0;
    int target_element_effectiveness_tenths = 10;
    int target_status_flags = 0;
    int instr_param_0x6 = 0;
};

struct BasicAttackSimulation {
    std::uint32_t end_state = 0;
    int draws_consumed = 0;
    std::uint16_t hit_rand = 0;
    bool hit_draw_spent = false;
    std::optional<std::uint16_t> crit_rand;
    std::optional<std::uint16_t> damage_spread_rand;
    std::optional<std::uint16_t> damage_bonus_rand;
    int attack_result = 0;
    int hit_check = 0;
    bool crit_draw_spent = false;
    bool damage_draws_spent = false;
    int base_damage = 0;
    int damage = 0;
};

int attack_hit_threshold(int attacker_hit, int target_dodge);
int attack_result_from_draw(std::uint16_t hit_rand, std::optional<std::uint16_t> crit_rand, const BasicAttackInputs& inputs);
int roll_damage_from_draws(
    int base_damage,
    int attacker_element,
    int target_element_effectiveness_tenths,
    std::uint16_t spread_rand,
    std::uint16_t bonus_rand,
    bool halve_damage);
BasicAttackSimulation simulate_basic_attack_burst(std::uint32_t state, const BasicAttackInputs& inputs);
BasicAttackSimulation simulate_forced_basic_attack_damage_burst(
    std::uint32_t state,
    const BasicAttackInputs& inputs);

} // namespace savor::predict
