#include "DirectInstructionTransitionSelectorModel.h"

#include "RngCore.h"

#include <algorithm>

namespace savor::predict {
namespace {

bool has_action_id(
    const DirectInstructionTransitionRequest& request,
    std::int16_t action_id) {
    return std::find(
        request.available_target_action_ids.begin(),
        request.available_target_action_ids.end(),
        action_id) != request.available_target_action_ids.end();
}

bool supported_origin_mode(std::int16_t mode) {
    return mode == 4 || mode == 5 || mode == 8;
}

bool select_fixed_mode(
    const DirectInstructionTransitionRequest& request,
    DirectInstructionTransitionResult& result,
    std::int16_t mode,
    DirectInstructionTransitionBranch branch,
    std::string provenance) {
    result.branch = branch;
    if (!has_action_id(request, mode)) {
        result.status = DirectInstructionTransitionStatus::MissingInput;
        result.provenance = std::move(provenance)
            + "; target STD resource does not expose the selected action row";
        return false;
    }
    result.status = DirectInstructionTransitionStatus::Matched;
    result.should_reset = true;
    result.selected_mode = mode;
    result.provenance = std::move(provenance);
    return true;
}

} // namespace

DirectInstructionTransitionResult select_direct_instruction_transition(
    const DirectInstructionTransitionRequest& request) {
    DirectInstructionTransitionResult result;
    if (request.producer == DirectInstructionTransitionProducer::QueuedStateTransition) {
        result.status = DirectInstructionTransitionStatus::Skipped;
        result.branch = DirectInstructionTransitionBranch::IneligibleQueuedTransition;
        result.provenance =
            "queued-state FUN_8002E5D0 callers are negative evidence and do not directly reset an instruction callback";
        return result;
    }
    if (request.producer != DirectInstructionTransitionProducer::ActionService
        && request.producer != DirectInstructionTransitionProducer::CollisionBox) {
        result.status = DirectInstructionTransitionStatus::Unsupported;
        result.provenance = "direct-transition producer family is unsupported";
        return result;
    }
    if (request.origin_slot < 0 || request.target_slot < 0
        || !request.origin_mode.has_value()
        || !request.origin_flags_0xec.has_value()) {
        result.status = DirectInstructionTransitionStatus::MissingInput;
        result.provenance =
            "FUN_8002E5D0 requires origin/target slots, origin mode, and origin EC flags";
        return result;
    }
    if (!supported_origin_mode(*request.origin_mode)) {
        result.status = DirectInstructionTransitionStatus::Unsupported;
        result.provenance =
            "only validated basic-attack origin modes 4, 5, and 8 are modeled";
        return result;
    }

    if ((*request.origin_flags_0xec & 0x00100000U) != 0) {
        result.branch = DirectInstructionTransitionBranch::RandomizedPassive;
        if (!request.rng_seed_before.has_value()) {
            result.status = DirectInstructionTransitionStatus::MissingInput;
            result.provenance =
                "FUN_8002EB4C random branch requires the current RNG seed";
            return result;
        }
        if (!has_action_id(request, 12) || !has_action_id(request, 13)) {
            result.status = DirectInstructionTransitionStatus::MissingInput;
            result.provenance =
                "FUN_8002EB4C requires both validated passive candidate rows 12 and 13";
            return result;
        }
        const auto draw = draw_rand15(*request.rng_seed_before);
        result.status = DirectInstructionTransitionStatus::Matched;
        result.should_reset = true;
        result.clear_origin_random_gate = true;
        result.draws_consumed = 1;
        result.rng_seed_before = *request.rng_seed_before;
        result.rng_seed_after = draw.next_state;
        result.rand_value = draw.value;
        result.candidate_index = draw.value % 2U;
        result.selected_mode = result.candidate_index == 0 ? 13 : 12;
        result.provenance =
            "FUN_8002E9C4 called FUN_8002EB4C; rand modulo two selected mode 13 or 12";
        return result;
    }

    if (!request.target_flags_0xf0.has_value()
        || !request.target_flags_0xf4.has_value()) {
        result.status = DirectInstructionTransitionStatus::MissingInput;
        result.provenance =
            "nonrandom FUN_8002E5D0 selection requires the staged target F0/F4 reaction flags";
        return result;
    }

    if ((*request.target_flags_0xf4 & 0x01000000U) != 0) {
        select_fixed_mode(
            request,
            result,
            13,
            DirectInstructionTransitionBranch::TargetMiss,
            "target reaction F4 bit 24 selected the validated mode-13 miss path at 0x8002E9F0");
        return result;
    }
    if ((*request.target_flags_0xf4 & 0x04000000U) != 0) {
        const auto selected = *request.origin_mode == 5
            ? static_cast<std::int16_t>(10)
            : static_cast<std::int16_t>(9);
        select_fixed_mode(
            request,
            result,
            selected,
            DirectInstructionTransitionBranch::TargetCounter,
            *request.origin_mode == 5
                ? "target reaction F4 bit 26 and ranged mode 5 selected mode 10 at 0x8002EA64"
                : "target reaction F4 bit 26 and direct mode 4/8 selected mode 9 at 0x8002EA78");
        return result;
    }

    if (*request.origin_mode == 8 && has_action_id(request, 32)) {
        select_fixed_mode(
            request,
            result,
            32,
            DirectInstructionTransitionBranch::GenericRow32,
            "FUN_8002111C selected target row 32 for critical origin mode 8");
        return result;
    }
    select_fixed_mode(
        request,
        result,
        11,
        DirectInstructionTransitionBranch::GenericRow11,
        *request.origin_mode == 8
            ? "FUN_8002111C fell back from target row 32 to row 11"
            : "FUN_8002111C selected target row 11 for the basic origin mode");
    return result;
}

const char* direct_instruction_transition_producer_name(
    DirectInstructionTransitionProducer producer) {
    switch (producer) {
    case DirectInstructionTransitionProducer::ActionService: return "ActionService";
    case DirectInstructionTransitionProducer::CollisionBox: return "CollisionBox";
    case DirectInstructionTransitionProducer::QueuedStateTransition: return "QueuedStateTransition";
    case DirectInstructionTransitionProducer::Unknown: return "Unknown";
    }
    return "Unknown";
}

const char* direct_instruction_transition_status_name(
    DirectInstructionTransitionStatus status) {
    switch (status) {
    case DirectInstructionTransitionStatus::Matched: return "Matched";
    case DirectInstructionTransitionStatus::Provisional: return "Provisional";
    case DirectInstructionTransitionStatus::Skipped: return "Skipped";
    case DirectInstructionTransitionStatus::MissingInput: return "MissingInput";
    case DirectInstructionTransitionStatus::Unsupported: return "Unsupported";
    }
    return "Unsupported";
}

const char* direct_instruction_transition_branch_name(
    DirectInstructionTransitionBranch branch) {
    switch (branch) {
    case DirectInstructionTransitionBranch::None: return "None";
    case DirectInstructionTransitionBranch::RandomizedPassive: return "RandomizedPassive";
    case DirectInstructionTransitionBranch::TargetMiss: return "TargetMiss";
    case DirectInstructionTransitionBranch::TargetCounter: return "TargetCounter";
    case DirectInstructionTransitionBranch::GenericRow32: return "GenericRow32";
    case DirectInstructionTransitionBranch::GenericRow11: return "GenericRow11";
    case DirectInstructionTransitionBranch::IneligibleQueuedTransition: return "IneligibleQueuedTransition";
    }
    return "None";
}

} // namespace savor::predict
