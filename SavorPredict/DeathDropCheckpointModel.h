#pragma once

#include "CheckpointTrace.h"

#include <optional>
#include <vector>

namespace savor::predict {

enum class DeathDropCheckpointStatus {
    ObservedOnly,
    MatchesExpectedFlow,
    MissingLiveDeathFields,
    MissingDeathHandler,
    UnexpectedDropForNonlethalDamage,
    MissingDropEntry,
    MissingDropRolls,
    DropOrderMismatch,
};

enum class DeathDropCheckpointKind {
    DamageApply,
    DeathHandler,
    DropEntry,
    DropRoll,
};

struct DeathDropCheckpointEvent {
    DeathDropCheckpointKind kind = DeathDropCheckpointKind::DamageApply;
    std::optional<int> draw_index;
    std::optional<int> target_slot;
    std::optional<int> attacker_slot;
    std::optional<int> enemy_entry_id;
    std::optional<int> damage;
    std::optional<int> hp_before;
    std::optional<int> hp_after;
    std::optional<int> cur_hp;
    std::optional<int> lethal;
    std::optional<int> entered_enemy_reward;
    std::optional<int> called_enemy_drop;
    std::optional<int> drop_row_index;
    std::optional<int> drop_success;
};

struct DeathDropDamageFlow {
    std::size_t damage_event_index = 0;
    std::optional<int> target_slot;
    std::optional<int> enemy_entry_id;
    std::optional<int> damage;
    std::optional<int> hp_before;
    std::optional<int> hp_after;
    std::optional<int> lethal;
    bool live_death_fields_complete = false;
    bool expects_death_handler = false;
    bool observed_death_handler = false;
    bool expects_drop_entry = false;
    bool observed_drop_entry = false;
    bool observed_drop_roll = false;
    bool unexpected_drop_for_nonlethal = false;
    bool drop_roll_before_drop_entry = false;
};

struct DeathDropCheckpointSummary {
    int observed_damage_apply_events = 0;
    int observed_death_handler_events = 0;
    int observed_drop_entry_events = 0;
    int observed_drop_rolls = 0;
    int damage_events_with_live_death_fields = 0;
    int lethal_damage_events = 0;
    int nonlethal_damage_events = 0;
    int damage_events_with_death_handler = 0;
    int lethal_events_with_drop_entry = 0;
    int lethal_events_with_drop_roll = 0;
    int nonlethal_events_with_unexpected_drop = 0;
    int drop_rolls_after_drop_entry = 0;
    int drop_rolls_before_drop_entry = 0;
    int missing_live_death_field_events = 0;
    int missing_death_handler_events = 0;
    int missing_drop_entry_events = 0;
    int missing_drop_roll_events = 0;
    std::optional<int> first_damage_apply_draw_index;
    std::optional<int> first_death_handler_draw_index;
    std::optional<int> first_drop_entry_draw_index;
    std::optional<int> first_drop_roll_draw_index;
    std::vector<DeathDropCheckpointEvent> events;
    std::vector<DeathDropDamageFlow> damage_flows;
    DeathDropCheckpointStatus status = DeathDropCheckpointStatus::ObservedOnly;
};

DeathDropCheckpointSummary summarize_death_drop_checkpoints(
    const std::vector<CheckpointEvent>& events);
const char* death_drop_checkpoint_status_name(DeathDropCheckpointStatus status);
const char* death_drop_checkpoint_kind_name(DeathDropCheckpointKind kind);
const char* first_battle_death_drop_checkpoint_rule_detail();

} // namespace savor::predict
