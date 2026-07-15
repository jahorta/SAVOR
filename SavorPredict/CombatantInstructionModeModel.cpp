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
        << "; queued_command_parameter=" << input.queued_command_parameter;
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

    if (!input.attack_landed) {
        result.status = CombatantInstructionModeStatus::Skipped;
        result.provenance = "unlanded_attack_no_post_attack_mode_handoff";
        result.detail += "; " + std::string(combatant_instruction_mode_rule_detail());
        return result;
    }

    if (input.counter_follow_up) {
        result.status = CombatantInstructionModeStatus::Skipped;
        result.provenance = "counter_follow_up_mode_not_modeled";
        result.detail += "; " + std::string(combatant_instruction_mode_rule_detail());
        return result;
    }

    if (input.action_kind != CombatantInstructionActionKind::BasicAttack) {
        result.status = CombatantInstructionModeStatus::Unsupported;
        result.provenance = "unsupported_non_basic_action";
        result.detail += "; " + std::string(combatant_instruction_mode_rule_detail());
        return result;
    }

    if (!input.attack_result.has_value()) {
        result.status = CombatantInstructionModeStatus::MissingInput;
        result.provenance = "missing_attack_result";
        result.detail += "; " + std::string(combatant_instruction_mode_rule_detail());
        return result;
    }

    if (*input.attack_result == 2) {
        result.status = CombatantInstructionModeStatus::Validated;
        result.instruction_mode_0x6 = 8;
        result.provenance = "validated_critical_result_to_mode8_handoff";
        result.detail += "; selected_instruction_mode_0x6=8";
        result.detail += "; " + std::string(combatant_instruction_mode_rule_detail());
        return result;
    }

    result.status = CombatantInstructionModeStatus::Unsupported;
    result.provenance = "noncritical_basic_attack_mode_producer_not_modeled";
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
        return provisional_controller_mode(
            input,
            5,
            "provisional_active_pc_direct_invocation_mode5");
    case CombatantInstructionProducerFamily::ActivePcFallback:
        return provisional_controller_mode(
            input,
            5,
            "provisional_active_pc_fallback_invocation_mode5");
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
           "Battle_CombatantInstructionWorksheet+0x6; the current validated rule "
           "is the captured critical-hit chain where attack_result 2 reaches mode 8 "
           "before action-view and effect consumers; controller transitions for active "
           "PC direct, active PC fallback, and ambient formation are provisional "
           "corpus-backed producers; the active movement handoff does not guess a mode; "
           "broader producer operand staging "
           "remains intentionally unmodeled";
}

} // namespace savor::predict
