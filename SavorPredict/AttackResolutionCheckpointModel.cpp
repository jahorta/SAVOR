#include "AttackResolutionCheckpointModel.h"

#include <algorithm>

namespace savor::predict {

namespace {

constexpr std::string_view kHitOwner = "attack_hit_dodge";
constexpr std::string_view kCritOwner = "attack_critical";
constexpr std::string_view kDamageSpreadOwner = "damage_spread";
constexpr std::string_view kDamageBonusOwner = "damage_low_bit_bonus";

bool owner_is(const CheckpointEvent& event, std::string_view owner) {
    return event.known_rng_owner == owner;
}

AttackResolutionCheckpointStatus classify_status(const AttackResolutionCheckpointSummary& summary) {
    if (!summary.expected_attack_events.has_value()) {
        return AttackResolutionCheckpointStatus::ObservedOnly;
    }

    const int expected = *summary.expected_attack_events;
    if (summary.observed_hit_draws < expected) {
        return AttackResolutionCheckpointStatus::MissingHitDraws;
    }
    if (summary.observed_hit_draws > expected) {
        return AttackResolutionCheckpointStatus::ExtraHitDraws;
    }
    if (summary.observed_damage_spread_draws < expected
        || summary.observed_damage_bonus_draws < expected) {
        return AttackResolutionCheckpointStatus::MissingDamageDraws;
    }
    if (summary.observed_damage_spread_draws > expected
        || summary.observed_damage_bonus_draws > expected) {
        return AttackResolutionCheckpointStatus::ExtraDamageDraws;
    }
    if (summary.expected_crit_draws.has_value()
        && summary.observed_crit_draws != *summary.expected_crit_draws) {
        return AttackResolutionCheckpointStatus::CritDrawCountMismatch;
    }
    if (summary.damage_pairs_out_of_order > 0
        || summary.damage_pairs_in_order < expected) {
        return AttackResolutionCheckpointStatus::DamagePairOrderMismatch;
    }
    return AttackResolutionCheckpointStatus::MatchesExpected;
}

} // namespace

AttackResolutionCheckpointExpectation first_battle_attack_resolution_checkpoint_expectation(
    const ParsedProgressEvents& events,
    std::optional<int> expected_crit_draws) {
    AttackResolutionCheckpointExpectation expectation;
    expectation.observed_attack_events = static_cast<int>(events.attacks.size());
    expectation.expected_hit_draws = expectation.observed_attack_events;
    expectation.expected_damage_spread_draws = expectation.observed_attack_events;
    expectation.expected_damage_bonus_draws = expectation.observed_attack_events;
    expectation.expected_crit_draws = expected_crit_draws;
    return expectation;
}

AttackResolutionCheckpointSummary summarize_attack_resolution_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<int> expected_attack_events,
    std::optional<int> expected_crit_draws) {
    AttackResolutionCheckpointSummary summary;
    summary.expected_attack_events = expected_attack_events;
    summary.expected_crit_draws = expected_crit_draws;

    std::vector<int> spread_indexes;
    std::vector<int> bonus_indexes;
    for (const auto& event : events) {
        if (owner_is(event, kHitOwner)) {
            ++summary.observed_hit_draws;
            if (!summary.first_hit_draw_index.has_value() && event.rng_draw_index_before.has_value()) {
                summary.first_hit_draw_index = *event.rng_draw_index_before;
            }
        } else if (owner_is(event, kCritOwner)) {
            ++summary.observed_crit_draws;
        } else if (owner_is(event, kDamageSpreadOwner)) {
            ++summary.observed_damage_spread_draws;
            if (event.rng_draw_index_before.has_value()) {
                spread_indexes.push_back(*event.rng_draw_index_before);
                if (!summary.first_damage_spread_draw_index.has_value()) {
                    summary.first_damage_spread_draw_index = *event.rng_draw_index_before;
                }
            }
        } else if (owner_is(event, kDamageBonusOwner)) {
            ++summary.observed_damage_bonus_draws;
            if (event.rng_draw_index_before.has_value()) {
                bonus_indexes.push_back(*event.rng_draw_index_before);
            }
        }
    }

    const auto pair_count = std::min(spread_indexes.size(), bonus_indexes.size());
    for (std::size_t i = 0; i < pair_count; ++i) {
        if (spread_indexes[i] < bonus_indexes[i]) {
            ++summary.damage_pairs_in_order;
        } else {
            ++summary.damage_pairs_out_of_order;
        }
    }
    const auto missing_pairs = spread_indexes.size() > bonus_indexes.size()
        ? spread_indexes.size() - bonus_indexes.size()
        : bonus_indexes.size() - spread_indexes.size();
    summary.damage_pairs_out_of_order += static_cast<int>(missing_pairs);

    summary.status = classify_status(summary);
    return summary;
}

const char* attack_resolution_checkpoint_status_name(AttackResolutionCheckpointStatus status) {
    switch (status) {
    case AttackResolutionCheckpointStatus::ObservedOnly:
        return "ObservedOnly";
    case AttackResolutionCheckpointStatus::MatchesExpected:
        return "MatchesExpected";
    case AttackResolutionCheckpointStatus::MissingHitDraws:
        return "MissingHitDraws";
    case AttackResolutionCheckpointStatus::ExtraHitDraws:
        return "ExtraHitDraws";
    case AttackResolutionCheckpointStatus::MissingDamageDraws:
        return "MissingDamageDraws";
    case AttackResolutionCheckpointStatus::ExtraDamageDraws:
        return "ExtraDamageDraws";
    case AttackResolutionCheckpointStatus::CritDrawCountMismatch:
        return "CritDrawCountMismatch";
    case AttackResolutionCheckpointStatus::DamagePairOrderMismatch:
        return "DamagePairOrderMismatch";
    default:
        return "Unknown";
    }
}

const char* first_battle_attack_resolution_checkpoint_rule_detail() {
    return "each observed first-battle damage event should have one hit/dodge draw followed by one damage spread draw and one low-bit bonus draw; crit draws are gated by instrParam_0x6 and hit result";
}

} // namespace savor::predict
