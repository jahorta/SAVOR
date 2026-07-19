#include "BattleTargetReactionStateModel.h"

#include <algorithm>

namespace savor::predict {
namespace {

bool qualifying_result_short(std::int16_t value) {
    return value == 5 || value == 6 || value == 0x44;
}

} // namespace

BattleTargetReactionResult model_battle_target_reaction(
    const BattleTargetReactionInput& input) {
    BattleTargetReactionResult result;
    result.origin_slot = input.origin_slot;
    result.target_slot = input.target_slot;

    if (input.action_kind != BattleTargetReactionActionKind::BasicAttack) {
        result.status = BattleTargetReactionStatus::Unsupported;
        result.provenance =
            "FUN_8002ECA4 result-5 semantics are only validated for basic attacks";
        return result;
    }
    if (input.origin_slot < 0 || input.target_slot < 0
        || !input.hit_check.has_value()
        || !input.pending_damage.has_value()
        || !input.target_hp_before_flush.has_value()
        || !input.target_dead.has_value()
        || !input.counter_accepted.has_value()) {
        result.status = BattleTargetReactionStatus::MissingInput;
        result.provenance =
            "post-counter target-reaction publication requires slots, hit_check, damage, pre-flush HP, death, and counter state";
        return result;
    }
    if (*input.pending_damage < 0 || *input.target_hp_before_flush < 0) {
        result.status = BattleTargetReactionStatus::Unsupported;
        result.provenance =
            "negative damage or pre-flush HP is outside the validated basic-attack contract";
        return result;
    }

    result.status = BattleTargetReactionStatus::Matched;
    result.queued_result = *input.target_dead ? 5 : *input.hit_check;
    result.queued_counter_byte = *input.counter_accepted ? 1U : 0U;
    result.pending_damage = *input.pending_damage;
    result.target_hp_before_flush = *input.target_hp_before_flush;
    result.result_shorts = input.result_shorts.value_or(
        std::array<std::int16_t, 3>{0, -1, -1});

    if (input.origin_slot == input.target_slot) {
        result.self_target_suppressed = true;
        result.provenance =
            "FUN_8002ECA4 suppresses reaction high bits when origin and target are the same slot";
        return result;
    }

    if (result.queued_result == 0) {
        result.reaction_flags_0x50 = 0x80000000U;
        result.selector_flags_0xf4 = 0x01000000U;
        result.provenance =
            "performAttack queued result 0; FUN_8002ECA4 published reaction bit 31 and selector F4 bit 24";
        return result;
    }

    const bool lethal_damage = result.queued_result == 5
        && result.target_hp_before_flush != 0
        && result.pending_damage >= result.target_hp_before_flush;
    const bool qualifying_short = result.queued_result == 5
        && std::any_of(
            result.result_shorts.begin(),
            result.result_shorts.end(),
            qualifying_result_short);
    if (result.queued_result == 5 && (lethal_damage || qualifying_short)) {
        result.reaction_flags_0x50 = 0x40000000U;
        result.selector_flags_0xf0 = 0x04000000U;
        result.provenance =
            "performAttack queued result 5; FUN_8002ECA4 published reaction bit 30 and selector F0 bit 26";
        return result;
    }
    if (*input.target_dead) {
        result.status = BattleTargetReactionStatus::Unsupported;
        result.provenance =
            "target_dead did not agree with the validated queued-result-5 lethal predicate";
        return result;
    }

    if (result.queued_counter_byte != 0) {
        result.reaction_flags_0x50 = 0x20000000U;
        result.selector_flags_0xf4 = 0x04000000U;
        result.provenance =
            "accepted counter queued byte was nonzero; FUN_8002ECA4 published reaction bit 29 and selector F4 bit 26";
        return result;
    }

    result.provenance =
        "ordinary basic-attack result produced no target reaction high bit";
    return result;
}

const char* battle_target_reaction_status_name(
    BattleTargetReactionStatus status) {
    switch (status) {
    case BattleTargetReactionStatus::Matched: return "Matched";
    case BattleTargetReactionStatus::Provisional: return "Provisional";
    case BattleTargetReactionStatus::MissingInput: return "MissingInput";
    case BattleTargetReactionStatus::Unsupported: return "Unsupported";
    }
    return "Unsupported";
}

} // namespace savor::predict
