#include "CritGateCheckpointModel.h"

#include <cstdlib>
#include <utility>

namespace savor::predict {

namespace {

constexpr const char* kHitOwner = "attack_hit_dodge";
constexpr const char* kCritOwner = "attack_critical";

bool owner_is(const CheckpointEvent& event, const char* owner) {
    return event.known_rng_owner == owner;
}

std::optional<int> parse_field_int(const CheckpointEvent& event, const char* field_name) {
    const auto found = event.fields.find(field_name);
    if (found == event.fields.end()) {
        return std::nullopt;
    }

    char* end = nullptr;
    const long parsed = std::strtol(found->second.c_str(), &end, 0);
    if (end == found->second.c_str() || *end != '\0') {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

std::optional<int> parse_hit_success(const CheckpointEvent& event) {
    if (const auto value = parse_field_int(event, "hit_success"); value.has_value()) {
        return *value != 0 ? 1 : 0;
    }
    if (const auto value = parse_field_int(event, "hit_result"); value.has_value()) {
        return *value != 0 ? 1 : 0;
    }
    if (const auto value = parse_field_int(event, "attack_result"); value.has_value()) {
        return *value != 0 ? 1 : 0;
    }
    return std::nullopt;
}

CritGateCheckpointStatus classify_status(const CritGateCheckpointSummary& summary) {
    if (summary.observed_hit_draws == 0) {
        return CritGateCheckpointStatus::ObservedOnly;
    }
    if (summary.hit_draws_with_instr_param != summary.observed_hit_draws
        || summary.hit_draws_with_hit_success != summary.observed_hit_draws) {
        return CritGateCheckpointStatus::MissingLiveGateFields;
    }
    if (summary.observed_crit_draws != summary.live_gate_expected_crit_draws) {
        return CritGateCheckpointStatus::CritDrawCountMismatch;
    }
    return CritGateCheckpointStatus::MatchesLiveGate;
}

} // namespace

CritGateCheckpointSummary summarize_crit_gate_checkpoints(
    const std::vector<CheckpointEvent>& events) {
    CritGateCheckpointSummary summary;

    for (const auto& event : events) {
        if (owner_is(event, kCritOwner)) {
            ++summary.observed_crit_draws;
            if (!summary.first_crit_draw_index.has_value()) {
                summary.first_crit_draw_index = event.rng_draw_index_before;
            }
            continue;
        }

        if (!owner_is(event, kHitOwner)) {
            continue;
        }

        CritGateHitDraw draw;
        draw.draw_index = event.rng_draw_index_before;
        draw.active_slot = event.active_slot.has_value()
            ? event.active_slot
            : parse_field_int(event, "active_slot");
        draw.target_slot = event.target_slot.has_value()
            ? event.target_slot
            : parse_field_int(event, "target_slot");
        draw.instr_param_0x6 = parse_field_int(event, "instr_param_0x6");
        draw.hit_success = parse_hit_success(event);
        draw.attack_result = parse_field_int(event, "attack_result");
        draw.rand_value = parse_field_int(event, "rand_value");

        ++summary.observed_hit_draws;
        if (!summary.first_hit_draw_index.has_value()) {
            summary.first_hit_draw_index = event.rng_draw_index_before;
        }
        if (draw.instr_param_0x6.has_value()) {
            ++summary.hit_draws_with_instr_param;
        }
        if (draw.hit_success.has_value()) {
            ++summary.hit_draws_with_hit_success;
        }
        if (draw.instr_param_0x6.has_value()
            && *draw.instr_param_0x6 == 0
            && draw.hit_success.has_value()
            && *draw.hit_success != 0) {
            ++summary.live_gate_expected_crit_draws;
        }

        summary.hit_draws.push_back(std::move(draw));
    }

    if (summary.hit_draws_with_instr_param == summary.observed_hit_draws
        && summary.hit_draws_with_hit_success == summary.observed_hit_draws) {
        summary.expected_crit_draws_from_live_gate = summary.live_gate_expected_crit_draws;
    }
    summary.status = classify_status(summary);
    return summary;
}

const char* crit_gate_checkpoint_status_name(CritGateCheckpointStatus status) {
    switch (status) {
    case CritGateCheckpointStatus::ObservedOnly: return "ObservedOnly";
    case CritGateCheckpointStatus::MatchesLiveGate: return "MatchesLiveGate";
    case CritGateCheckpointStatus::MissingLiveGateFields: return "MissingLiveGateFields";
    case CritGateCheckpointStatus::CritDrawCountMismatch: return "CritDrawCountMismatch";
    default: return "Unknown";
    }
}

const char* crit_gate_checkpoint_rule_detail() {
    return "live first-battle hit checkpoints should expose instr_param_0x6 and hit success; "
           "a crit draw at 80010c44 is expected only when instr_param_0x6 is zero and the hit lands";
}

} // namespace savor::predict
