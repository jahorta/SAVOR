#pragma once

#include <optional>
#include <string>

namespace savor::predict {

enum class CombatantInstructionActionKind {
    Unknown,
    BasicAttack,
    Guard,
};

enum class CombatantInstructionModeStatus {
    Validated,
    Provisional,
    Skipped,
    MissingInput,
    Unsupported,
};

enum class CombatantInstructionTransitionTrigger {
    MovementInvocation,
    ActiveMovementHandoff,
};

enum class CombatantInstructionProducerFamily {
    Unknown,
    ActivePcDirect,
    ActivePcFallback,
    EnemyDirect,
    EnemyFallback,
    AmbientPursuit,
    AmbientFormation,
    AffectedTargetReaction,
};

struct CombatantInstructionModeInput {
    int actor_slot = -1;
    int target_slot = -1;
    CombatantInstructionActionKind action_kind = CombatantInstructionActionKind::Unknown;
    std::optional<int> attack_result;
    bool attack_landed = false;
    bool counter_follow_up = false;
    int queued_command_parameter = 0;
    std::optional<int> prior_instruction_mode_0x6;
};

struct CombatantInstructionModeResult {
    CombatantInstructionModeStatus status = CombatantInstructionModeStatus::Unsupported;
    std::optional<int> instruction_mode_0x6;
    std::string provenance;
    std::string detail;
};

struct CombatantInstructionControllerTransitionInput {
    int action_ordinal = -1;
    int actor_slot = -1;
    int slot = -1;
    int target_slot = -1;
    CombatantInstructionActionKind action_kind = CombatantInstructionActionKind::Unknown;
    CombatantInstructionTransitionTrigger trigger =
        CombatantInstructionTransitionTrigger::MovementInvocation;
    CombatantInstructionProducerFamily producer_family =
        CombatantInstructionProducerFamily::Unknown;
    std::optional<int> prior_instruction_mode_0x6;
};

CombatantInstructionModeResult model_combatant_instruction_mode_transition(
    const CombatantInstructionModeInput& input);

CombatantInstructionModeResult model_combatant_instruction_controller_transition(
    const CombatantInstructionControllerTransitionInput& input);

const char* combatant_instruction_action_kind_name(CombatantInstructionActionKind kind);
const char* combatant_instruction_mode_status_name(CombatantInstructionModeStatus status);
const char* combatant_instruction_transition_trigger_name(
    CombatantInstructionTransitionTrigger trigger);
const char* combatant_instruction_producer_family_name(
    CombatantInstructionProducerFamily family);
const char* combatant_instruction_mode_rule_detail();

} // namespace savor::predict
