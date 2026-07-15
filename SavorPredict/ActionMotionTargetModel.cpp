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
    case ActionMotionTargetSource::ProvisionalFallback:
        return "ProvisionalFallback";
    case ActionMotionTargetSource::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

} // namespace savor::predict
