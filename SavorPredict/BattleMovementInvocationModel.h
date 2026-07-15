#pragma once

#include "BattleMovementPathModel.h"
#include "MovementModel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

enum class BattleMovementActionKind {
    BasicAttack,
    Guard,
    Unknown,
};

enum class BattleMovementRelationScope {
    SingleTarget,
    Unsupported,
    Unknown,
};

enum class BattleMovementTurnType {
    BackAttack = 0,
    Normal = 1,
    Advantage = 2,
    Unknown = -1,
};

enum class BattleMovementRelationRoute {
    Ambient0,
    AffectedTarget1,
    AffectedGroup2,
    Special3,
    Special4,
    Special5,
    Unknown,
};

enum class BattleMovementControllerFamily {
    ActivePcDirect,
    ActivePcFallback,
    EnemyHandler,
    EnemyDirect,
    EnemyFallback,
    PassiveRelay,
    PassiveDispatch,
    AmbientPursuit,
    AmbientFormation,
    AmbientIdle,
    AffectedTargetReaction,
    AffectedGroupReaction,
    StatusReactionD610,
    StatusReactionD960,
    SpecialReaction3,
    SpecialReaction4,
    SpecialReaction5,
    PursuitCoordination,
    Unsupported,
    Unknown,
};

enum class BattleMovementActivationTiming {
    NextThreadVisit,
    SameThreadVisitAfterHandoff,
};

enum class BattleMovementInvocationWorkerKind {
    None,
    ActivePcDirect,
    ActivePcFallback,
    EnemyDirect,
    EnemyFallback,
    PassiveController,
};

enum class BattleMovementInvocationLegPolicy {
    RebuildPath,
    AdvanceExistingPath,
    CompleteAfterLeg,
};

enum class BattleMovementControllerState {
    Unknown,
    Idle,
    ActivePcDirect,
    ActivePcFallback,
    EnemyHandler,
    EnemyDirect,
    EnemyFallback,
    Relay804B8,
    Relay801A8,
    Dispatch8DEEC,
    AmbientPursuit,
    AmbientFormation,
    AmbientIdle,
    AffectedTargetReaction,
    AffectedGroupReaction,
    StatusReaction,
    SpecialReaction,
    PursuitCoordination,
    CompletionDeferred,
    Removed,
    Unsupported,
};

enum class BattleMovementInvocationStatus {
    Matched,
    Provisional,
    Skipped,
    MissingInput,
    Unsupported,
    Ambiguous,
};

struct BattleMovementInvocationSlotState {
    int slot = -1;
    bool present = false;
    bool alive = false;
    bool is_player = false;
    std::uint32_t status_flags = 0;
    std::uint16_t movement_flags = 0;
};

struct BattleMovementInvocationThreadState {
    int slot = -1;
    bool active = false;
};

struct BattleMovementPriorControllerState {
    int slot = -1;
    BattleMovementControllerState state = BattleMovementControllerState::Unknown;
};

struct BattleMovementActiveInvocationInput {
    int action_ordinal = -1;
    int actor_slot = -1;
    int final_target_slot = -1;
    bool enemy_owned = false;
    MovementSelectedWorker selected_active_worker = MovementSelectedWorker::None;
    std::vector<BattleMovementInvocationSlotState> slots;
    std::vector<BattleMovementInvocationThreadState> packed_thread_order;
    std::vector<BattleMovementPriorControllerState> prior_controller_states;
};

struct BattleMovementPassiveDispatchInput {
    int action_ordinal = -1;
    int actor_slot = -1;
    int final_target_slot = -1;
    BattleMovementActionKind action_kind = BattleMovementActionKind::Unknown;
    BattleMovementRelationScope relation_scope = BattleMovementRelationScope::Unknown;
    BattleMovementTurnType turn_type = BattleMovementTurnType::Unknown;
    std::vector<BattleMovementInvocationSlotState> slots;
    std::vector<BattleMovementInvocationThreadState> packed_thread_order;
    std::vector<BattleMovementPriorControllerState> prior_controller_states;
};

struct BattleMovementInvocationDecision {
    int action_ordinal = -1;
    int slot = -1;
    std::optional<int> semantic_target_slot;
    bool should_invoke = false;
    BattleMovementRelationRoute relation_route = BattleMovementRelationRoute::Unknown;
    BattleMovementControllerFamily controller_family =
        BattleMovementControllerFamily::Unknown;
    BattleMovementInvocationWorkerKind worker_kind =
        BattleMovementInvocationWorkerKind::None;
    MovementSelectedWorker selected_worker = MovementSelectedWorker::None;
    std::uint32_t callback_pc = 0;
    BattleMovementPathSelectionPolicy path_selection_policy =
        BattleMovementPathSelectionPolicy::StraightRunPathIndex;
    BattleMovementInvocationLegPolicy leg_policy =
        BattleMovementInvocationLegPolicy::CompleteAfterLeg;
    BattleMovementActivationTiming activation_timing =
        BattleMovementActivationTiming::NextThreadVisit;
    int thread_order_index = -1;
    BattleMovementControllerState prior_controller_state =
        BattleMovementControllerState::Unknown;
    BattleMovementControllerState activation_controller_state =
        BattleMovementControllerState::Unknown;
    BattleMovementControllerState worker_controller_state =
        BattleMovementControllerState::Unknown;
    BattleMovementInvocationStatus status =
        BattleMovementInvocationStatus::MissingInput;
    std::string confidence;
    std::string provenance;
};

struct BattleMovementInvocationPlan {
    BattleMovementInvocationStatus status =
        BattleMovementInvocationStatus::MissingInput;
    std::vector<BattleMovementInvocationDecision> decisions;
    std::string confidence;
    std::string provenance;
};

BattleMovementInvocationDecision model_active_movement_invocation(
    const BattleMovementActiveInvocationInput& input);

BattleMovementInvocationPlan model_passive_movement_dispatch(
    const BattleMovementPassiveDispatchInput& input);

const char* battle_movement_action_kind_name(BattleMovementActionKind kind);
const char* battle_movement_relation_scope_name(BattleMovementRelationScope scope);
const char* battle_movement_relation_route_name(BattleMovementRelationRoute route);
const char* battle_movement_controller_family_name(
    BattleMovementControllerFamily family);
const char* battle_movement_activation_timing_name(
    BattleMovementActivationTiming timing);
const char* battle_movement_invocation_worker_kind_name(
    BattleMovementInvocationWorkerKind kind);
const char* battle_movement_invocation_leg_policy_name(
    BattleMovementInvocationLegPolicy policy);
const char* battle_movement_controller_state_name(
    BattleMovementControllerState state);
const char* battle_movement_invocation_status_name(
    BattleMovementInvocationStatus status);

} // namespace savor::predict
