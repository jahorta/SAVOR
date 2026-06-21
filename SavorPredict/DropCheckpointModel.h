#pragma once

#include "CheckpointTrace.h"

#include <optional>
#include <string_view>
#include <vector>

namespace savor::predict {

enum class DropCheckpointStatus {
    ObservedOnly,
    MatchesExpected,
    MissingDropRolls,
    ExtraDropRolls,
};

struct DropCheckpointExpectation {
    int expected_drop_rolls = 0;
    std::string_view owner = "enemy_drop_roll";
    std::string_view pc = "8002BAE8";
};

struct DropCheckpointDraw {
    std::optional<int> draw_index;
    std::optional<int> target_slot;
    std::optional<int> enemy_entry_id;
    std::optional<int> drop_row_index;
    std::optional<int> drop_item_id;
    std::optional<int> drop_amount;
    std::optional<int> rand_value;
    std::optional<int> rand_mod100;
    std::optional<int> drop_success;
};

struct DropCheckpointSummary {
    std::optional<int> expected_drop_rolls;
    int observed_drop_rolls = 0;
    std::optional<int> first_drop_roll_draw_index;
    std::optional<int> last_drop_roll_draw_index;
    int draws_with_target_slot = 0;
    int draws_with_drop_row = 0;
    int draws_with_rand_value = 0;
    int observed_damage_bonus_draws = 0;
    int damage_bonus_draws_before_first_drop = 0;
    int damage_bonus_draws_after_first_drop = 0;
    int observed_counter_rolls = 0;
    int counter_rolls_before_first_drop = 0;
    int observed_action_view_camera_draws = 0;
    int action_view_camera_draws_before_first_drop = 0;
    int observed_status_attempt_draws = 0;
    int status_attempt_draws_before_first_drop = 0;
    std::vector<DropCheckpointDraw> draws;
    DropCheckpointStatus status = DropCheckpointStatus::ObservedOnly;
};

DropCheckpointExpectation first_battle_drop_checkpoint_expectation(int expected_drop_rolls);
DropCheckpointSummary summarize_drop_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<int> expected_drop_rolls);
const char* drop_checkpoint_status_name(DropCheckpointStatus status);
const char* first_battle_drop_checkpoint_rule_detail();

} // namespace savor::predict
