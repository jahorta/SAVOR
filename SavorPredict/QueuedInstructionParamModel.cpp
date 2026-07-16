#include "QueuedInstructionParamModel.h"

#include <algorithm>
#include <utility>

namespace savor::predict {
namespace {

QueuedInstructionCommandResult fixed_command(
    std::int16_t instruction,
    std::int16_t parameter,
    QueuedInstructionParamKind kind,
    QueuedInstructionParamStage stage,
    std::string provenance) {
    return {
        .instruction = instruction,
        .parameter = {
            .raw_value = parameter,
            .kind = kind,
            .stage = stage,
            .status = QueuedInstructionParamStatus::Validated,
            .confidence = "high",
            .provenance = std::move(provenance),
        },
    };
}

QueuedInstructionCommandResult id_command(
    const QueuedInstructionCommandInput& input,
    std::int16_t instruction,
    QueuedInstructionParamKind kind,
    std::string provenance) {
    if (!input.selected_id.has_value()) {
        QueuedInstructionCommandResult result;
        result.instruction = instruction;
        result.parameter.kind = kind;
        result.parameter.stage = QueuedInstructionParamStage::CommandSelected;
        result.parameter.status = QueuedInstructionParamStatus::MissingInput;
        result.parameter.confidence = "high";
        result.parameter.provenance = std::move(provenance)
            + "; selected command ID is unavailable";
        return result;
    }
    return fixed_command(
        instruction,
        *input.selected_id,
        kind,
        QueuedInstructionParamStage::CommandSelected,
        std::move(provenance));
}

} // namespace

QueuedInstructionCommandResult model_queued_instruction_command(
    const QueuedInstructionCommandInput& input) {
    switch (input.command) {
    case QueuedInstructionCommandKind::Initialize:
        return fixed_command(
            -1, -1, QueuedInstructionParamKind::Unset,
            QueuedInstructionParamStage::Initialized,
            "setupBattle_80071990 initializes all twelve queued rows to instruction=-1 and instrParam=-1");
    case QueuedInstructionCommandKind::Reset:
        return fixed_command(
            -1, -1, QueuedInstructionParamKind::Unset,
            QueuedInstructionParamStage::Initialized,
            "queued-row reset restores instruction=-1 and instrParam=-1");
    case QueuedInstructionCommandKind::Attack: {
        QueuedInstructionCommandResult result;
        result.instruction = 3;
        result.parameter.kind = QueuedInstructionParamKind::BasicAttackRoute;
        result.parameter.stage = QueuedInstructionParamStage::CommandSelected;
        result.parameter.confidence = "high";
        if (!input.movement_flags.has_value()) {
            result.parameter.status = QueuedInstructionParamStatus::MissingInput;
            result.parameter.provenance =
                "ActionSelectController_8007CAB0 selected instruction 3, but EnemyTargetSelectController_800794D8 needs movement_flags bit 0x40";
            return result;
        }
        result.parameter.raw_value = (*input.movement_flags & 0x40u) != 0u ? 0 : 1;
        result.parameter.status = QueuedInstructionParamStatus::Validated;
        result.parameter.provenance =
            "EnemyTargetSelectController_800794D8 commits attack instrParam=0 when movement_flags&0x40 is set, otherwise 1";
        return result;
    }
    case QueuedInstructionCommandKind::Magic:
        return id_command(
            input, 1, QueuedInstructionParamKind::AbilityId,
            "LaunchTargetSelectorForAction_8007B958 publishes the selected magic ID through FUN_8007B328");
    case QueuedInstructionCommandKind::SMove:
        return id_command(
            input, 2, QueuedInstructionParamKind::AbilityId,
            "LaunchTargetSelectorForAction_8007B958 publishes the selected S-Move ID through FUN_8007B328");
    case QueuedInstructionCommandKind::Crew:
        return id_command(
            input, 8, QueuedInstructionParamKind::AbilityId,
            "LaunchTargetSelectorForAction_8007B958 publishes the selected crew-command ID through FUN_8007B328");
    case QueuedInstructionCommandKind::Item:
        return id_command(
            input, 5, QueuedInstructionParamKind::ItemId,
            "the item acceptance path publishes the selected item ID through FUN_8007B328");
    case QueuedInstructionCommandKind::Guard:
        return fixed_command(
            4, -1, QueuedInstructionParamKind::Unset,
            QueuedInstructionParamStage::CommandSelected,
            "DispatchAcceptedCommand_8007C600 publishes guard with target=self and instrParam=-1");
    case QueuedInstructionCommandKind::Focus:
        return fixed_command(
            0, -1, QueuedInstructionParamKind::Unset,
            QueuedInstructionParamStage::CommandSelected,
            "DispatchAcceptedCommand_8007C600 publishes focus with target=self and instrParam=-1");
    case QueuedInstructionCommandKind::Unknown:
        break;
    }

    QueuedInstructionCommandResult result;
    result.parameter.status = QueuedInstructionParamStatus::Unsupported;
    result.parameter.provenance = "queued command kind is not modeled";
    return result;
}

QueuedInstructionEvidenceResult reconcile_queued_instruction_macro_evidence(
    const QueuedInstructionEvidenceInput& input) {
    QueuedInstructionEvidenceResult result;
    result.macro_observed = !input.macro_observed_post_write_values.empty();
    result.macro_trusted = false;

    if (!input.reliable_pre_macro_value.has_value()) {
        result.status = QueuedInstructionParamStatus::MissingInput;
        result.provenance = "reliable pre-macro queued-row snapshot is missing";
        return result;
    }
    if (!input.reliable_post_macro_value.has_value()) {
        result.status = QueuedInstructionParamStatus::MissingInput;
        result.provenance =
            "reliable post-macro accepted-command snapshot is missing; InputMacro watchpoints cannot substitute for it";
        return result;
    }

    result.accepted_value = input.reliable_post_macro_value;
    result.macro_agrees_with_post_snapshot = result.macro_observed
        && input.macro_observed_post_write_values.back()
            == *input.reliable_post_macro_value;

    if (!result.macro_observed) {
        result.status = QueuedInstructionParamStatus::Validated;
        result.provenance =
            "accepted reliable post-macro snapshot; no macro-time write was observed and none is required";
    } else if (result.macro_agrees_with_post_snapshot) {
        result.status = QueuedInstructionParamStatus::Validated;
        result.provenance = input.macro_capture_complete
            ? "accepted reliable post-macro snapshot; complete macro diagnostics agree"
            : "accepted reliable post-macro snapshot; partial macro diagnostics agree";
    } else {
        result.status = QueuedInstructionParamStatus::Provisional;
        result.provenance =
            "accepted reliable post-macro snapshot; macro_untrusted observations disagree and are diagnostic only";
    }
    return result;
}

BasicAttackExecutionRoute basic_attack_route_from_final_parameter(
    std::optional<std::int16_t> final_parameter) {
    if (!final_parameter.has_value()) {
        return BasicAttackExecutionRoute::Unknown;
    }
    return *final_parameter == 0
        ? BasicAttackExecutionRoute::DirectMelee
        : BasicAttackExecutionRoute::FallbackRanged;
}

std::optional<std::int16_t> provisional_instruction_mode_for_route(
    BasicAttackExecutionRoute route) {
    switch (route) {
    case BasicAttackExecutionRoute::DirectMelee:
        return 4;
    case BasicAttackExecutionRoute::FallbackRanged:
        return 5;
    case BasicAttackExecutionRoute::Unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

BasicAttackQueuedStateResult model_basic_attack_queued_state(
    const BasicAttackQueuedStateInput& input) {
    BasicAttackQueuedStateResult result;
    result.confidence = "high";

    if (input.counter_follow_up) {
        result.status = QueuedInstructionParamStatus::Unsupported;
        result.provenance = "counter follow-up queued-state production remains separately modeled";
        return result;
    }
    if (input.route == BasicAttackExecutionRoute::Unknown) {
        result.status = QueuedInstructionParamStatus::MissingInput;
        result.provenance = "final basic-attack execution route is unknown";
        return result;
    }
    if (!input.attack_result.has_value()) {
        result.status = QueuedInstructionParamStatus::MissingInput;
        result.provenance = "attack result is required to publish queued state 5, 6, or 7";
        return result;
    }
    if (*input.attack_result < 0 || *input.attack_result > 2) {
        result.status = QueuedInstructionParamStatus::Inconsistent;
        result.provenance = "attack result is outside the validated 0=miss, 1=hit, 2=critical domain";
        return result;
    }

    if (input.route == BasicAttackExecutionRoute::FallbackRanged) {
        if (*input.attack_result == 2) {
            result.status = QueuedInstructionParamStatus::Inconsistent;
            result.provenance =
                "fallback/ranged critical is not supported by the captured queued-state producer chain";
            return result;
        }
        result.status = QueuedInstructionParamStatus::Validated;
        result.queued_state = QueuedStdActionState::FallbackRanged7;
        result.instruction_mode = 5;
        result.provenance =
            "final fallback route publishes queued state 7; MapQueuedStateToStdActionId_800217D0 maps 7 to mode 5";
        return result;
    }

    result.status = QueuedInstructionParamStatus::Validated;
    if (*input.attack_result == 2) {
        result.queued_state = QueuedStdActionState::DirectCritical6;
        result.instruction_mode = 8;
        result.provenance =
            "final direct route plus critical result 2 publishes queued state 6; MapQueuedStateToStdActionId_800217D0 maps 6 to mode 8";
    } else {
        result.queued_state = QueuedStdActionState::DirectNoncritical5;
        result.instruction_mode = 4;
        result.provenance =
            "final direct route plus miss/hit result publishes queued state 5; MapQueuedStateToStdActionId_800217D0 maps 5 to mode 4";
    }
    return result;
}

const char* queued_instruction_param_kind_name(QueuedInstructionParamKind kind) {
    switch (kind) {
    case QueuedInstructionParamKind::Unset: return "Unset";
    case QueuedInstructionParamKind::BasicAttackRoute: return "BasicAttackRoute";
    case QueuedInstructionParamKind::AbilityId: return "AbilityId";
    case QueuedInstructionParamKind::ItemId: return "ItemId";
    case QueuedInstructionParamKind::Unknown: return "Unknown";
    }
    return "Unknown";
}

const char* queued_instruction_param_stage_name(QueuedInstructionParamStage stage) {
    switch (stage) {
    case QueuedInstructionParamStage::Initialized: return "Initialized";
    case QueuedInstructionParamStage::CommandSelected: return "CommandSelected";
    case QueuedInstructionParamStage::ExecutionResolved: return "ExecutionResolved";
    }
    return "Initialized";
}

const char* queued_instruction_param_status_name(QueuedInstructionParamStatus status) {
    switch (status) {
    case QueuedInstructionParamStatus::Validated: return "Validated";
    case QueuedInstructionParamStatus::Provisional: return "Provisional";
    case QueuedInstructionParamStatus::MissingInput: return "MissingInput";
    case QueuedInstructionParamStatus::Unsupported: return "Unsupported";
    case QueuedInstructionParamStatus::Inconsistent: return "Inconsistent";
    }
    return "Unsupported";
}

const char* queued_instruction_command_kind_name(QueuedInstructionCommandKind command) {
    switch (command) {
    case QueuedInstructionCommandKind::Initialize: return "Initialize";
    case QueuedInstructionCommandKind::Attack: return "Attack";
    case QueuedInstructionCommandKind::Magic: return "Magic";
    case QueuedInstructionCommandKind::SMove: return "SMove";
    case QueuedInstructionCommandKind::Crew: return "Crew";
    case QueuedInstructionCommandKind::Item: return "Item";
    case QueuedInstructionCommandKind::Guard: return "Guard";
    case QueuedInstructionCommandKind::Focus: return "Focus";
    case QueuedInstructionCommandKind::Reset: return "Reset";
    case QueuedInstructionCommandKind::Unknown: return "Unknown";
    }
    return "Unknown";
}

const char* basic_attack_execution_route_name(BasicAttackExecutionRoute route) {
    switch (route) {
    case BasicAttackExecutionRoute::DirectMelee: return "DirectMelee";
    case BasicAttackExecutionRoute::FallbackRanged: return "FallbackRanged";
    case BasicAttackExecutionRoute::Unknown: return "Unknown";
    }
    return "Unknown";
}

const char* queued_std_action_state_name(QueuedStdActionState state) {
    switch (state) {
    case QueuedStdActionState::DirectNoncritical5: return "DirectNoncritical5";
    case QueuedStdActionState::DirectCritical6: return "DirectCritical6";
    case QueuedStdActionState::FallbackRanged7: return "FallbackRanged7";
    }
    return "Unknown";
}

} // namespace savor::predict
