#include "ActionMotionPlaybackModel.h"

#include <bit>
#include <cmath>
#include <sstream>

namespace savor::predict {
namespace {

constexpr std::uint32_t kMotionActiveBit = 0x80000000u;
constexpr std::uint32_t kDefaultDurationGate = 0x00040000u;
constexpr std::uint32_t kZeroBits = 0x00000000u;

float ppc_div_single(float lhs, float rhs) {
    volatile float result = lhs / rhs;
    return result;
}

float ppc_add_single(float lhs, float rhs) {
    volatile float result = lhs + rhs;
    return result;
}

std::uint32_t bits(float value) {
    return std::bit_cast<std::uint32_t>(value);
}

float value(std::uint32_t raw_bits) {
    return std::bit_cast<float>(raw_bits);
}

std::uint32_t advance_progress(
    std::uint32_t progress_bits,
    std::uint32_t increment_bits) {
    float next = ppc_add_single(value(progress_bits), value(increment_bits));
    if (next > 1.0f) {
        next = 1.0f;
    }
    return bits(next);
}

} // namespace

ActionMotionPlaybackInstallResult install_action_motion_playback(
    const ActionMotionPlaybackInstallRequest& request) {
    ActionMotionPlaybackInstallResult result;
    result.flags_before = request.instruction_flags_0xec;
    result.flags_after = request.instruction_flags_0xec;
    result.runtime.action_ordinal = request.action_ordinal;
    result.runtime.slot = request.slot;
    result.runtime.instruction_state_revision = request.instruction_state_revision;
    result.runtime.selected_action_row_index = request.selected_action_row_index;
    result.runtime.instruction_flags_0xec = request.instruction_flags_0xec;
    result.runtime.instruction_flags_0xf0 = request.instruction_flags_0xf0;
    result.runtime.provenance = request.provenance;

    if (request.action_ordinal < 0 || request.slot < 0
        || request.instruction_state_revision == 0
        || request.selected_action_row_index < 0
        || !request.selected_action_row_duration_bits.has_value()) {
        result.runtime.status = ActionMotionPlaybackStatus::MissingInput;
        result.runtime.phase = ActionMotionPlaybackPhase::Unsupported;
        result.detail =
            "action-motion playback requires an action, live instruction revision, "
            "selected row, and raw row duration; publication remains ungated";
        return result;
    }

    result.runtime.raw_duration_bits = *request.selected_action_row_duration_bits;
    float duration = value(result.runtime.raw_duration_bits);
    if (!std::isfinite(duration)) {
        result.runtime.status = ActionMotionPlaybackStatus::Unsupported;
        result.runtime.phase = ActionMotionPlaybackPhase::Unsupported;
        result.detail =
            "action-motion row duration is non-finite; publication remains ungated";
        return result;
    }

    if (duration < 1.0f
        && (request.instruction_flags_0xf0 & kDefaultDurationGate) != 0) {
        duration = 5.0f;
        result.runtime.substituted_default_duration = true;
    }
    if (!(duration > 0.0f)) {
        result.runtime.status = ActionMotionPlaybackStatus::Unsupported;
        result.runtime.phase = ActionMotionPlaybackPhase::Unsupported;
        result.detail =
            "action-motion row duration does not establish a positive divisor; "
            "publication remains ungated";
        return result;
    }

    const float increment = ppc_div_single(1.0f, duration);
    if (!std::isfinite(increment) || !(increment > 0.0f)) {
        result.runtime.status = ActionMotionPlaybackStatus::Unsupported;
        result.runtime.phase = ActionMotionPlaybackPhase::Unsupported;
        result.detail =
            "action-motion duration did not produce a finite positive increment; "
            "publication remains ungated";
        return result;
    }

    result.runtime.status = ActionMotionPlaybackStatus::Matched;
    result.runtime.phase = ActionMotionPlaybackPhase::Primed;
    result.runtime.effective_duration_bits = bits(duration);
    result.runtime.progress_bits_0x68 = kZeroBits;
    result.runtime.increment_bits_0x6c = bits(increment);
    result.runtime.instruction_flags_0xec |= kMotionActiveBit;
    result.runtime.callback_control_state = 6;
    result.flags_after = result.runtime.instruction_flags_0xec;
    result.installed = true;
    result.blocks_publication = true;

    std::ostringstream detail;
    detail << "FUN_8001EBA4/FUN_80076170 installed action-row motion"
           << "; row=" << request.selected_action_row_index
           << "; raw_duration_bits=" << result.runtime.raw_duration_bits
           << "; effective_duration_bits=" << result.runtime.effective_duration_bits
           << "; increment_bits=" << result.runtime.increment_bits_0x6c
           << "; default_duration_substituted="
           << (result.runtime.substituted_default_duration ? 1 : 0)
           << "; IW+0xEC_bit31=1";
    result.detail = detail.str();
    return result;
}

ActionMotionPlaybackVisitResult visit_action_motion_playback(
    const ActionMotionPlaybackRuntime& runtime) {
    ActionMotionPlaybackVisitResult result;
    result.runtime = runtime;
    result.progress_before = runtime.progress_bits_0x68;
    result.progress_after = runtime.progress_bits_0x68;
    result.flags_before = runtime.instruction_flags_0xec;
    result.flags_after = runtime.instruction_flags_0xec;
    result.control_state_before = runtime.callback_control_state;
    result.control_state_after = runtime.callback_control_state;

    switch (runtime.phase) {
    case ActionMotionPlaybackPhase::Primed:
        result.kind = ActionMotionPlaybackVisitKind::InitialRendererAdvance;
        result.renderer_advanced = true;
        result.runtime.progress_bits_0x68 = advance_progress(
            runtime.progress_bits_0x68,
            runtime.increment_bits_0x6c);
        ++result.runtime.renderer_visits;
        result.runtime.phase = ActionMotionPlaybackPhase::WaitingForState6;
        result.progress_after = result.runtime.progress_bits_0x68;
        result.detail =
            "FUN_80018CBC performed the renderer update before the first state-6 poll";
        break;
    case ActionMotionPlaybackPhase::WaitingForState6: {
        result.gate_polled = true;
        ++result.runtime.state6_polls;
        const bool active_bit_set =
            (runtime.instruction_flags_0xec & kMotionActiveBit) != 0;
        const bool satisfied = !active_bit_set
            || !(value(runtime.progress_bits_0x68) < 1.0f);
        result.gate_result = satisfied;
        if (satisfied) {
            result.kind = ActionMotionPlaybackVisitKind::State6Satisfied;
            result.runtime.instruction_flags_0xec &= ~kMotionActiveBit;
            result.runtime.phase = ActionMotionPlaybackPhase::State6Satisfied;
            result.flags_after = result.runtime.instruction_flags_0xec;
            result.detail = active_bit_set
                ? "FUN_80075D64 observed progress at least one, cleared IW+0xEC bit 31, and returned true"
                : "FUN_80075D64 observed IW+0xEC bit 31 clear and returned true";
        } else {
            result.kind = ActionMotionPlaybackVisitKind::State6Deferred;
            result.renderer_advanced = true;
            result.runtime.progress_bits_0x68 = advance_progress(
                runtime.progress_bits_0x68,
                runtime.increment_bits_0x6c);
            ++result.runtime.renderer_visits;
            result.progress_after = result.runtime.progress_bits_0x68;
            result.detail =
                "FUN_80075D64 returned false with bit 31 preserved; FUN_80018CBC advanced progress after the poll";
        }
        break;
    }
    case ActionMotionPlaybackPhase::State6Satisfied:
        result.kind = ActionMotionPlaybackVisitKind::PublicationReleased;
        result.runtime.phase = ActionMotionPlaybackPhase::PublicationReleased;
        result.runtime.callback_control_state = 11;
        result.control_state_after = 11;
        result.publication_released_this_visit = true;
        result.detail =
            "the next combatant-instruction visit executed callback states 8, 9, and 10 and reached publication control 11";
        break;
    case ActionMotionPlaybackPhase::Inactive:
    case ActionMotionPlaybackPhase::PublicationReleased:
    case ActionMotionPlaybackPhase::Unsupported:
        break;
    }
    return result;
}

bool action_motion_playback_blocks_publication(
    const ActionMotionPlaybackRuntime& runtime) {
    return runtime.phase == ActionMotionPlaybackPhase::Primed
        || runtime.phase == ActionMotionPlaybackPhase::WaitingForState6
        || runtime.phase == ActionMotionPlaybackPhase::State6Satisfied;
}

const char* action_motion_playback_status_name(ActionMotionPlaybackStatus status) {
    switch (status) {
    case ActionMotionPlaybackStatus::Matched:
        return "Matched";
    case ActionMotionPlaybackStatus::Provisional:
        return "Provisional";
    case ActionMotionPlaybackStatus::MissingInput:
        return "MissingInput";
    case ActionMotionPlaybackStatus::Unsupported:
        return "Unsupported";
    }
    return "Unknown";
}

const char* action_motion_playback_phase_name(ActionMotionPlaybackPhase phase) {
    switch (phase) {
    case ActionMotionPlaybackPhase::Inactive:
        return "Inactive";
    case ActionMotionPlaybackPhase::Primed:
        return "Primed";
    case ActionMotionPlaybackPhase::WaitingForState6:
        return "WaitingForState6";
    case ActionMotionPlaybackPhase::State6Satisfied:
        return "State6Satisfied";
    case ActionMotionPlaybackPhase::PublicationReleased:
        return "PublicationReleased";
    case ActionMotionPlaybackPhase::Unsupported:
        return "Unsupported";
    }
    return "Unknown";
}

const char* action_motion_playback_visit_kind_name(ActionMotionPlaybackVisitKind kind) {
    switch (kind) {
    case ActionMotionPlaybackVisitKind::None:
        return "None";
    case ActionMotionPlaybackVisitKind::InitialRendererAdvance:
        return "InitialRendererAdvance";
    case ActionMotionPlaybackVisitKind::State6Deferred:
        return "State6Deferred";
    case ActionMotionPlaybackVisitKind::State6Satisfied:
        return "State6Satisfied";
    case ActionMotionPlaybackVisitKind::PublicationReleased:
        return "PublicationReleased";
    }
    return "Unknown";
}

} // namespace savor::predict
