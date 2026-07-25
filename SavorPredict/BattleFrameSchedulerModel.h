#pragma once

#include "ActionMotionInvocationModel.h"
#include "ActionMotionPlaybackModel.h"
#include "ActionViewMode11Model.h"
#include "ActionViewRoleModel.h"
#include "BattleCollisionBoxModel.h"
#include "BattleFrameStateModel.h"
#include "BattleFrameThreadListModel.h"
#include "BattleMovementInvocationModel.h"
#include "BattleMovementPathModel.h"
#include "CombatantAuxiliaryPublicationModel.h"
#include "CombatantVisualDispatcherModel.h"
#include "DirectInstructionTransitionSelectorModel.h"
#include "MovementModel.h"
#include "BattleTargetReactionStateModel.h"
#include "ViewPlacementCacheModel.h"

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
constexpr std::int16_t AmbientFormation = 0x13;
constexpr std::int16_t PassiveBlock = 0x0c;
constexpr std::int16_t PassiveDodge = 0x0d;
constexpr std::int16_t ActionMotionAltSpeed = 0x13;
} // namespace BattleFrameActionMode

enum class BattleFrameWorkerKind {
    None,
    ActiveMovement,
    PassiveMovement,
    ActionView,
    ViewPlacement,
    MechanicalAttack,
    EffectChunk,
    Cleanup,
    ActiveDirectAttack,
    ActiveFallbackAttack,
    EnemyDirectAttack,
    EnemyFallbackAttack,
    PassiveController,
    VisualController,
    VisualActionService,
    VisualCollisionBox,
    VisualActionViewRecord,
    VisualUnsupportedCommand,
    CombatantInstruction,
    CleanupStanding,
    FrameStartPositionSync,
};

enum class BattleFrameEventStatus {
    Matched,
    Provisional,
    Skipped,
    MissingInput,
    Unsupported,
    Ambiguous,
};

enum class BattleFrameInstructionControlResetSource {
    QueuedTransition,
    DirectTransition,
};

enum class BattleFrameInstructionControlResetLifecycle {
    Applied,
    Consumed,
    TargetRemoved,
    TargetReplaced,
    Superseded,
    MissingInput,
};

enum class BattleFrameInstructionControlResetTiming {
    SameInstructionVisit,
    SameFrameLaterVisit,
    NextFrameVisit,
    Unknown,
};

enum class BattleFrameMovementLegPolicy {
    RebuildPath,
    AdvanceExistingPath,
    CompleteAfterLeg,
};

enum class BattleFrameWorkerStepKind {
    MovementInvocationActivate,
    MovementControllerHandoff,
    MovementInvocationSkipped,
    ActionPhaseTransition,
    PassiveRelayPublish,
    PassiveRelayAdvance,
    PassiveDispatchPublish,
    PassiveFamilySelect,
    PassiveCompletionDeferred,
    PassiveCompletionClear,
    PassiveDeathClear,
    ActionComplete,
    VisualControllerVisit,
    ActionViewRoleResolve,
    ActionViewRoleFlagSpawn,
    ActionViewRoleFlagVisit,
    VisualInstructionDecision,
    VisualInstructionStatePublish,
    TargetReactionPublish,
    DirectTransitionSelect,
    CollisionOccupancyRefresh,
    CollisionProbe,
    InstructionCallbackControlReset,
    InstructionCallbackControlResetConsume,
    ActionMotionInvocationDecision,
    ActionMotionPlaybackInstall,
    ActionMotionRendererAdvance,
    ActionMotionState6Poll,
    ActionMotionPostState6Delay,
    ActionMotionPublicationRelease,
    VisualStdRowProducerVisit,
    VisualInstructionInstall,
    VisualAuxiliaryPublication,
    VisualCommandPublish,
    VisualChildState0,
    VisualChildDelay,
    VisualChildNested,
    VisualChildCleanup,
    VisualMode0Rewrite,
    VisualMode0eCamera,
    VisualMode1Pathing,
    VisualMode11Setup,
    VisualMode11Advance,
    VisualInstructionGate,
    VisualActiveRecordReplace,
    VisualReplacementState,
    VisualUnsupportedWait,
    CallbackEntry,
    PathBuild,
    PathNodeSelection,
    GridRefresh,
    MovementCommit,
    PostCommit,
    ModeHelper,
    PassiveCleanup,
    FrameStartPositionSync,
    ActionMotionPositionSync,
    CombatantInstructionPublish,
    CombatantInstructionWait,
    ActionMotionSetup_8001fabc,
    ActionMotionRotateStep_8001b630_80061114,
    ActionMotionMoveStep_8001e910,
    MoveIncrementApply_80061340,
    MotionStopResult_8001eb54,
    NextLegOrRebuildDecision,
    ViewPlacementResolve,
    Rng,
    Marker,
    NoCommit,
    FallbackSetupWait,
    FallbackMode7Publish,
    FallbackAttackResolutionWait,
    FallbackVisualCompletionWait,
    FallbackTerminalHandoff,
    Unsupported,
};

constexpr std::size_t kBattleFrameMovementPathEntryCapacity = kBattleMovementPathCapacity;
constexpr std::size_t kBattleFrameCombatantSlotCapacity = 12;

enum class BattleFrameActionPhase {
    None,
    Scheduled,
    Active,
    HandoffPending,
    PassiveDispatched,
    Resolving,
    Draining,
    Complete,
};

enum class BattleFramePassiveParticipantPhase {
    Inactive,
    InitialRelayPending,
    WaitingForDispatch,
    DispatchRelayPending,
    Dispatching,
    FamilyPending,
    FamilyActive,
    CompletionDeferred,
    Cleared,
    Removed,
};

enum class BattleFrameCombatantInstructionPhase {
    Idle,
    SetupPending,
    Rotating,
    Moving,
    StopPending,
    Complete,
    Removed,
    Unsupported,
};

struct BattleFrameMovementPathState {
    bool available = false;
    std::uint8_t dist_to_target_0x14 = 0;
    std::uint8_t path_index_0x15 = 0;
    std::uint8_t status_0x16 = 0;
    std::array<MovementGridPosition, kBattleFrameMovementPathEntryCapacity> entries{};
    std::size_t entry_count = 0;
    bool terminator_seen = false;
    bool zero_distance_target = false;
};

struct BattleFrameMovementWorksheetRuntime {
    bool initialized = false;
    std::uint8_t dist_to_target_0x14 = 0;
    std::uint8_t path_index_0x15 = 0;
    std::uint8_t status_0x16 = 0;
    std::array<MovementGridPosition, kBattleFrameMovementPathEntryCapacity>
        raw_path_entries{};
    std::string provenance;
};

struct BattleFrameCombatantInstructionRuntime {
    bool active = false;
    int revision = 0;
    int action_ordinal = -1;
    int slot = -1;
    int target_slot = -1;
    int owner_worker_queue_sequence = -1;
    BattleFrameWorkerKind controller_worker_kind = BattleFrameWorkerKind::None;
    BattleMovementControllerFamily controller_family =
        BattleMovementControllerFamily::Unknown;
    BattleMovementRelationRoute relation_route =
        BattleMovementRelationRoute::Unknown;
    int thread_order_index = -1;
    std::int16_t action_mode = BattleFrameActionMode::Standing;
    BattleFrameVec3 provisional_fallback_target{};
    MovementGridPosition destination_grid{};
    MovementCommitDestinationSource destination_source =
        MovementCommitDestinationSource::Unknown;
    BattleFrameMovementPathState movement_path{};
    BattleFrameEventStatus status = BattleFrameEventStatus::Provisional;
    BattleFrameCombatantInstructionPhase phase =
        BattleFrameCombatantInstructionPhase::Idle;
    std::string provenance;
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
    int action_ordinal = -1;
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
    std::uint32_t callback_pc = 0;
    std::uint32_t current_pc = 0;
    std::optional<std::uint32_t> commit_callsite_pc;
    std::uint8_t worksheet_state_0x19 = 0;
    std::uint8_t path_index_0x15 = 0;
    BattleFrameMovementPathState movement_path{};
    int queue_sequence = 0;
    int action_motion_position_source_slot = -1;
    std::string view_placement_publisher_source_id;
    std::string view_placement_provenance;
    std::vector<BattleFrameWorkerProgramStep> program_steps;
    std::size_t program_index = 0;
    bool destination_committed = false;
    MovementCommitDestinationSource destination_source =
        MovementCommitDestinationSource::Unknown;
    BattleMovementPathSelectionPolicy path_selection_policy =
        BattleMovementPathSelectionPolicy::StraightRunPathIndex;
    BattleFrameMovementLegPolicy movement_leg_policy =
        BattleFrameMovementLegPolicy::CompleteAfterLeg;
    std::size_t movement_loop_start_index = 0;
    int completed_movement_legs = 0;
    int completed_motion_legs = 0;
    int maximum_movement_legs = 11;
    bool use_supplied_movement_path_once = false;
    bool activation_pending = false;
    BattleMovementControllerFamily controller_family =
        BattleMovementControllerFamily::Unknown;
    BattleMovementRelationRoute relation_route =
        BattleMovementRelationRoute::Unknown;
    BattleMovementActivationTiming activation_timing =
        BattleMovementActivationTiming::NextThreadVisit;
    int thread_order_index = -1;
    BattleMovementControllerState activation_controller_state =
        BattleMovementControllerState::Unknown;
    BattleMovementControllerState worker_controller_state =
        BattleMovementControllerState::Unknown;
    std::string invocation_confidence;
    std::string invocation_provenance;
    bool passive_completion_requested = false;
    bool passive_waits_for_action_resolution = false;
    bool semantic_target_locked = false;
    bool waiting_for_action_resolution = false;
    bool waiting_for_action_completion = false;
    bool waiting_for_combatant_instruction = false;
    int combatant_instruction_revision = 0;
};

struct BattleFrameStepEvent {
    int action_ordinal = -1;
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
    BattleFrameMovementPathState movement_path{};
    std::uint8_t old_path_index_0x15 = 0;
    std::uint8_t new_path_index_0x15 = 0;
    std::optional<MovementGridPosition> selected_path_node;
    MovementCommitDestinationSource destination_source =
        MovementCommitDestinationSource::Unknown;
    int combatant_instruction_revision = 0;
    BattleFrameCombatantInstructionPhase instruction_phase_before =
        BattleFrameCombatantInstructionPhase::Idle;
    BattleFrameCombatantInstructionPhase instruction_phase_after =
        BattleFrameCombatantInstructionPhase::Idle;
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
    bool combatant_state_available = false;
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
    ActionMotionPlaybackPhase action_motion_playback_phase_before =
        ActionMotionPlaybackPhase::Inactive;
    ActionMotionPlaybackPhase action_motion_playback_phase_after =
        ActionMotionPlaybackPhase::Inactive;
    std::uint32_t action_motion_duration_bits = 0;
    std::uint32_t action_motion_effective_duration_bits = 0;
    std::uint32_t action_motion_progress_before_bits = 0;
    std::uint32_t action_motion_progress_after_bits = 0;
    std::uint32_t action_motion_increment_bits = 0;
    std::uint32_t action_motion_flags_before = 0;
    std::uint32_t action_motion_flags_after = 0;
    int action_motion_control_before = 0;
    int action_motion_control_after = 0;
    bool action_motion_renderer_advanced = false;
    bool action_motion_gate_polled = false;
    std::optional<bool> action_motion_gate_result;
    bool action_motion_delay_lookup_performed = false;
    ActionMotionDelayStatus action_motion_delay_status =
        ActionMotionDelayStatus::MissingInput;
    int action_motion_delay_descriptor_record_index = -1;
    int action_motion_delay_before = 0;
    int action_motion_delay_after = 0;
    ActionMotionPersistentCallbackFamily action_motion_callback_family =
        ActionMotionPersistentCallbackFamily::Unknown;
    std::uint64_t persistent_callback_publication_revision = 0;
    std::int16_t persistent_callback_index = -1;
    bool persistent_callback_changed = false;
    bool persistent_callback_same_value = false;
    int instruction_control_reset_sequence = -1;
    BattleFrameInstructionControlResetSource instruction_control_reset_source =
        BattleFrameInstructionControlResetSource::QueuedTransition;
    BattleFrameInstructionControlResetLifecycle instruction_control_reset_lifecycle =
        BattleFrameInstructionControlResetLifecycle::MissingInput;
    BattleFrameInstructionControlResetTiming instruction_control_reset_timing =
        BattleFrameInstructionControlResetTiming::Unknown;
    int instruction_control_reset_producer_node_id = -1;
    int instruction_control_reset_target_node_id = -1;
    std::uint64_t instruction_control_reset_traversal_generation = 0;
    ActionMotionInvocationDecisionKind action_motion_invocation_decision =
        ActionMotionInvocationDecisionKind::Unsupported;
    ActionMotionInvocationStatus action_motion_invocation_status =
        ActionMotionInvocationStatus::MissingInput;
    ActionMotionResolverCode action_motion_resolver_code =
        ActionMotionResolverCode::NoChange;
    int action_motion_callback_state_before = 0;
    int action_motion_callback_state_after = 0;
    int action_motion_resolver_row = -1;
    int action_motion_resolved_motion_id = -1;
    bool rng_event = false;
    std::string rng_label;
    int draws_consumed = 0;
    std::optional<std::uint32_t> rng_seed_before;
    std::optional<std::uint32_t> rng_seed_after;
    std::optional<std::uint16_t> rand_value;
    std::optional<int> effect_source_key;
    std::optional<int> visual_candidate_selected_index;
    CombatantVisualCommandKind visual_command_kind =
        CombatantVisualCommandKind::Unknown;
    std::string visual_resource;
    int visual_record_index = -1;
    std::uint64_t visual_epoch = 0;
    int visual_task_sequence = -1;
    int visual_payload_mode = -1;
    int visual_effective_mode = -1;
    std::string visual_child_kind;
    BattleMovementControllerFamily controller_family =
        BattleMovementControllerFamily::Unknown;
    BattleMovementRelationRoute relation_route =
        BattleMovementRelationRoute::Unknown;
    BattleFrameActionPhase action_phase = BattleFrameActionPhase::None;
    BattleMovementActivationTiming activation_timing =
        BattleMovementActivationTiming::NextThreadVisit;
    int thread_order_index = -1;
    BattleMovementControllerState old_controller_state =
        BattleMovementControllerState::Unknown;
    BattleMovementControllerState new_controller_state =
        BattleMovementControllerState::Unknown;
    std::string invocation_confidence;
    std::string invocation_provenance;
    std::uint16_t passive_completion_mask_before = 0;
    std::uint16_t passive_completion_mask_after = 0;
    std::uint8_t completion_turn_phase = 0;
    bool completion_override = false;
    std::uint32_t deferred_callback_pc = 0;
    std::string completion_reason;
    std::string detail;
};

enum class BattleFrameMovementInvocationLifecycle {
    Pending,
    Activated,
    Handoff,
    Skipped,
};

struct BattleFramePendingMovementInvocation {
    BattleMovementInvocationDecision decision{};
    int worker_index = -1;
    int queue_sequence = -1;
    BattleFrameMovementInvocationLifecycle lifecycle =
        BattleFrameMovementInvocationLifecycle::Pending;
};

struct BattleFrameMovementInvocationHistoryEvent {
    int frame_index = 0;
    int action_ordinal = -1;
    int slot = -1;
    int semantic_target_slot = -1;
    int thread_order_index = -1;
    BattleMovementControllerFamily controller_family =
        BattleMovementControllerFamily::Unknown;
    BattleMovementRelationRoute relation_route =
        BattleMovementRelationRoute::Unknown;
    BattleMovementActivationTiming activation_timing =
        BattleMovementActivationTiming::NextThreadVisit;
    BattleFrameMovementInvocationLifecycle lifecycle =
        BattleFrameMovementInvocationLifecycle::Pending;
    BattleMovementControllerState old_controller_state =
        BattleMovementControllerState::Unknown;
    BattleMovementControllerState new_controller_state =
        BattleMovementControllerState::Unknown;
    int draws_consumed = 0;
    std::string provenance;
};

struct BattleFramePassiveParticipantRuntime {
    bool active = false;
    int action_ordinal = -1;
    int slot = -1;
    BattleFramePassiveParticipantPhase phase =
        BattleFramePassiveParticipantPhase::Inactive;
    std::uint32_t actual_callback_pc = 0x800804B8u;
    std::uint32_t deferred_callback_pc = 0;
    std::uint8_t thread_state_0x19 = 0;
    BattleMovementRelationRoute relation_route =
        BattleMovementRelationRoute::Unknown;
    BattleMovementControllerFamily controller_family =
        BattleMovementControllerFamily::PassiveRelay;
    std::optional<int> semantic_target_slot;
    bool completion_bit_set = false;
    int worker_index = -1;
    BattleMovementInvocationStatus status =
        BattleMovementInvocationStatus::Provisional;
    std::string confidence;
    std::string provenance;
};

struct BattleFrameActionRuntime {
    bool active = false;
    int action_ordinal = -1;
    int actor_slot = -1;
    int target_slot = -1;
    BattleMovementActionKind action_kind = BattleMovementActionKind::Unknown;
    BattleMovementRelationScope relation_scope = BattleMovementRelationScope::Unknown;
    BattleMovementTurnType turn_type = BattleMovementTurnType::Unknown;
    std::optional<std::int16_t> initial_instruction_parameter;
    std::optional<std::int16_t> final_instruction_parameter;
    BasicAttackExecutionRoute execution_route = BasicAttackExecutionRoute::Unknown;
    BattleFrameActionPhase phase = BattleFrameActionPhase::None;
    int active_worker_index = -1;
    std::uint16_t passive_completion_mask = 0;
    bool action_resolution_available = false;
    int attack_result = -1;
    bool attack_landed = false;
    bool target_dead = false;
    std::optional<BasicAttackQueuedStateResult> queued_state_transition;
    bool queued_state_transition_pending = false;
    bool queued_state_transition_published = false;
    std::uint8_t completion_turn_phase = 3;
    bool completion_override = false;
    bool completion_gate_open = false;
    bool setup_publication_events_pending = false;
    BattleMovementInvocationStatus status =
        BattleMovementInvocationStatus::Provisional;
    std::string provenance;
};

struct BattleFrameActionScheduleResult {
    bool scheduled = false;
    int action_ordinal = -1;
    BattleMovementInvocationStatus status =
        BattleMovementInvocationStatus::MissingInput;
    std::string detail;
};

struct BattleFrameActionResolution {
    int action_ordinal = -1;
    int attack_result = -1;
    bool attack_landed = false;
    bool target_dead = false;
};

struct BattleFrameTargetReactionPublication {
    int action_ordinal = -1;
    BattleTargetReactionResult reaction{};
};

struct BattleFrameTargetReactionRuntime {
    bool available = false;
    int action_ordinal = -1;
    std::uint64_t revision = 0;
    BattleTargetReactionResult reaction{};
};

enum class BattleFrameVisualChildKind {
    ActionService,
    CollisionBox,
    ActionViewRecord,
    UnsupportedCommand,
};

enum class BattleFrameVisualChildPhase {
    Published,
    State0,
    Delay,
    Active,
    CompletionWait,
    Complete,
};

struct BattleFrameActionViewControllerRuntime {
    bool initialized = false;
    std::int8_t effective_mode_0x2f = 0;
    std::int16_t selector_state_0x30 = 0;
    std::int16_t actor_slot_0x2 = -1;
    std::int16_t target_slot_0x4 = -1;
    std::uint64_t visit_revision = 0;
    std::uint64_t publication_revision = 0;
    std::vector<int> direct_view_action_ordinals;
};

struct BattleFrameActionViewRoleRuntime {
    bool valid = false;
    int action_ordinal = -1;
    int acting_actor_slot = -1;
    int queued_target_slot = -1;
    std::uint64_t revision = 0;
    ActionViewRoleStatus status = ActionViewRoleStatus::MissingInput;
    std::string provenance;
};

struct BattleFrameActionViewRoleFlagChildRuntime {
    int sequence = -1;
    int thread_node_id = -1;
    int parent_thread_node_id = -1;
    int action_ordinal = -1;
    int slot = -1;
    int visits = 0;
    ActionViewRoleFlagProducerState state{};
    ActionViewRoleStatus status = ActionViewRoleStatus::MissingInput;
    bool complete = false;
    std::string provenance;
};

struct BattleFrameActionViewActiveRecordRuntime {
    std::optional<int> task_sequence;
    std::uint64_t revision = 0;
    int publication_frame = -1;
    int publication_visit_cursor = -1;
    std::string provenance;
};

struct BattleFrameVisualChildTask {
    int sequence = -1;
    int thread_node_id = -1;
    int action_ordinal = -1;
    int origin_slot = -1;
    int target_slot = -1;
    std::string resource_stem;
    int record_index = -1;
    std::uint64_t publication_epoch = 0;
    CombatantVisualCommandKind command_kind =
        CombatantVisualCommandKind::Unknown;
    BattleFrameVisualChildKind kind = BattleFrameVisualChildKind::ActionViewRecord;
    BattleFrameVisualChildPhase phase = BattleFrameVisualChildPhase::Published;
    int publication_frame = 0;
    int first_eligible_frame = 0;
    int publication_visit_cursor = -1;
    bool participates_in_action_barrier = false;
    bool synthetic = false;
    bool complete = false;
    int thread_state_0x19 = 0;
    int visits = 0;
    int maximum_visits = 256;
    int initial_delay = 0;
    int delay_remaining = 0;
    int derived_mode = -1;
    int derived_subtype = -1;
    int payload_mode = -1;
    int effective_mode = -1;
    bool mode0_draw_consumed = false;
    bool mode0e_draw_consumed = false;
    bool mode1_pathing_consumed = false;
    bool mode11_initialized = false;
    ActionViewMode11Status mode11_status =
        ActionViewMode11Status::Provisional;
    ActionViewMode11Branch mode11_branch =
        ActionViewMode11Branch::ProvisionalInterpolation;
    int mode11_substate = 0;
    int mode11_counter = 0;
    int mode11_setup_frame = -1;
    bool mode11_gate_owned = false;
    bool mode11_gate_cleared = false;
    bool active_record_installed = false;
    bool active_record_replacement_pending = false;
    bool active_record_state_fa = false;
    int active_record_replacement_frame = -1;
    bool nested_call_complete = false;
    CombatantVisualModelStatus status = CombatantVisualModelStatus::Provisional;
    std::optional<CombatantVisualSetCommandPayload> set_command;
    std::optional<CombatantVisualCollisionBoxPayload> collision_box;
    std::optional<CombatantVisualSystemCameraPayload> system_camera;
    int collision_state = 0;
    int collision_counter = 0;
    BattleCollisionVec3 collision_current{};
    BattleCollisionVec3 collision_velocity{};
    std::array<bool, kBattleFrameCombatantSlotCapacity> collision_visited{};
    bool collision_selector_invoked = false;
    std::string provenance;
};

enum class BattleFrameInstructionCallbackPublicationSource {
    State0Initialization,
    State1CurrentInstruction,
    State1QueuedTransition,
    ExplicitModeledTransition,
};

struct BattleFramePersistentInstructionCallbackRuntime {
    bool installed = false;
    std::uint8_t thread_state_0x19 = 0;
    int action_ordinal = -1;
    int slot = -1;
    std::uint64_t publication_revision = 0;
    std::uint64_t instruction_state_revision = 0;
    std::int16_t callback_index = -1;
    ActionMotionPersistentCallbackFamily callback_family =
        ActionMotionPersistentCallbackFamily::Unknown;
    int callback_state = 0;
    std::optional<CombatantStdActionRow> current_instruction_row;
    std::optional<CombatantStdActionRow> previous_instruction_row;
    std::optional<bool> current_motion_resource_present;
    std::optional<std::int16_t> current_motion_id;
    int visits = 0;
    int publications = 0;
    int same_value_publications = 0;
    int callback_changes = 0;
    int installs = 0;
    int loads = 0;
    int state8_delay_remaining = -1;
    bool auxiliary_publication_pending = false;
    std::uint64_t auxiliary_publication_revision = 0;
    std::uint64_t last_auxiliary_instruction_revision = 0;
    ActionMotionInvocationStatus status =
        ActionMotionInvocationStatus::MissingInput;
    std::string provenance;
};

struct BattleFramePendingInstructionControlReset {
    int sequence = -1;
    int action_ordinal = -1;
    int target_slot = -1;
    int producer_node_id = -1;
    int producer_visual_task_sequence = -1;
    int target_node_id = -1;
    std::uint64_t target_node_creation_sequence = 0;
    int producer_node_index = -1;
    int target_node_index = -1;
    int staged_frame_index = -1;
    std::uint64_t staged_traversal_generation = 0;
    std::uint64_t eligible_traversal_generation = 0;
    std::uint64_t callback_publication_revision = 0;
    int reset_value = 0;
    BattleFrameInstructionControlResetSource source =
        BattleFrameInstructionControlResetSource::DirectTransition;
    BattleFrameInstructionControlResetTiming timing =
        BattleFrameInstructionControlResetTiming::Unknown;
    std::string provenance;
};

struct BattleFrameInstructionControlResetHistoryEvent {
    int sequence = -1;
    int frame_index = -1;
    int action_ordinal = -1;
    int target_slot = -1;
    int producer_node_id = -1;
    int producer_visual_task_sequence = -1;
    int target_node_id = -1;
    int producer_node_index = -1;
    int target_node_index = -1;
    std::uint64_t traversal_generation = 0;
    std::uint64_t callback_publication_revision = 0;
    int callback_state_before = 0;
    int callback_state_after = 0;
    BattleFrameInstructionControlResetSource source =
        BattleFrameInstructionControlResetSource::QueuedTransition;
    BattleFrameInstructionControlResetLifecycle lifecycle =
        BattleFrameInstructionControlResetLifecycle::MissingInput;
    BattleFrameInstructionControlResetTiming timing =
        BattleFrameInstructionControlResetTiming::Unknown;
    std::string provenance;
};

struct BattleFrameVisualRuntime {
    BattleFrameActionViewControllerRuntime controller{};
    BattleFrameActionViewRoleRuntime action_view_role{};
    BattleFrameActionViewActiveRecordRuntime active_record{};
    std::array<std::optional<ActionViewMode11CameraOperands>,
               kBattleFrameCombatantSlotCapacity>
        mode11_camera_operands{};
    std::array<std::optional<CombatantVisualResource>, kBattleFrameCombatantSlotCapacity>
        resources{};
    std::array<CombatantVisualTimelineState, kBattleFrameCombatantSlotCapacity>
        timelines{};
    std::array<int, kBattleFrameCombatantSlotCapacity> timeline_action_ordinals{};
    std::array<CombatantInstructionStdRowProducerCursor,
               kBattleFrameCombatantSlotCapacity>
        std_row_producers{};
    std::array<ActionMotionPlaybackRuntime, kBattleFrameCombatantSlotCapacity>
        action_motion_playbacks{};
    std::array<BattleFramePersistentInstructionCallbackRuntime,
               kBattleFrameCombatantSlotCapacity>
        persistent_instruction_callbacks{};
    std::vector<BattleFramePendingInstructionControlReset>
        pending_instruction_control_resets;
    std::vector<BattleFrameInstructionControlResetHistoryEvent>
        instruction_control_reset_history;
    std::vector<BattleFrameVisualChildTask> child_tasks;
    std::vector<BattleFrameActionViewRoleFlagChildRuntime>
        role_flag_children;
    std::vector<BattleFrameStepEvent> history;
    std::vector<BattleFrameStepEvent> pending_events;
    std::string pathing_profile_name;
    int next_child_sequence = 0;
    int next_role_flag_child_sequence = 0;
    int next_instruction_control_reset_sequence = 0;
    int current_visit_cursor = -1;
};

struct BattleFrameRuntime {
    BattleFrameState state{};
    BattleFrameThreadListRuntime thread_list{};
    BattleCollisionOccupancyRuntime collision_occupancy{};
    std::array<BattleFrameTargetReactionRuntime,
               kBattleFrameCombatantSlotCapacity>
        target_reactions{};
    std::optional<int> persistent_action_view_controller_node_id;
    std::optional<int> std_resource_worker_node_id;
    ViewPlacementCacheRuntime view_placement_cache{};
    BattleFrameVisualRuntime visual{};
    bool initialized = false;
    std::vector<BattleFrameWorker> workers;
    std::array<std::vector<int>, kBattleFrameCombatantSlotCapacity> slot_worker_queues;
    std::vector<BattleFramePendingMovementInvocation> pending_movement_invocations;
    std::array<BattleMovementControllerState, kBattleFrameCombatantSlotCapacity>
        movement_controller_states{};
    std::array<BattleFrameMovementWorksheetRuntime, kBattleFrameCombatantSlotCapacity>
        movement_worksheets{};
    std::array<BattleFrameCombatantInstructionRuntime,
               kBattleFrameCombatantSlotCapacity>
        combatant_instructions{};
    std::vector<BattleFrameMovementInvocationHistoryEvent> movement_invocation_history;
    std::optional<BattleFrameActionRuntime> active_action;
    std::array<BattleFramePassiveParticipantRuntime, kBattleFrameCombatantSlotCapacity>
        passive_participants{};
    int next_worker_sequence = 0;
    int next_action_ordinal = 0;
    std::vector<BattleFrameStepEvent> last_step_events;
    std::vector<BattleFrameStepEvent> history;
    std::vector<std::string> warnings;
};

struct BattleFrameStdResourcePublicationResult {
    BattleFrameEventStatus status = BattleFrameEventStatus::MissingInput;
    std::vector<int> publication_order;
    std::vector<std::string> resource_group_order;
    std::vector<int> missing_slots;
    std::string provenance;
};

struct BattleFrameScheduleActionInput {
    int action_ordinal = -1;
    int actor_slot = -1;
    int target_slot = -1;
    bool enemy_owned = false;
    int combatant_command_parameter = 0;
    std::optional<std::int16_t> initial_instruction_parameter;
    std::optional<std::int16_t> final_instruction_parameter;
    BasicAttackExecutionRoute execution_route = BasicAttackExecutionRoute::Unknown;
    MovementSelectedWorker selected_worker = MovementSelectedWorker::None;
    BattleMovementActionKind action_kind = BattleMovementActionKind::Unknown;
    BattleMovementRelationScope relation_scope = BattleMovementRelationScope::Unknown;
    BattleMovementTurnType turn_type = BattleMovementTurnType::Unknown;
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
    const std::vector<MovementSlotState>& slots,
    const std::optional<std::array<std::uint8_t, 81>>& terrain_source_9x9 = std::nullopt,
    soa::battle::TurnType initial_turn_type = soa::battle::TurnType::Normal);

MovementWorksheetSnapshot project_battle_frame_movement_worksheet_snapshot(
    const BattleFrameRuntime& runtime,
    int actor_slot,
    int target_slot);

void schedule_first_battle_action_workers(
    BattleFrameRuntime& runtime,
    const BattleFrameScheduleActionInput& input);

bool set_worker_movement_path_from_entries(
    BattleFrameState& state,
    BattleFrameWorker& worker,
    std::uint8_t dist_to_target_0x14,
    std::uint8_t path_index_0x15,
    std::uint8_t status_0x16,
    const std::vector<MovementGridPosition>& entries);

BattleFrameActionScheduleResult schedule_first_turn_actor_action(
    BattleFrameRuntime& runtime,
    const BattleFrameScheduleActionInput& input);

bool notify_first_turn_action_resolution(
    BattleFrameRuntime& runtime,
    const BattleFrameActionResolution& resolution);

bool publish_first_turn_target_reaction(
    BattleFrameRuntime& runtime,
    const BattleFrameTargetReactionPublication& publication);

bool configure_battle_frame_visual_resource(
    BattleFrameRuntime& runtime,
    CombatantVisualResource resource);

BattleFrameThreadMutationResult publish_battle_frame_std_resource(
    BattleFrameRuntime& runtime,
    CombatantVisualResource resource,
    std::string provenance,
    std::optional<int> parent_thread_node_id = std::nullopt);

BattleFrameStdResourcePublicationResult
publish_configured_battle_frame_std_resources(
    BattleFrameRuntime& runtime,
    std::string provenance);

void configure_battle_frame_visual_pathing_profile(
    BattleFrameRuntime& runtime,
    std::string profile_name);

bool stage_battle_frame_visual_instruction_state(
    BattleFrameRuntime& runtime,
    int action_ordinal,
    CombatantVisualInstructionSnapshot instruction);

bool stage_battle_frame_validated_instruction_transition(
    BattleFrameRuntime& runtime,
    int action_ordinal,
    int slot,
    int target_slot,
    std::int16_t instruction_mode,
    std::string provenance,
    int producer_visual_task_sequence = -1);

bool stage_battle_frame_queued_std_action_transition(
    BattleFrameRuntime& runtime,
    int action_ordinal);

bool publish_battle_frame_persistent_instruction_callback(
    BattleFrameRuntime& runtime,
    int slot,
    BattleFrameInstructionCallbackPublicationSource source,
    std::string provenance);

bool battle_frame_action_visual_publication_pending(
    const BattleFrameRuntime& runtime,
    int action_ordinal);

bool set_first_turn_action_completion_override(
    BattleFrameRuntime& runtime,
    int action_ordinal,
    bool enabled = true);

bool open_first_turn_action_completion(
    BattleFrameRuntime& runtime,
    int action_ordinal);

BattleFrameRunResult run_first_turn_action_until_complete(
    BattleFrameRuntime& runtime,
    std::uint32_t& rng_state,
    int action_ordinal,
    int max_frames);

void schedule_first_turn_action_view_rng(
    BattleFrameRuntime& runtime,
    int actor_slot,
    int target_slot,
    std::string label,
    int draws_consumed,
    std::string detail,
    BattleFrameEventStatus status = BattleFrameEventStatus::Provisional);

void schedule_first_turn_end_view_placement(
    BattleFrameRuntime& runtime,
    int actor_slot);

bool schedule_effect_chunks_for_source_key(
    BattleFrameRuntime& runtime,
    int actor_slot,
    int target_slot,
    int source_key);

void schedule_first_turn_mechanical_attack(
    BattleFrameRuntime& runtime,
    int actor_slot,
    int target_slot);

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
const char* battle_frame_action_phase_name(BattleFrameActionPhase phase);
const char* battle_frame_passive_participant_phase_name(
    BattleFramePassiveParticipantPhase phase);
const char* battle_frame_combatant_instruction_phase_name(
    BattleFrameCombatantInstructionPhase phase);
const char* battle_frame_visual_child_kind_name(BattleFrameVisualChildKind kind);
const char* battle_frame_visual_child_phase_name(BattleFrameVisualChildPhase phase);
const char* battle_frame_instruction_control_reset_source_name(
    BattleFrameInstructionControlResetSource source);
const char* battle_frame_instruction_control_reset_lifecycle_name(
    BattleFrameInstructionControlResetLifecycle lifecycle);
const char* battle_frame_instruction_control_reset_timing_name(
    BattleFrameInstructionControlResetTiming timing);

} // namespace savor::predict
