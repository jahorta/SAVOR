#pragma once

#include "BattleFrameStateModel.h"
#include "MovementModel.h"

#include <array>
#include <cstddef>
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
constexpr std::int16_t ActionMotionAltSpeed = 0x13;
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
    PassiveSpecial,
    CleanupStanding,
    FrameStartPositionSync,
};

enum class BattleFrameEventStatus {
    Matched,
    Provisional,
    MissingInput,
    Unsupported,
    Ambiguous,
};

enum class BattleFrameWorkerStepKind {
    CallbackEntry,
    PathBuild,
    GridRefresh,
    MovementCommit,
    PostCommit,
    ModeHelper,
    PassiveCleanup,
    FrameStartPositionSync,
    ActionMotionPositionSync,
    ActionMotionSetup_8001fabc,
    ActionMotionRotateStep_8001b630_80061114,
    ActionMotionMoveStep_8001e910,
    MoveIncrementApply_80061340,
    Rng,
    Marker,
    NoCommit,
    Unsupported,
};

struct BattleFrameWorkerProgramStep {
    BattleFrameWorkerStepKind kind = BattleFrameWorkerStepKind::CallbackEntry;
    std::uint32_t pc = 0;
    std::uint32_t helper_pc = 0;
    std::optional<std::uint32_t> commit_callsite_pc;
    std::int16_t action_mode = 0;
    int worksheet_state_after = -1;
    std::string label;
    std::string detail;
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
    std::uint32_t callback_pc = 0;
    std::uint32_t current_pc = 0;
    std::optional<std::uint32_t> commit_callsite_pc;
    std::uint8_t worksheet_state_0x19 = 0;
    std::uint8_t path_index_0x15 = 0;
    int queue_sequence = 0;
    int action_motion_position_source_slot = -1;
    std::vector<BattleFrameWorkerProgramStep> program_steps;
    std::size_t program_index = 0;
    bool destination_committed = false;
};

struct BattleFrameStepEvent {
    int frame_index = 0;
    int slot = -1;
    int target_slot = -1;
    std::string callback;
    BattleFrameWorkerKind worker_kind = BattleFrameWorkerKind::None;
    BattleFrameWorkerStepKind step_kind = BattleFrameWorkerStepKind::CallbackEntry;
    BattleFrameEventStatus status = BattleFrameEventStatus::Matched;
    std::uint32_t callback_pc = 0;
    std::uint32_t step_pc = 0;
    std::uint32_t helper_pc = 0;
    std::optional<std::uint32_t> commit_callsite_pc;
    std::int16_t old_action_mode = 0;
    std::int16_t new_action_mode = 0;
    MovementGridPosition old_grid{};
    MovementGridPosition new_grid{};
    BattleFrameVec3 old_pos_holder{};
    BattleFrameVec3 new_pos_holder{};
    BattleFrameVec3 old_combatant_cur_pos_0x1c{};
    BattleFrameVec3 new_combatant_cur_pos_0x1c{};
    BattleFrameVec3 pos_to_move_to_0x110{};
    BattleFrameVec3 move_increment_0x104{};
    BattleFrameVec3 applied_move_increment{};
    float selected_motion_speed = 0.0f;
    std::uint32_t old_combatant_facing_angle_0x2c = 0;
    std::uint32_t new_combatant_facing_angle_0x2c = 0;
    float turn_current_degrees_0x11c = 0.0f;
    float turn_target_degrees_0x120 = 0.0f;
    float turn_step_degrees_0x124 = 0.0f;
    float turn_speed_degrees_0x128 = 0.0f;
    std::uint32_t turn_speed_bits_0x128 = 0;
    bool action_motion_setup_event = false;
    bool rotation_apply_event = false;
    bool rotation_reached_target = false;
    bool combatant_facing_angle_changed = false;
    bool move_increment_apply_event = false;
    bool motion_reached_target = false;
    bool grid_changed = false;
    bool pos_holder_changed = false;
    bool combatant_cur_pos_changed = false;
    bool action_motion_position_synced = false;
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
    std::array<std::vector<int>, 8> slot_worker_queues;
    int next_worker_sequence = 0;
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

void schedule_first_turn_mechanical_attack(
    BattleFrameRuntime& runtime,
    int actor_slot,
    int target_slot);

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
const char* battle_frame_worker_step_kind_name(BattleFrameWorkerStepKind kind);

} // namespace savor::predict
