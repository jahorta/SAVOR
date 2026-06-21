#pragma once

#include "CheckpointTrace.h"

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
    SelectedModeMismatch,
    Mode0FallbackReached,
    ActionViewOrderMismatch,
};

struct ActionViewGateCheckpointEvent {
    std::optional<int> draw_index;
    std::optional<int> action_sequence_id;
    std::optional<int> active_slot;
    std::optional<int> source_slot;
    std::optional<int> target_slot;
    std::optional<int> source_field6_0x6;
    std::optional<int> actor_field6_0x6;
    std::optional<std::string> aux_list_root;
    std::optional<int> query_arg0;
    std::optional<int> query_arg1;
    std::optional<int> query_arg2;
    std::optional<int> query_arg3;
    std::optional<std::string> query_result;
    std::optional<int> selected_record_mode;
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
};

struct ActionViewGateCheckpointSummary {
    int observed_gate_events = 0;
    int events_with_aux_list_root = 0;
    int events_with_query_args = 0;
    int events_with_query_result = 0;
    int events_with_selected_record_mode = 0;
    int query_args_match = 0;
    int query_args_mismatch = 0;
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
    ActionViewGateCheckpointStatus status = ActionViewGateCheckpointStatus::ObservedOnly;
};

ActionViewGateCheckpointSummary summarize_action_view_gate_checkpoints(
    const std::vector<CheckpointEvent>& events);
const char* action_view_gate_checkpoint_status_name(ActionViewGateCheckpointStatus status);
const char* first_battle_action_view_gate_checkpoint_rule_detail();

} // namespace savor::predict
