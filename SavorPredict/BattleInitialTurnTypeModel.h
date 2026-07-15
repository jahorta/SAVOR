#pragma once

#include <Core/Memory/Soa/SoaConstants.h>

#include <cstdint>
#include <optional>
#include <string>

namespace savor::predict {

enum class BattleEncounterSourceKind {
    Unknown,
    EventDefinition,
    RandomTable,
};

enum class BattleInitialTurnTypeStatus {
    Exact,
    MissingInput,
    Unsupported,
};

struct BattleInitialTurnTypePrerequisites {
    BattleEncounterSourceKind encounter_source = BattleEncounterSourceKind::Unknown;
    std::optional<int> encounter_id;
    std::optional<int> vyse_level;
    std::optional<int> initiative;
    std::optional<int> advantage_chance_override;
    std::optional<int> back_attack_chance_override;
};

struct BattleInitialTurnTypeInput {
    BattleInitialTurnTypePrerequisites prerequisites;
    std::uint32_t rng_seed_before = 0;
};

struct BattleInitialTurnTypeDraw {
    std::uint32_t seed_before = 0;
    std::uint32_t seed_after = 0;
    std::uint16_t rand_value = 0;
    int roll_mod_101 = 0;
    int threshold = 0;
};

struct BattleInitialTurnTypeResult {
    BattleInitialTurnTypeStatus status = BattleInitialTurnTypeStatus::MissingInput;
    std::optional<soa::battle::TurnType> turn_type;
    std::uint32_t rng_seed_before = 0;
    std::uint32_t rng_seed_after = 0;
    int draws_consumed = 0;
    std::optional<int> advantage_threshold;
    std::optional<int> back_attack_threshold;
    std::optional<BattleInitialTurnTypeDraw> advantage_draw;
    std::optional<BattleInitialTurnTypeDraw> back_attack_draw;
    std::string provenance;
    std::string detail;
};

BattleInitialTurnTypeResult model_initial_battle_turn_type(
    const BattleInitialTurnTypeInput& input);

const char* battle_encounter_source_kind_name(BattleEncounterSourceKind kind);
const char* battle_initial_turn_type_status_name(BattleInitialTurnTypeStatus status);
const char* battle_turn_type_name(soa::battle::TurnType turn_type);

} // namespace savor::predict
