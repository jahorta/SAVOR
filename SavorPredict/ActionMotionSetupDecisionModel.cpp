#include "ActionMotionSetupDecisionModel.h"

#include "BattleFrameStateModel.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>

namespace savor::predict {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kAngleShortUnitsPerRadian = 10430.37890625f;

std::uint32_t heading_short_80071c74(
    const BattleFrameVec3& from,
    const BattleFrameVec3& to,
    std::uint32_t fallback) {
    const float x = to.x - from.x;
    const float z = to.z - from.z;
    if (x == 0.0f && z == 0.0f) {
        return fallback & 0xffffu;
    }
    const float radians = static_cast<float>(
        std::atan2(static_cast<double>(x), static_cast<double>(z)));
    const auto angle = static_cast<std::int32_t>(
        radians * kAngleShortUnitsPerRadian);
    return static_cast<std::uint32_t>(angle) & 0xffffu;
}

float signed_angle_delta_degrees(
    std::uint32_t desired,
    std::uint32_t current) {
    const auto delta = static_cast<std::int16_t>(
        static_cast<std::uint16_t>(desired - current));
    return static_cast<float>(delta) * 360.0f / 65536.0f;
}

BattleFrameVec3 move_increment_80071bcc(
    float speed,
    std::uint32_t heading) {
    const float radians =
        static_cast<float>(static_cast<std::uint16_t>(heading))
        * (2.0f * kPi / 65536.0f);
    return BattleFrameVec3{
        .x = speed * std::sin(radians),
        .y = 0.0f,
        .z = speed * std::cos(radians),
    };
}

bool special_flag_suppression_mode(std::int16_t mode) {
    return mode == 0x0b || mode == 0x0c || mode == 0x0d || mode == 0x20;
}

bool special_direct_facing_mode(std::int16_t mode) {
    return (mode >= 0x09 && mode <= 0x0d) || mode == 0x20;
}

ActionMotionSetupResult false_result(
    ActionMotionSetupResult result,
    ActionMotionSetupBranch branch,
    std::string provenance) {
    result.status = ActionMotionSetupStatus::Matched;
    result.branch = branch;
    result.route = ActionMotionSetupRoute::CurrentModeResolver;
    result.operations.push_back(ActionMotionSetupOperation{
        .stage = ActionMotionSetupOperationStage::BeforeResolver,
        .kind = ActionMotionSetupOperationKind::ClearInstructionFlagsF0,
        .flags_mask = 0x00000400u,
    });
    result.confidence = "validated static branch and complete live decision corpus";
    result.provenance = std::move(provenance);
    return result;
}

} // namespace

ActionMotionSetupResult model_action_motion_setup_8001fabc(
    const ActionMotionSetupInput& input) {
    ActionMotionSetupResult result;

    if (input.actor_slot < 0 || input.instruction_mode < 0
        || !input.selected_row.has_value()
        || !input.current_position.has_value()
        || !input.current_facing_angle.has_value()
        || !input.turn_speed_degrees.has_value()) {
        result.status = ActionMotionSetupStatus::MissingInput;
        result.provenance =
            "FUN_8001B1B0/FUN_8001FABC requires actor, selected row, "
            "current position, facing, and turn speed";
        return result;
    }
    if (!std::isfinite(*input.turn_speed_degrees)
        || *input.turn_speed_degrees < 0.0f) {
        result.status = ActionMotionSetupStatus::Unsupported;
        result.provenance =
            "FUN_8001FABC received a non-finite or negative turn speed";
        return result;
    }

    const bool readiness_bypass =
        (input.instruction_flags_0xf0 & 0x00000800u) != 0
        || (input.combatant_status_flag_0x400_set
            && (input.instruction_flags_0xec & 0x00000008u) != 0);
    if (readiness_bypass) {
        result.status = ActionMotionSetupStatus::Matched;
        result.branch = ActionMotionSetupBranch::State8Bypass;
        result.route = ActionMotionSetupRoute::State8Bypass;
        result.confidence = "validated FUN_8001B1B0 state-3 readiness predicate";
        result.provenance =
            "IW+0xF0 bit 0x800 or status 0x400 with IW+0xEC bit 3 "
            "bypassed FUN_8001FABC and selected callback state 8";
        return result;
    }

    result.target = select_action_motion_target(ActionMotionTargetInput{
        .actor_slot = input.actor_slot,
        .target_slot = input.target_slot,
        .action_mode = input.instruction_mode,
        .turn_phase = input.turn_phase,
        .own_pos_holder = input.own_pos_holder,
        .target_current_position = input.target_current_position,
        .secondary_target_current_position =
            input.secondary_target_current_position,
        .slot_zero_current_position = input.slot_zero_current_position,
    });
    if (!result.target.target.has_value()) {
        result.status =
            result.target.status == ActionMotionTargetStatus::Unsupported
            ? ActionMotionSetupStatus::Unsupported
            : ActionMotionSetupStatus::MissingInput;
        result.branch = ActionMotionSetupBranch::TargetUnavailable;
        result.provenance =
            "FUN_8001EE24 could not construct the action-motion target: "
            + result.target.confidence;
        return result;
    }

    result.operations.push_back(ActionMotionSetupOperation{
        .stage = ActionMotionSetupOperationStage::Setup,
        .kind = ActionMotionSetupOperationKind::PublishTarget,
        .vector = *result.target.target,
    });

    const auto row_mode = input.selected_row->action_id;
    const auto row_flags = input.selected_row->flags;
    if ((row_flags & 0x10000000u) != 0) {
        return false_result(
            std::move(result),
            ActionMotionSetupBranch::SelectedRowSuppression,
            "FUN_8001FABC published IW+0x110, then selected-row flag "
            "0x10000000 suppressed rotation setup");
    }
    if ((input.instruction_flags_0xec & 0x00800000u) != 0) {
        return false_result(
            std::move(result),
            ActionMotionSetupBranch::InstructionSuppression,
            "FUN_8001FABC published IW+0x110, then IW+0xEC bit "
            "0x00800000 suppressed rotation setup");
    }

    const auto& current = *input.current_position;
    const auto& target = *result.target.target;
    if (current.x == target.x && current.z == target.z) {
        return false_result(
            std::move(result),
            ActionMotionSetupBranch::SamePosition,
            "FUN_8001FABC target X/Z already matched the combatant position");
    }

    const auto current_facing = *input.current_facing_angle & 0xffffu;
    const auto movement_heading =
        heading_short_80071c74(current, target, current_facing);
    auto desired_facing = row_mode == 0x13
        ? heading_short_80071c74(target, current, current_facing)
        : movement_heading;

    if ((row_flags & 0x01000000u) != 0
        || (input.instruction_flags_0xf0 & 0x20000000u) != 0) {
        const auto speed = input.instruction_mode == 0x13
            ? input.motion_alt_speed
            : input.motion_base_speed;
        if (!speed.has_value() || !std::isfinite(*speed)
            || *speed < 0.0f) {
            result.status = ActionMotionSetupStatus::MissingInput;
            result.provenance =
                "FUN_8001FABC increment publication requires the selected "
                "base or mode-0x13 alternate motion speed";
            return result;
        }
        result.selected_motion_speed = speed;
        result.operations.push_back(ActionMotionSetupOperation{
            .stage = ActionMotionSetupOperationStage::Setup,
            .kind = ActionMotionSetupOperationKind::PublishMoveIncrement,
            .vector = move_increment_80071bcc(*speed, movement_heading),
        });
    }

    if ((row_flags & 0x00010000u) != 0) {
        const BattleFrameVec3 world_origin{};
        desired_facing =
            heading_short_80071c74(current, world_origin, current_facing);
    }
    result.desired_facing_angle = desired_facing;

    if (special_flag_suppression_mode(row_mode)
        && (input.instruction_flags_0xf0 & 0x40000000u) != 0) {
        result.operations.push_back(ActionMotionSetupOperation{
            .stage = ActionMotionSetupOperationStage::Setup,
            .kind = ActionMotionSetupOperationKind::SetInstructionFlagsF0,
            .flags_mask = 0x00000200u,
        });
        return false_result(
            std::move(result),
            ActionMotionSetupBranch::SpecialModeFlagSuppression,
            "FUN_8001FABC special selected-row mode and IW+0xF0 bit "
            "0x40000000 suppressed rotation and set bit 0x200");
    }

    const float absolute_delta = std::fabs(
        signed_angle_delta_degrees(desired_facing, current_facing));
    if (absolute_delta < *input.turn_speed_degrees) {
        result.operations.push_back(ActionMotionSetupOperation{
            .stage = ActionMotionSetupOperationStage::Setup,
            .kind = ActionMotionSetupOperationKind::PublishFacingAngle,
            .facing_angle = desired_facing,
        });
        result.operations.push_back(ActionMotionSetupOperation{
            .stage = ActionMotionSetupOperationStage::Setup,
            .kind = ActionMotionSetupOperationKind::SetInstructionFlagsF0,
            .flags_mask = 0x00000200u,
        });
        return false_result(
            std::move(result),
            ActionMotionSetupBranch::BelowRotationThreshold,
            "FUN_8001FABC snapped the facing angle because the signed "
            "short-angle delta was below IW+0x128");
    }

    float turn_current =
        battle_frame_angle_short_to_degrees_8006116c(current_facing);
    float turn_target =
        battle_frame_angle_short_to_degrees_8006116c(desired_facing);
    if (special_direct_facing_mode(row_mode)) {
        result.operations.push_back(ActionMotionSetupOperation{
            .stage = ActionMotionSetupOperationStage::Setup,
            .kind = ActionMotionSetupOperationKind::PublishTurnState,
            .turn_current_degrees = turn_current,
            .turn_target_degrees = turn_target,
            .turn_step_degrees = 0.0f,
        });
        result.operations.push_back(ActionMotionSetupOperation{
            .stage = ActionMotionSetupOperationStage::Setup,
            .kind = ActionMotionSetupOperationKind::PublishFacingAngle,
            .facing_angle = desired_facing,
        });
        result.operations.push_back(ActionMotionSetupOperation{
            .stage = ActionMotionSetupOperationStage::Setup,
            .kind = ActionMotionSetupOperationKind::SetInstructionFlagsF0,
            .flags_mask = 0x00000200u,
        });
        return false_result(
            std::move(result),
            ActionMotionSetupBranch::SpecialModeDirectFacing,
            "FUN_8001FABC special selected-row mode published zero turn "
            "increment, snapped facing, and set IW+0xF0 bit 0x200");
    }

    normalize_turn_shortest_path_80061080(turn_current, turn_target);
    const float turn_step = turn_target - turn_current >= 0.0f
        ? *input.turn_speed_degrees
        : -*input.turn_speed_degrees;
    result.operations.push_back(ActionMotionSetupOperation{
        .stage = ActionMotionSetupOperationStage::Setup,
        .kind = ActionMotionSetupOperationKind::PublishTurnState,
        .turn_current_degrees = turn_current,
        .turn_target_degrees = turn_target,
        .turn_step_degrees = turn_step,
    });
    result.operations.push_back(ActionMotionSetupOperation{
        .stage = ActionMotionSetupOperationStage::AfterResolver,
        .kind = ActionMotionSetupOperationKind::SetInstructionFlagsF0,
        .flags_mask = 0x00000400u,
    });
    result.operations.push_back(ActionMotionSetupOperation{
        .stage = ActionMotionSetupOperationStage::AfterResolver,
        .kind = ActionMotionSetupOperationKind::ClearInstructionFlagsF0,
        .flags_mask = 0x00000200u,
    });
    result.status = ActionMotionSetupStatus::Matched;
    result.branch = ActionMotionSetupBranch::InstallRotationPlayback;
    result.route = ActionMotionSetupRoute::Mode3Resolver;
    result.confidence =
        "validated 141-result live corpus and exact FUN_8001FABC branch order";
    result.provenance =
        "FUN_8001FABC published turn fields and returned true; "
        "FUN_8001B1B0 must resolve mode 3 before applying its routing flags";
    return result;
}

const char* action_motion_setup_status_name(ActionMotionSetupStatus status) {
    switch (status) {
    case ActionMotionSetupStatus::Matched: return "Matched";
    case ActionMotionSetupStatus::Provisional: return "Provisional";
    case ActionMotionSetupStatus::MissingInput: return "MissingInput";
    case ActionMotionSetupStatus::Unsupported: return "Unsupported";
    }
    return "Unsupported";
}

const char* action_motion_setup_branch_name(ActionMotionSetupBranch branch) {
    switch (branch) {
    case ActionMotionSetupBranch::Unknown: return "Unknown";
    case ActionMotionSetupBranch::State8Bypass: return "State8Bypass";
    case ActionMotionSetupBranch::TargetUnavailable: return "TargetUnavailable";
    case ActionMotionSetupBranch::SelectedRowSuppression:
        return "SelectedRowSuppression";
    case ActionMotionSetupBranch::InstructionSuppression:
        return "InstructionSuppression";
    case ActionMotionSetupBranch::ReadinessSuppression:
        return "ReadinessSuppression";
    case ActionMotionSetupBranch::SamePosition: return "SamePosition";
    case ActionMotionSetupBranch::SpecialModeFlagSuppression:
        return "SpecialModeFlagSuppression";
    case ActionMotionSetupBranch::BelowRotationThreshold:
        return "BelowRotationThreshold";
    case ActionMotionSetupBranch::SpecialModeDirectFacing:
        return "SpecialModeDirectFacing";
    case ActionMotionSetupBranch::InstallRotationPlayback:
        return "InstallRotationPlayback";
    }
    return "Unknown";
}

const char* action_motion_setup_operation_stage_name(
    ActionMotionSetupOperationStage stage) {
    switch (stage) {
    case ActionMotionSetupOperationStage::Setup: return "Setup";
    case ActionMotionSetupOperationStage::BeforeResolver:
        return "BeforeResolver";
    case ActionMotionSetupOperationStage::AfterResolver:
        return "AfterResolver";
    }
    return "Setup";
}

const char* action_motion_setup_operation_kind_name(
    ActionMotionSetupOperationKind kind) {
    switch (kind) {
    case ActionMotionSetupOperationKind::PublishTarget: return "PublishTarget";
    case ActionMotionSetupOperationKind::PublishMoveIncrement:
        return "PublishMoveIncrement";
    case ActionMotionSetupOperationKind::PublishFacingAngle:
        return "PublishFacingAngle";
    case ActionMotionSetupOperationKind::PublishTurnState:
        return "PublishTurnState";
    case ActionMotionSetupOperationKind::SetInstructionFlagsF0:
        return "SetInstructionFlagsF0";
    case ActionMotionSetupOperationKind::ClearInstructionFlagsF0:
        return "ClearInstructionFlagsF0";
    }
    return "PublishTarget";
}

} // namespace savor::predict
