#pragma once

#include "CheckpointTrace.h"
#include "CounterModel.h"

#include <optional>
#include <string_view>
#include <vector>

namespace savor::predict {

enum class CounterCheckpointStatus {
    ObservedOnly,
    WithinExpectedCeiling,
    ExceedsExpectedCeiling,
    MatchesLiveGate,
    MissingLiveGateFields,
    CounterResultMismatch,
    CounterQueueMismatch,
    CounterChanceUpdateMismatch,
};

struct CounterCheckpointExpectation {
    int expected_counter_roll_ceiling = 0;
    std::string_view owner = "counter_roll";
    std::string_view pc = "80081A88";
};

struct CounterCheckpointDraw {
    std::optional<int> draw_index;
    std::optional<int> attacker_slot;
    std::optional<int> target_slot;
    std::optional<int> target_status_flags;
    std::optional<int> target_movement_flags;
    std::optional<int> target_base_counter_chance;
    std::optional<int> target_current_counter_chance;
    std::optional<int> attacker_action_marker;
    std::optional<int> attack_was_critical;
    std::optional<int> counter_rand;
    std::optional<int> counter_result;
    std::optional<int> queued_field7_0xc;
    std::optional<int> updated_current_counter_chance;
    std::optional<int> expected_counter_result;
    std::optional<int> expected_queued_field7_0xc;
    std::optional<int> expected_updated_current_counter_chance;
    std::optional<CounterResultReason> expected_reason;
    bool live_gate_inputs_complete = false;
    bool live_gate_simulated = false;
    bool counter_result_matches = false;
    bool queued_field_matches = false;
    bool counter_chance_update_matches = false;
};

struct CounterCheckpointSummary {
    std::optional<int> expected_counter_roll_ceiling;
    int observed_counter_rolls = 0;
    std::optional<int> first_counter_roll_draw_index;
    std::optional<int> last_counter_roll_draw_index;
    int draws_with_actor_slots = 0;
    int draws_with_gate_inputs = 0;
    int draws_with_rand_value = 0;
    int draws_with_counter_result = 0;
    int draws_with_queue_result = 0;
    int draws_with_counter_chance_update = 0;
    int live_gate_simulated_draws = 0;
    int counter_result_matches = 0;
    int counter_result_mismatches = 0;
    int queued_field_matches = 0;
    int queued_field_mismatches = 0;
    int counter_chance_update_matches = 0;
    int counter_chance_update_mismatches = 0;
    int seed_transition_mismatches = 0;
    std::vector<CounterCheckpointDraw> draws;
    CounterCheckpointStatus status = CounterCheckpointStatus::ObservedOnly;
};

CounterCheckpointExpectation first_battle_counter_checkpoint_expectation(int counter_draw_candidate_events);
CounterCheckpointSummary summarize_counter_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<int> expected_counter_roll_ceiling);
const char* counter_checkpoint_status_name(CounterCheckpointStatus status);
const char* first_battle_counter_checkpoint_rule_detail();

} // namespace savor::predict
