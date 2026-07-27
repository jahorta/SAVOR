#include "ActionMotionPlaybackModel.h"

#include "ActionViewSelectorModel.h"

#include <bit>
#include <cmath>
#include <sstream>

namespace savor::predict {
namespace {

constexpr std::uint32_t kMotionActiveBit = 0x80000000u;
constexpr std::uint32_t kDefaultDurationGate = 0x00040000u;
constexpr std::uint32_t kZeroBits = 0x00000000u;
constexpr std::uint32_t kActionMotionDelayDescriptorType = 0x00030032u;
constexpr std::uint32_t kInstructionFlagSuppressCurrent = 0x00000800u;
constexpr std::uint32_t kInstructionFlagSpecialPrimary20 = 0x00001000u;
constexpr std::uint32_t kInstructionFlagUseAlternateB = 0x00004000u;
constexpr std::uint32_t kInstructionMotionAdvanceSuppressed = 0x00000800u;
constexpr std::uint32_t kInstructionMotionAdvancePaused = 0x00002000u;
constexpr std::uint32_t kInstructionMotionReverse = 0x20000000u;
constexpr std::uint32_t kInstructionMotionReverseEnabled = 0x08000000u;
constexpr std::uint32_t kInstructionMotionAdvanceOverride = 0x10000000u;
constexpr std::uint32_t kSelectedMotionClampAtEnd = 0x08000000u;
constexpr std::uint32_t kSelectedMotionImmediateComplete = 0x00800000u;
constexpr std::int16_t kSpecialMode0b = 0x0b;
constexpr std::int16_t kSpecialMode20 = 0x20;
constexpr std::int16_t kSpecialSelector = 2;

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

std::optional<std::int16_t> payload_s16(
    const std::vector<std::uint8_t>& payload,
    std::size_t offset) {
    if (offset + 1 >= payload.size()) {
        return std::nullopt;
    }
    const auto raw = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(payload[offset]) << 8U)
        | static_cast<std::uint16_t>(payload[offset + 1]));
    return static_cast<std::int16_t>(raw);
}

std::optional<bool> match_payload_key(
    std::int16_t payload_primary,
    std::int16_t payload_secondary,
    const std::optional<std::int16_t>& action_key,
    const std::optional<std::int16_t>& secondary_key) {
    if (!action_key.has_value()) {
        return std::nullopt;
    }
    if ((payload_primary == 0x18 || payload_primary == 0x1d || payload_primary == 0x1e)
        && !secondary_key.has_value()) {
        return std::nullopt;
    }
    return match_std_payload_action_key(
        payload_primary,
        payload_secondary,
        *action_key,
        secondary_key.value_or(-1));
}

} // namespace

SelectedActionMotionRendererInstallResult
install_selected_action_motion_renderer(
    const SelectedActionMotionRendererInstallRequest& request) {
    SelectedActionMotionRendererInstallResult result;
    auto& runtime = result.runtime;
    runtime.action_ordinal = request.action_ordinal;
    runtime.slot = request.slot;
    runtime.instruction_state_revision =
        request.instruction_state_revision;
    runtime.selected_action_row_index =
        request.selected_action_row_index;
    runtime.motion_id = request.motion_id;
    runtime.row_flags = request.row_flags;
    runtime.increment_bits_0x6c =
        request.motion_progress_step_bits;
    runtime.provenance = request.provenance;

    if (!request.motion_frame_count.has_value()) {
        runtime.status = SelectedActionMotionRendererStatus::MissingInput;
        result.detail =
            "selected-row motion frame count is unavailable; the "
            "nonblocking FUN_80018CBC lifetime was not invented";
        return result;
    }
    runtime.motion_frame_count = *request.motion_frame_count;
    if (runtime.motion_frame_count == 0
        || request.motion_id < 0
        || !std::isfinite(value(runtime.increment_bits_0x6c))
        || value(runtime.increment_bits_0x6c) <= 0.0f) {
        runtime.status = SelectedActionMotionRendererStatus::Unsupported;
        result.detail =
            "selected-row motion metadata is malformed or outside the "
            "forward-renderer contract";
        return result;
    }

    runtime.status = SelectedActionMotionRendererStatus::Matched;
    runtime.active = true;
    runtime.progress_bits_0x68 = kZeroBits;
    runtime.motion_complete_0x70 = false;
    result.installed = true;

    std::ostringstream detail;
    detail << "selected_row=" << runtime.selected_action_row_index
           << "; motion_id=" << runtime.motion_id
           << "; frame_count=" << runtime.motion_frame_count
           << "; progress_bits_0x68=0x" << std::hex
           << runtime.progress_bits_0x68
           << "; increment_bits_0x6c=0x"
           << runtime.increment_bits_0x6c
           << "; callback_blocked=0";
    result.detail = detail.str();
    return result;
}

SelectedActionMotionRendererVisitResult
visit_selected_action_motion_renderer(
    const SelectedActionMotionRendererRuntime& runtime,
    const SelectedActionMotionRendererVisitInput& input) {
    SelectedActionMotionRendererVisitResult result;
    result.runtime = runtime;
    result.progress_before = runtime.progress_bits_0x68;
    result.progress_after = runtime.progress_bits_0x68;
    if (!runtime.active
        || runtime.status != SelectedActionMotionRendererStatus::Matched) {
        result.detail =
            "selected-row motion renderer is inactive or lacks exact input";
        return result;
    }

    auto& next = result.runtime;
    ++next.renderer_visits;
    float progress = value(next.progress_bits_0x68);
    const float increment = value(next.increment_bits_0x6c);
    const bool advance_forward =
        ((((input.instruction_flags_0xec
                    & kInstructionMotionReverse) == 0)
                && ((input.instruction_flags_0xf0
                    & kInstructionMotionAdvanceSuppressed) == 0))
            || ((input.instruction_flags_0xec
                & kInstructionMotionAdvanceOverride) != 0))
        && ((input.instruction_flags_0xf0
            & kInstructionMotionAdvancePaused) == 0);
    if (advance_forward) {
        progress = ppc_add_single(progress, increment);
        result.renderer_advanced = true;
    }
    if ((input.instruction_flags_0xec & kInstructionMotionReverse) != 0
        && (input.instruction_flags_0xec
            & kInstructionMotionReverseEnabled) != 0
        && increment <= progress) {
        volatile float reversed = progress - increment;
        progress = reversed;
        result.renderer_advanced = true;
    }

    const float last_frame =
        static_cast<float>(next.motion_frame_count) - 1.0f;
    bool complete = false;
    if (last_frame < progress) {
        progress = (next.row_flags & kSelectedMotionClampAtEnd) != 0
            ? last_frame
            : 0.0f;
        complete = true;
    }
    if ((next.row_flags & kSelectedMotionImmediateComplete) != 0
        || (input.instruction_flags_0xf0
            & kInstructionMotionAdvanceSuppressed) != 0) {
        complete = true;
    }

    next.progress_bits_0x68 = bits(progress);
    next.motion_complete_0x70 = complete;
    if (complete) {
        next.active = false;
        result.completed_this_visit = true;
    }
    result.progress_after = next.progress_bits_0x68;

    std::ostringstream detail;
    detail << "selected_row=" << next.selected_action_row_index
           << "; motion_id=" << next.motion_id
           << "; frame_count=" << next.motion_frame_count
           << "; renderer_visit=" << next.renderer_visits
           << "; progress_before_bits=0x" << std::hex
           << result.progress_before
           << "; progress_after_bits=0x" << result.progress_after
           << "; complete_0x70=" << std::dec
           << (next.motion_complete_0x70 ? 1 : 0)
           << "; callback_blocked=0";
    result.detail = detail.str();
    return result;
}

ActionMotionInstructionGateResult evaluate_action_motion_instruction_gate(
    std::int16_t payload_primary,
    std::int16_t payload_selector,
    std::int16_t payload_secondary,
    const ActionMotionInstructionGateInput& input) {
    if (!input.current_action_key.has_value()) {
        return ActionMotionInstructionGateResult::MissingInput;
    }

    std::optional<bool> result = false;
    const auto current_action = *input.current_action_key;
    if (current_action == kSpecialMode0b || current_action == kSpecialMode20) {
        if (payload_selector != kSpecialSelector) {
            return ActionMotionInstructionGateResult::NoMatch;
        }
        if (!input.instruction_flags_0xec.has_value()) {
            return ActionMotionInstructionGateResult::MissingInput;
        }
        if ((*input.instruction_flags_0xec & kInstructionFlagSpecialPrimary20) != 0) {
            result = payload_primary == 0x20;
        } else {
            result = match_payload_key(
                payload_primary,
                payload_secondary,
                input.alternate_a_action_key,
                input.alternate_a_secondary_key);
        }
    }

    if (!input.instruction_flags_0xec.has_value()) {
        return ActionMotionInstructionGateResult::MissingInput;
    }
    const auto flags = *input.instruction_flags_0xec;
    if ((flags & kInstructionFlagSuppressCurrent) == 0) {
        const auto current_match = match_payload_key(
            payload_primary,
            payload_secondary,
            input.current_action_key,
            input.current_secondary_key);
        if (current_match.has_value() && *current_match) {
            result = true;
        } else if (!current_match.has_value() && !result.value_or(false)) {
            result = std::nullopt;
        }
    }

    if ((flags & kInstructionFlagUseAlternateB) != 0) {
        const auto alternate_match = match_payload_key(
            payload_primary,
            payload_secondary,
            input.alternate_b_action_key,
            input.alternate_b_secondary_key);
        if (!alternate_match.has_value()) {
            return ActionMotionInstructionGateResult::MissingInput;
        }
        return *alternate_match
            ? ActionMotionInstructionGateResult::Matched
            : ActionMotionInstructionGateResult::NoMatch;
    }

    if (!result.has_value()) {
        return ActionMotionInstructionGateResult::MissingInput;
    }
    return *result
        ? ActionMotionInstructionGateResult::Matched
        : ActionMotionInstructionGateResult::NoMatch;
}

ActionMotionDelayLookupResult resolve_action_motion_post_state6_delay(
    const ActionMotionDelayTable& table,
    const ActionMotionInstructionGateInput& input) {
    ActionMotionDelayLookupResult result;
    if (!table.table_known) {
        result.provenance =
            "FUN_8001DDE0 descriptor root was not available for the owning instruction resource";
        return result;
    }
    if (table.table_is_null) {
        result.status = ActionMotionDelayStatus::NoMatch;
        result.delay = 0;
        result.gate_result = false;
        result.provenance = "FUN_8001DDE0 observed a null descriptor table";
        return result;
    }

    bool sentinel_seen = false;
    for (const auto& descriptor : table.descriptors) {
        if (descriptor.location_code < 0) {
            sentinel_seen = true;
            break;
        }
        if (descriptor.combined_type != kActionMotionDelayDescriptorType) {
            continue;
        }
        result.descriptor_record_index = descriptor.record_index;
        if (!descriptor.payload_in_bounds
            || descriptor.payload_size < 0x12
            || descriptor.payload_bytes.size() < 0x12U) {
            result.status = ActionMotionDelayStatus::Malformed;
            result.provenance =
                "FUN_8001DDE0 matched a 0x00030032 descriptor with a truncated payload";
            return result;
        }

        const auto primary = payload_s16(descriptor.payload_bytes, 0x00);
        const auto selector = payload_s16(descriptor.payload_bytes, 0x02);
        const auto secondary = payload_s16(descriptor.payload_bytes, 0x04);
        const auto delay = payload_s16(descriptor.payload_bytes, 0x10);
        if (!primary.has_value() || !selector.has_value()
            || !secondary.has_value() || !delay.has_value()) {
            result.status = ActionMotionDelayStatus::Malformed;
            result.provenance =
                "FUN_8001DDE0 could not decode the 0x00030032 descriptor fields";
            return result;
        }

        switch (evaluate_action_motion_instruction_gate(
            *primary, *selector, *secondary, input)) {
        case ActionMotionInstructionGateResult::Matched:
            if (*delay < 0) {
                result.status = ActionMotionDelayStatus::Unsupported;
                result.provenance =
                    "FUN_8001DDE0 selected a negative signed delay not covered by the runtime model";
                return result;
            }
            result.status = ActionMotionDelayStatus::Matched;
            result.delay = *delay;
            result.gate_result = true;
            result.provenance =
                "FUN_8001DDE0 selected the first matching 0x00030032 descriptor";
            return result;
        case ActionMotionInstructionGateResult::NoMatch:
            result.gate_result = false;
            break;
        case ActionMotionInstructionGateResult::MissingInput:
            result.status = ActionMotionDelayStatus::MissingInput;
            result.provenance =
                "FUN_8003DCF4 requires an unavailable instruction key or alternate-key pair";
            return result;
        }
    }

    if (sentinel_seen || table.includes_sentinel) {
        result.status = ActionMotionDelayStatus::NoMatch;
        result.delay = 0;
        result.gate_result = false;
        result.provenance =
            "FUN_8001DDE0 reached the descriptor terminator without a matching delay row";
        return result;
    }
    result.status = ActionMotionDelayStatus::MissingInput;
    result.provenance =
        "FUN_8001DDE0 descriptor stream has no terminal row; no zero-delay conclusion is safe";
    return result;
}

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
    result.runtime.continuation = request.continuation;
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
    switch (request.continuation) {
    case ActionMotionPlaybackContinuation::State5LoadLookedUpTo4:
        result.runtime.callback_control_state = 5;
        break;
    case ActionMotionPlaybackContinuation::State6PostDelayTo11:
        result.runtime.callback_control_state = 6;
        break;
    case ActionMotionPlaybackContinuation::State7LoadLookedUpTo14:
        result.runtime.callback_control_state = 7;
        break;
    case ActionMotionPlaybackContinuation::SpecialState10LoadLookedUpTo2:
        result.runtime.callback_control_state = 10;
        break;
    case ActionMotionPlaybackContinuation::GenericRelease:
        result.runtime.callback_control_state = 0;
        break;
    }
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
           << "; continuation="
           << action_motion_playback_continuation_name(request.continuation)
           << "; IW+0xEC_bit31=1";
    result.detail = detail.str();
    return result;
}

ActionMotionPlaybackVisitResult visit_action_motion_playback(
    const ActionMotionPlaybackRuntime& runtime,
    const ActionMotionPlaybackVisitInput& input) {
    ActionMotionPlaybackVisitResult result;
    result.runtime = runtime;
    result.progress_before = runtime.progress_bits_0x68;
    result.progress_after = runtime.progress_bits_0x68;
    result.flags_before = runtime.instruction_flags_0xec;
    result.flags_after = runtime.instruction_flags_0xec;
    result.control_state_before = runtime.callback_control_state;
    result.control_state_after = runtime.callback_control_state;
    result.post_state6_delay_status = runtime.post_state6_delay_status;
    result.post_state6_delay_descriptor_record_index =
        runtime.post_state6_delay_descriptor_record_index;
    result.post_state6_delay_before = runtime.post_state6_delay_remaining;
    result.post_state6_delay_after = runtime.post_state6_delay_remaining;

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
            if (runtime.continuation
                == ActionMotionPlaybackContinuation::State6PostDelayTo11) {
                result.runtime.phase = ActionMotionPlaybackPhase::State6Satisfied;
            } else {
                result.runtime.phase = ActionMotionPlaybackPhase::PublicationReleased;
                result.publication_released_this_visit = true;
                switch (runtime.continuation) {
                case ActionMotionPlaybackContinuation::State5LoadLookedUpTo4:
                    result.runtime.callback_control_state = 4;
                    break;
                case ActionMotionPlaybackContinuation::State7LoadLookedUpTo14:
                    result.runtime.callback_control_state = 14;
                    break;
                case ActionMotionPlaybackContinuation::
                        SpecialState10LoadLookedUpTo2:
                    result.runtime.callback_control_state = 2;
                    break;
                case ActionMotionPlaybackContinuation::GenericRelease:
                    result.runtime.callback_control_state = 0;
                    break;
                case ActionMotionPlaybackContinuation::State6PostDelayTo11:
                    break;
                }
                result.control_state_after = result.runtime.callback_control_state;
            }
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
    case ActionMotionPlaybackPhase::State6Satisfied: {
        result.post_state6_delay_lookup_performed = true;
        result.runtime.post_state6_delay_lookup_complete = true;
        if (!input.post_state6_delay.has_value()) {
            result.kind = ActionMotionPlaybackVisitKind::PostState6DelayUnavailable;
            result.runtime.status = ActionMotionPlaybackStatus::MissingInput;
            result.runtime.phase = ActionMotionPlaybackPhase::Unsupported;
            result.detail =
                "FUN_8001B6F8 reached state 8 without a descriptor-backed post-state-6 delay result";
            break;
        }

        const auto& delay_lookup = *input.post_state6_delay;
        result.runtime.post_state6_delay_status = delay_lookup.status;
        result.runtime.post_state6_delay_descriptor_record_index =
            delay_lookup.descriptor_record_index;
        result.runtime.post_state6_delay_lookup_complete = true;
        result.post_state6_delay_status = delay_lookup.status;
        result.post_state6_delay_descriptor_record_index =
            delay_lookup.descriptor_record_index;
        switch (delay_lookup.status) {
        case ActionMotionDelayStatus::Matched:
        case ActionMotionDelayStatus::NoMatch:
            break;
        case ActionMotionDelayStatus::MissingInput:
            result.runtime.status = ActionMotionPlaybackStatus::MissingInput;
            result.runtime.phase = ActionMotionPlaybackPhase::Unsupported;
            result.kind = ActionMotionPlaybackVisitKind::PostState6DelayUnavailable;
            result.detail = "FUN_8001DDE0 delay lookup is missing input; "
                + delay_lookup.provenance;
            return result;
        case ActionMotionDelayStatus::Malformed:
        case ActionMotionDelayStatus::Unsupported:
            result.runtime.status = ActionMotionPlaybackStatus::Unsupported;
            result.runtime.phase = ActionMotionPlaybackPhase::Unsupported;
            result.kind = ActionMotionPlaybackVisitKind::PostState6DelayUnavailable;
            result.detail = "FUN_8001DDE0 delay lookup is unsupported; "
                + delay_lookup.provenance;
            return result;
        }
        if (!delay_lookup.delay.has_value() || *delay_lookup.delay < 0) {
            result.runtime.status = ActionMotionPlaybackStatus::Unsupported;
            result.runtime.phase = ActionMotionPlaybackPhase::Unsupported;
            result.kind = ActionMotionPlaybackVisitKind::PostState6DelayUnavailable;
            result.detail =
                "FUN_8001DDE0 did not provide a nonnegative signed state-9 delay";
            return result;
        }

        result.runtime.post_state6_delay_remaining = *delay_lookup.delay;
        result.post_state6_delay_before = *delay_lookup.delay;
        result.runtime.callback_control_state = 9;
        if (*delay_lookup.delay == 0) {
            result.kind = ActionMotionPlaybackVisitKind::PublicationReleased;
            result.runtime.phase = ActionMotionPlaybackPhase::PublicationReleased;
            result.runtime.callback_control_state = 11;
            result.control_state_after = 11;
            result.publication_released_this_visit = true;
            result.detail =
                "FUN_8001B6F8 executed states 8, 9, and 10 with a zero descriptor delay and reached publication control 11; "
                + delay_lookup.provenance;
            break;
        }

        --result.runtime.post_state6_delay_remaining;
        result.post_state6_delay_after = result.runtime.post_state6_delay_remaining;
        result.runtime.phase = ActionMotionPlaybackPhase::WaitingForPostState6Delay;
        result.control_state_after = 9;
        result.kind = ActionMotionPlaybackVisitKind::PostState6DelayDeferred;
        result.detail =
            "FUN_8001B6F8 state 8 selected a descriptor delay, stored it, and state 9 decremented once before returning; "
            + delay_lookup.provenance;
        break;
    }
    case ActionMotionPlaybackPhase::WaitingForPostState6Delay:
        result.kind = ActionMotionPlaybackVisitKind::PostState6DelayDeferred;
        result.runtime.callback_control_state = 9;
        if (runtime.post_state6_delay_remaining > 0) {
            --result.runtime.post_state6_delay_remaining;
            result.post_state6_delay_after = result.runtime.post_state6_delay_remaining;
            result.control_state_after = 9;
            result.detail =
                "FUN_8001B6F8 state 9 decremented the signed descriptor delay and returned";
            break;
        }
        result.kind = ActionMotionPlaybackVisitKind::PublicationReleased;
        result.runtime.phase = ActionMotionPlaybackPhase::PublicationReleased;
        result.runtime.callback_control_state = 11;
        result.control_state_after = 11;
        result.publication_released_this_visit = true;
        result.detail =
            "FUN_8001B6F8 state 9 observed a zero descriptor delay, fell through state 10, and reached publication control 11";
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
    if (runtime.phase == ActionMotionPlaybackPhase::Unsupported
        && runtime.post_state6_delay_lookup_complete) {
        return true;
    }
    return runtime.phase == ActionMotionPlaybackPhase::Primed
        || runtime.phase == ActionMotionPlaybackPhase::WaitingForState6
        || runtime.phase == ActionMotionPlaybackPhase::State6Satisfied
        || runtime.phase == ActionMotionPlaybackPhase::WaitingForPostState6Delay;
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
    case ActionMotionPlaybackPhase::WaitingForPostState6Delay:
        return "WaitingForPostState6Delay";
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
    case ActionMotionPlaybackVisitKind::PostState6DelayDeferred:
        return "PostState6DelayDeferred";
    case ActionMotionPlaybackVisitKind::PostState6DelayUnavailable:
        return "PostState6DelayUnavailable";
    case ActionMotionPlaybackVisitKind::PublicationReleased:
        return "PublicationReleased";
    }
    return "Unknown";
}

const char* action_motion_playback_continuation_name(
    ActionMotionPlaybackContinuation continuation) {
    switch (continuation) {
    case ActionMotionPlaybackContinuation::State5LoadLookedUpTo4:
        return "State5LoadLookedUpTo4";
    case ActionMotionPlaybackContinuation::State6PostDelayTo11:
        return "State6PostDelayTo11";
    case ActionMotionPlaybackContinuation::State7LoadLookedUpTo14:
        return "State7LoadLookedUpTo14";
    case ActionMotionPlaybackContinuation::SpecialState10LoadLookedUpTo2:
        return "SpecialState10LoadLookedUpTo2";
    case ActionMotionPlaybackContinuation::GenericRelease:
        return "GenericRelease";
    }
    return "State6PostDelayTo11";
}

const char* selected_action_motion_renderer_status_name(
    SelectedActionMotionRendererStatus status) {
    switch (status) {
    case SelectedActionMotionRendererStatus::Inactive:
        return "Inactive";
    case SelectedActionMotionRendererStatus::Matched:
        return "Matched";
    case SelectedActionMotionRendererStatus::MissingInput:
        return "MissingInput";
    case SelectedActionMotionRendererStatus::Unsupported:
        return "Unsupported";
    }
    return "Unknown";
}

const char* action_motion_delay_status_name(ActionMotionDelayStatus status) {
    switch (status) {
    case ActionMotionDelayStatus::Matched:
        return "Matched";
    case ActionMotionDelayStatus::NoMatch:
        return "NoMatch";
    case ActionMotionDelayStatus::MissingInput:
        return "MissingInput";
    case ActionMotionDelayStatus::Malformed:
        return "Malformed";
    case ActionMotionDelayStatus::Unsupported:
        return "Unsupported";
    }
    return "Unknown";
}

} // namespace savor::predict
