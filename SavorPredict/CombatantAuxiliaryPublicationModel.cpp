#include "CombatantAuxiliaryPublicationModel.h"

#include <algorithm>
#include <array>
#include <sstream>

namespace savor::predict {
namespace {

constexpr std::uint32_t kSuppressVisualCommands = 0x00008000u;
constexpr std::uint32_t kSuppressStaticLane = 0x00100000u;
constexpr std::uint32_t kMode2Flag = 0x08000000u;

constexpr std::array<std::uint32_t, 7> kSuppressedCommandIds{
    0x00030009u,
    0x0003001fu,
    0x0003002au,
    0x00030036u,
    0x0003003bu,
    0x00030042u,
    0x00030058u,
};

bool child_producing_command(CombatantVisualCommandKind kind) {
    switch (kind) {
    case CombatantVisualCommandKind::Sparc:
    case CombatantVisualCommandKind::SetCommand:
    case CombatantVisualCommandKind::MoveModel:
    case CombatantVisualCommandKind::PutModel:
    case CombatantVisualCommandKind::HitWeapon:
    case CombatantVisualCommandKind::CollisionBox:
    case CombatantVisualCommandKind::MotionPause:
    case CombatantVisualCommandKind::PointLight:
    case CombatantVisualCommandKind::SystemCamera:
    case CombatantVisualCommandKind::SeRequest:
        return true;
    case CombatantVisualCommandKind::Unknown:
    case CombatantVisualCommandKind::SyntheticActionView:
        return false;
    }
    return false;
}

bool already_dispatched(
    const CombatantAuxiliaryPublicationRequest& request,
    int record_index) {
    return std::find(
        request.already_dispatched_record_indices.begin(),
        request.already_dispatched_record_indices.end(),
        record_index) != request.already_dispatched_record_indices.end();
}

bool record_in_ranges(
    const CombatantAuxiliaryPublicationRequest& request,
    int record_index) {
    if (request.current_range_policy == CombatantAuxiliaryRangePolicy::FullTable) {
        return true;
    }
    return std::any_of(
        request.current_ranges.begin(),
        request.current_ranges.end(),
        [record_index](const CombatantAuxiliaryRowRange& range) {
            return record_index >= range.first_record_index
                && record_index < range.terminal_record_index;
        });
}

CombatantVisualModelStatus visual_status(
    CombatantAuxiliaryPublicationStatus status) {
    switch (status) {
    case CombatantAuxiliaryPublicationStatus::Matched:
        return CombatantVisualModelStatus::Matched;
    case CombatantAuxiliaryPublicationStatus::Provisional:
        return CombatantVisualModelStatus::Provisional;
    case CombatantAuxiliaryPublicationStatus::MissingInput:
        return CombatantVisualModelStatus::MissingInput;
    case CombatantAuxiliaryPublicationStatus::Skipped:
    case CombatantAuxiliaryPublicationStatus::Unsupported:
        return CombatantVisualModelStatus::Unsupported;
    }
    return CombatantVisualModelStatus::Unsupported;
}

void append_decision(
    CombatantAuxiliaryPublicationResult& result,
    CombatantAuxiliaryDispatchLane lane,
    const CombatantVisualCommandRecord& record,
    CombatantAuxiliaryCommandDecisionKind decision,
    CombatantAuxiliaryPublicationStatus status,
    std::string provenance) {
    result.decisions.push_back(CombatantAuxiliaryCommandDecision{
        .lane = lane,
        .decision = decision,
        .record_index = record.index,
        .command_kind = record.kind,
        .combined_type = record.combined_type,
        .status = status,
        .provenance = std::move(provenance),
    });
}

void dispatch_resource(
    const CombatantAuxiliaryPublicationRequest& request,
    const CombatantVisualResource& resource,
    CombatantAuxiliaryDispatchLane lane,
    CombatantAuxiliaryPublicationStatus dispatch_status,
    bool use_current_ranges,
    CombatantAuxiliaryPublicationResult& result) {
    for (const auto& record : resource.records) {
        if (record.location_code < 0) {
            break;
        }
        if (already_dispatched(request, record.index)) {
            continue;
        }
        if (use_current_ranges && !record_in_ranges(request, record.index)) {
            continue;
        }
        if (!record.gate_fields_known) {
            append_decision(
                result,
                lane,
                record,
                CombatantAuxiliaryCommandDecisionKind::RejectedPayload,
                CombatantAuxiliaryPublicationStatus::MissingInput,
                "FUN_8000832C row has no decoded FUN_8003DCF4 gate fields");
            continue;
        }
        if (request.instruction_flags_0xf0.has_value()
            && (*request.instruction_flags_0xf0 & kSuppressVisualCommands) != 0
            && std::find(
                kSuppressedCommandIds.begin(),
                kSuppressedCommandIds.end(),
                record.combined_type) != kSuppressedCommandIds.end()) {
            append_decision(
                result,
                lane,
                record,
                CombatantAuxiliaryCommandDecisionKind::Suppressed,
                dispatch_status,
                "FUN_8000832C suppressed this command through IW+0xF0 bit 0x8000");
            continue;
        }

        const auto gate = evaluate_action_motion_instruction_gate(
            record.gate_fields.primary_action_key,
            record.gate_fields.generic_secondary_key,
            record.gate_fields.direct_gate_secondary_key,
            request.gate_input);
        if (gate == ActionMotionInstructionGateResult::MissingInput) {
            append_decision(
                result,
                lane,
                record,
                CombatantAuxiliaryCommandDecisionKind::RejectedGate,
                CombatantAuxiliaryPublicationStatus::MissingInput,
                "FUN_8003DCF4 requires unavailable instruction or alternate-key input");
            continue;
        }
        if (gate == ActionMotionInstructionGateResult::NoMatch) {
            append_decision(
                result,
                lane,
                record,
                CombatantAuxiliaryCommandDecisionKind::RejectedGate,
                dispatch_status,
                "FUN_8003DCF4 rejected the command row");
            continue;
        }
        if (!child_producing_command(record.kind)) {
            append_decision(
                result,
                lane,
                record,
                CombatantAuxiliaryCommandDecisionKind::HandledNoChild,
                CombatantAuxiliaryPublicationStatus::Unsupported,
                "DispatchVisualCommand_800367E8 command family is not yet classified as child-producing");
            continue;
        }

        CombatantVisualPublication publication;
        publication.slot = request.slot;
        publication.target_slot = request.target_slot;
        publication.instruction_revision = request.instruction_revision;
        publication.epoch = request.publication_epoch;
        publication.owning_thread_visit = request.owning_thread_visit;
        publication.record_index = record.index;
        publication.kind = record.kind;
        publication.status = visual_status(dispatch_status);
        publication.key_source = CombatantVisualKeySource::RuntimeInstruction;
        publication.action_key = request.instruction_mode;
        publication.record = &record;
        publication.provenance = request.source
                == CombatantAuxiliaryPublicationSource::
                    SpecialMode11_8001C474
            ? "FUN_8001A4F0 state 0 -> FUN_8001C474 -> FUN_80008530 -> "
              "FUN_8000832C -> DispatchVisualCommand_800367E8; "
            : "FUN_8001B1B0 state 10 -> FUN_8001CAA8 -> FUN_800085EC -> "
              "FUN_800086BC -> FUN_8000832C -> DispatchVisualCommand_800367E8; ";
        publication.provenance +=
            "lane=" + std::string(combatant_auxiliary_dispatch_lane_name(lane))
            + "; resource=" + resource.binding.resource_stem
            + "; record_index=" + std::to_string(record.index);
        result.publications.push_back(std::move(publication));
        append_decision(
            result,
            lane,
            record,
            CombatantAuxiliaryCommandDecisionKind::CreatedChild,
            dispatch_status,
            "the decoded handler family creates a child through mkChildMenu_802268E8");
    }
}

} // namespace

CombatantAuxiliaryPublicationResult publish_combatant_auxiliary_commands(
    const CombatantAuxiliaryPublicationRequest& request) {
    CombatantAuxiliaryPublicationResult result;
    if (request.slot < 0
        || request.instruction_mode < 0
        || !request.instruction_subtype.has_value()
        || !request.instruction_flags_0xec.has_value()
        || !request.instruction_flags_0xf0.has_value()) {
        result.provenance =
            "FUN_8001CAA8 requires slot, mode, subtype, IW+0xEC, and IW+0xF0";
        return result;
    }

    if (request.instruction_mode == 2
        || request.instruction_mode == 7
        || (request.instruction_mode == 0x1e
            && (*request.instruction_subtype == 0x23
                || *request.instruction_subtype == 0x24
                || *request.instruction_subtype == 0x25))) {
        result.status = CombatantAuxiliaryPublicationStatus::Skipped;
        result.hard_skipped = true;
        result.cleared_mode2_flag = request.instruction_mode == 2
            && request.instruction_flags_0x50.has_value()
            && ((*request.instruction_flags_0x50 & kMode2Flag) != 0);
        result.provenance =
            "FUN_8001CAA8 exact mode/subtype early return";
        return result;
    }

    CombatantAuxiliaryPublicationStatus dispatch_status =
        CombatantAuxiliaryPublicationStatus::Matched;
    if (!request.readiness_uses_static_resource.has_value()
        || !request.selector_state.has_value()) {
        dispatch_status = CombatantAuxiliaryPublicationStatus::Provisional;
    }

    const bool use_static_first =
        request.readiness_uses_static_resource.value_or(false);
    if (use_static_first) {
        if (request.static_resource == nullptr) {
            result.status = CombatantAuxiliaryPublicationStatus::MissingInput;
            result.static_lane_missing = true;
            result.provenance =
                "FUN_8001C1F0 selected the static lane but its table is unavailable";
            return result;
        }
        dispatch_resource(
            request,
            *request.static_resource,
            CombatantAuxiliaryDispatchLane::StaticResource,
            dispatch_status,
            false,
            result);
    } else {
        if (request.current_resource == nullptr) {
            result.status = CombatantAuxiliaryPublicationStatus::MissingInput;
            result.provenance =
                "FUN_8001CAA8 current-resource dispatch has no parsed STD resource";
            return result;
        }
        if (request.current_range_policy == CombatantAuxiliaryRangePolicy::Unknown) {
            result.status = CombatantAuxiliaryPublicationStatus::MissingInput;
            result.provenance =
                "FUN_800085EC current-resource range traversal is unknown";
            return result;
        }
        result.current_lane_dispatched = true;
        dispatch_resource(
            request,
            *request.current_resource,
            CombatantAuxiliaryDispatchLane::CurrentResource,
            dispatch_status,
            true,
            result);
    }

    result.static_lane_suppressed =
        ((*request.instruction_flags_0xec & kSuppressStaticLane) != 0)
        || request.instruction_mode == 6
        || request.instruction_mode == 12
        || request.instruction_mode == 13;
    if (!result.static_lane_suppressed) {
        if (request.static_resource != nullptr) {
            dispatch_resource(
                request,
                *request.static_resource,
                CombatantAuxiliaryDispatchLane::StaticResource,
                dispatch_status,
                false,
                result);
        } else {
            result.static_lane_missing = true;
            dispatch_status = CombatantAuxiliaryPublicationStatus::Provisional;
        }
    }

    result.status = dispatch_status;
    std::ostringstream provenance;
    provenance
        << (request.source
                == CombatantAuxiliaryPublicationSource::
                    SpecialMode11_8001C474
            ? "FUN_8001A4F0 state-0 FUN_8001C474 auxiliary publication"
            : "FUN_8001B1B0 state-10 FUN_8001CAA8 auxiliary publication")
        << "; mode="
        << request.instruction_mode
        << "; subtype=" << *request.instruction_subtype
        << "; current_lane=" << (result.current_lane_dispatched ? 1 : 0)
        << "; static_lane_suppressed="
        << (result.static_lane_suppressed ? 1 : 0)
        << "; static_lane_missing=" << (result.static_lane_missing ? 1 : 0)
        << "; created_children=" << result.publications.size()
        << "; provenance=" << request.provenance;
    result.provenance = provenance.str();
    return result;
}

const char* combatant_auxiliary_publication_status_name(
    CombatantAuxiliaryPublicationStatus status) {
    switch (status) {
    case CombatantAuxiliaryPublicationStatus::Matched: return "Matched";
    case CombatantAuxiliaryPublicationStatus::Provisional: return "Provisional";
    case CombatantAuxiliaryPublicationStatus::Skipped: return "Skipped";
    case CombatantAuxiliaryPublicationStatus::MissingInput: return "MissingInput";
    case CombatantAuxiliaryPublicationStatus::Unsupported: return "Unsupported";
    }
    return "Unsupported";
}

const char* combatant_auxiliary_dispatch_lane_name(
    CombatantAuxiliaryDispatchLane lane) {
    switch (lane) {
    case CombatantAuxiliaryDispatchLane::CurrentResource:
        return "current_resource";
    case CombatantAuxiliaryDispatchLane::StaticResource:
        return "static_resource";
    }
    return "unknown";
}

const char* combatant_auxiliary_command_decision_name(
    CombatantAuxiliaryCommandDecisionKind decision) {
    switch (decision) {
    case CombatantAuxiliaryCommandDecisionKind::CreatedChild:
        return "CreatedChild";
    case CombatantAuxiliaryCommandDecisionKind::HandledNoChild:
        return "HandledNoChild";
    case CombatantAuxiliaryCommandDecisionKind::RejectedGate:
        return "RejectedGate";
    case CombatantAuxiliaryCommandDecisionKind::RejectedPayload:
        return "RejectedPayload";
    case CombatantAuxiliaryCommandDecisionKind::Suppressed:
        return "Suppressed";
    case CombatantAuxiliaryCommandDecisionKind::Unsupported:
        return "Unsupported";
    }
    return "Unsupported";
}

} // namespace savor::predict
