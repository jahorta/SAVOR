#pragma once

#include "CheckpointTrace.h"

#include <optional>
#include <vector>

namespace savor::predict {

enum class CritGateCheckpointStatus {
    ObservedOnly,
    MatchesLiveGate,
    MissingLiveGateFields,
    CritDrawCountMismatch,
};

struct CritGateHitDraw {
    std::optional<int> draw_index;
    std::optional<int> active_slot;
    std::optional<int> target_slot;
    std::optional<int> instr_param_0x6;
    std::optional<int> hit_success;
    std::optional<int> attack_result;
    std::optional<int> rand_value;
};

struct CritGateCheckpointSummary {
    int observed_hit_draws = 0;
    int observed_crit_draws = 0;
    int hit_draws_with_instr_param = 0;
    int hit_draws_with_hit_success = 0;
    int live_gate_expected_crit_draws = 0;
    std::optional<int> expected_crit_draws_from_live_gate;
    std::optional<int> first_hit_draw_index;
    std::optional<int> first_crit_draw_index;
    std::vector<CritGateHitDraw> hit_draws;
    CritGateCheckpointStatus status = CritGateCheckpointStatus::ObservedOnly;
};

CritGateCheckpointSummary summarize_crit_gate_checkpoints(
    const std::vector<CheckpointEvent>& events);
const char* crit_gate_checkpoint_status_name(CritGateCheckpointStatus status);
const char* crit_gate_checkpoint_rule_detail();

} // namespace savor::predict
