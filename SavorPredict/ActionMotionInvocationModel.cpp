#include "ActionMotionInvocationModel.h"

#include <bit>
#include <cmath>
#include <sstream>

namespace savor::predict {
namespace {

bool requires_secondary_key(std::int16_t mode) {
    return mode == 0x18 || mode == 0x1d || mode == 0x1e;
}

std::optional<CombatantStdActionRow> find_action_row(
    const std::vector<CombatantStdActionRow>& rows,
    std::int16_t mode,
    const std::optional<std::int16_t>& secondary_key) {
    for (const auto& row : rows) {
        if (row.row_type == 3) {
            break;
        }
        if (row.action_id != mode) {
            continue;
        }
        if (requires_secondary_key(mode)
            && (!secondary_key.has_value()
                || row.secondary_key != *secondary_key)) {
            continue;
        }
        return row;
    }
    return std::nullopt;
}

void lower_status(
    ActionMotionInvocationStatus candidate,
    ActionMotionInvocationStatus& status) {
    if (candidate == ActionMotionInvocationStatus::MissingInput
        || candidate == ActionMotionInvocationStatus::Unsupported) {
        status = candidate;
    } else if (candidate == ActionMotionInvocationStatus::Provisional
        && status == ActionMotionInvocationStatus::Matched) {
        status = candidate;
    }
}

ActionMotionInvocationResult invoke_resolver(
    const std::vector<CombatantStdActionRow>& rows,
    const ActionMotionInvocationRequest& request,
    std::int16_t mode,
    std::optional<std::int16_t> subtype,
    std::string resolver_callsite) {
    ActionMotionInvocationResult result;
    result.status = ActionMotionInvocationStatus::Matched;
    result.callback_state_before = request.callback_state;
    result.callback_state_after = request.callback_state;
    result.resolver_called = true;
    result.resolver_callsite = std::move(resolver_callsite);
    result.resolver = resolve_action_motion_row_8001ecb4(
        rows,
        ActionMotionRowResolverRequest{
            .requested_mode = mode,
            .secondary_key = subtype,
            .selection_blocked = request.selection_blocked,
            .current_motion_resource_present =
                request.current_motion_resource_present,
            .current_motion_id = request.current_motion_id,
            .instruction_flags_0xf0 = request.instruction_flags_0xf0,
            .mode_2_or_7_override_row = request.mode_2_or_7_override_row,
        });
    result.status = result.resolver.status;
    result.operation_row = result.resolver.row;
    result.row_source = result.resolver.row.has_value()
        ? ActionMotionInvocationRowSource::ResolverOutput
        : ActionMotionInvocationRowSource::None;
    return result;
}

void set_install(
    ActionMotionInvocationResult& result,
    int next_state,
    ActionMotionPlaybackContinuation continuation,
    std::string callsite) {
    result.decision = ActionMotionInvocationDecisionKind::InstallPlayback;
    result.callback_state_after = next_state;
    result.playback_continuation = continuation;
    result.operation_callsite = std::move(callsite);
}

void set_load(
    ActionMotionInvocationResult& result,
    ActionMotionInvocationDecisionKind decision,
    ActionMotionInvocationRowSource row_source,
    int next_state,
    std::string callsite) {
    result.decision = decision;
    result.row_source = row_source;
    result.callback_state_after = next_state;
    result.operation_callsite = std::move(callsite);
}

ActionMotionInvocationResult resolve_basic_callback(
    const std::vector<CombatantStdActionRow>& rows,
    const ActionMotionInvocationRequest& request) {
    ActionMotionInvocationRequest effective = request;
    ActionMotionInvocationStatus entry_status = ActionMotionInvocationStatus::Matched;
    std::string entry_provenance;

    if (effective.callback_state <= 2) {
        if (effective.callback_ready.has_value() && !*effective.callback_ready) {
            ActionMotionInvocationResult waiting;
            waiting.status = ActionMotionInvocationStatus::Matched;
            waiting.decision = ActionMotionInvocationDecisionKind::Wait;
            waiting.callback_state_before = request.callback_state;
            waiting.callback_state_after = request.callback_state;
            waiting.provenance =
                "FUN_8001B1B0 readiness predicates retained the pre-dispatch callback state";
            return waiting;
        }
        effective.callback_state = 3;
        if (!effective.callback_ready.has_value()) {
            entry_status = ActionMotionInvocationStatus::Provisional;
            entry_provenance =
                "initial readiness predicates are not yet modeled; the validated state-3 resolver boundary was used provisionally; ";
        }
    }

    ActionMotionInvocationResult result;
    switch (effective.callback_state) {
    case 3: {
        const bool mode3_route = effective.basic_uses_mode_3_route.value_or(false);
        result = invoke_resolver(
            rows,
            effective,
            mode3_route ? static_cast<std::int16_t>(3) : effective.instruction_mode,
            mode3_route ? std::optional<std::int16_t>{}
                        : effective.instruction_subtype,
            mode3_route ? "FUN_8001B1B0.state3.mode3"
                        : "FUN_8001B1B0.state3.current_mode");
        if (!effective.basic_uses_mode_3_route.has_value()) {
            lower_status(ActionMotionInvocationStatus::Provisional, result.status);
            result.provenance +=
                "FUN_8001FABC route result is unavailable; current-mode route selected provisionally; ";
        }
        if (result.resolver.code == ActionMotionResolverCode::InstallPlayback) {
            set_install(
                result,
                mode3_route ? 5 : 6,
                mode3_route
                    ? ActionMotionPlaybackContinuation::State5LoadLookedUpTo4
                    : ActionMotionPlaybackContinuation::State6PostDelayTo11,
                mode3_route ? "FUN_8001B1B0.state3.install_mode3"
                            : "FUN_8001B1B0.state3.install_current_mode");
        } else if (result.resolver.code
            == ActionMotionResolverCode::LoadWithoutPlayback) {
            set_load(
                result,
                mode3_route
                    ? ActionMotionInvocationDecisionKind::LoadLookedUp
                    : ActionMotionInvocationDecisionKind::LoadSelected,
                mode3_route
                    ? ActionMotionInvocationRowSource::LookedUpActionRow
                    : ActionMotionInvocationRowSource::SelectedInstructionRow,
                mode3_route ? 4 : 8,
                mode3_route ? "STD.LoadLookedUpActionRowMotion"
                            : "STD.LoadSelectedActionRowMotion");
            if (!mode3_route
                && effective.selected_instruction_row.has_value()) {
                result.operation_row = effective.selected_instruction_row;
            }
        } else {
            result.decision = ActionMotionInvocationDecisionKind::Wait;
            result.callback_state_after = mode3_route ? 4 : 8;
        }
        break;
    }
    case 4:
        if (effective.rotation_complete.has_value()
            && !*effective.rotation_complete) {
            result.status = ActionMotionInvocationStatus::Matched;
            result.decision = ActionMotionInvocationDecisionKind::Wait;
            result.callback_state_before = 4;
            result.callback_state_after = 4;
            result.provenance =
                "FUN_8001B1B0 state 4 retained control while rotation remained incomplete";
            break;
        }
        result = invoke_resolver(
            rows,
            effective,
            effective.instruction_mode,
            effective.instruction_subtype,
            "FUN_8001B1B0.state4.rotation_complete");
        if (!effective.rotation_complete.has_value()) {
            lower_status(ActionMotionInvocationStatus::Provisional, result.status);
            result.provenance +=
                "rotation completion was not supplied and was assumed at the resolver boundary; ";
        }
        if (result.resolver.code == ActionMotionResolverCode::InstallPlayback) {
            set_install(
                result,
                6,
                ActionMotionPlaybackContinuation::State6PostDelayTo11,
                "FUN_8001B1B0.state4.install_current_mode");
        } else if (result.resolver.code
            == ActionMotionResolverCode::LoadWithoutPlayback) {
            set_load(
                result,
                ActionMotionInvocationDecisionKind::LoadSelected,
                ActionMotionInvocationRowSource::SelectedInstructionRow,
                8,
                "STD.LoadSelectedActionRowMotion");
            if (effective.selected_instruction_row.has_value()) {
                result.operation_row = effective.selected_instruction_row;
            }
        } else {
            result.decision = ActionMotionInvocationDecisionKind::Wait;
            result.callback_state_after = 8;
        }
        break;
    case 5:
    case 6:
    case 7:
        result.status = ActionMotionInvocationStatus::Matched;
        result.decision = ActionMotionInvocationDecisionKind::Wait;
        result.callback_state_before = effective.callback_state;
        result.callback_state_after = effective.callback_state;
        result.provenance =
            "FUN_8001B1B0 playback wait state is owned by ActionMotionPlaybackModel";
        break;
    case 8:
        result.callback_state_before = 8;
        result.decision = ActionMotionInvocationDecisionKind::Wait;
        if (!effective.state8_descriptor_delay.has_value()) {
            result.status = ActionMotionInvocationStatus::MissingInput;
            result.callback_state_after = 8;
            result.provenance =
                "FUN_8001B1B0 state 8 requires the descriptor-backed state-9 delay";
            break;
        }
        if (*effective.state8_descriptor_delay < 0) {
            result.status = ActionMotionInvocationStatus::Unsupported;
            result.callback_state_after = 8;
            result.provenance =
                "FUN_8001B1B0 state 8 received a negative descriptor delay";
            break;
        }
        result.status = ActionMotionInvocationStatus::Matched;
        result.state8_delay_remaining = *effective.state8_descriptor_delay;
        if (result.state8_delay_remaining == 0) {
            result.callback_state_after = 11;
        } else {
            --result.state8_delay_remaining;
            result.callback_state_after = 9;
        }
        result.provenance =
            "FUN_8001B1B0 state 8 loaded the descriptor delay and executed the first state-9 visit";
        break;
    case 9:
        result.callback_state_before = 9;
        result.decision = ActionMotionInvocationDecisionKind::Wait;
        if (effective.state8_delay_remaining < 0) {
            result.status = ActionMotionInvocationStatus::MissingInput;
            result.callback_state_after = 9;
            result.provenance =
                "FUN_8001B1B0 state 9 has no retained descriptor delay";
            break;
        }
        result.status = ActionMotionInvocationStatus::Matched;
        result.state8_delay_remaining = effective.state8_delay_remaining;
        if (result.state8_delay_remaining == 0) {
            result.callback_state_after = 11;
        } else {
            --result.state8_delay_remaining;
            result.callback_state_after = 9;
        }
        result.provenance =
            "FUN_8001B1B0 state 9 decremented or completed the descriptor delay";
        break;
    case 10:
        result.status = ActionMotionInvocationStatus::Matched;
        result.decision = ActionMotionInvocationDecisionKind::Wait;
        result.callback_state_before = 10;
        result.callback_state_after = 11;
        result.provenance =
            "FUN_8001B1B0 state 10 published the state-11 transition";
        break;
    case 11: {
        const int motion_result = effective.post_motion_result.value_or(2);
        if (!effective.post_motion_result.has_value()) {
            entry_status = ActionMotionInvocationStatus::Provisional;
            entry_provenance +=
                "post-motion result is not yet produced by the frame runtime and result 2 was used provisionally; ";
        }
        if (motion_result != 2) {
            result.status = entry_status;
            result.decision = motion_result == 1
                ? ActionMotionInvocationDecisionKind::Restore
                : ActionMotionInvocationDecisionKind::Wait;
            result.callback_state_before = 11;
            result.callback_state_after = motion_result == 1 ? 17 : 11;
            result.provenance = entry_provenance
                + "FUN_8001B1B0 state 11 did not enter the mode-2 resolver branch";
            return result;
        }
        result = invoke_resolver(
            rows,
            effective,
            2,
            std::nullopt,
            "FUN_8001B1B0.state11.mode2");
        if (result.resolver.code == ActionMotionResolverCode::InstallPlayback
            || result.resolver.code
                == ActionMotionResolverCode::LoadWithoutPlayback) {
            set_load(
                result,
                ActionMotionInvocationDecisionKind::LoadLookedUp,
                ActionMotionInvocationRowSource::LookedUpActionRow,
                12,
                "STD.LoadLookedUpActionRowMotion");
        } else {
            result.decision = ActionMotionInvocationDecisionKind::Wait;
            result.callback_state_after = 17;
        }
        break;
    }
    case 12:
    case 17:
        result.status = ActionMotionInvocationStatus::Provisional;
        result.decision = ActionMotionInvocationDecisionKind::Restore;
        result.callback_state_before = effective.callback_state;
        result.callback_state_after = 15;
        result.provenance =
            "FUN_8001B1B0 cleanup prerequisites are not fully modeled; the observed final mode-2 resolver state was staged provisionally";
        break;
    case 15:
        result = invoke_resolver(
            rows,
            effective,
            2,
            std::nullopt,
            "FUN_8001B1B0.state15.mode2");
        if (result.resolver.code != ActionMotionResolverCode::NoChange) {
            set_install(
                result,
                7,
                ActionMotionPlaybackContinuation::State7LoadLookedUpTo14,
                "FUN_8001B1B0.state15.install_mode2");
        } else {
            result.decision = ActionMotionInvocationDecisionKind::Release;
            result.callback_state_after = 14;
        }
        break;
    case 13:
    case 14:
        result.status = ActionMotionInvocationStatus::Matched;
        result.decision = ActionMotionInvocationDecisionKind::Release;
        result.callback_state_before = effective.callback_state;
        result.callback_state_after = effective.callback_state;
        result.provenance =
            "FUN_8001B1B0 reached a terminal restore/release state";
        break;
    default:
        result.status = ActionMotionInvocationStatus::Unsupported;
        result.decision = ActionMotionInvocationDecisionKind::Unsupported;
        result.callback_state_before = effective.callback_state;
        result.callback_state_after = effective.callback_state;
        result.provenance =
            "FUN_8001B1B0 callback state is outside the modeled invocation contract";
        break;
    }

    lower_status(entry_status, result.status);
    result.provenance = entry_provenance + result.provenance
        + result.resolver.provenance;
    result.callback_state_before = request.callback_state;
    return result;
}

ActionMotionInvocationResult resolve_special_callback(
    const std::vector<CombatantStdActionRow>& rows,
    const ActionMotionInvocationRequest& request) {
    ActionMotionInvocationRequest effective = request;
    if (effective.callback_state == 0) {
        effective.callback_state = 1;
    }
    if (effective.callback_state == 1) {
        if (!effective.post_motion_result.has_value()) {
            ActionMotionInvocationResult waiting;
            waiting.status = ActionMotionInvocationStatus::Provisional;
            waiting.decision = ActionMotionInvocationDecisionKind::Wait;
            waiting.callback_state_before = request.callback_state;
            waiting.callback_state_after = 1;
            waiting.provenance =
                "FUN_8001A4F0 state 1 requires the action-motion result before choosing its load or state-9 install path";
            return waiting;
        }
        if (*effective.post_motion_result == 2) {
            auto result = invoke_resolver(
                rows,
                effective,
                2,
                std::nullopt,
                "FUN_8001A4F0.state1.mode2");
            set_load(
                result,
                ActionMotionInvocationDecisionKind::LoadLookedUp,
                ActionMotionInvocationRowSource::LookedUpActionRow,
                4,
                "STD.LoadLookedUpActionRowMotion");
            result.callback_state_before = request.callback_state;
            return result;
        }
        ActionMotionInvocationResult result;
        result.status = ActionMotionInvocationStatus::Matched;
        result.decision = ActionMotionInvocationDecisionKind::Wait;
        result.callback_state_before = request.callback_state;
        result.callback_state_after = *effective.post_motion_result == 1 ? 9 : 1;
        result.provenance =
            "FUN_8001A4F0 state 1 consumed the modeled action-motion result";
        return result;
    }
    if (effective.callback_state == 9) {
        auto result = invoke_resolver(
            rows,
            effective,
            2,
            std::nullopt,
            "FUN_8001A4F0.state9.mode2");
        if (result.resolver.code != ActionMotionResolverCode::NoChange) {
            set_install(
                result,
                10,
                ActionMotionPlaybackContinuation::GenericRelease,
                "FUN_8001A4F0.state9.install_mode2");
        } else {
            result.decision = ActionMotionInvocationDecisionKind::Wait;
            result.callback_state_after = 11;
        }
        result.callback_state_before = request.callback_state;
        return result;
    }
    ActionMotionInvocationResult result;
    result.status = ActionMotionInvocationStatus::Unsupported;
    result.decision = ActionMotionInvocationDecisionKind::Unsupported;
    result.callback_state_before = request.callback_state;
    result.callback_state_after = request.callback_state;
    result.provenance =
        "FUN_8001A4F0 callback state has no modeled invocation boundary";
    return result;
}

ActionMotionInvocationResult resolve_ranged_callback(
    const std::vector<CombatantStdActionRow>& rows,
    const ActionMotionInvocationRequest& request) {
    ActionMotionInvocationRequest effective = request;
    if (effective.callback_state <= 1) {
        effective.callback_state = 5;
    }
    if (effective.callback_state != 5) {
        ActionMotionInvocationResult result;
        result.status = ActionMotionInvocationStatus::Unsupported;
        result.decision = ActionMotionInvocationDecisionKind::Unsupported;
        result.callback_state_before = request.callback_state;
        result.callback_state_after = request.callback_state;
        result.provenance =
            "FUN_80019F0C callback state has no modeled invocation boundary";
        return result;
    }
    const bool use_current_mode = effective.ranged_flag_0x2_set.value_or(false);
    auto result = invoke_resolver(
        rows,
        effective,
        use_current_mode ? effective.instruction_mode : static_cast<std::int16_t>(7),
        use_current_mode ? effective.instruction_subtype
                         : std::optional<std::int16_t>{},
        use_current_mode ? "FUN_80019F0C.state5.current_mode"
                         : "FUN_80019F0C.state5.mode7");
    if (!effective.ranged_flag_0x2_set.has_value()) {
        lower_status(ActionMotionInvocationStatus::Provisional, result.status);
        result.provenance +=
            "IW+0xEC bit 1 is not yet modeled; the mode-7 branch was selected provisionally; ";
    }
    if (result.resolver.code == ActionMotionResolverCode::InstallPlayback) {
        set_install(
            result,
            use_current_mode ? 8 : 9,
            use_current_mode
                ? ActionMotionPlaybackContinuation::GenericRelease
                : ActionMotionPlaybackContinuation::State7LoadLookedUpTo14,
            "FUN_80019F0C.state5.install");
    } else if (use_current_mode) {
        set_load(
            result,
            ActionMotionInvocationDecisionKind::LoadSelected,
            ActionMotionInvocationRowSource::SelectedInstructionRow,
            2,
            "STD.LoadSelectedActionRowMotion");
        if (effective.selected_instruction_row.has_value()) {
            result.operation_row = effective.selected_instruction_row;
        }
    } else if (result.resolver.code
        == ActionMotionResolverCode::LoadWithoutPlayback) {
        set_load(
            result,
            ActionMotionInvocationDecisionKind::LoadLookedUp,
            ActionMotionInvocationRowSource::LookedUpActionRow,
            2,
            "STD.LoadLookedUpActionRowMotion");
    } else {
        result.decision = ActionMotionInvocationDecisionKind::Wait;
        result.callback_state_after = 2;
    }
    result.callback_state_before = request.callback_state;
    result.provenance += result.resolver.provenance;
    return result;
}

} // namespace

ActionMotionPersistentCallbackFamily action_motion_callback_family_for_index(
    std::int16_t callback_index) {
    switch (callback_index) {
    case 0: return ActionMotionPersistentCallbackFamily::GenericStanding_80073D10;
    case 1: return ActionMotionPersistentCallbackFamily::Callback_8007E88C;
    case 2: return ActionMotionPersistentCallbackFamily::Callback_8007E690;
    case 3: return ActionMotionPersistentCallbackFamily::Callback_80021CBC;
    case 4: return ActionMotionPersistentCallbackFamily::Callback_8007E4F8;
    case 5: return ActionMotionPersistentCallbackFamily::Callback_8007E44C;
    case 6: return ActionMotionPersistentCallbackFamily::GenericStanding_80073D0C;
    case 7: return ActionMotionPersistentCallbackFamily::PersistentDispatch_80022850;
    case 8: return ActionMotionPersistentCallbackFamily::ActionMotionBasic_8001B1B0;
    case 9: return ActionMotionPersistentCallbackFamily::ActionMotionSync_8001AB60;
    case 10: return ActionMotionPersistentCallbackFamily::ActionMotionSpecial_8001A4F0;
    case 11: return ActionMotionPersistentCallbackFamily::ActionMotion_80019E44;
    case 12: return ActionMotionPersistentCallbackFamily::ActionMotion_80019D7C;
    case 13: return ActionMotionPersistentCallbackFamily::ActionMotionRanged_80019F0C;
    case 14: return ActionMotionPersistentCallbackFamily::ActionMotion_8001977C;
    case 15: return ActionMotionPersistentCallbackFamily::ActionMotion_800193B8;
    case 16: return ActionMotionPersistentCallbackFamily::ActionMotion_80018084;
    case 17: return ActionMotionPersistentCallbackFamily::Callback_80021FCC;
    case 18: return ActionMotionPersistentCallbackFamily::ActionMotion_80017B18;
    case 19: return ActionMotionPersistentCallbackFamily::ActionMotion_80017A7C;
    case 20: return ActionMotionPersistentCallbackFamily::GenericStanding_80073D14;
    default: return ActionMotionPersistentCallbackFamily::Unknown;
    }
}

ActionMotionRowResolverResult resolve_action_motion_row_8001ecb4(
    const std::vector<CombatantStdActionRow>& rows,
    const ActionMotionRowResolverRequest& request) {
    ActionMotionRowResolverResult result;
    result.requested_mode = request.requested_mode;
    result.normalized_mode = request.requested_mode == 0x16
        ? static_cast<std::int16_t>(0x15)
        : request.requested_mode;

    if (rows.empty() || result.normalized_mode < 0) {
        result.status = ActionMotionInvocationStatus::MissingInput;
        result.provenance = rows.empty()
            ? "FUN_8001ECB4 has no STD action rows"
            : "FUN_8001ECB4 has no requested action mode";
        return result;
    }
    if (requires_secondary_key(result.normalized_mode)
        && !request.secondary_key.has_value()) {
        result.status = ActionMotionInvocationStatus::MissingInput;
        result.provenance =
            "FUN_8001ECB4 requires the secondary instruction key for this action mode";
        return result;
    }

    result.row = find_action_row(
        rows, result.normalized_mode, request.secondary_key);
    if (!result.row.has_value()) {
        result.status = ActionMotionInvocationStatus::Matched;
        result.code = ActionMotionResolverCode::NoChange;
        result.provenance =
            "FUN_8001ECB4 reached the type-3 terminator without a matching action row";
        return result;
    }

    if (request.selection_blocked.value_or(false)) {
        result.status = ActionMotionInvocationStatus::Matched;
        result.code = ActionMotionResolverCode::NoChange;
        result.provenance =
            "FUN_8001C1F0 blocked the selected row before motion resolution";
        return result;
    }
    if (!request.selection_blocked.has_value()) {
        result.status = ActionMotionInvocationStatus::Provisional;
        result.assumed_unblocked = true;
    } else {
        result.status = ActionMotionInvocationStatus::Matched;
    }

    if ((result.normalized_mode == 2 || result.normalized_mode == 7)
        && request.mode_2_or_7_override_row.has_value()) {
        result.row = request.mode_2_or_7_override_row;
        result.used_mode_2_or_7_override = true;
    }
    result.resolved_motion_id = result.row->callback_ordinal;

    const bool resource_present =
        request.current_motion_resource_present.value_or(false);
    if (!request.current_motion_resource_present.has_value()) {
        result.assumed_no_current_motion_resource = true;
        lower_status(ActionMotionInvocationStatus::Provisional, result.status);
    }
    if (resource_present) {
        if (!request.current_motion_id.has_value()) {
            result.status = ActionMotionInvocationStatus::MissingInput;
            result.provenance =
                "FUN_8001ECB4 has a current motion resource but no current motion identity";
            return result;
        }
        if (*request.current_motion_id == *result.resolved_motion_id) {
            result.code = ActionMotionResolverCode::NoChange;
            result.provenance =
                "FUN_8001ECB4 resolved the currently loaded motion identity";
            return result;
        }
    }

    const float duration = std::bit_cast<float>(
        result.row->transition_gate_divisor_bits);
    if (!std::isfinite(duration)) {
        result.status = ActionMotionInvocationStatus::Unsupported;
        result.provenance =
            "FUN_8001ECB4 selected a row with a non-finite duration field";
        return result;
    }
    if (duration == 0.0f) {
        if (!request.instruction_flags_0xf0.has_value()) {
            result.status = ActionMotionInvocationStatus::MissingInput;
            result.provenance =
                "FUN_8001ECB4 needs IW+0xF0 to classify a zero-duration row";
            return result;
        }
        if ((*request.instruction_flags_0xf0 & 0x00040000u) == 0) {
            result.code = ActionMotionResolverCode::LoadWithoutPlayback;
            result.provenance =
                "FUN_8001ECB4 selected a zero-duration row with IW+0xF0 bit 18 clear";
            return result;
        }
    }

    result.code = ActionMotionResolverCode::InstallPlayback;
    std::ostringstream detail;
    detail << "FUN_8001ECB4 selected row " << result.row->index
           << " with resolved motion " << *result.resolved_motion_id
           << "; install eligibility is established";
    if (result.assumed_unblocked) {
        detail << "; FUN_8001C1F0 was assumed false";
    }
    if (result.assumed_no_current_motion_resource) {
        detail << "; current motion resource was assumed absent";
    }
    result.provenance = detail.str();
    return result;
}

ActionMotionInvocationResult resolve_action_motion_invocation(
    const std::vector<CombatantStdActionRow>& rows,
    const ActionMotionInvocationRequest& request) {
    switch (request.callback_family) {
    case ActionMotionPersistentCallbackFamily::ActionMotionBasic_8001B1B0:
        return resolve_basic_callback(rows, request);
    case ActionMotionPersistentCallbackFamily::ActionMotionSync_8001AB60: {
        ActionMotionInvocationResult result;
        result.status = ActionMotionInvocationStatus::Provisional;
        result.decision = ActionMotionInvocationDecisionKind::Wait;
        result.callback_state_before = request.callback_state;
        result.callback_state_after = request.callback_state;
        result.provenance =
            "FUN_8001AB60 is the validated state-0 persistent callback; "
            "the captured action-motion corpus observed no playback installation "
            "from this family, while its non-playback synchronization internals "
            "remain outside the invocation model";
        return result;
    }
    case ActionMotionPersistentCallbackFamily::ActionMotionSpecial_8001A4F0:
        return resolve_special_callback(rows, request);
    case ActionMotionPersistentCallbackFamily::ActionMotionRanged_80019F0C:
        return resolve_ranged_callback(rows, request);
    default: {
        ActionMotionInvocationResult result;
        result.status = ActionMotionInvocationStatus::Unsupported;
        result.decision = ActionMotionInvocationDecisionKind::Unsupported;
        result.callback_state_before = request.callback_state;
        result.callback_state_after = request.callback_state;
        result.provenance =
            "persistent callback family has no evidence-backed action-motion invocation policy";
        return result;
    }
    }
}

const char* action_motion_invocation_status_name(ActionMotionInvocationStatus status) {
    switch (status) {
    case ActionMotionInvocationStatus::Matched: return "Matched";
    case ActionMotionInvocationStatus::Provisional: return "Provisional";
    case ActionMotionInvocationStatus::MissingInput: return "MissingInput";
    case ActionMotionInvocationStatus::Unsupported: return "Unsupported";
    }
    return "Unsupported";
}

const char* action_motion_resolver_code_name(ActionMotionResolverCode code) {
    switch (code) {
    case ActionMotionResolverCode::NoChange: return "NoChange";
    case ActionMotionResolverCode::LoadWithoutPlayback: return "LoadWithoutPlayback";
    case ActionMotionResolverCode::InstallPlayback: return "InstallPlayback";
    }
    return "NoChange";
}

const char* action_motion_callback_family_name(
    ActionMotionPersistentCallbackFamily family) {
    switch (family) {
    case ActionMotionPersistentCallbackFamily::Unknown: return "Unknown";
    case ActionMotionPersistentCallbackFamily::GenericStanding_80073D10: return "GenericStanding_80073D10";
    case ActionMotionPersistentCallbackFamily::Callback_8007E88C: return "Callback_8007E88C";
    case ActionMotionPersistentCallbackFamily::Callback_8007E690: return "Callback_8007E690";
    case ActionMotionPersistentCallbackFamily::Callback_80021CBC: return "Callback_80021CBC";
    case ActionMotionPersistentCallbackFamily::Callback_8007E4F8: return "Callback_8007E4F8";
    case ActionMotionPersistentCallbackFamily::Callback_8007E44C: return "Callback_8007E44C";
    case ActionMotionPersistentCallbackFamily::GenericStanding_80073D0C: return "GenericStanding_80073D0C";
    case ActionMotionPersistentCallbackFamily::PersistentDispatch_80022850: return "PersistentDispatch_80022850";
    case ActionMotionPersistentCallbackFamily::ActionMotionBasic_8001B1B0: return "ActionMotionBasic_8001B1B0";
    case ActionMotionPersistentCallbackFamily::ActionMotionSync_8001AB60: return "ActionMotionSync_8001AB60";
    case ActionMotionPersistentCallbackFamily::ActionMotionSpecial_8001A4F0: return "ActionMotionSpecial_8001A4F0";
    case ActionMotionPersistentCallbackFamily::ActionMotion_80019E44: return "ActionMotion_80019E44";
    case ActionMotionPersistentCallbackFamily::ActionMotion_80019D7C: return "ActionMotion_80019D7C";
    case ActionMotionPersistentCallbackFamily::ActionMotionRanged_80019F0C: return "ActionMotionRanged_80019F0C";
    case ActionMotionPersistentCallbackFamily::ActionMotion_8001977C: return "ActionMotion_8001977C";
    case ActionMotionPersistentCallbackFamily::ActionMotion_800193B8: return "ActionMotion_800193B8";
    case ActionMotionPersistentCallbackFamily::ActionMotion_80018084: return "ActionMotion_80018084";
    case ActionMotionPersistentCallbackFamily::Callback_80021FCC: return "Callback_80021FCC";
    case ActionMotionPersistentCallbackFamily::ActionMotion_80017B18: return "ActionMotion_80017B18";
    case ActionMotionPersistentCallbackFamily::ActionMotion_80017A7C: return "ActionMotion_80017A7C";
    case ActionMotionPersistentCallbackFamily::GenericStanding_80073D14: return "GenericStanding_80073D14";
    }
    return "Unknown";
}

const char* action_motion_invocation_decision_name(
    ActionMotionInvocationDecisionKind decision) {
    switch (decision) {
    case ActionMotionInvocationDecisionKind::InstallPlayback: return "InstallPlayback";
    case ActionMotionInvocationDecisionKind::LoadSelected: return "LoadSelected";
    case ActionMotionInvocationDecisionKind::LoadLookedUp: return "LoadLookedUp";
    case ActionMotionInvocationDecisionKind::Wait: return "Wait";
    case ActionMotionInvocationDecisionKind::Restore: return "Restore";
    case ActionMotionInvocationDecisionKind::Release: return "Release";
    case ActionMotionInvocationDecisionKind::Unsupported: return "Unsupported";
    }
    return "Unsupported";
}

const char* action_motion_invocation_row_source_name(
    ActionMotionInvocationRowSource source) {
    switch (source) {
    case ActionMotionInvocationRowSource::None: return "None";
    case ActionMotionInvocationRowSource::ResolverOutput: return "ResolverOutput";
    case ActionMotionInvocationRowSource::SelectedInstructionRow: return "SelectedInstructionRow";
    case ActionMotionInvocationRowSource::LookedUpActionRow: return "LookedUpActionRow";
    }
    return "None";
}

} // namespace savor::predict
