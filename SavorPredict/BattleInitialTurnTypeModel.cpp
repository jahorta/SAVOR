#include "BattleInitialTurnTypeModel.h"

#include "RngCore.h"

#include <algorithm>
#include <sstream>

namespace savor::predict {
namespace {

bool valid_signed_byte(const std::optional<int>& value) {
    return value.has_value() && *value >= -128 && *value <= 127;
}

bool valid_derived_inputs(const BattleInitialTurnTypePrerequisites& input) {
    return input.vyse_level.has_value()
        && *input.vyse_level >= -128
        && *input.vyse_level <= 127
        && input.initiative.has_value()
        && *input.initiative >= 0
        && *input.initiative <= 255;
}

int derived_advantage_threshold(const BattleInitialTurnTypePrerequisites& input) {
    return std::clamp(*input.vyse_level * 4 + 30 - *input.initiative, 0, 30);
}

int derived_back_attack_threshold(const BattleInitialTurnTypePrerequisites& input) {
    return std::clamp(*input.initiative - *input.vyse_level * 4, 0, 70);
}

BattleInitialTurnTypeDraw make_draw(
    std::uint32_t seed_before,
    int threshold) {
    const auto draw = draw_rand15(seed_before);
    return BattleInitialTurnTypeDraw{
        .seed_before = seed_before,
        .seed_after = draw.next_state,
        .rand_value = draw.value,
        .roll_mod_101 = static_cast<int>(draw.value % 101),
        .threshold = threshold,
    };
}

std::string input_detail(const BattleInitialTurnTypeInput& input) {
    const auto& prerequisites = input.prerequisites;
    std::ostringstream out;
    out << "encounter_source="
        << battle_encounter_source_kind_name(prerequisites.encounter_source);
    if (prerequisites.encounter_id.has_value()) {
        out << "; encounter_id=" << *prerequisites.encounter_id;
    }
    if (prerequisites.vyse_level.has_value()) {
        out << "; vyse_level=" << *prerequisites.vyse_level;
    }
    if (prerequisites.initiative.has_value()) {
        out << "; initiative=" << *prerequisites.initiative;
    }
    if (prerequisites.advantage_chance_override.has_value()) {
        out << "; advantage_override="
            << *prerequisites.advantage_chance_override;
    }
    if (prerequisites.back_attack_chance_override.has_value()) {
        out << "; back_attack_override="
            << *prerequisites.back_attack_chance_override;
    }
    return out.str();
}

} // namespace

BattleInitialTurnTypeResult model_initial_battle_turn_type(
    const BattleInitialTurnTypeInput& input) {
    BattleInitialTurnTypeResult result;
    result.rng_seed_before = input.rng_seed_before;
    result.rng_seed_after = input.rng_seed_before;
    result.detail = input_detail(input);
    const auto& prerequisites = input.prerequisites;

    if (prerequisites.encounter_source == BattleEncounterSourceKind::Unknown) {
        result.status = BattleInitialTurnTypeStatus::MissingInput;
        result.provenance = "missing_encounter_source";
        return result;
    }

    if (prerequisites.encounter_source == BattleEncounterSourceKind::EventDefinition) {
        if (!prerequisites.encounter_id.has_value()
            || *prerequisites.encounter_id < 0) {
            result.status = BattleInitialTurnTypeStatus::MissingInput;
            result.provenance = "missing_nonnegative_event_definition_id";
            return result;
        }
        result.status = BattleInitialTurnTypeStatus::Exact;
        result.turn_type = soa::battle::TurnType::Normal;
        result.provenance =
            "calculateTurnType_80010CF4 event-definition branch forces Normal";
        result.detail += "; selected_turn_type=Normal; draws_consumed=0";
        return result;
    }

    if (prerequisites.encounter_source != BattleEncounterSourceKind::RandomTable) {
        result.status = BattleInitialTurnTypeStatus::Unsupported;
        result.provenance = "unsupported_encounter_source";
        return result;
    }

    if (!valid_signed_byte(prerequisites.advantage_chance_override)
        || !valid_signed_byte(prerequisites.back_attack_chance_override)) {
        result.status = BattleInitialTurnTypeStatus::MissingInput;
        result.provenance = "missing_or_invalid_signed_chance_override";
        return result;
    }
    const bool needs_derived_inputs =
        *prerequisites.advantage_chance_override == -1
        || *prerequisites.back_attack_chance_override == -1;
    if (needs_derived_inputs && !valid_derived_inputs(prerequisites)) {
        result.status = BattleInitialTurnTypeStatus::MissingInput;
        result.provenance = "missing_or_invalid_level_or_initiative";
        return result;
    }

    const int advantage_threshold =
        *prerequisites.advantage_chance_override == -1
        ? derived_advantage_threshold(prerequisites)
        : *prerequisites.advantage_chance_override;
    const int back_attack_threshold =
        *prerequisites.back_attack_chance_override == -1
        ? derived_back_attack_threshold(prerequisites)
        : *prerequisites.back_attack_chance_override;
    result.advantage_threshold = advantage_threshold;
    result.back_attack_threshold = back_attack_threshold;

    result.advantage_draw = make_draw(input.rng_seed_before, advantage_threshold);
    result.draws_consumed = 1;
    result.rng_seed_after = result.advantage_draw->seed_after;
    if (result.advantage_draw->roll_mod_101 < advantage_threshold) {
        result.status = BattleInitialTurnTypeStatus::Exact;
        result.turn_type = soa::battle::TurnType::Advantage;
        result.provenance =
            "calculateTurnType_80010CF4 random Advantage branch";
        result.detail += "; advantage_threshold="
            + std::to_string(advantage_threshold)
            + "; advantage_roll="
            + std::to_string(result.advantage_draw->roll_mod_101)
            + "; selected_turn_type=Advantage; draws_consumed=1";
        return result;
    }

    result.back_attack_draw = make_draw(
        result.advantage_draw->seed_after,
        back_attack_threshold);
    result.draws_consumed = 2;
    result.rng_seed_after = result.back_attack_draw->seed_after;
    result.status = BattleInitialTurnTypeStatus::Exact;
    if (result.back_attack_draw->roll_mod_101 < back_attack_threshold) {
        result.turn_type = soa::battle::TurnType::BackAttack;
        result.provenance =
            "calculateTurnType_80010CF4 random Back Attack branch";
    } else {
        result.turn_type = soa::battle::TurnType::Normal;
        result.provenance =
            "calculateTurnType_80010CF4 random Normal branch";
    }
    result.detail += "; advantage_threshold="
        + std::to_string(advantage_threshold)
        + "; advantage_roll="
        + std::to_string(result.advantage_draw->roll_mod_101)
        + "; back_attack_threshold="
        + std::to_string(back_attack_threshold)
        + "; back_attack_roll="
        + std::to_string(result.back_attack_draw->roll_mod_101)
        + "; selected_turn_type=" + battle_turn_type_name(*result.turn_type)
        + "; draws_consumed=2";
    return result;
}

const char* battle_encounter_source_kind_name(BattleEncounterSourceKind kind) {
    switch (kind) {
    case BattleEncounterSourceKind::Unknown: return "Unknown";
    case BattleEncounterSourceKind::EventDefinition: return "EventDefinition";
    case BattleEncounterSourceKind::RandomTable: return "RandomTable";
    }
    return "Unknown";
}

const char* battle_initial_turn_type_status_name(BattleInitialTurnTypeStatus status) {
    switch (status) {
    case BattleInitialTurnTypeStatus::Exact: return "Exact";
    case BattleInitialTurnTypeStatus::MissingInput: return "MissingInput";
    case BattleInitialTurnTypeStatus::Unsupported: return "Unsupported";
    }
    return "Unsupported";
}

const char* battle_turn_type_name(soa::battle::TurnType turn_type) {
    switch (turn_type) {
    case soa::battle::TurnType::BackAttack: return "BackAttack";
    case soa::battle::TurnType::Normal: return "Normal";
    case soa::battle::TurnType::Advantage: return "Advantage";
    }
    return "Unknown";
}

} // namespace savor::predict
