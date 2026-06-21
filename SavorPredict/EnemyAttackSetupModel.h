#pragma once

#include "RngCore.h"

#include <cstdint>
#include <optional>

namespace savor::predict {

enum class EnemyAttackSetupPath {
    NotAttack,
    NoRandomSetupDraw,
    TargetAdjacencySetup,
    DirectCloseSetupCandidate,
};

struct EnemyAttackSetupInputs {
    int queued_instruction = 3;
    int movement_flags = 0;
};

struct EnemyAttackSetupSimulation {
    std::uint32_t end_state = 0;
    int draws_consumed = 0;
    std::optional<std::uint16_t> setup_rand;
    std::optional<int> setup_rand_mod10;
    EnemyAttackSetupPath path = EnemyAttackSetupPath::NotAttack;
    bool direct_close_branch_candidate = false;
};

EnemyAttackSetupSimulation simulate_enemy_attack_setup_gate(std::uint32_t state, const EnemyAttackSetupInputs& inputs);
const char* enemy_attack_setup_path_name(EnemyAttackSetupPath path);

} // namespace savor::predict
