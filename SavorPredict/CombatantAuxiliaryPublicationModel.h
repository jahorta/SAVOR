#pragma once

#include "ActionMotionPlaybackModel.h"
#include "CombatantVisualDispatcherModel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

enum class CombatantAuxiliaryPublicationStatus {
    Matched,
    Provisional,
    Skipped,
    MissingInput,
    Unsupported,
};

enum class CombatantAuxiliaryDispatchLane {
    CurrentResource,
    StaticResource,
};

enum class CombatantAuxiliaryRangePolicy {
    FullTable,
    ExplicitRanges,
    Unknown,
};

enum class CombatantAuxiliaryCommandDecisionKind {
    CreatedChild,
    HandledNoChild,
    RejectedGate,
    RejectedPayload,
    Suppressed,
    Unsupported,
};

enum class CombatantAuxiliaryPublicationSource {
    State10_8001CAA8,
    SpecialMode11_8001C474,
};

struct CombatantAuxiliaryRowRange {
    int first_record_index = 0;
    int terminal_record_index = 0;
};

struct CombatantAuxiliaryPublicationRequest {
    int action_ordinal = -1;
    int slot = -1;
    int target_slot = -1;
    std::uint64_t instruction_revision = 0;
    std::uint64_t publication_epoch = 0;
    std::uint32_t owning_thread_visit = 0;
    std::int16_t instruction_mode = -1;
    std::optional<std::int16_t> instruction_subtype;
    std::optional<std::uint32_t> instruction_flags_0x50;
    std::optional<std::uint32_t> instruction_flags_0xec;
    std::optional<std::uint32_t> instruction_flags_0xf0;
    ActionMotionInstructionGateInput gate_input{};
    const CombatantVisualResource* current_resource = nullptr;
    const CombatantVisualResource* static_resource = nullptr;
    std::optional<bool> readiness_uses_static_resource;
    CombatantAuxiliaryRangePolicy current_range_policy =
        CombatantAuxiliaryRangePolicy::Unknown;
    std::vector<CombatantAuxiliaryRowRange> current_ranges;
    std::optional<std::int16_t> selector_state;
    std::vector<int> already_dispatched_record_indices;
    CombatantAuxiliaryPublicationSource source =
        CombatantAuxiliaryPublicationSource::State10_8001CAA8;
    std::string provenance;
};

struct CombatantAuxiliaryCommandDecision {
    CombatantAuxiliaryDispatchLane lane =
        CombatantAuxiliaryDispatchLane::CurrentResource;
    CombatantAuxiliaryCommandDecisionKind decision =
        CombatantAuxiliaryCommandDecisionKind::Unsupported;
    int record_index = -1;
    CombatantVisualCommandKind command_kind =
        CombatantVisualCommandKind::Unknown;
    std::uint32_t combined_type = 0;
    CombatantAuxiliaryPublicationStatus status =
        CombatantAuxiliaryPublicationStatus::Unsupported;
    std::string provenance;
};

struct CombatantAuxiliaryPublicationResult {
    CombatantAuxiliaryPublicationStatus status =
        CombatantAuxiliaryPublicationStatus::MissingInput;
    bool hard_skipped = false;
    bool cleared_mode2_flag = false;
    bool current_lane_dispatched = false;
    bool static_lane_suppressed = false;
    bool static_lane_missing = false;
    std::vector<CombatantAuxiliaryCommandDecision> decisions;
    std::vector<CombatantVisualPublication> publications;
    std::string provenance;
};

CombatantAuxiliaryPublicationResult publish_combatant_auxiliary_commands(
    const CombatantAuxiliaryPublicationRequest& request);

const char* combatant_auxiliary_publication_status_name(
    CombatantAuxiliaryPublicationStatus status);
const char* combatant_auxiliary_dispatch_lane_name(
    CombatantAuxiliaryDispatchLane lane);
const char* combatant_auxiliary_command_decision_name(
    CombatantAuxiliaryCommandDecisionKind decision);

} // namespace savor::predict
