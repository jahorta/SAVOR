#pragma once

#include "CheckpointTrace.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace savor::predict {

enum class ActionSourceCheckpointStatus {
    ObservedOnly,
    MatchesExpected,
    MissingSourceSelectionCheckpoint,
    MissingLiveSourceFields,
    SourceSelectionMismatch,
    Field6Mismatch,
    HandlerMismatch,
    CallbackMismatch,
};

enum class ActionSourceCheckpointKind {
    SourceSelection,
    Field6Bridge,
};

struct ActionSourceCheckpointExpectation {
    std::optional<std::string> expected_handler_pc;
    std::optional<std::string> expected_callback_pc;
    std::string_view source_selection_pc = "8006782C";
    std::string_view action_source_pc = "8006721C";
    std::string_view expected_first_battle_handler_pc = "800662BC";
};

struct ActionSourceCheckpointEvent {
    ActionSourceCheckpointKind kind = ActionSourceCheckpointKind::Field6Bridge;
    std::optional<int> draw_index;
    std::optional<int> actor_slot;
    std::optional<int> source_slot;
    std::optional<int> target_slot;
    std::optional<int> action_sequence_id;
    std::optional<int> action_id;
    std::optional<int> source_field6_0x6;
    std::optional<int> actor_field6_0x6;
    std::optional<std::string> handler_pc;
    std::optional<std::string> callback_pc;
    std::optional<int> matched_source_selection_draw_index;
    std::optional<int> matched_source_selection_source_slot;
    std::optional<bool> source_selection_before_bridge;
    std::optional<bool> source_slot_matches_selection;
};

struct ActionSourceCheckpointSummary {
    std::optional<std::string> expected_handler_pc;
    std::optional<std::string> expected_callback_pc;
    int observed_source_selection_events = 0;
    int observed_action_source_events = 0;
    int source_selection_events_with_source_slot = 0;
    int source_selection_events_with_actor_slot = 0;
    int source_selection_events_with_target_slot = 0;
    int events_with_actor_slot = 0;
    int events_with_source_slot = 0;
    int events_with_action_sequence_id = 0;
    int source_selection_events_with_action_sequence_id = 0;
    int action_source_events_with_action_sequence_id = 0;
    int events_with_action_id = 0;
    int events_with_handler_pc = 0;
    int events_with_callback_pc = 0;
    int events_with_source_field6 = 0;
    int events_with_actor_field6 = 0;
    int source_selection_bridge_pairs = 0;
    int source_selection_bridge_matches = 0;
    int source_selection_bridge_mismatches = 0;
    int source_selection_bridge_pairs_by_action_sequence_id = 0;
    int source_selection_bridge_missing_by_action_sequence_id = 0;
    int source_selection_bridge_order_matches = 0;
    int source_selection_bridge_order_mismatches = 0;
    std::string source_selection_bridge_pairing_strategy = "trace_order";
    int field6_matches = 0;
    int field6_mismatches = 0;
    int handler_matches = 0;
    int handler_mismatches = 0;
    int callback_matches = 0;
    int callback_mismatches = 0;
    std::optional<int> first_source_selection_draw_index;
    std::optional<int> first_action_source_draw_index;
    std::vector<ActionSourceCheckpointEvent> events;
    ActionSourceCheckpointStatus status = ActionSourceCheckpointStatus::ObservedOnly;
};

ActionSourceCheckpointExpectation first_battle_action_source_checkpoint_expectation();
ActionSourceCheckpointSummary summarize_action_source_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<std::string> expected_handler_pc);
const char* action_source_checkpoint_status_name(ActionSourceCheckpointStatus status);
const char* action_source_checkpoint_kind_name(ActionSourceCheckpointKind kind);
const char* first_battle_action_source_checkpoint_rule_detail();

} // namespace savor::predict
