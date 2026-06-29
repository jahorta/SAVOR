#pragma once

#include "EnemyEventDataModel.h"
#include "EnemyAttackSetupModel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

enum class MovementBackend {
    HandlerLevelFirstBattle,
    FrameStateMachine,
};

enum class MovementSimulationStatus {
    Exact,
    Provisional,
    Skipped,
    MissingInput,
    Unsupported,
    Ambiguous,
};

enum class MovementSelectedWorker {
    None,
    PcDirectAttack_80086308,
    PcFallbackAttack_80085ce0,
    EnemyDirectAttack_80087f6c,
    EnemyFallbackAttack_80087844,
};

enum class MovementReachabilityStatus {
    Unknown,
    Failed0,
    Adjacent1,
    AdjustedAdjacent2,
    Path4,
    Ambiguous,
};

enum class PassiveMovementRouteKind {
    Unaffected,
    TargetParticipant,
    SameSideParticipant,
    SpecialParticipant,
};

struct MovementGridPosition {
    int grid_x = -1;
    int grid_z = -1;
};

struct MovementRawStagePosition {
    int raw_x = 0;
    int raw_y = 0;
    int raw_z = 0;
};

struct MovementSlotState {
    int slot = -1;
    bool present = false;
    bool is_player = false;
    bool alive = false;
    std::uint32_t status_flags = 0;
    std::uint16_t movement_flags = 0;
    float motion_base_speed = 0.0f;
    float motion_alt_speed = 0.0f;
    bool motion_speeds_known = false;
    int width = 1;
    int depth = 1;
    std::optional<BattleStartPosition> start_position;
};

struct MovementWorksheetSnapshot {
    bool available = false;
    std::optional<MovementGridPosition> actor_grid_position;
    std::optional<MovementGridPosition> target_grid_position;
    std::optional<MovementRawStagePosition> actor_raw_stage_position;
    std::optional<MovementRawStagePosition> target_raw_stage_position;
    std::optional<bool> target_adjacent;
    std::optional<int> reachability_result;
    std::optional<bool> path_shape_forces_fallback;
    std::optional<int> dist_to_target;
    std::optional<int> helper_8008a174_result;
    std::optional<int> helper_80082340_result;
    std::string source;
};

struct MovementModelInputs {
    MovementBackend backend = MovementBackend::HandlerLevelFirstBattle;
    std::uint32_t rng_state = 0;
    int actor_slot = -1;
    int target_slot = -1;
    int queued_instruction = 3;
    int instr_param_0x6 = 0;
    bool enemy_owned = false;
    std::vector<MovementSlotState> slots;
    MovementWorksheetSnapshot actor_worksheet{};
};

struct PassiveMovementRoute {
    int slot = -1;
    PassiveMovementRouteKind route = PassiveMovementRouteKind::Unaffected;
    MovementSelectedWorker selected_worker = MovementSelectedWorker::None;
    MovementSimulationStatus status = MovementSimulationStatus::Provisional;
};

struct MovementSimulation {
    std::uint32_t end_state = 0;
    int draws_consumed = 0;
    std::optional<std::uint16_t> setup_rand;
    std::optional<int> setup_rand_mod10;
    int original_target_slot = -1;
    int final_target_slot = -1;
    int initial_instr_param_0x6 = 0;
    int final_instr_param_0x6 = 0;
    bool target_repaired = false;
    bool can_execute = true;
    MovementSimulationStatus status = MovementSimulationStatus::Exact;
    MovementSimulationStatus target_repair_status = MovementSimulationStatus::Exact;
    MovementReachabilityStatus reachability = MovementReachabilityStatus::Unknown;
    MovementSelectedWorker selected_worker = MovementSelectedWorker::None;
    EnemyAttackSetupPath enemy_setup_path = EnemyAttackSetupPath::NotAttack;
    bool enemy_direct_close_candidate = false;
    std::vector<PassiveMovementRoute> passive_routes;
    std::string detail;
};

MovementSimulation simulate_first_battle_movement_setup(const MovementModelInputs& inputs);
MovementWorksheetSnapshot project_enemy_event0_movement_worksheet_snapshot(const MovementModelInputs& inputs);

const char* movement_simulation_status_name(MovementSimulationStatus status);
const char* movement_selected_worker_name(MovementSelectedWorker worker);
const char* movement_reachability_status_name(MovementReachabilityStatus status);
const char* passive_movement_route_kind_name(PassiveMovementRouteKind route);

} // namespace savor::predict
