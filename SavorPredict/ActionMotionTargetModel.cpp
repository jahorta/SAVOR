#include "ActionMotionTargetModel.h"

namespace savor::predict {

ActionMotionTargetResult select_action_motion_target(
    const ActionMotionTargetInput& input) {
    ActionMotionTargetResult result;
    result.provenance = "FUN_8001EE24 action-motion target selection";

    if (input.actor_slot < 0) {
        result.status = ActionMotionTargetStatus::MissingInput;
        result.confidence = "missing actor slot";
        return result;
    }

    if (input.turn_phase == std::optional<std::uint8_t>{8}) {
        result.status = ActionMotionTargetStatus::Unsupported;
        result.confidence =
            "turn-phase-8 target projection requires the unmodeled vector-normal branch";
        return result;
    }

    if (input.action_mode == 0x06 || input.action_mode == 0x13) {
        if (!input.own_pos_holder.has_value()) {
            result.status = ActionMotionTargetStatus::MissingInput;
            result.confidence = "validated mode requires the actor posHolder";
            return result;
        }
        result.status = ActionMotionTargetStatus::Exact;
        result.source = ActionMotionTargetSource::OwnPosHolder;
        result.target = input.own_pos_holder;
        result.confidence = "validated live and static mode-6/mode-0x13 own-posHolder path";
        return result;
    }

    if (input.action_mode == 0x04
        || input.action_mode == 0x05
        || input.action_mode == 0x08) {
        if (input.target_slot < 0) {
            result.status = ActionMotionTargetStatus::MissingInput;
            result.confidence =
                "target-combatant mode requires an instruction target slot";
            return result;
        }
        if (input.target_current_position.has_value()) {
            result.status = ActionMotionTargetStatus::Exact;
            result.source =
                ActionMotionTargetSource::TargetCombatantCurrentPosition;
            result.target = input.target_current_position;
            result.confidence =
                "validated FUN_8001EE24 mode-4/mode-5/mode-8 target-combatant branch";
            return result;
        }
        if (input.slot_zero_current_position.has_value()) {
            result.status = ActionMotionTargetStatus::Exact;
            result.source =
                ActionMotionTargetSource::SlotZeroCurrentPositionFallback;
            result.target = input.slot_zero_current_position;
            result.confidence =
                "validated FUN_8001EE24 missing-target fallback to combatant slot 0";
            return result;
        }
        result.status = ActionMotionTargetStatus::MissingInput;
        result.confidence =
            "target-combatant mode has neither its target nor the slot-0 fallback";
        return result;
    }

    if (input.action_mode == 0x09
        || input.action_mode == 0x0a
        || input.action_mode == 0x0b
        || input.action_mode == 0x0d
        || input.action_mode == 0x20) {
        if (input.secondary_target_current_position.has_value()) {
            result.status = ActionMotionTargetStatus::Exact;
            result.source =
                ActionMotionTargetSource::
                    SecondaryTargetCombatantCurrentPosition;
            result.target = input.secondary_target_current_position;
            result.confidence =
                "validated FUN_8001EE24 IW+0x48 target-combatant branch";
            return result;
        }
        if (input.slot_zero_current_position.has_value()) {
            result.status = ActionMotionTargetStatus::Exact;
            result.source =
                ActionMotionTargetSource::SlotZeroCurrentPositionFallback;
            result.target = input.slot_zero_current_position;
            result.confidence =
                "validated FUN_8001EE24 missing-IW+0x48-target fallback to combatant slot 0";
            return result;
        }
        result.status = ActionMotionTargetStatus::MissingInput;
        result.confidence =
            "IW+0x48 target-combatant mode has neither its secondary target nor the slot-0 fallback";
        return result;
    }

    if (input.provisional_fallback.has_value()) {
        result.status = ActionMotionTargetStatus::Provisional;
        result.source = ActionMotionTargetSource::ProvisionalFallback;
        result.target = input.provisional_fallback;
        result.confidence = "unsupported action mode retained prior predictor target";
        return result;
    }

    result.status = ActionMotionTargetStatus::Unsupported;
    result.confidence = "action mode target branch is not modeled";
    return result;
}

const char* action_motion_target_status_name(ActionMotionTargetStatus status) {
    switch (status) {
    case ActionMotionTargetStatus::Exact:
        return "Exact";
    case ActionMotionTargetStatus::Provisional:
        return "Provisional";
    case ActionMotionTargetStatus::MissingInput:
        return "MissingInput";
    case ActionMotionTargetStatus::Unsupported:
        return "Unsupported";
    }
    return "Unknown";
}

const char* action_motion_target_source_name(ActionMotionTargetSource source) {
    switch (source) {
    case ActionMotionTargetSource::OwnPosHolder:
        return "OwnPosHolder";
    case ActionMotionTargetSource::TargetCombatantCurrentPosition:
        return "TargetCombatantCurrentPosition";
    case ActionMotionTargetSource::SecondaryTargetCombatantCurrentPosition:
        return "SecondaryTargetCombatantCurrentPosition";
    case ActionMotionTargetSource::SlotZeroCurrentPositionFallback:
        return "SlotZeroCurrentPositionFallback";
    case ActionMotionTargetSource::ProvisionalFallback:
        return "ProvisionalFallback";
    case ActionMotionTargetSource::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

} // namespace savor::predict
