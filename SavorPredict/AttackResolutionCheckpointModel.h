#pragma once

#include "CheckpointTrace.h"
#include "ProgressEventParser.h"

#include <optional>
#include <string_view>
#include <vector>

namespace savor::predict {

enum class AttackResolutionCheckpointStatus {
    ObservedOnly,
    MatchesExpected,
    MissingHitDraws,
    ExtraHitDraws,
    MissingDamageDraws,
    ExtraDamageDraws,
    CritDrawCountMismatch,
    DamagePairOrderMismatch,
};

struct AttackResolutionCheckpointExpectation {
    int observed_attack_events = 0;
    int expected_hit_draws = 0;
    int expected_damage_spread_draws = 0;
    int expected_damage_bonus_draws = 0;
    std::optional<int> expected_crit_draws;
    std::string_view hit_owner = "attack_hit_dodge";
    std::string_view crit_owner = "attack_critical";
    std::string_view damage_spread_owner = "damage_spread";
    std::string_view damage_bonus_owner = "damage_low_bit_bonus";
};

struct AttackResolutionCheckpointSummary {
    std::optional<int> expected_attack_events;
    std::optional<int> expected_crit_draws;
    int observed_hit_draws = 0;
    int observed_crit_draws = 0;
    int observed_damage_spread_draws = 0;
    int observed_damage_bonus_draws = 0;
    std::optional<int> first_hit_draw_index;
    std::optional<int> first_damage_spread_draw_index;
    int damage_pairs_in_order = 0;
    int damage_pairs_out_of_order = 0;
    AttackResolutionCheckpointStatus status = AttackResolutionCheckpointStatus::ObservedOnly;
};

AttackResolutionCheckpointExpectation first_battle_attack_resolution_checkpoint_expectation(
    const ParsedProgressEvents& events,
    std::optional<int> expected_crit_draws);
AttackResolutionCheckpointSummary summarize_attack_resolution_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<int> expected_attack_events,
    std::optional<int> expected_crit_draws);
const char* attack_resolution_checkpoint_status_name(AttackResolutionCheckpointStatus status);
const char* first_battle_attack_resolution_checkpoint_rule_detail();

} // namespace savor::predict
