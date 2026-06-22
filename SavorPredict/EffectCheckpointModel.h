#pragma once

#include "CheckpointTrace.h"

#include <optional>
#include <vector>

namespace savor::predict {

enum class EffectCheckpointStatus {
    ObservedOnly,
    MatchesBinaryVariantBurstShape,
    UnbalancedCombatEffectBurst,
};

struct EffectCheckpointSummary {
    int observed_combat_effect_draws = 0;
    int observed_binary_position_draws = 0;
    int observed_four_way_position_draws = 0;
    int observed_scale_x_draws = 0;
    int observed_scale_y_draws = 0;
    int observed_scale_z_draws = 0;
    int observed_variant_index_draws = 0;
    int observed_axis_assignment_draws = 0;
    int combat_effect_draws_with_loop_count = 0;
    int combat_effect_draws_with_flags = 0;
    int combat_effect_draws_with_variant_count = 0;
    int combat_effect_draws_with_axis_mode = 0;
    int complete_binary_variant_iterations = 0;
    int complete_binary_variant_22_loop_executions = 0;
    int incomplete_binary_variant_iteration_remainder = 0;
    int observed_emitter_spawn_draws = 0;
    int observed_emitter_source_gate_events = 0;
    int emitter_source_gate_events_with_outer_count = 0;
    int emitter_source_gate_events_with_child_count = 0;
    int emitter_source_gate_events_with_variant_count = 0;
    int emitter_source_gate_events_with_axis_mode = 0;
    int observed_particle_tick_draws = 0;
    int particle_tick_draws_with_payload_source = 0;
    int particle_tick_draws_with_lifetime = 0;
    std::optional<int> first_combat_effect_draw_index;
    std::optional<int> last_combat_effect_draw_index;
    EffectCheckpointStatus status = EffectCheckpointStatus::ObservedOnly;
};

EffectCheckpointSummary summarize_effect_checkpoints(const std::vector<CheckpointEvent>& events);
const char* effect_checkpoint_status_name(EffectCheckpointStatus status);
const char* first_battle_effect_checkpoint_rule_detail();

} // namespace savor::predict
