#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

enum class ActionMotionPlaybackStatus {
    Matched,
    Provisional,
    MissingInput,
    Unsupported,
};

enum class ActionMotionPlaybackPhase {
    Inactive,
    Primed,
    WaitingForState6,
    State6Satisfied,
    WaitingForPostState6Delay,
    PublicationReleased,
    Unsupported,
};

enum class ActionMotionPlaybackVisitKind {
    None,
    InitialRendererAdvance,
    State6Deferred,
    State6Satisfied,
    PostState6DelayDeferred,
    PostState6DelayUnavailable,
    PublicationReleased,
};

enum class ActionMotionDelayStatus {
    Matched,
    NoMatch,
    MissingInput,
    Malformed,
    Unsupported,
};

// A raw FUN_8001DDE0 table entry. The runtime source owns record discovery;
// this model only evaluates the validated descriptor and instruction fields.
struct ActionMotionDelayDescriptor {
    int record_index = -1;
    std::int16_t location_code = -1;
    std::uint32_t combined_type = 0;
    int payload_size = 0;
    bool payload_in_bounds = false;
    std::vector<std::uint8_t> payload_bytes;
};

struct ActionMotionDelayTable {
    bool table_known = false;
    bool table_is_null = false;
    bool includes_sentinel = false;
    std::vector<ActionMotionDelayDescriptor> descriptors;
};

struct ActionMotionInstructionGateInput {
    std::optional<std::int16_t> current_action_key;
    std::optional<std::int16_t> current_secondary_key;
    std::optional<std::uint32_t> instruction_flags_0xec;
    std::optional<std::int16_t> alternate_a_action_key;
    std::optional<std::int16_t> alternate_a_secondary_key;
    std::optional<std::int16_t> alternate_b_action_key;
    std::optional<std::int16_t> alternate_b_secondary_key;
};

struct ActionMotionDelayLookupResult {
    ActionMotionDelayStatus status = ActionMotionDelayStatus::MissingInput;
    int descriptor_record_index = -1;
    std::optional<int> delay;
    std::optional<bool> gate_result;
    std::string provenance;
};

struct ActionMotionPlaybackVisitInput {
    std::optional<ActionMotionDelayLookupResult> post_state6_delay;
};

struct ActionMotionPlaybackInstallRequest {
    int action_ordinal = -1;
    int slot = -1;
    std::uint64_t instruction_state_revision = 0;
    int selected_action_row_index = -1;
    std::optional<std::uint32_t> selected_action_row_duration_bits;
    std::uint32_t instruction_flags_0xec = 0;
    std::uint32_t instruction_flags_0xf0 = 0;
    std::string provenance;
};

struct ActionMotionPlaybackRuntime {
    int action_ordinal = -1;
    int slot = -1;
    std::uint64_t instruction_state_revision = 0;
    int selected_action_row_index = -1;
    ActionMotionPlaybackStatus status = ActionMotionPlaybackStatus::MissingInput;
    ActionMotionPlaybackPhase phase = ActionMotionPlaybackPhase::Inactive;
    std::uint32_t raw_duration_bits = 0;
    std::uint32_t effective_duration_bits = 0;
    std::uint32_t progress_bits_0x68 = 0;
    std::uint32_t increment_bits_0x6c = 0;
    std::uint32_t instruction_flags_0xec = 0;
    std::uint32_t instruction_flags_0xf0 = 0;
    int callback_control_state = 0;
    int renderer_visits = 0;
    int state6_polls = 0;
    ActionMotionDelayStatus post_state6_delay_status =
        ActionMotionDelayStatus::MissingInput;
    int post_state6_delay_descriptor_record_index = -1;
    int post_state6_delay_remaining = 0;
    bool post_state6_delay_lookup_complete = false;
    bool substituted_default_duration = false;
    std::string provenance;
};

struct ActionMotionPlaybackInstallResult {
    ActionMotionPlaybackRuntime runtime{};
    bool installed = false;
    bool blocks_publication = false;
    std::uint32_t flags_before = 0;
    std::uint32_t flags_after = 0;
    std::string detail;
};

struct ActionMotionPlaybackVisitResult {
    ActionMotionPlaybackRuntime runtime{};
    ActionMotionPlaybackVisitKind kind = ActionMotionPlaybackVisitKind::None;
    bool renderer_advanced = false;
    bool gate_polled = false;
    std::optional<bool> gate_result;
    bool publication_released_this_visit = false;
    std::uint32_t progress_before = 0;
    std::uint32_t progress_after = 0;
    std::uint32_t flags_before = 0;
    std::uint32_t flags_after = 0;
    int control_state_before = 0;
    int control_state_after = 0;
    bool post_state6_delay_lookup_performed = false;
    ActionMotionDelayStatus post_state6_delay_status =
        ActionMotionDelayStatus::MissingInput;
    int post_state6_delay_descriptor_record_index = -1;
    int post_state6_delay_before = 0;
    int post_state6_delay_after = 0;
    std::string detail;
};

ActionMotionPlaybackInstallResult install_action_motion_playback(
    const ActionMotionPlaybackInstallRequest& request);

ActionMotionPlaybackVisitResult visit_action_motion_playback(
    const ActionMotionPlaybackRuntime& runtime,
    const ActionMotionPlaybackVisitInput& input = {});

ActionMotionDelayLookupResult resolve_action_motion_post_state6_delay(
    const ActionMotionDelayTable& table,
    const ActionMotionInstructionGateInput& input);

bool action_motion_playback_blocks_publication(
    const ActionMotionPlaybackRuntime& runtime);

const char* action_motion_playback_status_name(ActionMotionPlaybackStatus status);
const char* action_motion_playback_phase_name(ActionMotionPlaybackPhase phase);
const char* action_motion_playback_visit_kind_name(ActionMotionPlaybackVisitKind kind);
const char* action_motion_delay_status_name(ActionMotionDelayStatus status);

} // namespace savor::predict
