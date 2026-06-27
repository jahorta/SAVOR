#pragma once

#include "BattleFrameStateModel.h"
#include "MovementModel.h"

#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

enum class BattleFrameWorkerKind {
    None,
    ActiveDirectAttack,
    ActiveFallbackAttack,
    EnemyDirectAttack,
    EnemyFallbackAttack,
    PassiveTarget,
    PassiveSameSide,
    CleanupStanding,
};

struct BattleFrameWorker {
    int slot = -1;
    int target_slot = -1;
    BattleFrameWorkerKind kind = BattleFrameWorkerKind::None;
    MovementSelectedWorker selected_worker = MovementSelectedWorker::None;
    int total_frames = 0;
    int frame = 0;
    bool complete = false;
    MovementGridPosition start_grid{};
    MovementGridPosition destination_grid{};
    BattleFrameVec3 start_position{};
    BattleFrameVec3 destination_position{};
};

struct BattleFrameStepEvent {
    int frame_index = 0;
    int slot = -1;
    int target_slot = -1;
    std::string callback;
    BattleFrameWorkerKind worker_kind = BattleFrameWorkerKind::None;
    std::int16_t old_action_mode = 0;
    std::int16_t new_action_mode = 0;
    MovementGridPosition old_grid{};
    MovementGridPosition new_grid{};
    BattleFrameVec3 old_pos_holder{};
    BattleFrameVec3 new_pos_holder{};
    BattleFrameVec3 old_combatant_position{};
    BattleFrameVec3 new_combatant_position{};
    bool grid_changed = false;
    bool pos_holder_changed = false;
    bool combatant_position_changed = false;
    bool bridged_position = false;
};

struct BattleFrameRuntime {
    BattleFrameState state{};
    bool initialized = false;
    std::vector<BattleFrameWorker> workers;
    std::vector<BattleFrameStepEvent> last_step_events;
    std::vector<BattleFrameStepEvent> history;
    std::vector<std::string> warnings;
};

struct BattleFrameScheduleActionInput {
    int actor_slot = -1;
    int target_slot = -1;
    bool enemy_owned = false;
    int combatant_command_parameter = 0;
    MovementSelectedWorker selected_worker = MovementSelectedWorker::None;
    std::vector<PassiveMovementRoute> passive_routes;
};

struct BattleFrameRunResult {
    bool ok = true;
    bool ambiguous = false;
    int frames_executed = 0;
    std::vector<BattleFrameStepEvent> events;
    std::vector<std::string> warnings;
};

std::optional<BattleFrameRuntime> initialize_first_battle_frame_runtime(
    int enemy_event_id,
    const std::vector<MovementSlotState>& slots);

void schedule_first_battle_action_workers(
    BattleFrameRuntime& runtime,
    const BattleFrameScheduleActionInput& input);

BattleFrameRunResult run_scheduled_frame_workers(
    BattleFrameRuntime& runtime,
    int max_frames);

const char* battle_frame_worker_kind_name(BattleFrameWorkerKind kind);

} // namespace savor::predict
