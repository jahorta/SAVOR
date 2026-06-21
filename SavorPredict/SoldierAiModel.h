#pragma once

#include "RngCore.h"

#include <cstdint>
#include <optional>

namespace savor::predict {

struct SoldierAiDecision {
    int slot = 0;
    std::uint32_t action_state = 0;
    std::uint16_t action_rand = 0;
    bool attacks = false;
    std::optional<std::uint32_t> target_state;
    std::optional<std::uint16_t> target_rand;
    std::optional<std::uint32_t> attack_param_state;
    std::optional<std::uint16_t> attack_param_rand;
    std::optional<int> target_pc_slot;
};

int pre_ai_draws_for_fake_attacks(int fake_attacks_this_turn);
int soldier_attack_param_from_rand(std::uint16_t value);
SoldierAiDecision resolve_soldier_ai(int soldier_slot, std::uint32_t& state);

} // namespace savor::predict
