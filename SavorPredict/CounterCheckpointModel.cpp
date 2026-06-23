#include "CounterCheckpointModel.h"

#include <cstdlib>
#include <initializer_list>
#include <string>
#include <string_view>
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

bool event_named(const CheckpointEvent& event, std::initializer_list<std::string_view> names) {
    for (const auto name : names) {
        if (event.function == name || event.checkpoint == name) {
            return true;
        }
    }
    return false;
}

bool field_truthy(const CheckpointEvent& event, const char* key) {
    const auto value = parse_field_int(event, key);
    return value.has_value() && *value != 0;
}

bool is_counter_gate_attempt(const CheckpointEvent& event) {
    return event.pc == "800819D0"
        || event_named(event, {
            "counter_gate_entry",
            "counter_gate",
            "should_counter_gate",
        });
}

bool is_counter_gate_detail(const CheckpointEvent& event) {
    return event_named(event, {
            "counter_gate_inputs",
            "counter_roll_compare",
            "counter_gate_result",
            "counter_gate_success",
            "counter_gate_return",
        })
        || event.pc == "800819FC"
        || event.pc == "80081AB0"
        || event.pc == "80081B54"
        || event.pc == "80081B7C"
        || event.pc == "80081B80";
}

bool is_counter_follow_up(const CheckpointEvent& event) {
    return event_named(event, {
               "counter_follow_up",
               "counter_followup",
               "counter_setup_action",
               "counter_setupTurnAction",
               "counter_followup_dispatch",
               "counter_followup_action",
           })
        || (event.pc == "80082134" && field_truthy(event, "counter_follow_up"));
}

std::optional<int> actor_slot_from_event(const CheckpointEvent& event) {
    return parse_first_field_int(event, {"attacker_slot", "actor_slot", "active_slot"});
}

std::optional<int> target_slot_from_event(const CheckpointEvent& event) {
    if (event.target_slot.has_value()) {
        return event.target_slot;
    }
    return parse_first_field_int(event, {"target_slot", "target"});
}

std::optional<int> slot_field_from_event(
    const CheckpointEvent& event,
    std::optional<int> slot,
    const char* suffix) {
    if (!slot.has_value() || *slot < 0) {
        return std::nullopt;
    }
    const std::string field_name =
        "slot" + std::to_string(*slot) + "_" + suffix;
    return parse_field_int(event, field_name.c_str());
}

std::optional<int> attacker_action_marker_from_event(
    const CheckpointEvent& event,
    std::optional<int> attacker_slot) {
    if (const auto direct = parse_first_field_int(
            event,
            {"attacker_action_marker", "action_marker"}); direct.has_value()) {
        return direct;
    }
    return slot_field_from_event(event, attacker_slot, "action_marker_0x0");
}

std::optional<int> attack_was_critical_from_event(
    const CheckpointEvent& event,
    std::optional<int> target_slot) {
    if (const auto direct = parse_first_field_int(
            event,
            {"attack_was_critical", "critical_marker"}); direct.has_value()) {
        return direct;
    }
    return slot_field_from_event(event, target_slot, "critical_marker_0x8");
}

std::optional<int> queued_field_from_event(
    const CheckpointEvent& event,
    std::optional<int> target_slot) {
    if (const auto direct = parse_first_field_int(
            event,
            {"queued_field7_0xc", "field7_0xc", "counter_queue_result"}); direct.has_value()) {
        return direct;
    }
    return slot_field_from_event(event, target_slot, "attack_result_0xc");
}

bool event_has_counter_chance_update(const CheckpointEvent& event) {
    return event_named(event, {
            "counter_gate_result",
            "counter_gate_success",
            "counter_gate_return",
        })
        || event.pc == "80081B54"
        || event.pc == "80081B7C"
        || event.pc == "80081B80";
}

std::optional<int> updated_current_counter_chance_from_event(const CheckpointEvent& event) {
    if (const auto direct = parse_field_int(event, "updated_current_counter_chance"); direct.has_value()) {
        return direct;
    }
    if (event_has_counter_chance_update(event)) {
        return parse_field_int(event, "target_current_counter_chance");
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

bool has_complete_live_gate_inputs_without_rand(const CounterCheckpointDraw& draw) {
    return draw.attacker_slot.has_value()
        && draw.target_slot.has_value()
        && draw.target_status_flags.has_value()
        && draw.target_movement_flags.has_value()
        && draw.target_base_counter_chance.has_value()
        && draw.target_current_counter_chance.has_value()
        && draw.attacker_action_marker.has_value()
        && draw.attack_was_critical.has_value();
}

bool has_complete_live_gate_inputs(const CounterCheckpointDraw& draw) {
    return has_complete_live_gate_inputs_without_rand(draw)
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

bool counter_gate_requires_roll(const CounterInputs& inputs) {
    if ((inputs.target_status_flags & 0x6D00) != 0) {
        return false;
    }

    const bool attacker_is_pc = inputs.attacker_slot < 4;
    const bool target_is_pc = inputs.target_slot < 4;
    if (attacker_is_pc == target_is_pc) {
        return false;
    }

    if (inputs.attack_was_critical) {
        return false;
    }

    const bool force_counter = (inputs.target_status_flags & 0x2) != 0
        || (inputs.target_status_flags & 0x800000) != 0;
    if (force_counter) {
        return false;
    }

    return inputs.target_base_counter_chance != 0;
}

void apply_counter_simulation_result(
    CounterCheckpointSummary& summary,
    CounterCheckpointDraw& draw,
    const CounterSimulation& simulation) {
    draw.live_gate_simulated = true;
    draw.expected_counter_result = simulation.counter ? 1 : 0;
    if (simulation.queued_field7_0xc.has_value()) {
        draw.expected_queued_field7_0xc = *simulation.queued_field7_0xc;
    }
    draw.expected_updated_current_counter_chance = simulation.updated_current_counter_chance;
    draw.expected_reason = simulation.reason;
    draw.expected_counter_follow_up = simulation.counter ? 1 : 0;

    if (draw.kind == CounterCheckpointKind::CounterRoll) {
        ++summary.live_gate_simulated_draws;
    } else if (draw.kind == CounterCheckpointKind::GateAttempt
        && draw.expected_counter_rolls.has_value()
        && *draw.expected_counter_rolls == 0) {
        ++summary.no_draw_gate_attempts_simulated;
    }
    if (draw.kind == CounterCheckpointKind::GateAttempt && simulation.counter) {
        ++summary.expected_counter_follow_up_events;
    }

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

void simulate_live_gate(CounterCheckpointSummary& summary, CounterCheckpointDraw& draw) {
    draw.live_gate_inputs_complete = draw.kind == CounterCheckpointKind::GateAttempt
        ? has_complete_live_gate_inputs_without_rand(draw)
        : has_complete_live_gate_inputs(draw);
    if (!draw.live_gate_inputs_complete) {
        return;
    }

    const auto inputs = make_counter_inputs(draw);
    const bool requires_roll = counter_gate_requires_roll(inputs);
    if (draw.kind == CounterCheckpointKind::GateAttempt) {
        draw.expected_counter_rolls = requires_roll ? 1 : 0;
        summary.expected_counter_rolls_from_gate_inputs += *draw.expected_counter_rolls;
        if (!requires_roll) {
            ++summary.expected_no_draw_gate_attempts;
        }
    }

    if (!requires_roll) {
        apply_counter_simulation_result(
            summary,
            draw,
            simulate_counter_check(0, inputs));
        return;
    }

    if (!draw.counter_rand.has_value()) {
        return;
    }

    apply_counter_simulation_result(
        summary,
        draw,
        simulate_counter_check_from_rand(
            inputs,
            static_cast<std::uint16_t>(*draw.counter_rand)));
}

void merge_optional(std::optional<int>& target, std::optional<int> value) {
    if (value.has_value()) {
        target = *value;
    }
}

void merge_optional_if_missing(std::optional<int>& target, std::optional<int> value) {
    if (!target.has_value() && value.has_value()) {
        target = *value;
    }
}

void merge_counter_fields(CounterCheckpointDraw& target, const CounterCheckpointDraw& source) {
    merge_optional_if_missing(target.draw_index, source.draw_index);
    merge_optional_if_missing(target.attacker_slot, source.attacker_slot);
    merge_optional_if_missing(target.target_slot, source.target_slot);
    merge_optional_if_missing(target.target_status_flags, source.target_status_flags);
    merge_optional_if_missing(target.target_movement_flags, source.target_movement_flags);
    merge_optional_if_missing(target.target_base_counter_chance, source.target_base_counter_chance);
    merge_optional_if_missing(target.target_current_counter_chance, source.target_current_counter_chance);
    merge_optional_if_missing(target.attacker_action_marker, source.attacker_action_marker);
    merge_optional_if_missing(target.attack_was_critical, source.attack_was_critical);
    merge_optional(target.counter_rand, source.counter_rand);
    merge_optional(target.counter_result, source.counter_result);
    merge_optional(target.queued_field7_0xc, source.queued_field7_0xc);
    merge_optional(target.updated_current_counter_chance, source.updated_current_counter_chance);
    merge_optional(target.observed_counter_follow_up, source.observed_counter_follow_up);
}

CounterCheckpointDraw make_counter_draw_from_event(
    const CheckpointEvent& event,
    CounterCheckpointKind kind,
    bool parse_rand,
    int& seed_transition_mismatches) {
    CounterCheckpointDraw draw;
    draw.kind = kind;
    draw.draw_index = event.rng_draw_index_before;
    draw.attacker_slot = actor_slot_from_event(event);
    draw.target_slot = target_slot_from_event(event);
    draw.target_status_flags = parse_field_int(event, "target_status_flags");
    draw.target_movement_flags = parse_field_int(event, "target_movement_flags");
    draw.target_base_counter_chance = parse_field_int(event, "target_base_counter_chance");
    draw.target_current_counter_chance = parse_field_int(event, "target_current_counter_chance");
    draw.attacker_action_marker = attacker_action_marker_from_event(event, draw.attacker_slot);
    draw.attack_was_critical = attack_was_critical_from_event(event, draw.target_slot);
    if (parse_rand) {
        draw.counter_rand = rand_value_from_event(event, seed_transition_mismatches);
    }
    draw.counter_result = parse_field_int(event, "counter_result");
    draw.queued_field7_0xc = queued_field_from_event(event, draw.target_slot);
    draw.updated_current_counter_chance = updated_current_counter_chance_from_event(event);
    draw.observed_counter_follow_up = parse_field_int(event, "counter_follow_up");
    return draw;
}

void recompute_counter_summary_counts(CounterCheckpointSummary& summary) {
    summary.observed_counter_rolls = 0;
    summary.first_counter_roll_draw_index.reset();
    summary.last_counter_roll_draw_index.reset();
    summary.draws_with_actor_slots = 0;
    summary.draws_with_gate_inputs = 0;
    summary.draws_with_rand_value = 0;
    summary.draws_with_counter_result = 0;
    summary.draws_with_queue_result = 0;
    summary.draws_with_counter_chance_update = 0;
    summary.live_gate_simulated_draws = 0;
    summary.observed_counter_gate_attempts = 0;
    summary.gate_attempts_with_live_inputs = 0;
    summary.expected_counter_rolls_from_gate_inputs = 0;
    summary.expected_no_draw_gate_attempts = 0;
    summary.no_draw_gate_attempts_simulated = 0;
    summary.observed_counter_follow_up_events = 0;
    summary.expected_counter_follow_up_events = 0;
    summary.counter_result_matches = 0;
    summary.counter_result_mismatches = 0;
    summary.queued_field_matches = 0;
    summary.queued_field_mismatches = 0;
    summary.counter_chance_update_matches = 0;
    summary.counter_chance_update_mismatches = 0;

    for (auto& draw : summary.draws) {
        if (draw.kind == CounterCheckpointKind::CounterRoll) {
            ++summary.observed_counter_rolls;
            if (draw.draw_index.has_value()) {
                if (!summary.first_counter_roll_draw_index.has_value()) {
                    summary.first_counter_roll_draw_index = *draw.draw_index;
                }
                summary.last_counter_roll_draw_index = *draw.draw_index;
            }
        } else if (draw.kind == CounterCheckpointKind::GateAttempt) {
            ++summary.observed_counter_gate_attempts;
        } else if (draw.kind == CounterCheckpointKind::CounterFollowUp) {
            ++summary.observed_counter_follow_up_events;
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
        if (draw.kind == CounterCheckpointKind::GateAttempt
            && has_complete_live_gate_inputs_without_rand(draw)) {
            ++summary.gate_attempts_with_live_inputs;
        }
        simulate_live_gate(summary, draw);
    }
}

CounterCheckpointStatus classify_status(const CounterCheckpointSummary& summary) {
    if (summary.expected_counter_roll_ceiling.has_value()
        && summary.observed_counter_rolls > *summary.expected_counter_roll_ceiling) {
        return CounterCheckpointStatus::ExceedsExpectedCeiling;
    }

    if (summary.observed_counter_gate_attempts > 0) {
        if (summary.gate_attempts_with_live_inputs < summary.observed_counter_gate_attempts) {
            return CounterCheckpointStatus::MissingLiveGateFields;
        }
        if (summary.observed_counter_rolls < summary.expected_counter_rolls_from_gate_inputs) {
            return CounterCheckpointStatus::MissingCounterRolls;
        }
        if (summary.observed_counter_rolls > summary.expected_counter_rolls_from_gate_inputs) {
            return CounterCheckpointStatus::UnexpectedCounterRolls;
        }
        if (summary.observed_counter_follow_up_events < summary.expected_counter_follow_up_events) {
            return CounterCheckpointStatus::MissingCounterFollowUp;
        }
        if (summary.observed_counter_follow_up_events > summary.expected_counter_follow_up_events) {
            return CounterCheckpointStatus::UnexpectedCounterFollowUp;
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
    std::optional<std::size_t> current_gate_index;
    std::optional<std::size_t> current_roll_index;

    for (const auto& event : events) {
        const bool counter_roll = owner_is(event, kCounterOwner);
        const bool gate_attempt = !counter_roll && is_counter_gate_attempt(event);
        const bool gate_detail = !counter_roll && is_counter_gate_detail(event);
        const bool follow_up = is_counter_follow_up(event);
        if (!counter_roll && !gate_attempt && !gate_detail && !follow_up) {
            continue;
        }

        if (counter_roll) {
            auto draw = make_counter_draw_from_event(
                event,
                CounterCheckpointKind::CounterRoll,
                true,
                summary.seed_transition_mismatches);
            if (current_gate_index.has_value()) {
                merge_counter_fields(draw, summary.draws[*current_gate_index]);
            }
            summary.draws.push_back(std::move(draw));
            current_roll_index = summary.draws.size() - 1u;
            if (current_gate_index.has_value()) {
                merge_counter_fields(summary.draws[*current_gate_index], summary.draws[*current_roll_index]);
            }
            continue;
        }

        if (gate_attempt) {
            auto draw = make_counter_draw_from_event(
                event,
                CounterCheckpointKind::GateAttempt,
                false,
                summary.seed_transition_mismatches);
            summary.draws.push_back(std::move(draw));
            current_gate_index = summary.draws.size() - 1u;
            current_roll_index.reset();
            continue;
        }

        if (gate_detail) {
            auto draw = make_counter_draw_from_event(
                event,
                CounterCheckpointKind::GateAttempt,
                false,
                summary.seed_transition_mismatches);
            if (!current_gate_index.has_value()) {
                summary.draws.push_back(draw);
                current_gate_index = summary.draws.size() - 1u;
            } else {
                merge_counter_fields(summary.draws[*current_gate_index], draw);
            }
            if (current_roll_index.has_value()) {
                merge_counter_fields(summary.draws[*current_roll_index], draw);
            }
            continue;
        }

        if (follow_up) {
            auto draw = make_counter_draw_from_event(
                event,
                CounterCheckpointKind::CounterFollowUp,
                false,
                summary.seed_transition_mismatches);
            summary.draws.push_back(std::move(draw));
            continue;
        }
    }

    recompute_counter_summary_counts(summary);
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
    case CounterCheckpointStatus::MissingCounterRolls:
        return "MissingCounterRolls";
    case CounterCheckpointStatus::UnexpectedCounterRolls:
        return "UnexpectedCounterRolls";
    case CounterCheckpointStatus::MissingCounterFollowUp:
        return "MissingCounterFollowUp";
    case CounterCheckpointStatus::UnexpectedCounterFollowUp:
        return "UnexpectedCounterFollowUp";
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

const char* counter_checkpoint_kind_name(CounterCheckpointKind kind) {
    switch (kind) {
    case CounterCheckpointKind::GateAttempt:
        return "GateAttempt";
    case CounterCheckpointKind::CounterRoll:
        return "CounterRoll";
    case CounterCheckpointKind::CounterFollowUp:
        return "CounterFollowUp";
    default:
        return "Unknown";
    }
}

const char* first_battle_counter_checkpoint_rule_detail() {
    return "first-battle counter rolls at 80081a88 are bounded by nonlethal observed attacks; "
           "shouldCounter checkpoints should expose the 800819d0 gate attempt, any roll, and any "
           "counter follow-up through 80082134; crit, status, side, forced-counter, "
           "zero-base-chance, and movement gates determine whether the roll or follow-up is expected";
}

} // namespace savor::predict
