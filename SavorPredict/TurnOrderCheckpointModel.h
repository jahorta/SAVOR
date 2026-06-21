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
    MissingLivePriorityFields,
    PriorityMismatch,
    ExecutionOrderMismatch,
};

enum class TurnOrderCheckpointKind {
    PriorityJitterDraw,
    QueueEntry,
    ExecutionOrderEntry,
};

struct TurnOrderCheckpointExpectation {
    int expected_priority_jitter_draws = 0;
    int expected_queued_entries = 0;
    int expected_jitter_modulus = 0;
    std::string_view owner = "turn_order_priority_jitter";
    std::string_view pc = "800711F8";
};

struct TurnOrderCheckpointDraw {
    TurnOrderCheckpointKind kind = TurnOrderCheckpointKind::PriorityJitterDraw;
    std::optional<int> draw_index;
    std::optional<int> slot;
    std::optional<int> quick;
    std::optional<int> queued_instruction;
    std::optional<int> target_slot;
    std::optional<int> initial_priority;
    std::optional<int> fixed_priority_result;
    std::optional<int> fixed_priority_value;
    std::optional<int> jitter_modulus;
    std::optional<int> sum_quick;
    std::optional<int> queued_count;
    std::optional<int> queue_index;
    std::optional<int> execution_index;
    std::optional<int> assigned_priority;
    std::optional<int> rand_value;
    std::optional<int> expected_assigned_priority;
    std::optional<int> expected_execution_slot;
    bool priority_matches = false;
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
    int observed_queue_entries = 0;
    int observed_execution_order_entries = 0;
    int queue_entries_with_slot = 0;
    int queue_entries_with_quick = 0;
    int queue_entries_with_assigned_priority = 0;
    int priority_draws_with_expected_priority = 0;
    int priority_matches = 0;
    int priority_mismatches = 0;
    int incomplete_queue_entries = 0;
    int incomplete_execution_order_entries = 0;
    bool execution_order_compared = false;
    bool execution_order_exact = true;
    bool priority_ties_observed = false;
    int execution_order_matches = 0;
    int execution_order_mismatches = 0;
    std::vector<int> expected_execution_slots;
    std::vector<int> observed_execution_slots;
    std::vector<TurnOrderCheckpointDraw> draws;
    TurnOrderCheckpointStatus status = TurnOrderCheckpointStatus::ObservedOnly;
};

TurnOrderCheckpointExpectation turn_order_checkpoint_expectation(const TurnOrderSimulation& turn_order);
TurnOrderCheckpointSummary summarize_turn_order_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<int> expected_priority_jitter_draws);
const char* turn_order_checkpoint_status_name(TurnOrderCheckpointStatus status);
const char* turn_order_checkpoint_kind_name(TurnOrderCheckpointKind kind);
const char* turn_order_checkpoint_rule_detail();

} // namespace savor::predict
