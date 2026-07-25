#include "ActionViewRoleModel.h"

#include <utility>

namespace savor::predict {
namespace {

constexpr std::uint32_t kRoleReversalFlag = 0x00000004u;

} // namespace

ActionViewRoleResolutionResult resolve_action_view_roles_8001d41c(
    const ActionViewRoleResolutionRequest& request) {
    ActionViewRoleResolutionResult result;
    if (!request.acting_actor_slot.has_value()
        || !request.queued_target_slot.has_value()
        || !request.target_instruction_flags_0xf0.has_value()
        || !request.target_present.has_value()) {
        result.provenance =
            "FUN_8001D41C requires the acting actor, queued target, target "
            "presence, and target IW+0xF0";
        return result;
    }
    if (*request.acting_actor_slot < 0
        || *request.queued_target_slot < 0
        || !*request.target_present) {
        result.provenance =
            "FUN_8001D41C cannot resolve roles for an absent actor or target";
        return result;
    }

    result.status = ActionViewRoleStatus::Matched;
    result.actor_slot = *request.acting_actor_slot;
    result.secondary_slot = *request.queued_target_slot;
    result.reversed =
        (*request.target_instruction_flags_0xf0 & kRoleReversalFlag) != 0;
    if (result.reversed) {
        std::swap(result.actor_slot, result.secondary_slot);
    }
    result.provenance = result.reversed
        ? "FUN_8001D41C observed target IW+0xF0 bit 0x00000004 and swapped "
          "the acting actor with the queued target"
        : "FUN_8001D41C retained the acting actor and queued target";
    return result;
}

ActionViewRoleFlagProducerVisitResult
visit_action_view_role_flag_producer_80019b70(
    const ActionViewRoleFlagProducerVisitRequest& request) {
    ActionViewRoleFlagProducerVisitResult result;
    result.state = request.state;

    switch (request.state.phase) {
    case ActionViewRoleFlagProducerPhase::State0:
        result.status = ActionViewRoleStatus::Matched;
        result.state.phase =
            ActionViewRoleFlagProducerPhase::WaitForMode0B;
        result.requested_instruction_mode = 0x0b;
        result.provenance =
            "FUN_80019B70 state 0 published fixed comparison mode 0x0B "
            "from the child payload created by FUN_80019D7C";
        return result;

    case ActionViewRoleFlagProducerPhase::WaitForMode0B:
        if (!request.instruction_mode_0x6.has_value()) {
            result.provenance =
                "FUN_80019B70 state 1 requires current IW+0x6";
            return result;
        }
        result.status = ActionViewRoleStatus::Matched;
        if (*request.instruction_mode_0x6 == 0x0b) {
            result.state.phase =
                ActionViewRoleFlagProducerPhase::AdvanceFixedSequence;
            result.provenance =
                "FUN_80019B70 state 1 matched the first fixed sequence mode "
                "0x0B";
        } else {
            result.provenance =
                "FUN_80019B70 state 1 is waiting for fixed sequence mode 0x0B";
        }
        return result;

    case ActionViewRoleFlagProducerPhase::AdvanceFixedSequence:
        result.status = ActionViewRoleStatus::Matched;
        result.state.phase =
            ActionViewRoleFlagProducerPhase::PublishRoleFlag;
        result.requested_instruction_mode = 5;
        result.provenance =
            "FUN_80019B70 state 2 published fixed comparison mode 5 and "
            "advanced the child payload to its terminator";
        return result;

    case ActionViewRoleFlagProducerPhase::PublishRoleFlag:
        result.status = ActionViewRoleStatus::Matched;
        result.state.phase =
            ActionViewRoleFlagProducerPhase::WaitForRelease;
        result.state.role_flag_owned = true;
        result.set_role_flag = true;
        result.provenance =
            "FUN_80019B70 state 3 published IW+0xF0 bit 0x00000004 at "
            "0x80019CFC";
        return result;

    case ActionViewRoleFlagProducerPhase::WaitForRelease: {
        const bool release_for_turn_phase =
            request.turn_phase.has_value() && *request.turn_phase == 5;
        const bool release_for_override =
            request.override_view_thread_present.value_or(false);
        if (release_for_turn_phase || release_for_override) {
            result.status = ActionViewRoleStatus::Matched;
            result.state.phase =
                ActionViewRoleFlagProducerPhase::Complete;
            result.state.role_flag_owned = false;
            result.clear_role_flag = true;
            result.complete = true;
            result.provenance =
                "FUN_80019B70 state 4 cleared IW+0xF0 bit 0x00000004 at "
                "0x80019D4C/0x80019D68";
            return result;
        }

        result.status = request.turn_phase.has_value()
                || request.override_view_thread_present.has_value()
            ? ActionViewRoleStatus::Matched
            : ActionViewRoleStatus::Provisional;
        result.provenance =
            "FUN_80019B70 state 4 is waiting for turn phase 5 or an active "
            "override-view thread";
        return result;
    }

    case ActionViewRoleFlagProducerPhase::Complete:
        result.status = ActionViewRoleStatus::Matched;
        result.complete = true;
        result.provenance =
            "FUN_80019B70 role-flag child is complete";
        return result;
    }

    result.provenance = "unknown FUN_80019B70 producer phase";
    return result;
}

const char* action_view_role_status_name(ActionViewRoleStatus status) {
    switch (status) {
    case ActionViewRoleStatus::Matched: return "Matched";
    case ActionViewRoleStatus::Provisional: return "Provisional";
    case ActionViewRoleStatus::MissingInput: return "MissingInput";
    }
    return "MissingInput";
}

const char* action_view_role_flag_producer_phase_name(
    ActionViewRoleFlagProducerPhase phase) {
    switch (phase) {
    case ActionViewRoleFlagProducerPhase::State0: return "State0";
    case ActionViewRoleFlagProducerPhase::WaitForMode0B:
        return "WaitForMode0B";
    case ActionViewRoleFlagProducerPhase::AdvanceFixedSequence:
        return "AdvanceFixedSequence";
    case ActionViewRoleFlagProducerPhase::PublishRoleFlag:
        return "PublishRoleFlag";
    case ActionViewRoleFlagProducerPhase::WaitForRelease:
        return "WaitForRelease";
    case ActionViewRoleFlagProducerPhase::Complete: return "Complete";
    }
    return "Complete";
}

} // namespace savor::predict
