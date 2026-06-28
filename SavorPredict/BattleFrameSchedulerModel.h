#pragma once

#include "BattleFrameStateModel.h"
#include "MovementModel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

namespace BattleFrameActionMode {
constexpr std::int16_t Standing = 0x02;
constexpr std::int16_t ActiveApproach = 0x06;
constexpr std::int16_t ActiveFallback = 0x05;
constexpr std::int16_t PassiveTarget = 0x0b;
constexpr std::int16_t PassiveSameSide = 0x06;
constexpr std::int16_t PassiveBlock = 0x0c;
constexpr std::int16_t PassiveDodge = 0x0d;
} // namespace BattleFrameActionMode

enum class BattleFrameWorkerKind {
    None,
    ActiveMovement,
    PassiveMovement,
    ActionView,
    MechanicalAttack,
    EffectChunk,
    PassiveClashReaction,
    Cleanup,
    ActiveDirectAttack,
    ActiveFallbackAttack,
    EnemyDirectAttack,
    EnemyFallbackAttack,
    PassiveTarget,
    PassiveSameSide,
    CleanupStanding,
};

enum class BattleFrameEventStatus {
    Matched,
    Provisional,
    MissingInput,
    Unsupported,
    Ambiguous,
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
    int draws_consumed = 0;
    std::optional<int> effect_source_key;
    int effect_chunk_index = -1;
    std::string rng_label;
    std::string detail;
    BattleFrameEventStatus event_status = BattleFrameEventStatus::Matched;
    bool clash_emitted = false;
};

struct BattleFrameStepEvent {
    int frame_index = 0;
    int slot = -1;
    int target_slot = -1;
    std::string callback;
    BattleFrameWorkerKind worker_kind = BattleFrameWorkerKind::None;
    BattleFrameEventStatus status = BattleFrameEventStatus::Matched;
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
    bool rng_event = false;
    std::string rng_label;
    int draws_consumed = 0;
    std::optional<std::uint32_t> rng_seed_before;
    std::optional<std::uint32_t> rng_seed_after;
    std::optional<std::uint16_t> rand_value;
    std::optional<int> effect_source_key;
    std::optional<int> passive_clash_selected_index;
    std::string detail;
};

struct BattleFrameRuntime {
    BattleFrameState state{};
    bool initialized = false;
    bool enable_passive_clash_reaction = false;
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

void schedule_first_turn_actor_action(
    BattleFrameRuntime& runtime,
    const BattleFrameScheduleActionInput& input);

void schedule_first_turn_action_view_rng(
    BattleFrameRuntime& runtime,
    int actor_slot,
    int target_slot,
    std::string label,
    int draws_consumed,
    std::string detail,
    BattleFrameEventStatus status = BattleFrameEventStatus::Provisional);

bool schedule_effect_chunks_for_source_key(
    BattleFrameRuntime& runtime,
    int actor_slot,
    int target_slot,
    int source_key);

std::optional<BattleFrameStepEvent> maybe_emit_passive_clash_reaction(
    BattleFrameRuntime& runtime,
    BattleFrameWorker& completed_worker,
    std::uint32_t& rng_state);

BattleFrameRunResult run_first_turn_frame(
    BattleFrameRuntime& runtime,
    std::uint32_t& rng_state);

BattleFrameRunResult run_first_turn_until_idle(
    BattleFrameRuntime& runtime,
    std::uint32_t& rng_state,
    int max_frames);

BattleFrameRunResult run_scheduled_frame_workers(
    BattleFrameRuntime& runtime,
    int max_frames);

const char* battle_frame_worker_kind_name(BattleFrameWorkerKind kind);
const char* battle_frame_event_status_name(BattleFrameEventStatus status);
const char* battle_frame_action_mode_name(std::int16_t action_mode);

} // namespace savor::predict
