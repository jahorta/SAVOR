#pragma once

#include "CheckpointTrace.h"
#include "TurnOrderModel.h"

#include <optional>
#include <string_view>
#include <vector>

namespace savor::predict {

enum class TurnOrderCheckpointStatus {
    ObservedOnly,
    MatchesExpected,
    MissingPriorityJitterDraws,
    ExtraPriorityJitterDraws,
};

struct TurnOrderCheckpointExpectation {
    int expected_priority_jitter_draws = 0;
    int expected_queued_entries = 0;
    int expected_jitter_modulus = 0;
    std::string_view owner = "turn_order_priority_jitter";
    std::string_view pc = "800711F8";
};

struct TurnOrderCheckpointDraw {
    std::optional<int> draw_index;
    std::optional<int> slot;
    std::optional<int> quick;
    std::optional<int> assigned_priority;
    std::optional<int> rand_value;
};

struct TurnOrderCheckpointSummary {
    std::optional<int> expected_priority_jitter_draws;
    int observed_priority_jitter_draws = 0;
    std::optional<int> first_priority_jitter_draw_index;
    std::optional<int> last_priority_jitter_draw_index;
    int draws_with_slot = 0;
    int draws_with_quick = 0;
    int draws_with_assigned_priority = 0;
    int draws_with_rand_value = 0;
    std::vector<TurnOrderCheckpointDraw> draws;
    TurnOrderCheckpointStatus status = TurnOrderCheckpointStatus::ObservedOnly;
};

TurnOrderCheckpointExpectation turn_order_checkpoint_expectation(const TurnOrderSimulation& turn_order);
TurnOrderCheckpointSummary summarize_turn_order_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<int> expected_priority_jitter_draws);
const char* turn_order_checkpoint_status_name(TurnOrderCheckpointStatus status);
const char* turn_order_checkpoint_rule_detail();

} // namespace savor::predict
