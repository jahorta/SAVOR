#pragma once

#include "AttackResolutionModel.h"
#include "CounterModel.h"
#include "EnemyEventDataModel.h"
#include "EnemyAttackSetupModel.h"
#include "SoldierAiModel.h"
#include "TurnOrderModel.h"

#include <Core/Input/SoaBattle/ActionTypes.h>
#include <Core/Memory/Soa/Battle/BattleContext.h>

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace savor::predict {

enum class BattlePredictionOutcome {
    ReachedNextTurn,
    Victory,
    Defeat,
    Unsupported,
    Ambiguous,
};

enum class BattlePredictionEventStatus {
    Exact,
    Provisional,
    Skipped,
    Unsupported,
    Ambiguous,
};

enum class BattlePredictionValidationStatus {
    Exact,
    Validated,
    Provisional,
    NotExercised,
    Unsupported,
    Ambiguous,
};

struct BattlePredictionProfile {
    std::string name = "first-battle";
    int supported_turn_index = 1;
    bool supports_status_effects = false;
    bool supports_non_soldier_ai = false;
};

struct BattlePredictionOptions {
    bool include_visual_rng_gap_events = true;
    bool continue_after_visual_rng_gap = true;
};

struct BattlePredictionInput {
    std::uint32_t starting_rng_seed = 0;
    std::optional<int> enemy_event_id;
    soa::battle::ctx::BattleContext context{};
    soa::battle::actions::TurnPlan turn_plan{};
    BattlePredictionProfile profile{};
    BattlePredictionOptions options{};
};

struct BattlePredictionSlotState {
    int slot = -1;
    bool present = false;
    bool is_player = false;
    bool alive = false;
    int id = 0;
    int current_hp = 0;
    int max_hp = 0;
    std::uint32_t status_flags = 0;
    std::uint16_t movement_flags = 0;
    int base_counter_chance = 0;
    int current_counter_chance = 0;
    int counter_chance_increment = 0;
    int quick = 0;
    int agile = 0;
    int attack = 0;
    int defense = 0;
    int hit = 0;
    int dodge = 0;
    int element = -1;
    bool quick_known = false;
    bool attack_inputs_known = false;
    std::optional<BattleStartPosition> start_position;
};

struct BattlePredictionEvent {
    int sequence = 0;
    std::string phase;
    std::string label;
    BattlePredictionEventStatus status = BattlePredictionEventStatus::Exact;
    int actor_slot = -1;
    int target_slot = -1;
    std::optional<std::uint32_t> rng_seed_before;
    std::optional<std::uint32_t> rng_seed_after;
    int draws_consumed = 0;
    std::optional<std::uint16_t> rand_value;
    std::optional<int> attack_result;
    std::optional<int> damage;
    std::optional<int> hp_before;
    std::optional<int> hp_after;
    std::optional<int> effect_source_key;
    std::optional<int> instr_param_0x6;
    std::optional<int> movement_reachability;
    std::optional<int> item_id;
    std::optional<int> amount;
    std::string movement_worker;
    std::string detail;
};

struct BattlePredictionValidationItem {
    std::string scope;
    BattlePredictionValidationStatus status = BattlePredictionValidationStatus::Provisional;
    int draws_exact_through = -1;
    std::string detail;
};

struct BattlePredictionResult {
    BattlePredictionProfile profile{};
    BattlePredictionOutcome outcome = BattlePredictionOutcome::ReachedNextTurn;
    std::uint32_t starting_rng_seed = 0;
    std::uint32_t final_rng_seed = 0;
    std::optional<int> enemy_event_id;
    int total_draws_consumed = 0;
    int exact_draws_through_turn_order = 0;
    bool exact_through_turn_order = false;
    bool has_ambiguous_events = false;
    bool has_unsupported_events = false;
    std::vector<BattlePredictionValidationItem> validation;
    std::vector<BattlePredictionSlotState> final_slots;
    std::vector<BattlePredictionEvent> events;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

BattlePredictionProfile first_battle_prediction_profile();
std::optional<BattlePredictionProfile> battle_prediction_profile_by_name(std::string_view name);

BattlePredictionResult predict_battle(const BattlePredictionInput& input);

const char* battle_prediction_outcome_name(BattlePredictionOutcome outcome);
const char* battle_prediction_event_status_name(BattlePredictionEventStatus status);
const char* battle_prediction_validation_status_name(BattlePredictionValidationStatus status);

void write_battle_prediction_text(const BattlePredictionResult& result, std::ostream& out);
void write_battle_prediction_json(const BattlePredictionResult& result, std::ostream& out);

} // namespace savor::predict
