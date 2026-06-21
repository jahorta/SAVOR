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

DropCheckpointStatus classify_status(std::optional<int> expected, int observed) {
    if (!expected.has_value()) {
        return DropCheckpointStatus::ObservedOnly;
    }
    if (observed < *expected) {
        return DropCheckpointStatus::MissingDropRolls;
    }
    if (observed > *expected) {
        return DropCheckpointStatus::ExtraDropRolls;
    }
    return DropCheckpointStatus::MatchesExpected;
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

    summary.status = classify_status(expected_drop_rolls, summary.observed_drop_rolls);
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
    default:
        return "Unknown";
    }
}

const char* first_battle_drop_checkpoint_rule_detail() {
    return "first-battle Soldier deaths enter enemyDropItem_8002ba8c only when curHp is below one; each enabled drop row consumes one 8002bae8 roll until a row succeeds or the table ends";
}

} // namespace savor::predict
