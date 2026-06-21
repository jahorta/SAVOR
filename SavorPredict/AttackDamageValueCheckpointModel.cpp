#include "AttackDamageValueCheckpointModel.h"

#include "AttackResolutionModel.h"
#include "RngCore.h"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <utility>

namespace savor::predict {

namespace {

constexpr std::string_view kHitOwner = "attack_hit_dodge";
constexpr std::string_view kCritOwner = "attack_critical";
constexpr std::string_view kDamageSpreadOwner = "damage_spread";
constexpr std::string_view kDamageBonusOwner = "damage_low_bit_bonus";

bool owner_is(const CheckpointEvent& event, std::string_view owner) {
    return event.known_rng_owner == owner;
}

bool is_attack_resolution_draw(const CheckpointEvent& event) {
    return owner_is(event, kHitOwner)
        || owner_is(event, kCritOwner)
        || owner_is(event, kDamageSpreadOwner)
        || owner_is(event, kDamageBonusOwner);
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

void fill_if_missing(std::optional<int>& target, const std::optional<int>& source) {
    if (!target.has_value() && source.has_value()) {
        target = *source;
    }
}

std::optional<int> slot_from_event(const CheckpointEvent& event) {
    if (event.active_slot.has_value()) {
        return event.active_slot;
    }
    return parse_first_field_int(event, {"actor_slot", "attacker_slot", "slot", "active_slot"});
}

std::optional<int> target_from_event(const CheckpointEvent& event) {
    if (event.target_slot.has_value()) {
        return event.target_slot;
    }
    return parse_first_field_int(event, {"target_slot", "target"});
}

std::optional<int> rand_value_from_event(
    const CheckpointEvent& event,
    std::initializer_list<const char*> field_names,
    int& seed_transition_mismatches) {
    for (const auto* field_name : field_names) {
        if (const auto value = parse_field_int(event, field_name); value.has_value()) {
            return value;
        }
    }
    if (const auto value = parse_field_int(event, "rand_value"); value.has_value()) {
        return value;
    }
    if (!event.rng_seed_before.has_value()) {
        return std::nullopt;
    }

    const auto draw = draw_rand15(*event.rng_seed_before);
    if (event.rng_seed_after.has_value() && *event.rng_seed_after != draw.next_state) {
        ++seed_transition_mismatches;
    }
    return static_cast<int>(draw.value);
}

void merge_live_fields(
    AttackDamageValueCheckpointEvent& attack,
    const CheckpointEvent& event) {
    fill_if_missing(attack.active_slot, slot_from_event(event));
    fill_if_missing(attack.target_slot, target_from_event(event));
    fill_if_missing(attack.attacker_attack, parse_first_field_int(event, {
        "attacker_attack",
        "attack",
        "atk",
    }));
    fill_if_missing(attack.attacker_hit, parse_first_field_int(event, {
        "attacker_hit",
        "hit",
    }));
    fill_if_missing(attack.attacker_agile, parse_first_field_int(event, {
        "attacker_agile",
        "agile",
    }));
    fill_if_missing(attack.attacker_element, parse_first_field_int(event, {
        "attacker_element",
        "attack_element",
        "element",
    }));
    fill_if_missing(attack.target_defense, parse_first_field_int(event, {
        "target_defense",
        "defense",
        "def",
    }));
    fill_if_missing(attack.target_dodge, parse_first_field_int(event, {
        "target_dodge",
        "dodge",
    }));
    fill_if_missing(attack.target_element_effectiveness_tenths, parse_first_field_int(event, {
        "target_element_effectiveness_tenths",
        "element_effectiveness_tenths",
        "element_tenths",
    }));
    fill_if_missing(attack.target_status_flags, parse_first_field_int(event, {
        "target_status_flags",
        "status_flags",
    }));
    fill_if_missing(attack.instr_param_0x6, parse_first_field_int(event, {
        "instr_param_0x6",
        "instr_param",
        "param_0x6",
    }));
    fill_if_missing(attack.observed_attack_result, parse_first_field_int(event, {
        "observed_attack_result",
        "attack_result",
    }));
    fill_if_missing(attack.observed_hit_check, parse_first_field_int(event, {
        "observed_hit_check",
        "hit_check",
    }));
    fill_if_missing(attack.observed_damage, parse_first_field_int(event, {
        "observed_damage",
        "damage",
        "damage_value",
    }));
}

int count_missing_live_inputs(const AttackDamageValueCheckpointEvent& attack) {
    int missing = 0;
    const std::optional<int> AttackDamageValueCheckpointEvent::* fields[] = {
        &AttackDamageValueCheckpointEvent::attacker_attack,
        &AttackDamageValueCheckpointEvent::attacker_hit,
        &AttackDamageValueCheckpointEvent::attacker_agile,
        &AttackDamageValueCheckpointEvent::attacker_element,
        &AttackDamageValueCheckpointEvent::target_defense,
        &AttackDamageValueCheckpointEvent::target_dodge,
        &AttackDamageValueCheckpointEvent::target_element_effectiveness_tenths,
        &AttackDamageValueCheckpointEvent::target_status_flags,
        &AttackDamageValueCheckpointEvent::instr_param_0x6,
    };
    for (const auto field : fields) {
        if (!(attack.*field).has_value()) {
            ++missing;
        }
    }
    return missing;
}

BasicAttackInputs make_inputs(const AttackDamageValueCheckpointEvent& attack) {
    BasicAttackInputs inputs;
    inputs.attacker_attack = *attack.attacker_attack;
    inputs.attacker_hit = *attack.attacker_hit;
    inputs.attacker_agile = *attack.attacker_agile;
    inputs.attacker_element = *attack.attacker_element;
    inputs.target_defense = *attack.target_defense;
    inputs.target_dodge = *attack.target_dodge;
    inputs.target_element_effectiveness_tenths = *attack.target_element_effectiveness_tenths;
    inputs.target_status_flags = *attack.target_status_flags;
    inputs.instr_param_0x6 = *attack.instr_param_0x6;
    return inputs;
}

void finalize_attack(
    AttackDamageValueCheckpointSummary& summary,
    AttackDamageValueCheckpointEvent& attack) {
    attack.missing_live_input_fields = count_missing_live_inputs(attack);
    attack.live_inputs_complete = attack.missing_live_input_fields == 0;
    if (attack.live_inputs_complete) {
        ++summary.bursts_with_live_inputs;
    } else {
        ++summary.missing_live_field_bursts;
        return;
    }

    if (!attack.hit_rand.has_value()) {
        ++summary.incomplete_draw_bursts;
        return;
    }

    const auto inputs = make_inputs(attack);
    const bool hit_succeeds =
        (*attack.hit_rand % 101) >= attack_hit_threshold(inputs.attacker_hit, inputs.target_dodge);
    if (hit_succeeds && inputs.instr_param_0x6 == 0 && !attack.crit_rand.has_value()) {
        ++summary.incomplete_draw_bursts;
        return;
    }

    attack.expected_attack_result = attack_result_from_draw(
        static_cast<std::uint16_t>(*attack.hit_rand),
        attack.crit_rand.has_value()
            ? std::optional<std::uint16_t>(static_cast<std::uint16_t>(*attack.crit_rand))
            : std::nullopt,
        inputs);
    attack.expected_hit_check = *attack.expected_attack_result;
    if (*attack.expected_attack_result != 0) {
        if ((inputs.target_status_flags & 4) != 0) {
            attack.expected_hit_check = 4;
        } else if ((inputs.target_status_flags & 1) != 0) {
            attack.expected_hit_check = 3;
        }
    }

    attack.damage_expected = *attack.expected_hit_check != 0 && *attack.expected_hit_check != 4;
    if (attack.damage_expected
        && (!attack.damage_spread_rand.has_value()
            || !attack.damage_bonus_rand.has_value()
            || !attack.observed_damage.has_value())) {
        ++summary.incomplete_draw_bursts;
        return;
    }

    attack.required_draws_complete = true;
    ++summary.bursts_with_required_draws;
    attack.simulated = true;
    ++summary.simulated_bursts;

    if (attack.observed_attack_result.has_value()) {
        ++summary.bursts_with_observed_attack_result;
        attack.attack_result_matches = *attack.observed_attack_result == *attack.expected_attack_result;
        if (attack.attack_result_matches) {
            ++summary.attack_result_matches;
        } else {
            ++summary.attack_result_mismatches;
        }
    }

    if (attack.damage_expected) {
        ++summary.bursts_with_observed_damage;
        const int target_defense = (*attack.expected_attack_result == 2) ? 0 : inputs.target_defense;
        attack.expected_base_damage = (inputs.attacker_attack * 2) - target_defense;
        attack.expected_damage = roll_damage_from_draws(
            *attack.expected_base_damage,
            inputs.attacker_element,
            inputs.target_element_effectiveness_tenths,
            static_cast<std::uint16_t>(*attack.damage_spread_rand),
            static_cast<std::uint16_t>(*attack.damage_bonus_rand),
            (inputs.target_status_flags & 1) != 0);
        if (*attack.expected_attack_result == 2 && *attack.expected_damage == 0) {
            attack.expected_damage = 1;
        }
        attack.damage_matches = *attack.observed_damage == *attack.expected_damage;
        if (attack.damage_matches) {
            ++summary.damage_matches;
        } else {
            ++summary.damage_mismatches;
        }
    }
}

AttackDamageValueCheckpointStatus classify_status(const AttackDamageValueCheckpointSummary& summary) {
    if (summary.observed_attack_bursts == 0 && summary.orphan_damage_draw_events == 0) {
        return AttackDamageValueCheckpointStatus::ObservedOnly;
    }
    if (summary.missing_live_field_bursts > 0) {
        return AttackDamageValueCheckpointStatus::MissingLiveDamageFields;
    }
    if (summary.incomplete_draw_bursts > 0 || summary.orphan_damage_draw_events > 0) {
        return AttackDamageValueCheckpointStatus::IncompleteDrawSequence;
    }
    if (summary.attack_result_mismatches > 0) {
        return AttackDamageValueCheckpointStatus::AttackResultMismatch;
    }
    if (summary.damage_mismatches > 0) {
        return AttackDamageValueCheckpointStatus::DamageMismatch;
    }
    return AttackDamageValueCheckpointStatus::MatchesFormula;
}

} // namespace

AttackDamageValueCheckpointSummary summarize_attack_damage_value_checkpoints(
    const std::vector<CheckpointEvent>& events) {
    AttackDamageValueCheckpointSummary summary;
    std::optional<AttackDamageValueCheckpointEvent> current;
    int next_attack_index = 0;

    auto flush_current = [&]() {
        if (!current.has_value()) {
            return;
        }
        finalize_attack(summary, *current);
        summary.attacks.push_back(std::move(*current));
        current.reset();
    };

    for (const auto& event : events) {
        if (!is_attack_resolution_draw(event)) {
            continue;
        }

        if (owner_is(event, kHitOwner)) {
            flush_current();
            AttackDamageValueCheckpointEvent attack;
            attack.attack_index = next_attack_index++;
            attack.hit_draw_index = event.rng_draw_index_before;
            if (!summary.first_hit_draw_index.has_value() && event.rng_draw_index_before.has_value()) {
                summary.first_hit_draw_index = *event.rng_draw_index_before;
            }
            merge_live_fields(attack, event);
            attack.hit_rand = rand_value_from_event(
                event,
                {"hit_rand", "attack_hit_rand"},
                summary.seed_transition_mismatches);
            current = std::move(attack);
            ++summary.observed_attack_bursts;
            continue;
        }

        if (!current.has_value()) {
            ++summary.orphan_damage_draw_events;
            continue;
        }

        merge_live_fields(*current, event);
        if (owner_is(event, kCritOwner)) {
            current->crit_draw_index = event.rng_draw_index_before;
            current->crit_rand = rand_value_from_event(
                event,
                {"crit_rand", "critical_rand"},
                summary.seed_transition_mismatches);
        } else if (owner_is(event, kDamageSpreadOwner)) {
            current->damage_spread_draw_index = event.rng_draw_index_before;
            current->damage_spread_rand = rand_value_from_event(
                event,
                {"damage_spread_rand", "spread_rand"},
                summary.seed_transition_mismatches);
            if (!summary.first_damage_value_draw_index.has_value()
                && event.rng_draw_index_before.has_value()) {
                summary.first_damage_value_draw_index = *event.rng_draw_index_before;
            }
        } else if (owner_is(event, kDamageBonusOwner)) {
            current->damage_bonus_draw_index = event.rng_draw_index_before;
            current->damage_bonus_rand = rand_value_from_event(
                event,
                {"damage_bonus_rand", "bonus_rand", "low_bit_bonus_rand"},
                summary.seed_transition_mismatches);
            if (!summary.first_damage_value_draw_index.has_value()
                && event.rng_draw_index_before.has_value()) {
                summary.first_damage_value_draw_index = *event.rng_draw_index_before;
            }
        }
    }

    flush_current();
    summary.status = classify_status(summary);
    return summary;
}

const char* attack_damage_value_checkpoint_status_name(AttackDamageValueCheckpointStatus status) {
    switch (status) {
    case AttackDamageValueCheckpointStatus::ObservedOnly:
        return "ObservedOnly";
    case AttackDamageValueCheckpointStatus::MatchesFormula:
        return "MatchesFormula";
    case AttackDamageValueCheckpointStatus::MissingLiveDamageFields:
        return "MissingLiveDamageFields";
    case AttackDamageValueCheckpointStatus::IncompleteDrawSequence:
        return "IncompleteDrawSequence";
    case AttackDamageValueCheckpointStatus::AttackResultMismatch:
        return "AttackResultMismatch";
    case AttackDamageValueCheckpointStatus::DamageMismatch:
        return "DamageMismatch";
    default:
        return "Unknown";
    }
}

const char* first_battle_attack_damage_value_checkpoint_rule_detail() {
    return "live first-battle attack damage checkpoints should expose attacker hit/agile/attack/element, target dodge/defense/element/status, instrParam_0x6, RNG draw values, observed attack result, and observed damage so the shared attack formula can be checked per attack burst";
}

} // namespace savor::predict
