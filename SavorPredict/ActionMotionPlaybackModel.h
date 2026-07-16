#pragma once

#include <cstdint>
#include <optional>
#include <string>

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
    PublicationReleased,
    Unsupported,
};

enum class ActionMotionPlaybackVisitKind {
    None,
    InitialRendererAdvance,
    State6Deferred,
    State6Satisfied,
    PublicationReleased,
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
    std::string detail;
};

ActionMotionPlaybackInstallResult install_action_motion_playback(
    const ActionMotionPlaybackInstallRequest& request);

ActionMotionPlaybackVisitResult visit_action_motion_playback(
    const ActionMotionPlaybackRuntime& runtime);

bool action_motion_playback_blocks_publication(
    const ActionMotionPlaybackRuntime& runtime);

const char* action_motion_playback_status_name(ActionMotionPlaybackStatus status);
const char* action_motion_playback_phase_name(ActionMotionPlaybackPhase phase);
const char* action_motion_playback_visit_kind_name(ActionMotionPlaybackVisitKind kind);

} // namespace savor::predict
