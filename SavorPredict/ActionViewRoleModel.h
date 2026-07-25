#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace savor::predict {

enum class ActionViewRoleStatus {
    Matched,
    Provisional,
    MissingInput,
};

struct ActionViewRoleResolutionRequest {
    std::optional<int> acting_actor_slot;
    std::optional<int> queued_target_slot;
    std::optional<std::uint32_t> target_instruction_flags_0xf0;
    std::optional<bool> target_present;
};

struct ActionViewRoleResolutionResult {
    ActionViewRoleStatus status = ActionViewRoleStatus::MissingInput;
    int actor_slot = -1;
    int secondary_slot = -1;
    bool reversed = false;
    std::string provenance;
};

enum class ActionViewRoleFlagProducerPhase {
    State0,
    WaitForMode0B,
    AdvanceFixedSequence,
    PublishRoleFlag,
    WaitForRelease,
    Complete,
};

struct ActionViewRoleFlagProducerState {
    ActionViewRoleFlagProducerPhase phase =
        ActionViewRoleFlagProducerPhase::State0;
    bool role_flag_owned = false;
};

struct ActionViewRoleFlagProducerVisitRequest {
    ActionViewRoleFlagProducerState state{};
    std::optional<std::int16_t> instruction_mode_0x6;
    std::optional<std::uint8_t> turn_phase;
    std::optional<bool> override_view_thread_present;
};

struct ActionViewRoleFlagProducerVisitResult {
    ActionViewRoleStatus status = ActionViewRoleStatus::MissingInput;
    ActionViewRoleFlagProducerState state{};
    std::optional<std::int16_t> requested_instruction_mode;
    bool set_role_flag = false;
    bool clear_role_flag = false;
    bool complete = false;
    std::string provenance;
};

ActionViewRoleResolutionResult resolve_action_view_roles_8001d41c(
    const ActionViewRoleResolutionRequest& request);

ActionViewRoleFlagProducerVisitResult
visit_action_view_role_flag_producer_80019b70(
    const ActionViewRoleFlagProducerVisitRequest& request);

const char* action_view_role_status_name(ActionViewRoleStatus status);
const char* action_view_role_flag_producer_phase_name(
    ActionViewRoleFlagProducerPhase phase);

} // namespace savor::predict
