#include "BattleMovementInvocationModel.h"

#include <algorithm>
#include <unordered_set>

namespace savor::predict {
namespace {

template <typename Input>
const BattleMovementInvocationSlotState* find_slot(const Input& input, int slot) {
    const auto found = std::find_if(
        input.slots.begin(),
        input.slots.end(),
        [slot](const BattleMovementInvocationSlotState& candidate) {
            return candidate.slot == slot;
        });
    return found == input.slots.end() ? nullptr : &*found;
}

template <typename Input>
int find_thread_order_index(const Input& input, int slot) {
    for (std::size_t index = 0; index < input.packed_thread_order.size(); ++index) {
        const auto& thread = input.packed_thread_order[index];
        if (thread.active && thread.slot == slot) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

template <typename Input>
BattleMovementControllerState find_prior_state(const Input& input, int slot) {
    const auto found = std::find_if(
        input.prior_controller_states.begin(),
        input.prior_controller_states.end(),
        [slot](const BattleMovementPriorControllerState& candidate) {
            return candidate.slot == slot;
        });
    return found == input.prior_controller_states.end()
        ? BattleMovementControllerState::Unknown
        : found->state;
}

BattleMovementInvocationStatus combine_status(
    BattleMovementInvocationStatus left,
    BattleMovementInvocationStatus right) {
    const auto rank = [](BattleMovementInvocationStatus status) {
        switch (status) {
        case BattleMovementInvocationStatus::MissingInput: return 5;
        case BattleMovementInvocationStatus::Ambiguous: return 4;
        case BattleMovementInvocationStatus::Unsupported: return 3;
        case BattleMovementInvocationStatus::Provisional: return 2;
        case BattleMovementInvocationStatus::Skipped: return 1;
        case BattleMovementInvocationStatus::Matched: return 0;
        }
        return 5;
    };
    return rank(right) > rank(left) ? right : left;
}

template <typename Input>
void validate_invocation(
    const Input& input,
    std::unordered_set<int>& assigned_slots,
    BattleMovementInvocationDecision& decision) {
    decision.prior_controller_state = find_prior_state(input, decision.slot);
    const auto* slot = find_slot(input, decision.slot);
    if (slot == nullptr) {
        decision.should_invoke = false;
        decision.status = BattleMovementInvocationStatus::MissingInput;
        decision.confidence = "combatant slot state is missing";
        return;
    }
    if (!slot->present || !slot->alive) {
        decision.should_invoke = false;
        decision.status = BattleMovementInvocationStatus::Skipped;
        decision.confidence = "combatant is absent or dead";
        return;
    }

    decision.thread_order_index = find_thread_order_index(input, decision.slot);
    if (decision.thread_order_index < 0) {
        decision.should_invoke = false;
        decision.status = BattleMovementInvocationStatus::MissingInput;
        decision.confidence = "combatant has no active packed-thread entry";
        return;
    }
    if (!assigned_slots.insert(decision.slot).second) {
        decision.should_invoke = false;
        decision.status = BattleMovementInvocationStatus::Ambiguous;
        decision.confidence = "duplicate movement invocation for one slot";
        return;
    }

    decision.should_invoke = true;
    if (decision.prior_controller_state != BattleMovementControllerState::Unknown
        && decision.prior_controller_state != BattleMovementControllerState::Idle
        && decision.prior_controller_state != BattleMovementControllerState::Relay804B8
        && decision.prior_controller_state != BattleMovementControllerState::Relay801A8) {
        decision.status = combine_status(
            decision.status,
            BattleMovementInvocationStatus::Provisional);
        decision.confidence += "; prior controller state is still active";
    }
}

BattleMovementControllerState controller_state_for_family(
    BattleMovementControllerFamily family) {
    switch (family) {
    case BattleMovementControllerFamily::AmbientPursuit:
        return BattleMovementControllerState::AmbientPursuit;
    case BattleMovementControllerFamily::AmbientFormation:
        return BattleMovementControllerState::AmbientFormation;
    case BattleMovementControllerFamily::AmbientIdle:
        return BattleMovementControllerState::AmbientIdle;
    case BattleMovementControllerFamily::AffectedTargetReaction:
        return BattleMovementControllerState::AffectedTargetReaction;
    case BattleMovementControllerFamily::AffectedGroupReaction:
        return BattleMovementControllerState::AffectedGroupReaction;
    case BattleMovementControllerFamily::StatusReactionD610:
    case BattleMovementControllerFamily::StatusReactionD960:
        return BattleMovementControllerState::StatusReaction;
    case BattleMovementControllerFamily::SpecialReaction3:
    case BattleMovementControllerFamily::SpecialReaction4:
    case BattleMovementControllerFamily::SpecialReaction5:
        return BattleMovementControllerState::SpecialReaction;
    case BattleMovementControllerFamily::Unsupported:
        return BattleMovementControllerState::Unsupported;
    default:
        return BattleMovementControllerState::Unknown;
    }
}

std::uint32_t callback_pc_for_family(BattleMovementControllerFamily family) {
    switch (family) {
    case BattleMovementControllerFamily::AmbientPursuit: return 0x8008C21Cu;
    case BattleMovementControllerFamily::AmbientFormation: return 0x8008C7B0u;
    case BattleMovementControllerFamily::AmbientIdle: return 0x8008C98Cu;
    case BattleMovementControllerFamily::AffectedTargetReaction: return 0x8008D3B0u;
    case BattleMovementControllerFamily::AffectedGroupReaction: return 0x8008CBB8u;
    case BattleMovementControllerFamily::StatusReactionD610: return 0x8008D610u;
    case BattleMovementControllerFamily::StatusReactionD960: return 0x8008D960u;
    case BattleMovementControllerFamily::SpecialReaction3: return 0x8008C6BCu;
    case BattleMovementControllerFamily::SpecialReaction4: return 0x8008D4A4u;
    case BattleMovementControllerFamily::SpecialReaction5: return 0x8008D610u;
    default: return 0;
    }
}

BattleMovementControllerFamily route0_family(
    const BattleMovementPassiveDispatchInput& input,
    const BattleMovementInvocationSlotState& slot) {
    const auto status = slot.status_flags;
    const auto movement = slot.movement_flags;

    if ((status & 0x800u) != 0) {
        return BattleMovementControllerFamily::StatusReactionD960;
    }
    if ((status & 0x6500u) != 0) {
        return BattleMovementControllerFamily::StatusReactionD610;
    }
    if ((input.turn_type == BattleMovementTurnType::BackAttack && slot.is_player)
        || (input.turn_type == BattleMovementTurnType::Advantage && !slot.is_player)) {
        return BattleMovementControllerFamily::AmbientIdle;
    }
    if ((movement & 0x1u) == 0) {
        return BattleMovementControllerFamily::AmbientIdle;
    }
    if ((status & 0x1u) == 0 || (movement & 0x2u) == 0) {
        if ((movement & 0x10u) == 0 || (movement & 0x2u) == 0) {
            return (movement & 0x20u) == 0
                ? BattleMovementControllerFamily::AmbientPursuit
                : BattleMovementControllerFamily::AmbientIdle;
        }
        return slot.is_player
            ? BattleMovementControllerFamily::AmbientFormation
            : BattleMovementControllerFamily::AmbientIdle;
    }
    return slot.is_player
        ? BattleMovementControllerFamily::AmbientFormation
        : BattleMovementControllerFamily::AmbientIdle;
}

BattleMovementInvocationDecision passive_decision(
    const BattleMovementPassiveDispatchInput& input,
    const BattleMovementInvocationSlotState& slot) {
    BattleMovementInvocationDecision decision;
    decision.action_ordinal = input.action_ordinal;
    decision.slot = slot.slot;
    decision.worker_kind = BattleMovementInvocationWorkerKind::PassiveController;
    decision.activation_timing = BattleMovementActivationTiming::NextThreadVisit;
    decision.status = BattleMovementInvocationStatus::Matched;

    if (input.action_kind == BattleMovementActionKind::BasicAttack
        && input.relation_scope == BattleMovementRelationScope::SingleTarget) {
        if (slot.slot == input.final_target_slot) {
            decision.relation_route = BattleMovementRelationRoute::AffectedTarget1;
            decision.controller_family =
                BattleMovementControllerFamily::AffectedTargetReaction;
            decision.semantic_target_slot = input.actor_slot;
            decision.path_selection_policy =
                BattleMovementPathSelectionPolicy::StraightRunPathIndex;
            decision.leg_policy = BattleMovementInvocationLegPolicy::CompleteAfterLeg;
            decision.confidence = "validated single-target route 1";
            decision.provenance =
                "FUN_8008DC1C route 1 dispatches FUN_8008D3B0 for the affected target";
        } else {
            decision.relation_route = BattleMovementRelationRoute::Ambient0;
            decision.controller_family = route0_family(input, slot);
            decision.path_selection_policy =
                BattleMovementPathSelectionPolicy::NextPathingGridSquare;
            decision.leg_policy = decision.controller_family
                    == BattleMovementControllerFamily::AmbientPursuit
                || decision.controller_family
                    == BattleMovementControllerFamily::AmbientFormation
                ? BattleMovementInvocationLegPolicy::RebuildPath
                : BattleMovementInvocationLegPolicy::CompleteAfterLeg;
            decision.confidence = "static FUN_8008DEEC route-0 policy";
            decision.provenance =
                "route 0 callback selected from turn type, status flags, movement flags, and side";
        }
    } else {
        decision.relation_route = BattleMovementRelationRoute::Unknown;
        decision.controller_family = BattleMovementControllerFamily::Unsupported;
        decision.status = BattleMovementInvocationStatus::Unsupported;
        decision.confidence = "action kind or relation scope is not modeled";
        decision.provenance =
            "unknown passive route remains drainable without inventing callback semantics";
    }

    decision.callback_pc = callback_pc_for_family(decision.controller_family);
    decision.activation_controller_state =
        controller_state_for_family(decision.controller_family);
    decision.worker_controller_state = decision.activation_controller_state;
    return decision;
}

} // namespace

BattleMovementInvocationDecision model_active_movement_invocation(
    const BattleMovementActiveInvocationInput& input) {
    BattleMovementInvocationDecision decision;
    decision.action_ordinal = input.action_ordinal;
    decision.slot = input.actor_slot;
    decision.semantic_target_slot = input.final_target_slot;
    decision.selected_worker = input.selected_active_worker;
    decision.path_selection_policy =
        BattleMovementPathSelectionPolicy::StraightRunPathIndex;
    decision.activation_timing = BattleMovementActivationTiming::NextThreadVisit;

    switch (input.selected_active_worker) {
    case MovementSelectedWorker::PcDirectAttack_80086308:
        decision.controller_family = BattleMovementControllerFamily::ActivePcDirect;
        decision.worker_kind = BattleMovementInvocationWorkerKind::ActivePcDirect;
        decision.leg_policy = BattleMovementInvocationLegPolicy::AdvanceExistingPath;
        decision.activation_controller_state = BattleMovementControllerState::ActivePcDirect;
        decision.worker_controller_state = BattleMovementControllerState::ActivePcDirect;
        decision.callback_pc = 0x80086308u;
        decision.status = BattleMovementInvocationStatus::Matched;
        decision.confidence = "observed active-PC direct controller family";
        decision.provenance = "active worker selected by movement setup";
        break;
    case MovementSelectedWorker::PcFallbackAttack_80085ce0:
        decision.controller_family = BattleMovementControllerFamily::ActivePcFallback;
        decision.worker_kind = BattleMovementInvocationWorkerKind::ActivePcFallback;
        decision.leg_policy = BattleMovementInvocationLegPolicy::CompleteAfterLeg;
        decision.activation_controller_state = BattleMovementControllerState::ActivePcFallback;
        decision.worker_controller_state = BattleMovementControllerState::ActivePcFallback;
        decision.callback_pc = 0x80085CE0u;
        decision.status = BattleMovementInvocationStatus::Provisional;
        decision.confidence = "fallback assignment retained from movement setup";
        decision.provenance = "active PC fallback internals remain provisional";
        break;
    case MovementSelectedWorker::EnemyDirectAttack_80087f6c:
        decision.controller_family = BattleMovementControllerFamily::EnemyDirect;
        decision.worker_kind = BattleMovementInvocationWorkerKind::EnemyDirect;
        decision.leg_policy = BattleMovementInvocationLegPolicy::AdvanceExistingPath;
        decision.activation_timing =
            BattleMovementActivationTiming::SameThreadVisitAfterHandoff;
        decision.activation_controller_state = BattleMovementControllerState::EnemyHandler;
        decision.worker_controller_state = BattleMovementControllerState::EnemyDirect;
        decision.callback_pc = 0x80087F6Cu;
        decision.status = BattleMovementInvocationStatus::Provisional;
        decision.confidence = "observed enemy handler-to-direct transition";
        decision.provenance = "enemy direct callback executes in the handler handoff visit";
        break;
    case MovementSelectedWorker::EnemyFallbackAttack_80087844:
        decision.controller_family = BattleMovementControllerFamily::EnemyFallback;
        decision.worker_kind = BattleMovementInvocationWorkerKind::EnemyFallback;
        decision.leg_policy = BattleMovementInvocationLegPolicy::AdvanceExistingPath;
        decision.activation_controller_state = BattleMovementControllerState::EnemyFallback;
        decision.worker_controller_state = BattleMovementControllerState::EnemyFallback;
        decision.callback_pc = 0x80087844u;
        decision.status = BattleMovementInvocationStatus::Provisional;
        decision.confidence = "fallback assignment retained from movement setup";
        decision.provenance = "enemy fallback invocation timing remains provisional";
        break;
    case MovementSelectedWorker::None:
        decision.controller_family = input.enemy_owned
            ? BattleMovementControllerFamily::EnemyFallback
            : BattleMovementControllerFamily::ActivePcFallback;
        decision.worker_kind = input.enemy_owned
            ? BattleMovementInvocationWorkerKind::EnemyFallback
            : BattleMovementInvocationWorkerKind::ActivePcFallback;
        decision.leg_policy = BattleMovementInvocationLegPolicy::CompleteAfterLeg;
        decision.activation_controller_state = input.enemy_owned
            ? BattleMovementControllerState::EnemyFallback
            : BattleMovementControllerState::ActivePcFallback;
        decision.worker_controller_state = decision.activation_controller_state;
        decision.callback_pc = input.enemy_owned ? 0x80087844u : 0x80085CE0u;
        decision.status = BattleMovementInvocationStatus::Provisional;
        decision.confidence = "missing selected worker uses the typed fallback";
        decision.provenance = "fallback preserves predictor progress without inventing a direct path";
        break;
    }

    std::unordered_set<int> assigned_slots;
    validate_invocation(input, assigned_slots, decision);
    return decision;
}

BattleMovementInvocationPlan model_passive_movement_dispatch(
    const BattleMovementPassiveDispatchInput& input) {
    BattleMovementInvocationPlan result;
    result.status = BattleMovementInvocationStatus::Matched;
    result.provenance =
        "passive relation route and callback policy are modeled separately";
    result.confidence =
        "basic single-target route and FUN_8008DEEC route-0 policy are evidence-backed";

    if (input.action_ordinal < 0 || input.actor_slot < 0
        || input.final_target_slot < 0) {
        result.status = BattleMovementInvocationStatus::MissingInput;
        result.confidence = "action ordinal, actor, and final target are required";
        return result;
    }

    std::unordered_set<int> assigned_slots;
    for (const auto& slot : input.slots) {
        if (!slot.present || !slot.alive || slot.slot == input.actor_slot) {
            continue;
        }
        auto decision = passive_decision(input, slot);
        validate_invocation(input, assigned_slots, decision);
        result.status = combine_status(result.status, decision.status);
        result.decisions.push_back(std::move(decision));
    }
    return result;
}

const char* battle_movement_action_kind_name(BattleMovementActionKind kind) {
    switch (kind) {
    case BattleMovementActionKind::BasicAttack: return "BasicAttack";
    case BattleMovementActionKind::Guard: return "Guard";
    case BattleMovementActionKind::Unknown: return "Unknown";
    }
    return "Unknown";
}

const char* battle_movement_relation_scope_name(BattleMovementRelationScope scope) {
    switch (scope) {
    case BattleMovementRelationScope::SingleTarget: return "SingleTarget";
    case BattleMovementRelationScope::Unsupported: return "Unsupported";
    case BattleMovementRelationScope::Unknown: return "Unknown";
    }
    return "Unknown";
}

const char* battle_movement_relation_route_name(BattleMovementRelationRoute route) {
    switch (route) {
    case BattleMovementRelationRoute::Ambient0: return "Ambient0";
    case BattleMovementRelationRoute::AffectedTarget1: return "AffectedTarget1";
    case BattleMovementRelationRoute::AffectedGroup2: return "AffectedGroup2";
    case BattleMovementRelationRoute::Special3: return "Special3";
    case BattleMovementRelationRoute::Special4: return "Special4";
    case BattleMovementRelationRoute::Special5: return "Special5";
    case BattleMovementRelationRoute::Unknown: return "Unknown";
    }
    return "Unknown";
}

const char* battle_movement_controller_family_name(
    BattleMovementControllerFamily family) {
    switch (family) {
    case BattleMovementControllerFamily::ActivePcDirect: return "ActivePcDirect";
    case BattleMovementControllerFamily::ActivePcFallback: return "ActivePcFallback";
    case BattleMovementControllerFamily::EnemyHandler: return "EnemyHandler";
    case BattleMovementControllerFamily::EnemyDirect: return "EnemyDirect";
    case BattleMovementControllerFamily::EnemyFallback: return "EnemyFallback";
    case BattleMovementControllerFamily::PassiveRelay: return "PassiveRelay";
    case BattleMovementControllerFamily::PassiveDispatch: return "PassiveDispatch";
    case BattleMovementControllerFamily::AmbientPursuit: return "AmbientPursuit";
    case BattleMovementControllerFamily::AmbientFormation: return "AmbientFormation";
    case BattleMovementControllerFamily::AmbientIdle: return "AmbientIdle";
    case BattleMovementControllerFamily::AffectedTargetReaction: return "AffectedTargetReaction";
    case BattleMovementControllerFamily::AffectedGroupReaction: return "AffectedGroupReaction";
    case BattleMovementControllerFamily::StatusReactionD610: return "StatusReactionD610";
    case BattleMovementControllerFamily::StatusReactionD960: return "StatusReactionD960";
    case BattleMovementControllerFamily::SpecialReaction3: return "SpecialReaction3";
    case BattleMovementControllerFamily::SpecialReaction4: return "SpecialReaction4";
    case BattleMovementControllerFamily::SpecialReaction5: return "SpecialReaction5";
    case BattleMovementControllerFamily::PursuitCoordination: return "PursuitCoordination";
    case BattleMovementControllerFamily::Unsupported: return "Unsupported";
    case BattleMovementControllerFamily::Unknown: return "Unknown";
    }
    return "Unknown";
}

const char* battle_movement_activation_timing_name(
    BattleMovementActivationTiming timing) {
    switch (timing) {
    case BattleMovementActivationTiming::NextThreadVisit: return "NextThreadVisit";
    case BattleMovementActivationTiming::SameThreadVisitAfterHandoff:
        return "SameThreadVisitAfterHandoff";
    }
    return "Unknown";
}

const char* battle_movement_invocation_worker_kind_name(
    BattleMovementInvocationWorkerKind kind) {
    switch (kind) {
    case BattleMovementInvocationWorkerKind::None: return "None";
    case BattleMovementInvocationWorkerKind::ActivePcDirect: return "ActivePcDirect";
    case BattleMovementInvocationWorkerKind::ActivePcFallback: return "ActivePcFallback";
    case BattleMovementInvocationWorkerKind::EnemyDirect: return "EnemyDirect";
    case BattleMovementInvocationWorkerKind::EnemyFallback: return "EnemyFallback";
    case BattleMovementInvocationWorkerKind::PassiveController: return "PassiveController";
    }
    return "Unknown";
}

const char* battle_movement_invocation_leg_policy_name(
    BattleMovementInvocationLegPolicy policy) {
    switch (policy) {
    case BattleMovementInvocationLegPolicy::RebuildPath: return "RebuildPath";
    case BattleMovementInvocationLegPolicy::AdvanceExistingPath: return "AdvanceExistingPath";
    case BattleMovementInvocationLegPolicy::CompleteAfterLeg: return "CompleteAfterLeg";
    }
    return "Unknown";
}

const char* battle_movement_controller_state_name(
    BattleMovementControllerState state) {
    switch (state) {
    case BattleMovementControllerState::Unknown: return "Unknown";
    case BattleMovementControllerState::Idle: return "Idle";
    case BattleMovementControllerState::ActivePcDirect: return "ActivePcDirect";
    case BattleMovementControllerState::ActivePcFallback: return "ActivePcFallback";
    case BattleMovementControllerState::EnemyHandler: return "EnemyHandler";
    case BattleMovementControllerState::EnemyDirect: return "EnemyDirect";
    case BattleMovementControllerState::EnemyFallback: return "EnemyFallback";
    case BattleMovementControllerState::Relay804B8: return "Relay804B8";
    case BattleMovementControllerState::Relay801A8: return "Relay801A8";
    case BattleMovementControllerState::Dispatch8DEEC: return "Dispatch8DEEC";
    case BattleMovementControllerState::AmbientPursuit: return "AmbientPursuit";
    case BattleMovementControllerState::AmbientFormation: return "AmbientFormation";
    case BattleMovementControllerState::AmbientIdle: return "AmbientIdle";
    case BattleMovementControllerState::AffectedTargetReaction: return "AffectedTargetReaction";
    case BattleMovementControllerState::AffectedGroupReaction: return "AffectedGroupReaction";
    case BattleMovementControllerState::StatusReaction: return "StatusReaction";
    case BattleMovementControllerState::SpecialReaction: return "SpecialReaction";
    case BattleMovementControllerState::PursuitCoordination: return "PursuitCoordination";
    case BattleMovementControllerState::CompletionDeferred: return "CompletionDeferred";
    case BattleMovementControllerState::Removed: return "Removed";
    case BattleMovementControllerState::Unsupported: return "Unsupported";
    }
    return "Unknown";
}

const char* battle_movement_invocation_status_name(
    BattleMovementInvocationStatus status) {
    switch (status) {
    case BattleMovementInvocationStatus::Matched: return "Matched";
    case BattleMovementInvocationStatus::Provisional: return "Provisional";
    case BattleMovementInvocationStatus::Skipped: return "Skipped";
    case BattleMovementInvocationStatus::MissingInput: return "MissingInput";
    case BattleMovementInvocationStatus::Unsupported: return "Unsupported";
    case BattleMovementInvocationStatus::Ambiguous: return "Ambiguous";
    }
    return "Unknown";
}

} // namespace savor::predict
