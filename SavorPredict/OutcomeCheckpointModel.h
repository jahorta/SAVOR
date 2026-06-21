#pragma once

#include "CheckpointTrace.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace savor::predict {

enum class OutcomeCheckpointStatus {
    ObservedOnly,
    MatchesExpected,
    EndTurnStatusDrawMismatch,
    LevelUpStatRollMismatch,
};

struct OutcomeCheckpointExpectation {
    std::optional<int> expected_end_turn_status_draws;
    std::optional<int> expected_level_up_stat_rolls;
    std::string_view end_turn_owner = "end_turn_status_cleanup";
    std::string_view end_turn_pc = "8006FF38";
    std::string_view level_up_roll_1_owner = "level_up_stat_roll_1";
    std::string_view level_up_roll_2_owner = "level_up_stat_roll_2";
    std::string_view level_up_roll_3_owner = "level_up_stat_roll_3";
};

struct OutcomeCheckpointDraw {
    std::optional<int> draw_index;
    std::string owner;
    std::optional<int> actor_slot;
    std::optional<int> status_effect_id;
    std::optional<int> stat_index;
    std::optional<int> level;
    std::optional<int> rand_value;
};

struct OutcomeCheckpointSummary {
    std::optional<int> expected_end_turn_status_draws;
    std::optional<int> expected_level_up_stat_rolls;
    int observed_end_turn_status_draws = 0;
    int observed_level_up_stat_rolls = 0;
    int observed_level_up_roll_1_draws = 0;
    int observed_level_up_roll_2_draws = 0;
    int observed_level_up_roll_3_draws = 0;
    std::optional<int> first_end_turn_status_draw_index;
    std::optional<int> first_level_up_stat_roll_index;
    int draws_with_actor_slot = 0;
    int draws_with_rand_value = 0;
    std::vector<OutcomeCheckpointDraw> draws;
    OutcomeCheckpointStatus status = OutcomeCheckpointStatus::ObservedOnly;
};

OutcomeCheckpointExpectation first_battle_outcome_checkpoint_expectation(int battle_outcome);
OutcomeCheckpointSummary summarize_outcome_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<int> expected_end_turn_status_draws,
    std::optional<int> expected_level_up_stat_rolls);
const char* outcome_checkpoint_status_name(OutcomeCheckpointStatus status);
const char* first_battle_outcome_checkpoint_rule_detail();

} // namespace savor::predict
