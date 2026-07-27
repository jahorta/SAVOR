#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace savor::predict {

enum class BattlePursuitCoordinationStatus {
    Matched,
    Provisional,
    MissingInput,
    Unsupported,
};

enum class BattlePursuitCoordinationBranch {
    PeerReadyPublication,
    CoordinatedPublication,
    CountdownDecrement,
    CountdownOrderingWait,
    PeerStateWait,
    MissingInput,
};

struct BattlePursuitCoordinationInput {
    std::optional<std::uint8_t> owner_state_0x50;
    std::optional<std::uint8_t> peer_state_0x50;
    std::optional<std::int8_t> owner_countdown_0x51;
    std::optional<std::int8_t> peer_countdown_0x51;
};

struct BattlePursuitCoordinationResult {
    BattlePursuitCoordinationStatus status =
        BattlePursuitCoordinationStatus::MissingInput;
    BattlePursuitCoordinationBranch branch =
        BattlePursuitCoordinationBranch::MissingInput;
    bool publish_state17 = false;
    std::uint8_t owner_state_after_0x50 = 0;
    std::uint8_t peer_state_after_0x50 = 0;
    std::int8_t owner_countdown_after_0x51 = 0;
    std::int8_t peer_countdown_after_0x51 = 0;
    std::string confidence;
    std::string provenance;
};

enum class BattlePursuitInstructionPollReason {
    QueuedField9ModeMatch,
    QueuedField9ModeMismatch,
    ReadinessFalse,
    ReadinessUnknown,
    GuardedTurnPhase,
    FallbackCounterWait,
    FallbackForcedReady,
    MissingInput,
};

struct BattlePursuitInstructionPollInput {
    std::optional<std::uint8_t> queued_field9;
    std::optional<std::int16_t> instruction_mode;
    std::optional<bool> readiness;
    std::optional<std::uint8_t> turn_phase;
    int fallback_counter = 0;
};

struct BattlePursuitInstructionPollResult {
    BattlePursuitCoordinationStatus status =
        BattlePursuitCoordinationStatus::MissingInput;
    BattlePursuitInstructionPollReason reason =
        BattlePursuitInstructionPollReason::MissingInput;
    std::optional<int> terminal_result;
    int fallback_counter_after = 0;
    std::uint8_t queued_field9_after = 0;
    bool forced_ready = false;
    std::string confidence;
    std::string provenance;
};

BattlePursuitCoordinationResult model_battle_pursuit_coordination(
    const BattlePursuitCoordinationInput& input);

BattlePursuitInstructionPollResult model_battle_pursuit_instruction_poll(
    const BattlePursuitInstructionPollInput& input);

const char* battle_pursuit_coordination_status_name(
    BattlePursuitCoordinationStatus status);
const char* battle_pursuit_coordination_branch_name(
    BattlePursuitCoordinationBranch branch);
const char* battle_pursuit_instruction_poll_reason_name(
    BattlePursuitInstructionPollReason reason);

} // namespace savor::predict
