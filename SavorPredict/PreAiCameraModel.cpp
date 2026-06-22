#include "PreAiCameraModel.h"

#include <algorithm>

namespace savor::predict {

PreAiCameraDrawModel model_pre_ai_camera_draws(int fake_attacks_this_turn, int pc_count) {
    PreAiCameraDrawModel model;
    model.fake_attacks = std::max(0, fake_attacks_this_turn);
    model.pc_count = std::max(0, pc_count);
    model.fake_attack_draws = model.fake_attacks;

    // V1 macro contract: one camera draw before command input, one target
    // camera draw per PC, and one RNG draw per fake attack.
    model.baseline_camera_draws = 1 + model.pc_count;
    model.suppresses_normal_attack_targeting_camera = false;
    model.suppressed_attack_targeting_camera_draws = 0;
    model.expected_camera_draws = model.baseline_camera_draws;
    model.unsuppressed_total_draws = model.fake_attack_draws + model.baseline_camera_draws;
    model.expected_total_draws = model.fake_attack_draws + model.expected_camera_draws;
    return model;
}

PreAiCameraDrawModel model_pre_ai_camera_draws(int fake_attacks_this_turn) {
    return model_pre_ai_camera_draws(fake_attacks_this_turn, 2);
}

const char* pre_ai_camera_rule_name(const PreAiCameraDrawModel& model) {
    if (model.fake_attacks > 0) {
        return "FakeAttacksPlusFixedCameraDraws";
    }
    return "FixedCameraDraws";
}

const char* pre_ai_camera_rule_detail(const PreAiCameraDrawModel& model) {
    (void)model;
    return "v1 macro contract is fake_attack_count fake draws plus fixed camera draws: one battle-start camera draw and one targeting-camera draw per PC";
}

} // namespace savor::predict
