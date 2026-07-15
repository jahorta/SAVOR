#pragma once

#include "BattleFrameStateModel.h"

#include <cstdint>
#include <optional>
#include <string>

namespace savor::predict {

enum class ActionMotionTargetStatus {
    Exact,
    Provisional,
    MissingInput,
    Unsupported,
};

enum class ActionMotionTargetSource {
    OwnPosHolder,
    ProvisionalFallback,
    Unknown,
};

struct ActionMotionTargetInput {
    int actor_slot = -1;
    std::int16_t action_mode = 0;
    std::optional<BattleFrameVec3> own_pos_holder;
    std::optional<BattleFrameVec3> provisional_fallback;
};

struct ActionMotionTargetResult {
    ActionMotionTargetStatus status = ActionMotionTargetStatus::MissingInput;
    ActionMotionTargetSource source = ActionMotionTargetSource::Unknown;
    std::optional<BattleFrameVec3> target;
    std::string confidence;
    std::string provenance;
};

ActionMotionTargetResult select_action_motion_target(
    const ActionMotionTargetInput& input);

const char* action_motion_target_status_name(ActionMotionTargetStatus status);
const char* action_motion_target_source_name(ActionMotionTargetSource source);

} // namespace savor::predict
