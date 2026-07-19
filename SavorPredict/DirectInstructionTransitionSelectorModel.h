#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

enum class DirectInstructionTransitionProducer {
    ActionService,
    CollisionBox,
    QueuedStateTransition,
    Unknown,
};

enum class DirectInstructionTransitionStatus {
    Matched,
    Provisional,
    Skipped,
    MissingInput,
    Unsupported,
};

enum class DirectInstructionTransitionBranch {
    None,
    RandomizedPassive,
    TargetMiss,
    TargetCounter,
    GenericRow32,
    GenericRow11,
    IneligibleQueuedTransition,
};

struct DirectInstructionTransitionRequest {
    DirectInstructionTransitionProducer producer =
        DirectInstructionTransitionProducer::Unknown;
    int origin_slot = -1;
    int target_slot = -1;
    std::optional<std::int16_t> origin_mode;
    std::optional<std::uint32_t> origin_flags_0xec;
    std::optional<std::uint32_t> target_flags_0xf0;
    std::optional<std::uint32_t> target_flags_0xf4;
    std::vector<std::int16_t> available_target_action_ids;
    std::optional<std::uint32_t> rng_seed_before;
};

struct DirectInstructionTransitionResult {
    DirectInstructionTransitionStatus status =
        DirectInstructionTransitionStatus::MissingInput;
    DirectInstructionTransitionBranch branch =
        DirectInstructionTransitionBranch::None;
    bool should_reset = false;
    bool clear_origin_random_gate = false;
    std::optional<std::int16_t> selected_mode;
    int draws_consumed = 0;
    std::optional<std::uint32_t> rng_seed_before;
    std::optional<std::uint32_t> rng_seed_after;
    std::optional<std::uint16_t> rand_value;
    std::optional<int> candidate_index;
    std::string provenance;
};

DirectInstructionTransitionResult select_direct_instruction_transition(
    const DirectInstructionTransitionRequest& request);

const char* direct_instruction_transition_producer_name(
    DirectInstructionTransitionProducer producer);
const char* direct_instruction_transition_status_name(
    DirectInstructionTransitionStatus status);
const char* direct_instruction_transition_branch_name(
    DirectInstructionTransitionBranch branch);

} // namespace savor::predict
