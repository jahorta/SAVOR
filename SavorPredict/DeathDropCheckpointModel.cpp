#include "DeathDropCheckpointModel.h"

#include <cstdlib>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace savor::predict {

namespace {

constexpr std::string_view kDropOwner = "enemy_drop_roll";

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

bool is_damage_apply_checkpoint(const CheckpointEvent& event) {
    return event.pc == "8002DD14"
        || event_named(event, {
            "zzDealDamage",
            "zzDealDamage_8002dc38",
            "deal_damage",
            "damage_apply",
            "zz_deal_damage",
        });
}

bool is_death_handler_checkpoint(const CheckpointEvent& event) {
    return event.pc == "8002BC4C"
        || event.pc == "8002BD20"
        || event_named(event, {
            "HandleCombatantDeath",
            "HandleCombatantDeath_8002bc4c",
            "death_handler",
            "combatant_death",
        });
}

bool is_drop_entry_checkpoint(const CheckpointEvent& event) {
    if (owner_is(event, kDropOwner)) {
        return false;
    }
    return event.pc == "8002BA8C"
        || event.pc == "8002BAD8"
        || event_named(event, {
            "enemyDropItem",
            "enemyDropItem_8002ba8c",
            "enemy_drop_entry",
            "drop_entry",
        });
}

std::optional<int> target_slot_from_event(const CheckpointEvent& event) {
    if (event.target_slot.has_value()) {
        return event.target_slot;
    }
    return parse_first_field_int(event, {"target_slot", "target", "defender_slot"});
}

std::optional<int> attacker_slot_from_event(const CheckpointEvent& event) {
    if (event.active_slot.has_value()) {
        return event.active_slot;
    }
    return parse_first_field_int(event, {"attacker_slot", "active_slot", "actor_slot"});
}

DeathDropCheckpointEvent make_event(
    const CheckpointEvent& event,
    DeathDropCheckpointKind kind) {
    DeathDropCheckpointEvent observed;
    observed.kind = kind;
    observed.draw_index = event.rng_draw_index_before;
    observed.target_slot = target_slot_from_event(event);
    observed.attacker_slot = attacker_slot_from_event(event);
    observed.enemy_entry_id = parse_first_field_int(event, {"enemy_entry_id", "enemy_id"});
    observed.damage = parse_first_field_int(event, {"damage", "observed_damage", "damage_value"});
    observed.hp_before = parse_first_field_int(event, {"hp_before", "target_hp_before"});
    observed.hp_after = parse_first_field_int(event, {"hp_after", "target_hp_after"});
    observed.cur_hp = parse_first_field_int(event, {"cur_hp", "target_cur_hp", "current_hp"});
    observed.lethal = parse_first_field_int(event, {"lethal", "target_died", "killed"});
    observed.entered_enemy_reward =
        parse_first_field_int(event, {"entered_enemy_reward", "enemy_reward_path"});
    observed.called_enemy_drop =
        parse_first_field_int(event, {"called_enemy_drop", "called_drop", "entered_drop"});
    observed.drop_row_index = parse_field_int(event, "drop_row_index");
    observed.drop_success = parse_field_int(event, "drop_success");
    return observed;
}

bool same_target(
    const DeathDropCheckpointEvent& lhs,
    const DeathDropCheckpointEvent& rhs) {
    if (!lhs.target_slot.has_value() || !rhs.target_slot.has_value()) {
        return true;
    }
    return *lhs.target_slot == *rhs.target_slot;
}

bool is_lethal(const DeathDropCheckpointEvent& event) {
    if (event.lethal.has_value()) {
        return *event.lethal != 0;
    }
    if (event.hp_after.has_value()) {
        return *event.hp_after < 1;
    }
    if (event.cur_hp.has_value()) {
        return *event.cur_hp < 1;
    }
    return false;
}

bool has_live_death_fields(const DeathDropCheckpointEvent& event) {
    return event.target_slot.has_value()
        && (event.lethal.has_value() || event.hp_after.has_value() || event.cur_hp.has_value());
}

bool is_first_battle_enemy_target(const DeathDropCheckpointEvent& event) {
    if (event.enemy_entry_id.has_value()) {
        return *event.enemy_entry_id == 0;
    }
    if (event.target_slot.has_value()) {
        return *event.target_slot >= 4;
    }
    return false;
}

std::optional<std::size_t> find_next_event(
    const std::vector<DeathDropCheckpointEvent>& events,
    std::size_t start,
    DeathDropCheckpointKind kind,
    const DeathDropCheckpointEvent& target_event) {
    for (std::size_t i = start + 1; i < events.size(); ++i) {
        if (events[i].kind == DeathDropCheckpointKind::DamageApply) {
            break;
        }
        if (events[i].kind == kind && same_target(events[i], target_event)) {
            return i;
        }
    }
    return std::nullopt;
}

DeathDropCheckpointStatus classify_status(const DeathDropCheckpointSummary& summary) {
    if (summary.events.empty()) {
        return DeathDropCheckpointStatus::ObservedOnly;
    }
    if (summary.drop_rolls_before_drop_entry > 0) {
        return DeathDropCheckpointStatus::DropOrderMismatch;
    }
    if (summary.missing_live_death_field_events > 0) {
        return DeathDropCheckpointStatus::MissingLiveDeathFields;
    }
    if (summary.missing_death_handler_events > 0) {
        return DeathDropCheckpointStatus::MissingDeathHandler;
    }
    if (summary.nonlethal_events_with_unexpected_drop > 0) {
        return DeathDropCheckpointStatus::UnexpectedDropForNonlethalDamage;
    }
    if (summary.missing_drop_entry_events > 0) {
        return DeathDropCheckpointStatus::MissingDropEntry;
    }
    if (summary.missing_drop_roll_events > 0) {
        return DeathDropCheckpointStatus::MissingDropRolls;
    }
    if (summary.observed_damage_apply_events > 0) {
        return DeathDropCheckpointStatus::MatchesExpectedFlow;
    }
    return DeathDropCheckpointStatus::ObservedOnly;
}

} // namespace

DeathDropCheckpointSummary summarize_death_drop_checkpoints(
    const std::vector<CheckpointEvent>& events) {
    DeathDropCheckpointSummary summary;

    for (const auto& event : events) {
        std::optional<DeathDropCheckpointKind> kind;
        if (owner_is(event, kDropOwner)) {
            kind = DeathDropCheckpointKind::DropRoll;
        } else if (is_damage_apply_checkpoint(event)) {
            kind = DeathDropCheckpointKind::DamageApply;
        } else if (is_death_handler_checkpoint(event)) {
            kind = DeathDropCheckpointKind::DeathHandler;
        } else if (is_drop_entry_checkpoint(event)) {
            kind = DeathDropCheckpointKind::DropEntry;
        }

        if (!kind.has_value()) {
            continue;
        }

        auto observed = make_event(event, *kind);
        if (observed.kind == DeathDropCheckpointKind::DamageApply) {
            ++summary.observed_damage_apply_events;
            if (!summary.first_damage_apply_draw_index.has_value()
                && observed.draw_index.has_value()) {
                summary.first_damage_apply_draw_index = *observed.draw_index;
            }
        } else if (observed.kind == DeathDropCheckpointKind::DeathHandler) {
            ++summary.observed_death_handler_events;
            if (!summary.first_death_handler_draw_index.has_value()
                && observed.draw_index.has_value()) {
                summary.first_death_handler_draw_index = *observed.draw_index;
            }
        } else if (observed.kind == DeathDropCheckpointKind::DropEntry) {
            ++summary.observed_drop_entry_events;
            if (!summary.first_drop_entry_draw_index.has_value()
                && observed.draw_index.has_value()) {
                summary.first_drop_entry_draw_index = *observed.draw_index;
            }
        } else if (observed.kind == DeathDropCheckpointKind::DropRoll) {
            ++summary.observed_drop_rolls;
            if (!summary.first_drop_roll_draw_index.has_value()
                && observed.draw_index.has_value()) {
                summary.first_drop_roll_draw_index = *observed.draw_index;
            }
        }
        summary.events.push_back(std::move(observed));
    }

    for (std::size_t i = 0; i < summary.events.size(); ++i) {
        const auto& event = summary.events[i];
        if (event.kind != DeathDropCheckpointKind::DamageApply) {
            continue;
        }

        DeathDropDamageFlow flow;
        flow.damage_event_index = i;
        flow.target_slot = event.target_slot;
        flow.enemy_entry_id = event.enemy_entry_id;
        flow.damage = event.damage;
        flow.hp_before = event.hp_before;
        flow.hp_after = event.hp_after;
        flow.live_death_fields_complete = has_live_death_fields(event);
        flow.expects_death_handler = true;

        if (!flow.live_death_fields_complete) {
            ++summary.missing_live_death_field_events;
        } else {
            ++summary.damage_events_with_live_death_fields;
            flow.lethal = is_lethal(event) ? 1 : 0;
            if (*flow.lethal != 0) {
                ++summary.lethal_damage_events;
                flow.expects_drop_entry = is_first_battle_enemy_target(event);
            } else {
                ++summary.nonlethal_damage_events;
            }
        }

        const auto death_handler =
            find_next_event(summary.events, i, DeathDropCheckpointKind::DeathHandler, event);
        if (death_handler.has_value()) {
            flow.observed_death_handler = true;
            ++summary.damage_events_with_death_handler;
        } else {
            ++summary.missing_death_handler_events;
        }

        const auto drop_entry =
            find_next_event(summary.events, i, DeathDropCheckpointKind::DropEntry, event);
        const auto drop_roll =
            find_next_event(summary.events, i, DeathDropCheckpointKind::DropRoll, event);
        flow.observed_drop_entry = drop_entry.has_value();
        flow.observed_drop_roll = drop_roll.has_value();

        if (flow.lethal.has_value() && *flow.lethal == 0) {
            if (flow.observed_drop_entry || flow.observed_drop_roll) {
                flow.unexpected_drop_for_nonlethal = true;
                ++summary.nonlethal_events_with_unexpected_drop;
            }
        } else if (flow.lethal.has_value() && *flow.lethal != 0 && flow.expects_drop_entry) {
            if (flow.observed_drop_entry) {
                ++summary.lethal_events_with_drop_entry;
            } else {
                ++summary.missing_drop_entry_events;
            }
            if (flow.observed_drop_roll) {
                ++summary.lethal_events_with_drop_roll;
            } else {
                ++summary.missing_drop_roll_events;
            }
        }

        if (drop_roll.has_value()) {
            if (drop_entry.has_value() && *drop_entry < *drop_roll) {
                ++summary.drop_rolls_after_drop_entry;
            } else {
                flow.drop_roll_before_drop_entry = true;
                ++summary.drop_rolls_before_drop_entry;
            }
        }

        summary.damage_flows.push_back(std::move(flow));
    }

    summary.status = classify_status(summary);
    return summary;
}

const char* death_drop_checkpoint_status_name(DeathDropCheckpointStatus status) {
    switch (status) {
    case DeathDropCheckpointStatus::ObservedOnly:
        return "ObservedOnly";
    case DeathDropCheckpointStatus::MatchesExpectedFlow:
        return "MatchesExpectedFlow";
    case DeathDropCheckpointStatus::MissingLiveDeathFields:
        return "MissingLiveDeathFields";
    case DeathDropCheckpointStatus::MissingDeathHandler:
        return "MissingDeathHandler";
    case DeathDropCheckpointStatus::UnexpectedDropForNonlethalDamage:
        return "UnexpectedDropForNonlethalDamage";
    case DeathDropCheckpointStatus::MissingDropEntry:
        return "MissingDropEntry";
    case DeathDropCheckpointStatus::MissingDropRolls:
        return "MissingDropRolls";
    case DeathDropCheckpointStatus::DropOrderMismatch:
        return "DropOrderMismatch";
    default:
        return "Unknown";
    }
}

const char* death_drop_checkpoint_kind_name(DeathDropCheckpointKind kind) {
    switch (kind) {
    case DeathDropCheckpointKind::DamageApply:
        return "DamageApply";
    case DeathDropCheckpointKind::DeathHandler:
        return "DeathHandler";
    case DeathDropCheckpointKind::DropEntry:
        return "DropEntry";
    case DeathDropCheckpointKind::DropRoll:
        return "DropRoll";
    default:
        return "Unknown";
    }
}

const char* first_battle_death_drop_checkpoint_rule_detail() {
    return "first-battle death/drop flow should show zzDealDamage applying HP, HandleCombatantDeath after each damage application, enemyDropItem only for lethal first-battle Soldier damage, and drop-row RNG only after the drop entry";
}

} // namespace savor::predict
