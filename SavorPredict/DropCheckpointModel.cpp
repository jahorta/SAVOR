#include "DropCheckpointModel.h"

#include <cstdlib>
#include <utility>

namespace savor::predict {

namespace {

constexpr std::string_view kDropOwner = "enemy_drop_roll";
constexpr std::string_view kDamageBonusOwner = "damage_low_bit_bonus";
constexpr std::string_view kCounterOwner = "counter_roll";
constexpr std::string_view kMode0eCameraOwner = "mode0e_action_view_camera";
constexpr std::string_view kMode0CameraFallbackOwner = "mode0_action_view_camera_fallback";
constexpr std::string_view kPcStatusOwner = "pc_status_attempt";
constexpr std::string_view kEnemyStatusOwner = "enemy_status_attempt";

struct FirstBattleDropRow {
    int row_index = 0;
    int item_id = -1;
    int amount = 0;
    int threshold_percent = 0;
};

bool owner_is(const CheckpointEvent& event, std::string_view owner) {
    return event.known_rng_owner == owner;
}

bool owner_is_action_view_camera(const CheckpointEvent& event) {
    return owner_is(event, kMode0eCameraOwner) || owner_is(event, kMode0CameraFallbackOwner);
}

bool owner_is_status_attempt(const CheckpointEvent& event) {
    return owner_is(event, kPcStatusOwner) || owner_is(event, kEnemyStatusOwner);
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

std::optional<FirstBattleDropRow> first_battle_drop_row(
    std::optional<int> enemy_entry_id,
    std::optional<int> row_index) {
    if (!enemy_entry_id.has_value() || *enemy_entry_id != 0 || !row_index.has_value()) {
        return std::nullopt;
    }

    if (*row_index == 1) {
        return FirstBattleDropRow{1, 273, 1, 1};
    }
    if (*row_index == 2) {
        return FirstBattleDropRow{2, 258, 1, 1};
    }
    return std::nullopt;
}

std::optional<int> effective_rand_mod100(const DropCheckpointDraw& draw) {
    if (draw.rand_mod100.has_value()) {
        return draw.rand_mod100;
    }
    if (draw.rand_value.has_value()) {
        return *draw.rand_value % 100;
    }
    return std::nullopt;
}

bool has_first_battle_enemy_entry(const DropCheckpointDraw& draw) {
    return draw.enemy_entry_id.has_value() && *draw.enemy_entry_id == 0;
}

bool has_live_outcome_fields(const DropCheckpointDraw& draw) {
    return draw.drop_row_index.has_value()
        && draw.drop_item_id.has_value()
        && draw.drop_amount.has_value()
        && effective_rand_mod100(draw).has_value()
        && draw.drop_success.has_value();
}

DropCheckpointStatus classify_status(const DropCheckpointSummary& summary) {
    if (summary.drop_table_mismatches > 0 || summary.disabled_first_battle_rows_observed > 0) {
        return DropCheckpointStatus::DropTableMismatch;
    }
    if (summary.drop_outcome_mismatches > 0) {
        return DropCheckpointStatus::DropOutcomeMismatch;
    }
    if (summary.drop_rolls_after_success > 0) {
        return DropCheckpointStatus::DropContinuationMismatch;
    }
    if (summary.expected_drop_rolls.has_value()
        && summary.observed_drop_rolls < *summary.expected_drop_rolls) {
        return DropCheckpointStatus::MissingDropRolls;
    }
    if (summary.expected_drop_rolls.has_value()
        && summary.observed_drop_rolls > *summary.expected_drop_rolls) {
        return DropCheckpointStatus::ExtraDropRolls;
    }
    if (summary.drop_rolls_missing_live_outcome_fields > 0) {
        return DropCheckpointStatus::MissingLiveDropFields;
    }
    if (summary.expected_drop_rolls.has_value()
        || summary.first_battle_drop_rows_validated > 0) {
        return DropCheckpointStatus::MatchesExpected;
    }
    return DropCheckpointStatus::ObservedOnly;
}

void apply_first_battle_drop_validation(
    DropCheckpointSummary& summary,
    DropCheckpointDraw& draw,
    bool saw_success_before_draw) {
    if (!has_first_battle_enemy_entry(draw)) {
        return;
    }

    if (saw_success_before_draw) {
        draw.roll_after_success = true;
        ++summary.drop_rolls_after_success;
    }

    const auto expected_row = first_battle_drop_row(draw.enemy_entry_id, draw.drop_row_index);
    if (!expected_row.has_value()) {
        if (draw.drop_row_index.has_value()) {
            ++summary.disabled_first_battle_rows_observed;
        } else {
            ++summary.drop_rolls_missing_live_outcome_fields;
        }
        return;
    }

    draw.first_battle_row_validated = true;
    draw.expected_drop_item_id = expected_row->item_id;
    draw.expected_drop_amount = expected_row->amount;
    draw.expected_drop_threshold = expected_row->threshold_percent;

    ++summary.first_battle_drop_rows_validated;
    if (has_live_outcome_fields(draw)) {
        ++summary.drop_rolls_with_live_outcome_fields;
    } else {
        ++summary.drop_rolls_missing_live_outcome_fields;
    }

    if (draw.drop_item_id.has_value() && draw.drop_amount.has_value()) {
        draw.drop_table_matches =
            *draw.drop_item_id == expected_row->item_id
            && *draw.drop_amount == expected_row->amount;
        if (draw.drop_table_matches) {
            ++summary.drop_table_matches;
        } else {
            ++summary.drop_table_mismatches;
        }
    }

    if (const auto mod100 = effective_rand_mod100(draw); mod100.has_value()) {
        draw.expected_drop_success = *mod100 < expected_row->threshold_percent ? 1 : 0;
    }
    if (draw.drop_success.has_value() && draw.expected_drop_success.has_value()) {
        draw.drop_outcome_matches = *draw.drop_success == *draw.expected_drop_success;
        if (draw.drop_outcome_matches) {
            ++summary.drop_outcome_matches;
        } else {
            ++summary.drop_outcome_mismatches;
        }
    }
}

} // namespace

DropCheckpointExpectation first_battle_drop_checkpoint_expectation(int expected_drop_rolls) {
    DropCheckpointExpectation expectation;
    expectation.expected_drop_rolls = expected_drop_rolls;
    return expectation;
}

DropCheckpointSummary summarize_drop_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<int> expected_drop_rolls) {
    DropCheckpointSummary summary;
    summary.expected_drop_rolls = expected_drop_rolls;
    bool saw_successful_drop = false;

    for (const auto& event : events) {
        if (owner_is(event, kDropOwner)) {
            DropCheckpointDraw draw;
            draw.draw_index = event.rng_draw_index_before;
            draw.target_slot = event.target_slot.has_value() ? event.target_slot : parse_field_int(event, "target_slot");
            draw.enemy_entry_id = parse_field_int(event, "enemy_entry_id");
            draw.drop_row_index = parse_field_int(event, "drop_row_index");
            draw.drop_item_id = parse_field_int(event, "drop_item_id");
            draw.drop_amount = parse_field_int(event, "drop_amount");
            draw.rand_value = parse_field_int(event, "rand_value");
            draw.rand_mod100 = parse_field_int(event, "rand_mod100");
            draw.drop_success = parse_field_int(event, "drop_success");

            ++summary.observed_drop_rolls;
            if (draw.draw_index.has_value()) {
                if (!summary.first_drop_roll_draw_index.has_value()) {
                    summary.first_drop_roll_draw_index = *draw.draw_index;
                }
                summary.last_drop_roll_draw_index = *draw.draw_index;
            }
            if (draw.target_slot.has_value()) {
                ++summary.draws_with_target_slot;
            }
            if (draw.drop_row_index.has_value()) {
                ++summary.draws_with_drop_row;
            }
            if (draw.rand_value.has_value()) {
                ++summary.draws_with_rand_value;
            }
            apply_first_battle_drop_validation(summary, draw, saw_successful_drop);
            if (draw.drop_success.has_value()) {
                if (*draw.drop_success != 0) {
                    ++summary.successful_drop_rolls;
                    if (!saw_successful_drop) {
                        summary.final_drop_row_index = draw.drop_row_index;
                        summary.final_drop_item_id = draw.drop_item_id;
                        summary.final_drop_amount = draw.drop_amount;
                    }
                    saw_successful_drop = true;
                } else {
                    ++summary.failed_drop_rolls;
                }
            }
            summary.draws.push_back(std::move(draw));
        } else if (owner_is(event, kDamageBonusOwner)) {
            ++summary.observed_damage_bonus_draws;
        } else if (owner_is(event, kCounterOwner)) {
            ++summary.observed_counter_rolls;
        } else if (owner_is_action_view_camera(event)) {
            ++summary.observed_action_view_camera_draws;
        } else if (owner_is_status_attempt(event)) {
            ++summary.observed_status_attempt_draws;
        }
    }

    if (summary.first_drop_roll_draw_index.has_value()) {
        for (const auto& event : events) {
            if (!event.rng_draw_index_before.has_value()) {
                continue;
            }
            const bool before_first_drop = *event.rng_draw_index_before < *summary.first_drop_roll_draw_index;
            if (owner_is(event, kDamageBonusOwner)) {
                if (before_first_drop) {
                    ++summary.damage_bonus_draws_before_first_drop;
                } else {
                    ++summary.damage_bonus_draws_after_first_drop;
                }
            } else if (owner_is(event, kCounterOwner)) {
                if (before_first_drop) {
                    ++summary.counter_rolls_before_first_drop;
                }
            } else if (owner_is_action_view_camera(event)) {
                if (before_first_drop) {
                    ++summary.action_view_camera_draws_before_first_drop;
                }
            } else if (owner_is_status_attempt(event)) {
                if (before_first_drop) {
                    ++summary.status_attempt_draws_before_first_drop;
                }
            }
        }
    }

    summary.status = classify_status(summary);
    return summary;
}

const char* drop_checkpoint_status_name(DropCheckpointStatus status) {
    switch (status) {
    case DropCheckpointStatus::ObservedOnly:
        return "ObservedOnly";
    case DropCheckpointStatus::MatchesExpected:
        return "MatchesExpected";
    case DropCheckpointStatus::MissingDropRolls:
        return "MissingDropRolls";
    case DropCheckpointStatus::ExtraDropRolls:
        return "ExtraDropRolls";
    case DropCheckpointStatus::MissingLiveDropFields:
        return "MissingLiveDropFields";
    case DropCheckpointStatus::DropTableMismatch:
        return "DropTableMismatch";
    case DropCheckpointStatus::DropOutcomeMismatch:
        return "DropOutcomeMismatch";
    case DropCheckpointStatus::DropContinuationMismatch:
        return "DropContinuationMismatch";
    default:
        return "Unknown";
    }
}

const char* first_battle_drop_checkpoint_rule_detail() {
    return "first-battle Soldier deaths enter enemyDropItem_8002ba8c only when curHp is below one; enabled entry-0 rows are Electri Box row 1 and Moonberry row 2 at one percent each, and each row consumes one 8002bae8 roll until a row succeeds or the table ends";
}

} // namespace savor::predict
