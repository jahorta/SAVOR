#pragma once

#include "ActionViewSelectorModel.h"
#include "CheckpointTrace.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

enum class ActionViewGateCheckpointStatus {
    ObservedOnly,
    MatchesExpected,
    MissingLiveGateFields,
    MissingSchedulerFields,
    QueryArgsMismatch,
    SelectorModelMismatch,
    AuxTableCountMismatch,
    SelectedModeMismatch,
    Mode0FallbackReached,
    ActionViewOrderMismatch,
};

struct ActionViewGateCheckpointOptions {
    std::filesystem::path action_view_std_json_dir;
};

struct ActionViewGateCheckpointEvent {
    std::string checkpoint;
    std::optional<int> draw_index;
    std::optional<int> action_sequence_id;
    std::optional<int> active_slot;
    std::optional<int> source_slot;
    std::optional<int> target_slot;
    std::optional<int> source_field6_0x6;
    std::optional<int> actor_field6_0x6;
    std::optional<int> actor_subtype_0x8;
    std::optional<int> gate_category_0x2f;
    std::optional<int> gate_state_0x30;
    std::optional<int> gate_active_slot_0x02;
    std::optional<int> gate_target_slot_0x04;
    std::optional<unsigned int> instruction_flags_0xf0;
    std::optional<std::string> aux_list_root;
    std::optional<int> query_arg0;
    std::optional<int> query_arg1;
    std::optional<int> query_arg2;
    std::optional<int> query_arg3;
    std::optional<int> query_result_count;
    std::optional<std::string> query_result;
    std::optional<Std0CountQuery> selector_expected_query;
    std::optional<ActionViewSelectorResult> selector_result_without_table;
    std::optional<bool> selector_query_args_match;
    int sampled_aux_table_rows = 0;
    bool sampled_aux_table_includes_sentinel = false;
    std::optional<int> sampled_aux_table_count;
    std::optional<int> matched_query_result_count;
    std::optional<bool> sampled_aux_table_count_matches_query_result;
    int matched_std0_candidate_count = 0;
    std::optional<std::string> matched_resource_stem;
    std::optional<std::string> matched_std_filename;
    std::optional<std::string> matched_std0_filename;
    std::optional<std::string> matched_std0_json_path;
    std::optional<std::string> matched_std0_materialization_source;
    std::optional<int> matched_std0_sample_row_offset;
    std::optional<std::string> actor_slot_expected_std0_filename;
    std::optional<bool> actor_slot_expected_std0_matches_sample;
    std::optional<int> actor_slot_expected_std0_sample_row_offset;
    std::optional<int> selected_record_mode;
    bool helper_call_event = false;
    std::optional<unsigned int> helper_call_site_pc;
    std::optional<std::string> helper_callee;
    std::optional<int> helper_actor_slot;
    std::optional<int> helper_mode_arg;
    std::optional<std::string> selector_expected_helper_role;
    std::optional<std::string> selector_expected_helper_callee;
    std::optional<int> selector_expected_helper_mode_arg;
    std::optional<int> selector_expected_spawned_record_mode;
    std::optional<bool> selector_helper_call_matches;
    std::optional<std::string> action_child_thread;
    std::optional<std::string> child_payload;
    std::optional<std::string> nested_payload;
    std::optional<int> child_thread_state_byte;
    std::optional<bool> mode0_fallback_reached;
    std::optional<int> matched_mode0e_draw_index;
    std::optional<int> matched_attack_hit_draw_index;
    std::optional<bool> gate_before_mode0e_draw;
    std::optional<bool> gate_before_attack_hit_draw;
    std::optional<bool> mode0e_draw_before_attack_hit_draw;
    bool legacy_gate_event = false;
    bool query_call_event = false;
    bool query_result_event = false;
};

struct ActionViewDispatchCheckpointEvent {
    std::optional<int> draw_index;
    std::optional<int> payload_primary_key;
    std::optional<int> payload_secondary_key;
    std::optional<std::string> payload_flags;
    std::optional<int> payload_start_frame;
    std::optional<int> payload_end_frame;
    std::optional<int> payload_hold;
    std::optional<int> payload_step;
    std::optional<int> payload_mode;
    std::optional<int> saved_mode;
    std::optional<int> effective_mode;
    std::optional<int> worksheet_turn_timer;
    std::optional<std::string> instruction_flags;
    std::optional<std::string> global_camera_override;
    std::optional<std::string> global_camera_flags;
};

struct ActionViewGateCheckpointSummary {
    int observed_gate_events = 0;
    int legacy_gate_events = 0;
    int query_call_events = 0;
    int query_result_events = 0;
    int query_call_events_with_query_args = 0;
    int query_result_events_with_query_result = 0;
    int observed_dispatch_events = 0;
    int dispatch_events_with_payload_mode = 0;
    int dispatch_events_with_effective_mode = 0;
    int dispatch_events_with_spicestd_payload_fields = 0;
    int dispatch_serialized_mode0_events = 0;
    int dispatch_effective_mode0_events = 0;
    int dispatch_effective_mode0e_events = 0;
    int dispatch_mode0_to_mode0e_rewrites = 0;
    int dispatch_mode0_stays_mode0_events = 0;
    int events_with_aux_list_root = 0;
    int events_with_query_args = 0;
    int events_with_query_result = 0;
    int events_with_selected_record_mode = 0;
    int query_args_match = 0;
    int query_args_mismatch = 0;
    int events_with_selector_inputs = 0;
    int selector_model_comparisons = 0;
    int selector_query_args_match = 0;
    int selector_query_args_mismatch = 0;
    int selector_model_missing_expected_query = 0;
    int observed_helper_call_events = 0;
    int helper_call_events_with_selector_inputs = 0;
    int selector_helper_call_comparisons = 0;
    int selector_helper_call_matches = 0;
    int selector_helper_call_mismatches = 0;
    int selector_helper_call_missing_expected = 0;
    int events_with_aux_table_fingerprint = 0;
    int events_with_aux_table_count = 0;
    int aux_table_count_matches_query_result = 0;
    int aux_table_count_mismatches_query_result = 0;
    int aux_table_count_missing_query_result = 0;
    int aux_table_fingerprint_matches_known_std0 = 0;
    int aux_table_fingerprint_ambiguous_known_std0 = 0;
    int aux_table_fingerprint_matches_actor_slot_std0 = 0;
    int aux_table_fingerprint_mismatches_actor_slot_std0 = 0;
    int selected_mode_matches = 0;
    int selected_mode_mismatches = 0;
    int events_with_action_child_thread = 0;
    int events_with_child_payload = 0;
    int events_with_nested_payload = 0;
    int events_with_child_thread_state = 0;
    int events_with_scheduler_chain = 0;
    int events_with_mode0_fallback_flag = 0;
    int mode0_fallback_reached_events = 0;
    int observed_mode0e_camera_draws = 0;
    int observed_mode0_fallback_draws = 0;
    int observed_attack_hit_draws = 0;
    std::optional<int> first_gate_draw_index;
    std::optional<int> first_mode0e_draw_index;
    std::optional<int> first_mode0_fallback_draw_index;
    std::optional<int> first_attack_hit_draw_index;
    int gate_events_before_first_mode0e = 0;
    int gate_events_before_first_attack_hit = 0;
    int mode0e_draws_before_first_attack_hit = 0;
    int mode0_fallback_draws_before_first_mode0e = 0;
    int mode0_fallback_draws_before_first_attack_hit = 0;
    int events_with_action_sequence_id = 0;
    int action_sequence_order_comparisons = 0;
    int action_sequence_order_matches = 0;
    int action_sequence_order_mismatches = 0;
    int action_sequence_order_missing_camera_or_hit = 0;
    std::vector<ActionViewGateCheckpointEvent> events;
    std::vector<ActionViewDispatchCheckpointEvent> dispatch_events;
    ActionViewGateCheckpointStatus status = ActionViewGateCheckpointStatus::ObservedOnly;
};

ActionViewGateCheckpointSummary summarize_action_view_gate_checkpoints(
    const std::vector<CheckpointEvent>& events);
ActionViewGateCheckpointSummary summarize_action_view_gate_checkpoints(
    const std::vector<CheckpointEvent>& events,
    const ActionViewGateCheckpointOptions& options);
const char* action_view_gate_checkpoint_status_name(ActionViewGateCheckpointStatus status);
const char* first_battle_action_view_gate_checkpoint_rule_detail();

} // namespace savor::predict
