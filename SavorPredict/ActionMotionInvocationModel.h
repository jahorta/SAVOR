#pragma once

#include "ActionMotionPlaybackModel.h"
#include "CombatantVisualDispatcherModel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

enum class ActionMotionInvocationStatus {
    Matched,
    Provisional,
    MissingInput,
    Unsupported,
};

enum class ActionMotionResolverCode {
    NoChange = 0,
    LoadWithoutPlayback = 1,
    InstallPlayback = 2,
};

enum class ActionMotionPersistentCallbackFamily {
    Unknown,
    GenericStanding_80073D10,
    Callback_8007E88C,
    Callback_8007E690,
    Callback_80021CBC,
    Callback_8007E4F8,
    Callback_8007E44C,
    GenericStanding_80073D0C,
    PersistentDispatch_80022850,
    ActionMotionBasic_8001B1B0,
    ActionMotionSync_8001AB60,
    ActionMotionSpecial_8001A4F0,
    ActionMotion_80019E44,
    ActionMotion_80019D7C,
    ActionMotionRanged_80019F0C,
    ActionMotion_8001977C,
    ActionMotion_800193B8,
    ActionMotion_80018084,
    Callback_80021FCC,
    ActionMotion_80017B18,
    ActionMotion_80017A7C,
    GenericStanding_80073D14,
};

enum class ActionMotionInvocationDecisionKind {
    InstallPlayback,
    LoadSelected,
    LoadLookedUp,
    Wait,
    Restore,
    Release,
    Unsupported,
};

enum class ActionMotionInvocationRowSource {
    None,
    ResolverOutput,
    SelectedInstructionRow,
    LookedUpActionRow,
};

struct ActionMotionRowResolverRequest {
    std::int16_t requested_mode = -1;
    std::optional<std::int16_t> secondary_key;
    std::optional<bool> selection_blocked;
    std::optional<bool> current_motion_resource_present;
    std::optional<std::int16_t> current_motion_id;
    std::optional<std::uint32_t> instruction_flags_0xf0;
    std::optional<CombatantStdActionRow> mode_2_or_7_override_row;
};

struct ActionMotionRowResolverResult {
    ActionMotionInvocationStatus status = ActionMotionInvocationStatus::MissingInput;
    ActionMotionResolverCode code = ActionMotionResolverCode::NoChange;
    std::int16_t requested_mode = -1;
    std::int16_t normalized_mode = -1;
    std::optional<CombatantStdActionRow> row;
    std::optional<std::int16_t> resolved_motion_id;
    bool used_mode_2_or_7_override = false;
    bool assumed_unblocked = false;
    bool assumed_no_current_motion_resource = false;
    std::string provenance;
};

struct ActionMotionInvocationRequest {
    ActionMotionPersistentCallbackFamily callback_family =
        ActionMotionPersistentCallbackFamily::Unknown;
    int callback_state = 0;
    std::int16_t instruction_mode = -1;
    std::optional<std::int16_t> instruction_subtype;
    std::uint32_t instruction_flags_0xf0 = 0;
    std::optional<bool> callback_ready;
    std::optional<bool> basic_uses_mode_3_route;
    std::optional<bool> rotation_complete;
    std::optional<int> post_motion_result;
    std::optional<int> state8_descriptor_delay;
    int state8_delay_remaining = -1;
    std::optional<bool> special_motion_complete;
    std::optional<bool> ranged_flag_0x2_set;
    std::optional<bool> selection_blocked;
    std::optional<bool> current_motion_resource_present;
    std::optional<std::int16_t> current_motion_id;
    std::optional<CombatantStdActionRow> selected_instruction_row;
    std::optional<CombatantStdActionRow> mode_2_or_7_override_row;
};

struct ActionMotionInvocationResult {
    ActionMotionInvocationStatus status = ActionMotionInvocationStatus::MissingInput;
    ActionMotionInvocationDecisionKind decision =
        ActionMotionInvocationDecisionKind::Unsupported;
    ActionMotionInvocationRowSource row_source =
        ActionMotionInvocationRowSource::None;
    int callback_state_before = 0;
    int callback_state_after = 0;
    bool resolver_called = false;
    ActionMotionRowResolverResult resolver{};
    std::optional<CombatantStdActionRow> operation_row;
    ActionMotionPlaybackContinuation playback_continuation =
        ActionMotionPlaybackContinuation::State6PostDelayTo11;
    int state8_delay_remaining = -1;
    bool auxiliary_publication_requested = false;
    bool entered_state10_via_fallthrough = false;
    std::string resolver_callsite;
    std::string operation_callsite;
    std::string provenance;
};

ActionMotionPersistentCallbackFamily action_motion_callback_family_for_index(
    std::int16_t callback_index);

ActionMotionRowResolverResult resolve_action_motion_row_8001ecb4(
    const std::vector<CombatantStdActionRow>& rows,
    const ActionMotionRowResolverRequest& request);

ActionMotionInvocationResult resolve_action_motion_invocation(
    const std::vector<CombatantStdActionRow>& rows,
    const ActionMotionInvocationRequest& request);

const char* action_motion_invocation_status_name(ActionMotionInvocationStatus status);
const char* action_motion_resolver_code_name(ActionMotionResolverCode code);
const char* action_motion_callback_family_name(
    ActionMotionPersistentCallbackFamily family);
const char* action_motion_invocation_decision_name(
    ActionMotionInvocationDecisionKind decision);
const char* action_motion_invocation_row_source_name(
    ActionMotionInvocationRowSource source);

} // namespace savor::predict
