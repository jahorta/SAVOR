#include "EnemyAttackSetupModel.h"

namespace savor::predict {

EnemyAttackSetupSimulation simulate_enemy_attack_setup_gate(std::uint32_t state, const EnemyAttackSetupInputs& inputs) {
    EnemyAttackSetupSimulation result;
    result.end_state = state;

    if (inputs.queued_instruction != 3) {
        result.path = EnemyAttackSetupPath::NotAttack;
        return result;
    }

    const bool movement_0x20 = (inputs.movement_flags & 0x20) != 0;
    const bool movement_0x40 = (inputs.movement_flags & 0x40) != 0;
    const bool movement_0x80 = (inputs.movement_flags & 0x80) != 0;

    if (!movement_0x20 || !movement_0x80) {
        if (movement_0x80) {
            const auto draw = draw_rand15(state);
            state = draw.next_state;
            result.end_state = state;
            result.setup_rand = draw.value;
            result.setup_rand_mod10 = draw.value % 10;
            result.draws_consumed = 1;
            if (*result.setup_rand_mod10 > 3 && movement_0x40) {
                result.path = EnemyAttackSetupPath::DirectCloseSetupCandidate;
                result.direct_close_branch_candidate = true;
            } else {
                result.path = EnemyAttackSetupPath::TargetAdjacencySetup;
            }
            return result;
        }

        result.path = EnemyAttackSetupPath::NoRandomSetupDraw;
        return result;
    }

    result.path = EnemyAttackSetupPath::TargetAdjacencySetup;
    return result;
}

const char* enemy_attack_setup_path_name(EnemyAttackSetupPath path) {
    switch (path) {
    case EnemyAttackSetupPath::NotAttack:
        return "NotAttack";
    case EnemyAttackSetupPath::NoRandomSetupDraw:
        return "NoRandomSetupDraw";
    case EnemyAttackSetupPath::TargetAdjacencySetup:
        return "TargetAdjacencySetup";
    case EnemyAttackSetupPath::DirectCloseSetupCandidate:
        return "DirectCloseSetupCandidate";
    }
    return "NotAttack";
}

} // namespace savor::predict
