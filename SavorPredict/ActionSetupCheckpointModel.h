#pragma once

#include "CheckpointTrace.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace savor::predict {

enum class ActionSetupCheckpointStatus {
    ObservedOnly,
    MatchesExpected,
    MissingLiveSetupFields,
    HandlerMismatch,
    MissingEnemySetupDraws,
    ExtraEnemySetupDraws,
};

enum class ActionSetupCheckpointKind {
    SetupAction,
    PcHandlerEntry,
    EnemyHandlerEntry,
    EnemySetupDraw,
};

struct ActionSetupCheckpointEvent {
    ActionSetupCheckpointKind kind = ActionSetupCheckpointKind::SetupAction;
    std::optional<int> draw_index;
    std::optional<int> actor_slot;
    std::optional<int> target_slot;
    std::optional<int> instruction;
    std::optional<int> instr_param_0x6;
    std::optional<int> movement_flags;
    std::optional<std::string> expected_handler_pc;
    std::optional<std::string> handler_pc;
    std::optional<int> setup_rand;
    std::optional<int> setup_rand_mod10;
    std::optional<int> direct_close_candidate;
};

struct ActionSetupCheckpointSummary {
    std::optional<int> expected_enemy_setup_draws;
    int observed_setup_action_events = 0;
    int observed_pc_handler_entries = 0;
    int observed_enemy_handler_entries = 0;
    int observed_enemy_setup_draws = 0;
    int setup_events_with_actor_slot = 0;
    int setup_events_with_handler_pc = 0;
    int setup_events_with_instruction = 0;
    int setup_events_with_target_slot = 0;
    int setup_events_with_instr_param = 0;
    int handler_entries_with_instruction = 0;
    int handler_entries_with_target_slot = 0;
    int handler_entries_with_instr_param = 0;
    int handler_entries_with_movement_flags = 0;
    int handler_matches = 0;
    int handler_mismatches = 0;
    int enemy_setup_draws_with_gate_inputs = 0;
    int enemy_setup_draws_with_rand_value = 0;
    int enemy_setup_draws_with_rand_mod10 = 0;
    int enemy_setup_draws_with_direct_close_candidate = 0;
    std::optional<int> first_setup_action_draw_index;
    std::optional<int> first_pc_handler_draw_index;
    std::optional<int> first_enemy_handler_draw_index;
    std::optional<int> first_enemy_setup_draw_index;
    std::optional<int> first_attack_hit_draw_index;
    int enemy_setup_draws_before_first_attack_hit = 0;
    std::vector<ActionSetupCheckpointEvent> events;
    ActionSetupCheckpointStatus status = ActionSetupCheckpointStatus::ObservedOnly;
};

ActionSetupCheckpointSummary summarize_action_setup_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<int> expected_enemy_setup_draws);
const char* action_setup_checkpoint_status_name(ActionSetupCheckpointStatus status);
const char* action_setup_checkpoint_kind_name(ActionSetupCheckpointKind kind);
const char* first_battle_action_setup_checkpoint_rule_detail();
std::string expected_action_setup_handler_pc_for_slot(int actor_slot);

} // namespace savor::predict
