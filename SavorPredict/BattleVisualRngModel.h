#pragma once

#include "ActionViewSelectorModel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

enum class BattleVisualRngStepStatus {
    Exact,
    Provisional,
    MissingInput,
    Ambiguous,
    Unsupported,
};

struct ActionViewDispatchEvidence {
    std::optional<int> record_mode_0x22;
    std::optional<int> effective_mode_0x112;
    std::optional<int> saved_mode_0x110;
    std::optional<std::uint32_t> dispatch_pc;
    std::optional<std::uint32_t> rng_pc;
    std::string source_tag;
};

struct BattleVisualRngActionInput {
    std::string profile_name = "first-battle";
    int actor_slot = -1;
    int target_slot = -1;
    bool attack_landed = false;
    bool attack_was_critical = false;
    bool counter_follow_up = false;
    bool include_action_view_camera = true;
    bool include_effect_bursts = true;
    std::optional<ActionViewSelectorResult> action_view_selector;
    std::optional<ActionViewDispatchEvidence> action_view_dispatch_evidence;
};

struct BattleVisualRngStep {
    std::string phase = "action_visual_rng";
    std::string label;
    BattleVisualRngStepStatus status = BattleVisualRngStepStatus::Exact;
    int draws_consumed = 0;
    std::optional<int> effect_source_key;
    std::string detail;
};

struct BattleVisualRngModelResult {
    std::vector<BattleVisualRngStep> steps;
    int total_draws = 0;
    bool has_missing_input_steps = false;
    bool has_ambiguous_steps = false;
    bool has_unsupported_steps = false;
};

std::optional<int> first_battle_basic_attack_effect_source_key(
    int actor_slot,
    bool attack_was_critical,
    bool counter_follow_up = false);

BattleVisualRngModelResult model_first_battle_basic_attack_visual_rng(
    const BattleVisualRngActionInput& input);

const char* battle_visual_rng_step_status_name(BattleVisualRngStepStatus status);
const char* first_battle_basic_attack_visual_rng_rule_detail();

} // namespace savor::predict
