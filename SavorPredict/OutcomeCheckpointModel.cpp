#include "OutcomeCheckpointModel.h"

#include <cstdlib>
#include <utility>

namespace savor::predict {

namespace {

constexpr const char* kEndTurnStatusCleanupOwner = "end_turn_status_cleanup";
constexpr const char* kLevelUpStatRoll1Owner = "level_up_stat_roll_1";
constexpr const char* kLevelUpStatRoll2Owner = "level_up_stat_roll_2";
constexpr const char* kLevelUpStatRoll3Owner = "level_up_stat_roll_3";

bool owner_is(const CheckpointEvent& event, const char* owner) {
    return event.known_rng_owner == owner;
}

bool owner_is_level_up(const CheckpointEvent& event) {
    return owner_is(event, kLevelUpStatRoll1Owner)
        || owner_is(event, kLevelUpStatRoll2Owner)
        || owner_is(event, kLevelUpStatRoll3Owner);
}

std::optional<int> parse_field_int(const CheckpointEvent& event, const char* field_name) {
    const auto found = event.fields.find(field_name);
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

} // namespace

OutcomeCheckpointExpectation first_battle_outcome_checkpoint_expectation(int battle_outcome) {
    OutcomeCheckpointExpectation expectation;

    if (battle_outcome == 6) {
        expectation.expected_end_turn_status_draws = 0;
        expectation.expected_level_up_stat_rolls = 0;
    }

    return expectation;
}

OutcomeCheckpointSummary summarize_outcome_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<int> expected_end_turn_status_draws,
    std::optional<int> expected_level_up_stat_rolls) {
    OutcomeCheckpointSummary summary;
    summary.expected_end_turn_status_draws = expected_end_turn_status_draws;
    summary.expected_level_up_stat_rolls = expected_level_up_stat_rolls;

    for (const auto& event : events) {
        const bool end_turn_status = owner_is(event, kEndTurnStatusCleanupOwner);
        const bool level_up_roll = owner_is_level_up(event);
        if (!end_turn_status && !level_up_roll) {
            continue;
        }

        OutcomeCheckpointDraw draw;
        draw.draw_index = event.rng_draw_index_before;
        draw.owner = event.known_rng_owner;
        draw.actor_slot = parse_field_int(event, "actor_slot");
        if (!draw.actor_slot.has_value()) {
            draw.actor_slot = event.active_slot;
        }
        draw.status_effect_id = parse_field_int(event, "status_effect_id");
        draw.stat_index = parse_field_int(event, "stat_index");
        draw.level = parse_field_int(event, "level");
        draw.rand_value = parse_field_int(event, "rand_value");

        if (end_turn_status) {
            ++summary.observed_end_turn_status_draws;
            if (!summary.first_end_turn_status_draw_index.has_value()) {
                summary.first_end_turn_status_draw_index = event.rng_draw_index_before;
            }
        }
        if (level_up_roll) {
            ++summary.observed_level_up_stat_rolls;
            if (!summary.first_level_up_stat_roll_index.has_value()) {
                summary.first_level_up_stat_roll_index = event.rng_draw_index_before;
            }
        }
        if (owner_is(event, kLevelUpStatRoll1Owner)) {
            ++summary.observed_level_up_roll_1_draws;
        } else if (owner_is(event, kLevelUpStatRoll2Owner)) {
            ++summary.observed_level_up_roll_2_draws;
        } else if (owner_is(event, kLevelUpStatRoll3Owner)) {
            ++summary.observed_level_up_roll_3_draws;
        }

        if (draw.actor_slot.has_value()) {
            ++summary.draws_with_actor_slot;
        }
        if (draw.rand_value.has_value()) {
            ++summary.draws_with_rand_value;
        }

        summary.draws.push_back(std::move(draw));
    }

    if (expected_end_turn_status_draws.has_value()
        && summary.observed_end_turn_status_draws != *expected_end_turn_status_draws) {
        summary.status = OutcomeCheckpointStatus::EndTurnStatusDrawMismatch;
    } else if (expected_level_up_stat_rolls.has_value()
        && summary.observed_level_up_stat_rolls != *expected_level_up_stat_rolls) {
        summary.status = OutcomeCheckpointStatus::LevelUpStatRollMismatch;
    } else if (expected_end_turn_status_draws.has_value() || expected_level_up_stat_rolls.has_value()) {
        summary.status = OutcomeCheckpointStatus::MatchesExpected;
    } else {
        summary.status = OutcomeCheckpointStatus::ObservedOnly;
    }

    return summary;
}

const char* outcome_checkpoint_status_name(OutcomeCheckpointStatus status) {
    switch (status) {
    case OutcomeCheckpointStatus::ObservedOnly: return "ObservedOnly";
    case OutcomeCheckpointStatus::MatchesExpected: return "MatchesExpected";
    case OutcomeCheckpointStatus::EndTurnStatusDrawMismatch: return "EndTurnStatusDrawMismatch";
    case OutcomeCheckpointStatus::LevelUpStatRollMismatch: return "LevelUpStatRollMismatch";
    default: return "Unknown";
    }
}

const char* first_battle_outcome_checkpoint_rule_detail() {
    return "first-battle ReachedNextTurn jobs should skip endTurn:8006ff38 status cleanup "
           "and level-up stat rolls; Victory jobs need later EXP and threshold modeling before "
           "exact level-up expectations";
}

} // namespace savor::predict
