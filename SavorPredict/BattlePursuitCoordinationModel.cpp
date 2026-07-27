#include "BattlePursuitCoordinationModel.h"

#include <utility>

namespace savor::predict {
namespace {

constexpr std::uint8_t kCoordinationOwnerPublishedBit = 0x20;
constexpr std::uint8_t kCoordinationPeerPublishedBit = 0x10;
constexpr std::int8_t kPublishedCountdown = 8;

BattlePursuitCoordinationResult publication_result(
    const BattlePursuitCoordinationInput& input,
    BattlePursuitCoordinationBranch branch,
    std::string provenance) {
    BattlePursuitCoordinationResult result;
    result.status = BattlePursuitCoordinationStatus::Matched;
    result.branch = branch;
    result.publish_state17 = true;
    result.owner_state_after_0x50 = static_cast<std::uint8_t>(
        *input.owner_state_0x50 | kCoordinationOwnerPublishedBit);
    result.peer_state_after_0x50 = static_cast<std::uint8_t>(
        *input.peer_state_0x50 | kCoordinationPeerPublishedBit);
    result.owner_countdown_after_0x51 = kPublishedCountdown;
    result.peer_countdown_after_0x51 = *input.peer_countdown_0x51;
    result.confidence = "high";
    result.provenance = std::move(provenance);
    return result;
}

} // namespace

BattlePursuitCoordinationResult model_battle_pursuit_coordination(
    const BattlePursuitCoordinationInput& input) {
    BattlePursuitCoordinationResult result;
    if (!input.owner_state_0x50.has_value()
        || !input.peer_state_0x50.has_value()
        || !input.owner_countdown_0x51.has_value()
        || !input.peer_countdown_0x51.has_value()) {
        result.provenance =
            "FUN_8008D6B4 state 2 requires both participants' +0x50 and signed +0x51 fields";
        return result;
    }

    result.owner_state_after_0x50 = *input.owner_state_0x50;
    result.peer_state_after_0x50 = *input.peer_state_0x50;
    result.owner_countdown_after_0x51 = *input.owner_countdown_0x51;
    result.peer_countdown_after_0x51 = *input.peer_countdown_0x51;
    if (*input.owner_state_0x50 != 2) {
        result.status = BattlePursuitCoordinationStatus::Unsupported;
        result.branch = BattlePursuitCoordinationBranch::PeerStateWait;
        result.confidence = "medium";
        result.provenance =
            "FUN_8008D6B4 state 2 only enters the captured pairing tree while owner +0x50 equals 2";
        return result;
    }
    if (*input.peer_state_0x50 == 1) {
        return publication_result(
            input,
            BattlePursuitCoordinationBranch::PeerReadyPublication,
            "FUN_8008D6B4 peer-ready branch at 0x8008D7EC published queued state 0x11");
    }
    if (*input.peer_state_0x50 != 2) {
        result.status = BattlePursuitCoordinationStatus::Provisional;
        result.branch = BattlePursuitCoordinationBranch::PeerStateWait;
        result.confidence = "medium";
        result.provenance =
            "FUN_8008D6B4 leaves unsupported peer +0x50 values waiting in state 2";
        return result;
    }

    const int owner_countdown = *input.owner_countdown_0x51;
    const int peer_countdown = *input.peer_countdown_0x51;
    if (owner_countdown > peer_countdown) {
        result.status = BattlePursuitCoordinationStatus::Provisional;
        result.branch = BattlePursuitCoordinationBranch::CountdownOrderingWait;
        result.confidence = "medium";
        result.provenance =
            "FUN_8008D6B4 signed countdown ordering deferred the coordinated publication";
        return result;
    }
    if (owner_countdown > 0) {
        result.status = BattlePursuitCoordinationStatus::Provisional;
        result.branch = BattlePursuitCoordinationBranch::CountdownDecrement;
        result.owner_countdown_after_0x51 =
            static_cast<std::int8_t>(owner_countdown - 1);
        result.confidence = "medium";
        result.provenance =
            "FUN_8008D6B4 decremented positive owner +0x51 at 0x8008D8CC; this branch is statically proven but not present in the accepted live corpus";
        return result;
    }
    return publication_result(
        input,
        BattlePursuitCoordinationBranch::CoordinatedPublication,
        "FUN_8008D6B4 coordinated branch at 0x8008D884 published queued state 0x11");
}

BattlePursuitInstructionPollResult model_battle_pursuit_instruction_poll(
    const BattlePursuitInstructionPollInput& input) {
    BattlePursuitInstructionPollResult result;
    result.fallback_counter_after = input.fallback_counter;
    if (!input.queued_field9.has_value()
        || !input.instruction_mode.has_value()) {
        result.provenance =
            "FUN_8007FFE8 requires queued +0x09 and the current instruction mode";
        return result;
    }

    result.queued_field9_after = *input.queued_field9;
    if (*input.queued_field9 != 0) {
        const bool accepted_mode =
            *input.instruction_mode == 4 || *input.instruction_mode == 8;
        result.status = BattlePursuitCoordinationStatus::Matched;
        result.reason = accepted_mode
            ? BattlePursuitInstructionPollReason::QueuedField9ModeMatch
            : BattlePursuitInstructionPollReason::QueuedField9ModeMismatch;
        result.terminal_result = accepted_mode ? 1 : -1;
        result.confidence = "high";
        result.provenance = accepted_mode
            ? "FUN_8007FFE8 state 0x11 accepted queued +0x09 with instruction mode 4 or 8"
            : "FUN_8007FFE8 state 0x11 returned -1 because queued +0x09 was set under another live mode";
        return result;
    }

    if (!input.readiness.has_value()) {
        result.status = BattlePursuitCoordinationStatus::Provisional;
        result.reason = BattlePursuitInstructionPollReason::ReadinessUnknown;
        result.confidence = "low";
        result.provenance =
            "FUN_8006D1C4 readiness is not scheduled yet, so the poll remains waiting without advancing the fallback counter";
        return result;
    }
    if (!*input.readiness) {
        result.status = BattlePursuitCoordinationStatus::Matched;
        result.reason = BattlePursuitInstructionPollReason::ReadinessFalse;
        result.confidence = "high";
        result.provenance =
            "FUN_8007FFE8 returned zero because FUN_8006D1C4 readiness was false";
        return result;
    }
    if (!input.turn_phase.has_value()) {
        result.status = BattlePursuitCoordinationStatus::MissingInput;
        result.reason = BattlePursuitInstructionPollReason::MissingInput;
        result.provenance =
            "FUN_8007FFE8 requires the current turn phase after readiness succeeds";
        return result;
    }
    if (*input.turn_phase == 4) {
        result.status = BattlePursuitCoordinationStatus::Matched;
        result.reason = BattlePursuitInstructionPollReason::GuardedTurnPhase;
        result.confidence = "high";
        result.provenance =
            "FUN_8007FFE8 suppresses its fallback counter while turn phase equals 4";
        return result;
    }

    const int counter_before = input.fallback_counter;
    result.fallback_counter_after = counter_before + 1;
    if (counter_before > 30 && result.fallback_counter_after > 240) {
        result.status = BattlePursuitCoordinationStatus::Provisional;
        result.reason = BattlePursuitInstructionPollReason::FallbackForcedReady;
        result.terminal_result = 1;
        result.fallback_counter_after = 0;
        result.queued_field9_after = 1;
        result.forced_ready = true;
        result.confidence = "medium";
        result.provenance =
            "FUN_8007FFE8 forced-ready fallback is statically proven but was not reached in the accepted live corpus";
        return result;
    }

    result.status = BattlePursuitCoordinationStatus::Provisional;
    result.reason = BattlePursuitInstructionPollReason::FallbackCounterWait;
    result.confidence = "medium";
    result.provenance =
        "FUN_8007FFE8 incremented the slot-local fallback counter and continued waiting";
    return result;
}

const char* battle_pursuit_coordination_status_name(
    BattlePursuitCoordinationStatus status) {
    switch (status) {
    case BattlePursuitCoordinationStatus::Matched: return "Matched";
    case BattlePursuitCoordinationStatus::Provisional: return "Provisional";
    case BattlePursuitCoordinationStatus::MissingInput: return "MissingInput";
    case BattlePursuitCoordinationStatus::Unsupported: return "Unsupported";
    }
    return "Unsupported";
}

const char* battle_pursuit_coordination_branch_name(
    BattlePursuitCoordinationBranch branch) {
    switch (branch) {
    case BattlePursuitCoordinationBranch::PeerReadyPublication:
        return "PeerReadyPublication";
    case BattlePursuitCoordinationBranch::CoordinatedPublication:
        return "CoordinatedPublication";
    case BattlePursuitCoordinationBranch::CountdownDecrement:
        return "CountdownDecrement";
    case BattlePursuitCoordinationBranch::CountdownOrderingWait:
        return "CountdownOrderingWait";
    case BattlePursuitCoordinationBranch::PeerStateWait:
        return "PeerStateWait";
    case BattlePursuitCoordinationBranch::MissingInput:
        return "MissingInput";
    }
    return "MissingInput";
}

const char* battle_pursuit_instruction_poll_reason_name(
    BattlePursuitInstructionPollReason reason) {
    switch (reason) {
    case BattlePursuitInstructionPollReason::QueuedField9ModeMatch:
        return "QueuedField9ModeMatch";
    case BattlePursuitInstructionPollReason::QueuedField9ModeMismatch:
        return "QueuedField9ModeMismatch";
    case BattlePursuitInstructionPollReason::ReadinessFalse:
        return "ReadinessFalse";
    case BattlePursuitInstructionPollReason::ReadinessUnknown:
        return "ReadinessUnknown";
    case BattlePursuitInstructionPollReason::GuardedTurnPhase:
        return "GuardedTurnPhase";
    case BattlePursuitInstructionPollReason::FallbackCounterWait:
        return "FallbackCounterWait";
    case BattlePursuitInstructionPollReason::FallbackForcedReady:
        return "FallbackForcedReady";
    case BattlePursuitInstructionPollReason::MissingInput:
        return "MissingInput";
    }
    return "MissingInput";
}

} // namespace savor::predict
