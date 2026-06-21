#include "CounterCheckpointModel.h"

#include <cstdlib>
#include <initializer_list>
#include <utility>

namespace savor::predict {

namespace {

constexpr std::string_view kCounterOwner = "counter_roll";

bool owner_is(const CheckpointEvent& event, std::string_view owner) {
    return event.known_rng_owner == owner;
}

std::optional<int> parse_field_int(const CheckpointEvent& event, const char* key) {
    const auto found = event.fields.find(key);
    if (found == event.fields.end()) {
        return std::nullopt;
    }

    char* end = nullptr;
    const long parsed = std::strtol(found->second.c_str(), &end, 0);
    if (end == found->second.c_str() || *end != '\0') {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

std::optional<int> parse_first_field_int(
    const CheckpointEvent& event,
    std::initializer_list<const char*> field_names) {
    for (const auto* field_name : field_names) {
        if (const auto value = parse_field_int(event, field_name); value.has_value()) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<int> rand_value_from_event(
    const CheckpointEvent& event,
    int& seed_transition_mismatches) {
    if (const auto value = parse_first_field_int(event, {"counter_rand", "rand_value"}); value.has_value()) {
        return value;
    }
    if (!event.rng_seed_before.has_value()) {
        return std::nullopt;
    }

    const auto draw = draw_rand15(*event.rng_seed_before);
    if (event.rng_seed_after.has_value() && *event.rng_seed_after != draw.next_state) {
        ++seed_transition_mismatches;
    }
    return static_cast<int>(draw.value);
}

bool has_any_live_gate_fields(const CounterCheckpointDraw& draw) {
    return draw.attacker_slot.has_value()
        || draw.target_slot.has_value()
        || draw.target_status_flags.has_value()
        || draw.target_movement_flags.has_value()
        || draw.target_base_counter_chance.has_value()
        || draw.target_current_counter_chance.has_value()
        || draw.attacker_action_marker.has_value()
        || draw.attack_was_critical.has_value()
        || draw.counter_rand.has_value()
        || draw.counter_result.has_value()
        || draw.queued_field7_0xc.has_value()
        || draw.updated_current_counter_chance.has_value();
}

bool has_complete_live_gate_inputs(const CounterCheckpointDraw& draw) {
    return draw.attacker_slot.has_value()
        && draw.target_slot.has_value()
        && draw.target_status_flags.has_value()
        && draw.target_movement_flags.has_value()
        && draw.target_base_counter_chance.has_value()
        && draw.target_current_counter_chance.has_value()
        && draw.attacker_action_marker.has_value()
        && draw.attack_was_critical.has_value()
        && draw.counter_rand.has_value();
}

CounterInputs make_counter_inputs(const CounterCheckpointDraw& draw) {
    CounterInputs inputs;
    inputs.attacker_slot = *draw.attacker_slot;
    inputs.target_slot = *draw.target_slot;
    inputs.target_status_flags = *draw.target_status_flags;
    inputs.target_movement_flags = *draw.target_movement_flags;
    inputs.target_base_counter_chance = *draw.target_base_counter_chance;
    inputs.target_current_counter_chance = *draw.target_current_counter_chance;
    inputs.attacker_action_marker = *draw.attacker_action_marker;
    inputs.attack_was_critical = *draw.attack_was_critical != 0;
    return inputs;
}

void simulate_live_gate(CounterCheckpointSummary& summary, CounterCheckpointDraw& draw) {
    draw.live_gate_inputs_complete = has_complete_live_gate_inputs(draw);
    if (!draw.live_gate_inputs_complete) {
        return;
    }

    const auto simulation = simulate_counter_check_from_rand(
        make_counter_inputs(draw),
        static_cast<std::uint16_t>(*draw.counter_rand));
    draw.live_gate_simulated = true;
    draw.expected_counter_result = simulation.counter ? 1 : 0;
    if (simulation.queued_field7_0xc.has_value()) {
        draw.expected_queued_field7_0xc = *simulation.queued_field7_0xc;
    }
    draw.expected_updated_current_counter_chance = simulation.updated_current_counter_chance;
    draw.expected_reason = simulation.reason;
    ++summary.live_gate_simulated_draws;

    if (draw.counter_result.has_value()) {
        draw.counter_result_matches = *draw.counter_result == *draw.expected_counter_result;
        if (draw.counter_result_matches) {
            ++summary.counter_result_matches;
        } else {
            ++summary.counter_result_mismatches;
        }
    }
    if (draw.queued_field7_0xc.has_value() && draw.expected_queued_field7_0xc.has_value()) {
        draw.queued_field_matches = *draw.queued_field7_0xc == *draw.expected_queued_field7_0xc;
        if (draw.queued_field_matches) {
            ++summary.queued_field_matches;
        } else {
            ++summary.queued_field_mismatches;
        }
    }
    if (draw.updated_current_counter_chance.has_value()) {
        draw.counter_chance_update_matches =
            *draw.updated_current_counter_chance == *draw.expected_updated_current_counter_chance;
        if (draw.counter_chance_update_matches) {
            ++summary.counter_chance_update_matches;
        } else {
            ++summary.counter_chance_update_mismatches;
        }
    }
}

CounterCheckpointStatus classify_status(const CounterCheckpointSummary& summary) {
    if (summary.expected_counter_roll_ceiling.has_value()
        && summary.observed_counter_rolls > *summary.expected_counter_roll_ceiling) {
        return CounterCheckpointStatus::ExceedsExpectedCeiling;
    }

    if (summary.live_gate_simulated_draws > 0) {
        if (summary.live_gate_simulated_draws < summary.observed_counter_rolls) {
            return CounterCheckpointStatus::MissingLiveGateFields;
        }
        for (const auto& draw : summary.draws) {
            if (!draw.live_gate_simulated) {
                continue;
            }
            if (!draw.counter_result.has_value()
                || !draw.updated_current_counter_chance.has_value()
                || (draw.expected_queued_field7_0xc.has_value()
                    && !draw.queued_field7_0xc.has_value())) {
                return CounterCheckpointStatus::MissingLiveGateFields;
            }
        }
        if (summary.counter_result_mismatches > 0) {
            return CounterCheckpointStatus::CounterResultMismatch;
        }
        if (summary.queued_field_mismatches > 0) {
            return CounterCheckpointStatus::CounterQueueMismatch;
        }
        if (summary.counter_chance_update_mismatches > 0) {
            return CounterCheckpointStatus::CounterChanceUpdateMismatch;
        }
        return CounterCheckpointStatus::MatchesLiveGate;
    }

    for (const auto& draw : summary.draws) {
        if (has_any_live_gate_fields(draw) && !draw.live_gate_inputs_complete) {
            return CounterCheckpointStatus::MissingLiveGateFields;
        }
    }

    if (summary.expected_counter_roll_ceiling.has_value()) {
        return CounterCheckpointStatus::WithinExpectedCeiling;
    }
    return CounterCheckpointStatus::ObservedOnly;
}

} // namespace

CounterCheckpointExpectation first_battle_counter_checkpoint_expectation(int counter_draw_candidate_events) {
    CounterCheckpointExpectation expectation;
    expectation.expected_counter_roll_ceiling = counter_draw_candidate_events;
    return expectation;
}

CounterCheckpointSummary summarize_counter_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<int> expected_counter_roll_ceiling) {
    CounterCheckpointSummary summary;
    summary.expected_counter_roll_ceiling = expected_counter_roll_ceiling;

    for (const auto& event : events) {
        if (!owner_is(event, kCounterOwner)) {
            continue;
        }

        CounterCheckpointDraw draw;
        draw.draw_index = event.rng_draw_index_before;
        draw.attacker_slot = parse_field_int(event, "attacker_slot");
        draw.target_slot = event.target_slot.has_value() ? event.target_slot : parse_field_int(event, "target_slot");
        draw.target_status_flags = parse_field_int(event, "target_status_flags");
        draw.target_movement_flags = parse_field_int(event, "target_movement_flags");
        draw.target_base_counter_chance = parse_field_int(event, "target_base_counter_chance");
        draw.target_current_counter_chance = parse_field_int(event, "target_current_counter_chance");
        draw.attacker_action_marker = parse_field_int(event, "attacker_action_marker");
        draw.attack_was_critical = parse_field_int(event, "attack_was_critical");
        draw.counter_rand = rand_value_from_event(event, summary.seed_transition_mismatches);
        draw.counter_result = parse_field_int(event, "counter_result");
        draw.queued_field7_0xc = parse_field_int(event, "queued_field7_0xc");
        draw.updated_current_counter_chance = parse_field_int(event, "updated_current_counter_chance");

        ++summary.observed_counter_rolls;
        if (draw.draw_index.has_value()) {
            if (!summary.first_counter_roll_draw_index.has_value()) {
                summary.first_counter_roll_draw_index = *draw.draw_index;
            }
            summary.last_counter_roll_draw_index = *draw.draw_index;
        }
        if (draw.attacker_slot.has_value() && draw.target_slot.has_value()) {
            ++summary.draws_with_actor_slots;
        }
        if (draw.target_status_flags.has_value()
            && draw.target_movement_flags.has_value()
            && draw.target_base_counter_chance.has_value()
            && draw.target_current_counter_chance.has_value()
            && draw.attack_was_critical.has_value()) {
            ++summary.draws_with_gate_inputs;
        }
        if (draw.counter_rand.has_value()) {
            ++summary.draws_with_rand_value;
        }
        if (draw.counter_result.has_value()) {
            ++summary.draws_with_counter_result;
        }
        if (draw.queued_field7_0xc.has_value()) {
            ++summary.draws_with_queue_result;
        }
        if (draw.updated_current_counter_chance.has_value()) {
            ++summary.draws_with_counter_chance_update;
        }
        simulate_live_gate(summary, draw);
        summary.draws.push_back(std::move(draw));
    }

    summary.status = classify_status(summary);
    return summary;
}

const char* counter_checkpoint_status_name(CounterCheckpointStatus status) {
    switch (status) {
    case CounterCheckpointStatus::ObservedOnly:
        return "ObservedOnly";
    case CounterCheckpointStatus::WithinExpectedCeiling:
        return "WithinExpectedCeiling";
    case CounterCheckpointStatus::ExceedsExpectedCeiling:
        return "ExceedsExpectedCeiling";
    case CounterCheckpointStatus::MatchesLiveGate:
        return "MatchesLiveGate";
    case CounterCheckpointStatus::MissingLiveGateFields:
        return "MissingLiveGateFields";
    case CounterCheckpointStatus::CounterResultMismatch:
        return "CounterResultMismatch";
    case CounterCheckpointStatus::CounterQueueMismatch:
        return "CounterQueueMismatch";
    case CounterCheckpointStatus::CounterChanceUpdateMismatch:
        return "CounterChanceUpdateMismatch";
    default:
        return "Unknown";
    }
}

const char* first_battle_counter_checkpoint_rule_detail() {
    return "first-battle counter rolls at 80081a88 are bounded by nonlethal observed attacks; crit, status, side, and movement gates can suppress the draw";
}

} // namespace savor::predict
