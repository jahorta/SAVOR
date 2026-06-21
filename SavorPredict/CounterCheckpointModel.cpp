#include "CounterCheckpointModel.h"

#include <cstdlib>
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

CounterCheckpointStatus classify_status(std::optional<int> expected_ceiling, int observed) {
    if (!expected_ceiling.has_value()) {
        return CounterCheckpointStatus::ObservedOnly;
    }
    if (observed > *expected_ceiling) {
        return CounterCheckpointStatus::ExceedsExpectedCeiling;
    }
    return CounterCheckpointStatus::WithinExpectedCeiling;
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
        draw.counter_rand = parse_field_int(event, "counter_rand");
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
        if (draw.counter_result.has_value()) {
            ++summary.draws_with_counter_result;
        }
        summary.draws.push_back(std::move(draw));
    }

    summary.status = classify_status(expected_counter_roll_ceiling, summary.observed_counter_rolls);
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
    default:
        return "Unknown";
    }
}

const char* first_battle_counter_checkpoint_rule_detail() {
    return "first-battle counter rolls at 80081a88 are bounded by nonlethal observed attacks; crit, status, side, and movement gates can suppress the draw";
}

} // namespace savor::predict
