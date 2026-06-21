#include "TurnOrderCheckpointModel.h"

#include <cstdlib>
#include <utility>

namespace savor::predict {

namespace {

constexpr std::string_view kTurnOrderOwner = "turn_order_priority_jitter";

bool owner_is(const CheckpointEvent& event, std::string_view owner) {
    return event.known_rng_owner == owner;
}

std::optional<int> parse_field_int(const CheckpointEvent& event, const char* key) {
    const auto found = event.fields.find(key);
    if (found == event.fields.end()) {
        return std::nullopt;
    }

    char* end = nullptr;
    const long parsed = std::strtol(found->second.c_str(), &end, 10);
    if (end == found->second.c_str() || *end != '\0') {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

TurnOrderCheckpointStatus classify_status(std::optional<int> expected, int observed) {
    if (!expected.has_value()) {
        return TurnOrderCheckpointStatus::ObservedOnly;
    }
    if (observed < *expected) {
        return TurnOrderCheckpointStatus::MissingPriorityJitterDraws;
    }
    if (observed > *expected) {
        return TurnOrderCheckpointStatus::ExtraPriorityJitterDraws;
    }
    return TurnOrderCheckpointStatus::MatchesExpected;
}

} // namespace

TurnOrderCheckpointExpectation turn_order_checkpoint_expectation(const TurnOrderSimulation& turn_order) {
    TurnOrderCheckpointExpectation expectation;
    expectation.expected_priority_jitter_draws = turn_order.draws_consumed;
    expectation.expected_queued_entries = turn_order.queued_count;
    expectation.expected_jitter_modulus = turn_order.jitter_modulus;
    return expectation;
}

TurnOrderCheckpointSummary summarize_turn_order_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<int> expected_priority_jitter_draws) {
    TurnOrderCheckpointSummary summary;
    summary.expected_priority_jitter_draws = expected_priority_jitter_draws;

    for (const auto& event : events) {
        if (!owner_is(event, kTurnOrderOwner)) {
            continue;
        }

        TurnOrderCheckpointDraw draw;
        draw.draw_index = event.rng_draw_index_before;
        draw.slot = event.active_slot.has_value() ? event.active_slot : parse_field_int(event, "slot");
        draw.quick = parse_field_int(event, "quick");
        draw.assigned_priority = parse_field_int(event, "assigned_priority");
        draw.rand_value = parse_field_int(event, "rand_value");

        ++summary.observed_priority_jitter_draws;
        if (draw.draw_index.has_value()) {
            if (!summary.first_priority_jitter_draw_index.has_value()) {
                summary.first_priority_jitter_draw_index = *draw.draw_index;
            }
            summary.last_priority_jitter_draw_index = *draw.draw_index;
        }
        if (draw.slot.has_value()) {
            ++summary.draws_with_slot;
        }
        if (draw.quick.has_value()) {
            ++summary.draws_with_quick;
        }
        if (draw.assigned_priority.has_value()) {
            ++summary.draws_with_assigned_priority;
        }
        if (draw.rand_value.has_value()) {
            ++summary.draws_with_rand_value;
        }
        summary.draws.push_back(std::move(draw));
    }

    summary.status = classify_status(expected_priority_jitter_draws, summary.observed_priority_jitter_draws);
    return summary;
}

const char* turn_order_checkpoint_status_name(TurnOrderCheckpointStatus status) {
    switch (status) {
    case TurnOrderCheckpointStatus::ObservedOnly:
        return "ObservedOnly";
    case TurnOrderCheckpointStatus::MatchesExpected:
        return "MatchesExpected";
    case TurnOrderCheckpointStatus::MissingPriorityJitterDraws:
        return "MissingPriorityJitterDraws";
    case TurnOrderCheckpointStatus::ExtraPriorityJitterDraws:
        return "ExtraPriorityJitterDraws";
    default:
        return "Unknown";
    }
}

const char* turn_order_checkpoint_rule_detail() {
    return "first-battle turn order should spend one 800711f8 priority-jitter draw per queued action when jitter_modulus is nonzero";
}

} // namespace savor::predict
