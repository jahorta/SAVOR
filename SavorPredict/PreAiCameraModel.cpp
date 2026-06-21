#include "PreAiCameraModel.h"

#include <algorithm>

namespace savor::predict {

PreAiCameraDrawModel model_pre_ai_camera_draws(int fake_attacks_this_turn) {
    PreAiCameraDrawModel model;
    model.fake_attacks = std::max(0, fake_attacks_this_turn);
    model.fake_attack_draws = model.fake_attacks;

    // First-battle baseline: one camera draw before command input and one
    // normal attack-target zoom-out draw for each PC target selection.
    model.baseline_camera_draws = 3;
    model.suppresses_normal_attack_targeting_camera = model.fake_attacks > 0;
    model.suppressed_attack_targeting_camera_draws =
        model.suppresses_normal_attack_targeting_camera ? 1 : 0;
    model.expected_camera_draws =
        model.baseline_camera_draws - model.suppressed_attack_targeting_camera_draws;
    model.unsuppressed_total_draws = model.fake_attack_draws + model.baseline_camera_draws;
    model.expected_total_draws = model.fake_attack_draws + model.expected_camera_draws;
    return model;
}

const char* pre_ai_camera_rule_name(const PreAiCameraDrawModel& model) {
    if (model.suppresses_normal_attack_targeting_camera) {
        return "FakeAttackSuppressesOneTargetingCameraDraw";
    }
    return "BaselineThreeCameraDraws";
}

const char* pre_ai_camera_rule_detail(const PreAiCameraDrawModel& model) {
    if (model.suppresses_normal_attack_targeting_camera) {
        return "aggregate fit is fake_attacks + 2; staged hypothesis is fake_attacks + 3 baseline with one 800608dc targeting-camera draw suppressed";
    }
    return "baseline fit is three camera draws before Soldier AI: battle-start camera plus two PC attack-targeting zoom-out draws";
}

} // namespace savor::predict
