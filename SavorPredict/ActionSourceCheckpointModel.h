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
    MissingLiveSourceFields,
    Field6Mismatch,
    HandlerMismatch,
};

struct ActionSourceCheckpointExpectation {
    std::optional<std::string> expected_handler_pc;
    std::string_view action_source_pc = "8006721C";
    std::string_view expected_first_battle_handler_pc = "800662BC";
};

struct ActionSourceCheckpointEvent {
    std::optional<int> draw_index;
    std::optional<int> actor_slot;
    std::optional<int> source_slot;
    std::optional<int> target_slot;
    std::optional<int> action_id;
    std::optional<int> source_field6_0x6;
    std::optional<int> actor_field6_0x6;
    std::optional<std::string> handler_pc;
};

struct ActionSourceCheckpointSummary {
    std::optional<std::string> expected_handler_pc;
    int observed_action_source_events = 0;
    int events_with_actor_slot = 0;
    int events_with_source_slot = 0;
    int events_with_action_id = 0;
    int events_with_handler_pc = 0;
    int events_with_source_field6 = 0;
    int events_with_actor_field6 = 0;
    int field6_matches = 0;
    int field6_mismatches = 0;
    int handler_matches = 0;
    int handler_mismatches = 0;
    std::optional<int> first_action_source_draw_index;
    std::vector<ActionSourceCheckpointEvent> events;
    ActionSourceCheckpointStatus status = ActionSourceCheckpointStatus::ObservedOnly;
};

ActionSourceCheckpointExpectation first_battle_action_source_checkpoint_expectation();
ActionSourceCheckpointSummary summarize_action_source_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<std::string> expected_handler_pc);
const char* action_source_checkpoint_status_name(ActionSourceCheckpointStatus status);
const char* first_battle_action_source_checkpoint_rule_detail();

} // namespace savor::predict
