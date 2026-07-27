#pragma once

#include "ActionMotionInvocationModel.h"
#include "ActionMotionTargetModel.h"
#include "CombatantVisualDispatcherModel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

enum class ActionMotionSetupStatus {
    Matched,
    Provisional,
    MissingInput,
    Unsupported,
};

enum class ActionMotionSetupBranch {
    Unknown,
    State8Bypass,
    TargetUnavailable,
    SelectedRowSuppression,
    InstructionSuppression,
    ReadinessSuppression,
    SamePosition,
    SpecialModeFlagSuppression,
    BelowRotationThreshold,
    SpecialModeDirectFacing,
    InstallRotationPlayback,
};

enum class ActionMotionSetupOperationStage {
    Setup,
    BeforeResolver,
    AfterResolver,
};

enum class ActionMotionSetupOperationKind {
    PublishTarget,
    PublishMoveIncrement,
    PublishFacingAngle,
    PublishTurnState,
    SetInstructionFlagsF0,
    ClearInstructionFlagsF0,
};

struct ActionMotionSetupOperation {
    ActionMotionSetupOperationStage stage =
        ActionMotionSetupOperationStage::Setup;
    ActionMotionSetupOperationKind kind =
        ActionMotionSetupOperationKind::PublishTarget;
    BattleFrameVec3 vector{};
    std::uint32_t facing_angle = 0;
    float turn_current_degrees = 0.0f;
    float turn_target_degrees = 0.0f;
    float turn_step_degrees = 0.0f;
    std::uint32_t flags_mask = 0;
};

struct ActionMotionSetupInput {
    int actor_slot = -1;
    int target_slot = -1;
    std::int16_t instruction_mode = -1;
    std::optional<std::uint8_t> turn_phase;
    std::optional<CombatantStdActionRow> selected_row;
    std::uint32_t instruction_flags_0xec = 0;
    std::uint32_t instruction_flags_0xf0 = 0;
    bool combatant_status_flag_0x400_set = false;
    std::optional<BattleFrameVec3> current_position;
    std::optional<BattleFrameVec3> own_pos_holder;
    std::optional<BattleFrameVec3> target_current_position;
    std::optional<BattleFrameVec3> secondary_target_current_position;
    std::optional<BattleFrameVec3> slot_zero_current_position;
    std::optional<std::uint32_t> current_facing_angle;
    std::optional<float> turn_speed_degrees;
    std::optional<float> motion_base_speed;
    std::optional<float> motion_alt_speed;
};

struct ActionMotionSetupResult {
    ActionMotionSetupStatus status = ActionMotionSetupStatus::MissingInput;
    ActionMotionSetupBranch branch = ActionMotionSetupBranch::Unknown;
    ActionMotionSetupRoute route = ActionMotionSetupRoute::Unknown;
    ActionMotionTargetResult target{};
    std::vector<ActionMotionSetupOperation> operations;
    std::optional<std::uint32_t> desired_facing_angle;
    std::optional<float> selected_motion_speed;
    std::string confidence;
    std::string provenance;
};

ActionMotionSetupResult model_action_motion_setup_8001fabc(
    const ActionMotionSetupInput& input);

const char* action_motion_setup_status_name(ActionMotionSetupStatus status);
const char* action_motion_setup_branch_name(ActionMotionSetupBranch branch);
const char* action_motion_setup_operation_stage_name(
    ActionMotionSetupOperationStage stage);
const char* action_motion_setup_operation_kind_name(
    ActionMotionSetupOperationKind kind);

} // namespace savor::predict
