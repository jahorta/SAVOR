#pragma once

#include "CheckpointTrace.h"

#include <optional>
#include <vector>

namespace savor::predict {

enum class AttackDamageValueCheckpointStatus {
    ObservedOnly,
    MatchesFormula,
    MissingLiveDamageFields,
    IncompleteDrawSequence,
    AttackResultMismatch,
    DamageMismatch,
};

struct AttackDamageValueCheckpointEvent {
    int attack_index = 0;
    std::optional<int> active_slot;
    std::optional<int> target_slot;
    std::optional<int> hit_draw_index;
    std::optional<int> crit_draw_index;
    std::optional<int> damage_spread_draw_index;
    std::optional<int> damage_bonus_draw_index;
    std::optional<int> hit_rand;
    std::optional<int> crit_rand;
    std::optional<int> damage_spread_rand;
    std::optional<int> damage_bonus_rand;
    std::optional<int> attacker_attack;
    std::optional<int> attacker_hit;
    std::optional<int> attacker_agile;
    std::optional<int> attacker_element;
    std::optional<int> target_defense;
    std::optional<int> target_dodge;
    std::optional<int> target_element_effectiveness_tenths;
    std::optional<int> target_status_flags;
    std::optional<int> instr_param_0x6;
    std::optional<int> observed_attack_result;
    std::optional<int> observed_hit_check;
    std::optional<int> observed_damage;
    std::optional<int> expected_attack_result;
    std::optional<int> expected_hit_check;
    std::optional<int> expected_base_damage;
    std::optional<int> expected_damage;
    bool live_inputs_complete = false;
    bool required_draws_complete = false;
    bool damage_expected = false;
    bool simulated = false;
    bool attack_result_matches = false;
    bool damage_matches = false;
    int missing_live_input_fields = 0;
};

struct AttackDamageValueCheckpointSummary {
    int observed_attack_bursts = 0;
    int orphan_damage_draw_events = 0;
    int bursts_with_live_inputs = 0;
    int bursts_with_required_draws = 0;
    int bursts_with_observed_attack_result = 0;
    int bursts_with_observed_damage = 0;
    int simulated_bursts = 0;
    int attack_result_matches = 0;
    int attack_result_mismatches = 0;
    int damage_matches = 0;
    int damage_mismatches = 0;
    int missing_live_field_bursts = 0;
    int incomplete_draw_bursts = 0;
    int seed_transition_mismatches = 0;
    std::optional<int> first_hit_draw_index;
    std::optional<int> first_damage_value_draw_index;
    std::vector<AttackDamageValueCheckpointEvent> attacks;
    AttackDamageValueCheckpointStatus status = AttackDamageValueCheckpointStatus::ObservedOnly;
};

AttackDamageValueCheckpointSummary summarize_attack_damage_value_checkpoints(
    const std::vector<CheckpointEvent>& events);
const char* attack_damage_value_checkpoint_status_name(AttackDamageValueCheckpointStatus status);
const char* first_battle_attack_damage_value_checkpoint_rule_detail();

} // namespace savor::predict
