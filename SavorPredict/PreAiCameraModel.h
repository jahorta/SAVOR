#pragma once

namespace savor::predict {

struct PreAiCameraDrawModel {
    int fake_attacks = 0;
    int fake_attack_draws = 0;
    int baseline_camera_draws = 0;
    int expected_camera_draws = 0;
    int suppressed_attack_targeting_camera_draws = 0;
    int unsuppressed_total_draws = 0;
    int expected_total_draws = 0;
    bool suppresses_normal_attack_targeting_camera = false;
};

PreAiCameraDrawModel model_pre_ai_camera_draws(int fake_attacks_this_turn);
const char* pre_ai_camera_rule_name(const PreAiCameraDrawModel& model);
const char* pre_ai_camera_rule_detail(const PreAiCameraDrawModel& model);

} // namespace savor::predict
