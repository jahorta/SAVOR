#include "CombatantInstructionModeModel.h"

#include <sstream>
#include <utility>

namespace savor::predict {
namespace {

std::string input_detail(const CombatantInstructionModeInput& input) {
    std::ostringstream out;
    out << "actor_slot=" << input.actor_slot
        << "; target_slot=" << input.target_slot
        << "; action_kind=" << combatant_instruction_action_kind_name(input.action_kind)
        << "; attack_landed=" << (input.attack_landed ? 1 : 0)
        << "; counter_follow_up=" << (input.counter_follow_up ? 1 : 0)
        << "; queued_command_parameter=" << input.queued_command_parameter
        << "; execution_route="
        << basic_attack_execution_route_name(input.execution_route);
    if (input.attack_result.has_value()) {
        out << "; attack_result=" << *input.attack_result;
    }
    if (input.prior_instruction_mode_0x6.has_value()) {
        out << "; prior_instruction_mode_0x6=" << *input.prior_instruction_mode_0x6;
    }
    return out.str();
}

std::string controller_input_detail(
    const CombatantInstructionControllerTransitionInput& input) {
    std::ostringstream out;
    out << "action_ordinal=" << input.action_ordinal
        << "; actor_slot=" << input.actor_slot
        << "; slot=" << input.slot
        << "; target_slot=" << input.target_slot
        << "; action_kind="
        << combatant_instruction_action_kind_name(input.action_kind)
        << "; trigger="
        << combatant_instruction_transition_trigger_name(input.trigger)
        << "; producer_family="
        << combatant_instruction_producer_family_name(input.producer_family);
    if (input.prior_instruction_mode_0x6.has_value()) {
        out << "; prior_instruction_mode_0x6="
            << *input.prior_instruction_mode_0x6;
    }
    return out.str();
}

CombatantInstructionModeResult provisional_controller_mode(
    const CombatantInstructionControllerTransitionInput& input,
    int mode,
    std::string provenance) {
    CombatantInstructionModeResult result;
    result.status = CombatantInstructionModeStatus::Provisional;
    result.instruction_mode_0x6 = mode;
    result.provenance = std::move(provenance);
    result.detail = controller_input_detail(input)
        + "; selected_instruction_mode_0x6=" + std::to_string(mode)
        + "; evidence_scope=trusted_fake_attack_0_lifecycle_corpus";
    return result;
}

} // namespace

CombatantInstructionModeResult model_combatant_instruction_mode_transition(
    const CombatantInstructionModeInput& input) {
    CombatantInstructionModeResult result;
    result.detail = input_detail(input);

    if (input.action_kind == CombatantInstructionActionKind::Unknown) {
        result.status = CombatantInstructionModeStatus::Unsupported;
        result.provenance = "unsupported_action_kind";
        result.detail += "; " + std::string(combatant_instruction_mode_rule_detail());
        return result;
    }

    if (input.action_kind != CombatantInstructionActionKind::BasicAttack) {
        result.status = CombatantInstructionModeStatus::Unsupported;
        result.provenance = "unsupported_non_basic_action";
        result.detail += "; " + std::string(combatant_instruction_mode_rule_detail());
        return result;
    }

    const auto queued = model_basic_attack_queued_state({
        .route = input.execution_route,
        .attack_result = input.attack_result,
        .counter_follow_up = input.counter_follow_up,
    });
    switch (queued.status) {
    case QueuedInstructionParamStatus::Validated:
        result.status = CombatantInstructionModeStatus::Validated;
        break;
    case QueuedInstructionParamStatus::Provisional:
        result.status = CombatantInstructionModeStatus::Provisional;
        break;
    case QueuedInstructionParamStatus::MissingInput:
        result.status = CombatantInstructionModeStatus::MissingInput;
        break;
    case QueuedInstructionParamStatus::Unsupported:
    case QueuedInstructionParamStatus::Inconsistent:
        result.status = CombatantInstructionModeStatus::Unsupported;
        break;
    }
    result.queued_state = queued.queued_state;
    result.instruction_mode_0x6 = queued.instruction_mode;
    result.provenance = queued.provenance;
    if (queued.queued_state.has_value()) {
        result.detail += "; queued_state="
            + std::to_string(static_cast<int>(*queued.queued_state));
    }
    if (queued.instruction_mode.has_value()) {
        result.detail += "; selected_instruction_mode_0x6="
            + std::to_string(*queued.instruction_mode);
    }
    result.detail += "; " + std::string(combatant_instruction_mode_rule_detail());
    return result;
}

CombatantInstructionModeResult model_combatant_instruction_controller_transition(
    const CombatantInstructionControllerTransitionInput& input) {
    CombatantInstructionModeResult result;
    result.detail = controller_input_detail(input);

    if (input.action_kind != CombatantInstructionActionKind::BasicAttack) {
        result.status = input.action_kind == CombatantInstructionActionKind::Unknown
            ? CombatantInstructionModeStatus::MissingInput
            : CombatantInstructionModeStatus::Unsupported;
        result.provenance = "controller_transition_action_kind_not_modeled";
        return result;
    }

    if (input.trigger == CombatantInstructionTransitionTrigger::ActiveMovementHandoff) {
        result.status = CombatantInstructionModeStatus::Unsupported;
        result.provenance = "active_handoff_instruction_mode_not_modeled";
        return result;
    }

    switch (input.producer_family) {
    case CombatantInstructionProducerFamily::ActivePcDirect:
    case CombatantInstructionProducerFamily::ActivePcFallback:
        result.status = CombatantInstructionModeStatus::Unsupported;
        result.provenance =
            "active attack mode requires the final execution route and post-resolution queued state";
        return result;
    case CombatantInstructionProducerFamily::AmbientFormation:
        return provisional_controller_mode(
            input,
            0x13,
            "provisional_ambient_formation_invocation_mode19");
    default:
        result.status = CombatantInstructionModeStatus::Unsupported;
        result.provenance = "movement_invocation_family_not_modeled";
        return result;
    }
}

const char* combatant_instruction_action_kind_name(CombatantInstructionActionKind kind) {
    switch (kind) {
    case CombatantInstructionActionKind::Unknown: return "Unknown";
    case CombatantInstructionActionKind::BasicAttack: return "BasicAttack";
    case CombatantInstructionActionKind::Guard: return "Guard";
    }
    return "Unknown";
}

const char* combatant_instruction_mode_status_name(CombatantInstructionModeStatus status) {
    switch (status) {
    case CombatantInstructionModeStatus::Validated: return "Validated";
    case CombatantInstructionModeStatus::Provisional: return "Provisional";
    case CombatantInstructionModeStatus::Skipped: return "Skipped";
    case CombatantInstructionModeStatus::MissingInput: return "MissingInput";
    case CombatantInstructionModeStatus::Unsupported: return "Unsupported";
    }
    return "Unsupported";
}

const char* combatant_instruction_transition_trigger_name(
    CombatantInstructionTransitionTrigger trigger) {
    switch (trigger) {
    case CombatantInstructionTransitionTrigger::MovementInvocation:
        return "MovementInvocation";
    case CombatantInstructionTransitionTrigger::QueuedStdActionResolution:
        return "QueuedStdActionResolution";
    case CombatantInstructionTransitionTrigger::ActiveMovementHandoff:
        return "ActiveMovementHandoff";
    }
    return "MovementInvocation";
}

const char* combatant_instruction_producer_family_name(
    CombatantInstructionProducerFamily family) {
    switch (family) {
    case CombatantInstructionProducerFamily::Unknown: return "Unknown";
    case CombatantInstructionProducerFamily::ActivePcDirect: return "ActivePcDirect";
    case CombatantInstructionProducerFamily::ActivePcFallback: return "ActivePcFallback";
    case CombatantInstructionProducerFamily::EnemyDirect: return "EnemyDirect";
    case CombatantInstructionProducerFamily::EnemyFallback: return "EnemyFallback";
    case CombatantInstructionProducerFamily::AmbientPursuit: return "AmbientPursuit";
    case CombatantInstructionProducerFamily::AmbientFormation: return "AmbientFormation";
    case CombatantInstructionProducerFamily::AffectedTargetReaction:
        return "AffectedTargetReaction";
    }
    return "Unknown";
}

const char* combatant_instruction_mode_rule_detail() {
    return "generic instruction-mode handoff models frame-relevant writes to "
           "Battle_CombatantInstructionWorksheet+0x6; final direct attacks publish "
           "queued state 5 for miss/hit or 6 for critical and map to modes 4 or 8; "
           "final fallback attacks publish queued state 7 and map to mode 5; "
           "ambient formation remains a separate provisional controller mode; "
           "the active movement handoff does not guess an attack mode";
}

} // namespace savor::predict
