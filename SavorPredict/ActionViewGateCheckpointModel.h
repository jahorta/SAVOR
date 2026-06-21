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
    QueryArgsMismatch,
    SelectedModeMismatch,
};

struct ActionViewGateCheckpointEvent {
    std::optional<int> draw_index;
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
    int observed_mode0e_camera_draws = 0;
    int observed_attack_hit_draws = 0;
    std::optional<int> first_gate_draw_index;
    std::optional<int> first_mode0e_draw_index;
    std::optional<int> first_attack_hit_draw_index;
    int gate_events_before_first_mode0e = 0;
    int gate_events_before_first_attack_hit = 0;
    int mode0e_draws_before_first_attack_hit = 0;
    std::vector<ActionViewGateCheckpointEvent> events;
    ActionViewGateCheckpointStatus status = ActionViewGateCheckpointStatus::ObservedOnly;
};

ActionViewGateCheckpointSummary summarize_action_view_gate_checkpoints(
    const std::vector<CheckpointEvent>& events);
const char* action_view_gate_checkpoint_status_name(ActionViewGateCheckpointStatus status);
const char* first_battle_action_view_gate_checkpoint_rule_detail();

} // namespace savor::predict
