#pragma once

#include "CheckpointTrace.h"

#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

enum class EffectCheckpointStatus {
    ObservedOnly,
    MatchesBinaryVariantBurstShape,
    UnbalancedCombatEffectBurst,
};

struct EffectSourceKeyPairCount {
    int source_key = -1;
    int first_loop_count = 0;
    int second_loop_count = 0;
    int loop_count_sum = 0;
    int draw_count = 0;
    int pair_count = 0;
};

struct EffectRecordCopyCheckpointEvent {
    std::optional<int> draw_index;
    std::optional<std::string> effect_buffer;
    std::optional<std::string> source_record;
    std::optional<std::string> parent_action_thread;
    std::optional<std::string> copied_parent_action_thread;
    std::optional<int> source_key;
    std::optional<int> source_record_key;
    std::optional<int> loop_count;
    std::optional<int> source_record_loop_count;
    std::optional<int> matched_combat_effect_first_draw_index;
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
    int combat_effect_draws_with_source_key = 0;
    int combat_effect_draws_with_source_subtype = 0;
    int combat_effect_draws_with_source_secondary = 0;
    int combat_effect_draws_with_source_resource_id = 0;
    int combat_effect_draws_with_buffer_pointer = 0;
    int observed_effect_record_copy_events = 0;
    int effect_record_copy_events_with_effect_buffer = 0;
    int effect_record_copy_events_with_parent_action_thread = 0;
    int effect_record_copy_events_with_source_key = 0;
    int effect_record_copy_events_with_loop_count = 0;
    int effect_record_copy_events_matching_source_record_fields = 0;
    int effect_record_copy_events_matching_combat_effect_buffer = 0;
    int observed_combat_effect_buffers = 0;
    int complete_binary_variant_iterations = 0;
    int complete_binary_variant_buffers = 0;
    int complete_binary_variant_16_loop_buffers = 0;
    int complete_binary_variant_6_loop_buffers = 0;
    int complete_binary_variant_4_loop_buffers = 0;
    int complete_first_battle_landed_attack_effect_pairs = 0;
    int complete_first_battle_16_6_effect_pairs = 0;
    int complete_first_battle_16_4_effect_pairs = 0;
    int complete_first_battle_landed_attack_effect_pair_iterations = 0;
    int complete_first_battle_landed_attack_effect_pair_draws = 0;
    int complete_first_battle_landed_attack_effect_pairs_with_matching_source_key = 0;
    int complete_first_battle_landed_attack_effect_pairs_without_matching_source_key = 0;
    std::vector<EffectSourceKeyPairCount> complete_first_battle_effect_pairs_by_source_key;
    int unpaired_first_battle_effect_buffers = 0;
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
    std::vector<EffectRecordCopyCheckpointEvent> record_copy_events;
    std::optional<int> first_combat_effect_draw_index;
    std::optional<int> last_combat_effect_draw_index;
    EffectCheckpointStatus status = EffectCheckpointStatus::ObservedOnly;
};

EffectCheckpointSummary summarize_effect_checkpoints(const std::vector<CheckpointEvent>& events);
const char* effect_checkpoint_status_name(EffectCheckpointStatus status);
const char* first_battle_effect_checkpoint_rule_detail();

} // namespace savor::predict
