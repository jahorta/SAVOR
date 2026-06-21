#include "OutcomeCheckpointModel.h"

#include <cstdlib>
#include <initializer_list>
#include <string_view>
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

bool is_run_case9_checkpoint(const CheckpointEvent& event) {
    return event.pc == "8006F020"
        || event_named(event, {
            "runCase9",
            "runCase9_8006f020",
            "Battle::Run::runCase9",
            "battle_result_phase",
            "battle_result_branch",
        });
}

bool is_battle_success_checkpoint(const CheckpointEvent& event) {
    return event.pc == "8006F4B0"
        || event.pc == "8006F9F0"
        || event_named(event, {
            "endBattleSuccess",
            "endBattleSuccess_8006f4b0",
            "Battle::Run::endBattleSuccess",
            "battle_success",
            "victory_rewards",
        });
}

bool is_level_up_entry_checkpoint(const CheckpointEvent& event) {
    return !owner_is_level_up(event)
        && (event.pc == "801F2A34"
            || event_named(event, {
                "LevelUp",
                "LevelUp_801f2a34",
                "level_up_entry",
            }));
}

std::optional<int> actor_slot_from_event(const CheckpointEvent& event) {
    if (event.active_slot.has_value()) {
        return event.active_slot;
    }
    return parse_first_field_int(event, {"actor_slot", "pc_slot", "slot"});
}

std::optional<int> victory_from_event(const CheckpointEvent& event) {
    if (const auto value = parse_first_field_int(event, {"victory", "battle_success"}); value.has_value()) {
        return *value != 0 ? 1 : 0;
    }
    if (const auto outcome = parse_first_field_int(event, {"battle_outcome", "battle_result"}); outcome.has_value()) {
        return *outcome == 0 ? 1 : 0;
    }
    return std::nullopt;
}

bool is_victory_branch_event(const OutcomeCheckpointDraw& draw) {
    if (draw.kind == OutcomeCheckpointKind::BattleSuccessEntry
        || draw.kind == OutcomeCheckpointKind::LevelUpEntry
        || draw.kind == OutcomeCheckpointKind::LevelUpStatRoll) {
        return true;
    }
    return draw.victory.has_value() && *draw.victory != 0;
}

bool has_battle_success_reward_context(const OutcomeCheckpointDraw& draw) {
    return draw.exp_awarded.has_value();
}

bool has_level_up_exp_context(const OutcomeCheckpointDraw& draw) {
    return draw.actor_slot.has_value()
        && draw.level_before.has_value()
        && draw.exp_before.has_value()
        && draw.exp_after.has_value()
        && draw.next_level_exp.has_value();
}

OutcomeCheckpointDraw make_draw(
    const CheckpointEvent& event,
    OutcomeCheckpointKind kind,
    std::string owner) {
    OutcomeCheckpointDraw draw;
    draw.kind = kind;
    draw.draw_index = event.rng_draw_index_before;
    draw.owner = std::move(owner);
    draw.actor_slot = actor_slot_from_event(event);
    draw.status_effect_id = parse_field_int(event, "status_effect_id");
    draw.stat_index = parse_field_int(event, "stat_index");
    draw.level = parse_field_int(event, "level");
    draw.level_before = parse_field_int(event, "level_before");
    draw.level_after = parse_field_int(event, "level_after");
    draw.exp_before = parse_first_field_int(event, {"exp_before", "pc_exp_before"});
    draw.exp_after = parse_first_field_int(event, {"exp_after", "pc_exp_after"});
    draw.next_level_exp = parse_first_field_int(event, {"next_level_exp", "level_threshold"});
    draw.exp_awarded = parse_first_field_int(event, {"exp_awarded", "battle_exp", "party_exp"});
    draw.expected_stat_rolls =
        parse_first_field_int(event, {"expected_stat_rolls", "expected_level_up_stat_rolls"});
    draw.battle_outcome = parse_first_field_int(event, {"battle_outcome", "battle_result"});
    draw.victory = victory_from_event(event);
    draw.rand_value = parse_field_int(event, "rand_value");
    return draw;
}

bool is_reached_next_turn_zero_draw_expectation(const OutcomeCheckpointSummary& summary) {
    return summary.expected_end_turn_status_draws.has_value()
        && *summary.expected_end_turn_status_draws == 0
        && summary.expected_level_up_stat_rolls.has_value()
        && *summary.expected_level_up_stat_rolls == 0;
}

OutcomeCheckpointStatus classify_status(const OutcomeCheckpointSummary& summary) {
    if (summary.expected_end_turn_status_draws.has_value()
        && summary.observed_end_turn_status_draws != *summary.expected_end_turn_status_draws) {
        return OutcomeCheckpointStatus::EndTurnStatusDrawMismatch;
    }
    if (summary.expected_level_up_stat_rolls.has_value()
        && summary.observed_level_up_stat_rolls != *summary.expected_level_up_stat_rolls) {
        return OutcomeCheckpointStatus::LevelUpStatRollMismatch;
    }
    if (is_reached_next_turn_zero_draw_expectation(summary)
        && summary.observed_victory_branch_events > 0) {
        return OutcomeCheckpointStatus::UnexpectedVictoryBranch;
    }
    if (summary.observed_run_case9_events > 0
        && summary.observed_victory_branch_events > 0
        && summary.observed_battle_success_events == 0) {
        return OutcomeCheckpointStatus::MissingBattleSuccessEntry;
    }
    if (summary.observed_level_up_stat_rolls > 0 && summary.observed_level_up_entries == 0) {
        return OutcomeCheckpointStatus::MissingLevelUpEntry;
    }
    if (summary.stat_rolls_before_level_up_entry > 0) {
        return OutcomeCheckpointStatus::LevelUpOrderMismatch;
    }
    if (summary.observed_battle_success_events > 0
        && summary.battle_success_events_with_reward_context != summary.observed_battle_success_events) {
        return OutcomeCheckpointStatus::MissingVictoryRewardContext;
    }
    if (summary.observed_level_up_entries > 0
        && summary.level_up_entries_with_exp_context != summary.observed_level_up_entries) {
        return OutcomeCheckpointStatus::MissingLevelUpContext;
    }
    if (summary.observed_level_up_entries > 0
        && summary.level_up_entries_with_expected_rolls == summary.observed_level_up_entries
        && summary.expected_level_up_stat_rolls_from_entries != summary.observed_level_up_stat_rolls) {
        return OutcomeCheckpointStatus::LevelUpEntryRollMismatch;
    }
    if (summary.observed_run_case9_events > 0
        || summary.observed_battle_success_events > 0
        || summary.observed_level_up_entries > 0) {
        return OutcomeCheckpointStatus::MatchesExpectedFlow;
    }
    if (summary.expected_end_turn_status_draws.has_value() || summary.expected_level_up_stat_rolls.has_value()) {
        return OutcomeCheckpointStatus::MatchesExpected;
    }
    return OutcomeCheckpointStatus::ObservedOnly;
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
        const bool run_case9 = is_run_case9_checkpoint(event);
        const bool battle_success = is_battle_success_checkpoint(event);
        const bool level_up_entry = is_level_up_entry_checkpoint(event);
        if (!end_turn_status && !level_up_roll && !run_case9 && !battle_success && !level_up_entry) {
            continue;
        }

        OutcomeCheckpointKind kind = OutcomeCheckpointKind::EndTurnStatusDraw;
        std::string owner = event.known_rng_owner;
        if (level_up_roll) {
            kind = OutcomeCheckpointKind::LevelUpStatRoll;
        } else if (run_case9) {
            kind = OutcomeCheckpointKind::RunCase9Entry;
            owner = "run_case9";
        } else if (battle_success) {
            kind = OutcomeCheckpointKind::BattleSuccessEntry;
            owner = "battle_success";
        } else if (level_up_entry) {
            kind = OutcomeCheckpointKind::LevelUpEntry;
            owner = "level_up_entry";
        }

        auto draw = make_draw(event, kind, std::move(owner));

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
        if (run_case9) {
            ++summary.observed_run_case9_events;
            if (!summary.first_run_case9_draw_index.has_value()) {
                summary.first_run_case9_draw_index = event.rng_draw_index_before;
            }
        }
        if (battle_success) {
            ++summary.observed_battle_success_events;
            if (!summary.first_battle_success_draw_index.has_value()) {
                summary.first_battle_success_draw_index = event.rng_draw_index_before;
            }
            if (has_battle_success_reward_context(draw)) {
                ++summary.battle_success_events_with_reward_context;
            }
        }
        if (level_up_entry) {
            ++summary.observed_level_up_entries;
            if (!summary.first_level_up_entry_draw_index.has_value()) {
                summary.first_level_up_entry_draw_index = event.rng_draw_index_before;
            }
            if (has_level_up_exp_context(draw)) {
                ++summary.level_up_entries_with_exp_context;
            }
            if (draw.expected_stat_rolls.has_value()) {
                ++summary.level_up_entries_with_expected_rolls;
                summary.expected_level_up_stat_rolls_from_entries += *draw.expected_stat_rolls;
            }
        }
        if (is_victory_branch_event(draw)) {
            ++summary.observed_victory_branch_events;
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

    for (const auto& draw : summary.draws) {
        if (draw.kind != OutcomeCheckpointKind::LevelUpStatRoll) {
            continue;
        }
        if (summary.first_level_up_entry_draw_index.has_value()
            && draw.draw_index.has_value()
            && *draw.draw_index >= *summary.first_level_up_entry_draw_index) {
            ++summary.stat_rolls_after_level_up_entry;
        } else {
            ++summary.stat_rolls_before_level_up_entry;
        }
    }

    summary.status = classify_status(summary);
    return summary;
}

const char* outcome_checkpoint_status_name(OutcomeCheckpointStatus status) {
    switch (status) {
    case OutcomeCheckpointStatus::ObservedOnly: return "ObservedOnly";
    case OutcomeCheckpointStatus::MatchesExpected: return "MatchesExpected";
    case OutcomeCheckpointStatus::MatchesExpectedFlow: return "MatchesExpectedFlow";
    case OutcomeCheckpointStatus::EndTurnStatusDrawMismatch: return "EndTurnStatusDrawMismatch";
    case OutcomeCheckpointStatus::LevelUpStatRollMismatch: return "LevelUpStatRollMismatch";
    case OutcomeCheckpointStatus::UnexpectedVictoryBranch: return "UnexpectedVictoryBranch";
    case OutcomeCheckpointStatus::MissingBattleSuccessEntry: return "MissingBattleSuccessEntry";
    case OutcomeCheckpointStatus::MissingVictoryRewardContext: return "MissingVictoryRewardContext";
    case OutcomeCheckpointStatus::MissingLevelUpEntry: return "MissingLevelUpEntry";
    case OutcomeCheckpointStatus::MissingLevelUpContext: return "MissingLevelUpContext";
    case OutcomeCheckpointStatus::LevelUpOrderMismatch: return "LevelUpOrderMismatch";
    case OutcomeCheckpointStatus::LevelUpEntryRollMismatch: return "LevelUpEntryRollMismatch";
    default: return "Unknown";
    }
}

const char* outcome_checkpoint_kind_name(OutcomeCheckpointKind kind) {
    switch (kind) {
    case OutcomeCheckpointKind::EndTurnStatusDraw: return "EndTurnStatusDraw";
    case OutcomeCheckpointKind::RunCase9Entry: return "RunCase9Entry";
    case OutcomeCheckpointKind::BattleSuccessEntry: return "BattleSuccessEntry";
    case OutcomeCheckpointKind::LevelUpEntry: return "LevelUpEntry";
    case OutcomeCheckpointKind::LevelUpStatRoll: return "LevelUpStatRoll";
    default: return "Unknown";
    }
}

const char* first_battle_outcome_checkpoint_rule_detail() {
    return "first-battle ReachedNextTurn jobs should skip endTurn:8006ff38 status cleanup "
           "and victory reward branches; Victory jobs should prove runCase9 -> endBattleSuccess "
           "-> LevelUp entry context before exact EXP threshold and level-up stat-roll "
           "expectations are promoted";
}

} // namespace savor::predict
