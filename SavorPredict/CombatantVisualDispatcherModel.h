#pragma once

#include "ActionViewSelectorModel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

enum class CombatantVisualCommandKind {
    Unknown,
    SetCommand,
    MoveModel,
    PutModel,
    HitWeapon,
    CollisionBox,
    MotionPause,
    PointLight,
    SystemCamera,
    SyntheticActionView,
};

enum class CombatantVisualModelStatus {
    Matched,
    Provisional,
    MissingInput,
    Unsupported,
};

enum class CombatantVisualInstructionKnowledge {
    Unknown,
    Provisional,
    Known,
};

enum class CombatantVisualKeySource {
    Missing,
    RuntimeInstruction,
    ValidatedTransition,
    SelectedStdAction,
};

struct CombatantVisualSetCommandPayload {
    std::int16_t command_mode = 0;
    std::int16_t command_subtype = 0;
    std::int16_t synchronization_flags = 0;
    std::uint32_t service_flags = 0;
    std::int16_t delay = 0;
    std::int16_t forced_mode = 0;
};

struct CombatantVisualSystemCameraPayload {
    std::uint32_t flags = 0;
    std::uint32_t scalar_bits = 0;
    std::uint32_t start_frame = 0;
    std::uint16_t end_frame = 0;
    std::uint16_t hold_frames = 0;
    std::uint16_t step_frames = 0;
    std::int16_t mode = 0;
};

struct CombatantVisualCollisionBoxPayload {
    std::uint32_t behavior_flags = 0;
    std::int16_t start_counter = 0;
    std::int16_t end_counter = 0;
    std::int16_t object_id = -1;
    std::uint32_t current_x_bits = 0;
    std::uint32_t current_y_bits = 0;
    std::uint32_t current_z_bits = 0;
    std::uint32_t velocity_x_bits = 0;
    std::uint32_t velocity_y_bits = 0;
    std::uint32_t velocity_z_bits = 0;
    std::uint32_t trailing_flags = 0;
};

struct CombatantVisualCommandRecord {
    int index = -1;
    std::int16_t location_code = -1;
    std::int16_t opcode = 0;
    std::uint32_t combined_type = 0;
    int payload_size = 0;
    bool payload_in_bounds = false;
    bool gate_fields_known = false;
    Std0PayloadGateFields gate_fields{};
    std::int16_t synchronization_gate = 0;
    CombatantVisualCommandKind kind = CombatantVisualCommandKind::Unknown;
    std::vector<std::uint8_t> payload_bytes;
    std::optional<CombatantVisualSetCommandPayload> set_command;
    std::optional<CombatantVisualCollisionBoxPayload> collision_box;
    std::optional<CombatantVisualSystemCameraPayload> system_camera;
};

struct CombatantVisualResourceBinding {
    int slot = -1;
    std::string resource_stem;
    std::optional<bool> mode0_rewrite_gate;
};

struct CombatantStdActionRow {
    int index = -1;
    std::int16_t action_id = -1;
    std::int16_t row_type = -1;
    std::int16_t callback_index = -1;
    std::int16_t callback_ordinal = -1;
    std::uint32_t flags = 0;
    std::int16_t secondary_key = -1;
    std::int16_t callback_aux_param = 0;
    std::uint32_t transition_gate_divisor_bits = 0;
    std::uint32_t motion_progress_step_bits = 0;
};

enum class CombatantStdActionRowSelectionStatus {
    Matched,
    Provisional,
    MissingInput,
    Unsupported,
};

struct CombatantStdActionRowSelectionRequest {
    std::int16_t action_id = -1;
    std::optional<std::int16_t> secondary_key;
    bool allow_transition_fallback = true;
};

struct CombatantStdActionRowSelectionResult {
    CombatantStdActionRowSelectionStatus status =
        CombatantStdActionRowSelectionStatus::MissingInput;
    std::optional<CombatantStdActionRow> row;
    std::int16_t requested_action_id = -1;
    std::int16_t selected_action_id = -1;
    bool used_transition_fallback = false;
    std::string provenance;
};

enum class CombatantStdMotionInitializationStatus {
    Matched,
    MissingInput,
    Unsupported,
};

struct CombatantStdMotionInitializationResult {
    CombatantStdMotionInitializationStatus status =
        CombatantStdMotionInitializationStatus::MissingInput;
    int base_row_index = -1;
    int alternate_row_index = -1;
    float base_speed = 0.0f;
    float alternate_speed = 0.0f;
    float turn_speed_degrees = 0.0f;
    std::uint32_t base_speed_bits = 0;
    std::uint32_t alternate_speed_bits = 0;
    std::uint32_t turn_speed_bits = 0;
    bool alternate_uses_base = false;
    std::string provenance;
};

struct CombatantVisualResource {
    CombatantVisualResourceBinding binding{};
    std::vector<CombatantVisualCommandRecord> records;
    std::vector<CombatantStdActionRow> action_rows;
    Std0Table selector_table;
    bool includes_sentinel = false;
    std::string provenance;
};

struct CombatantVisualInstructionSnapshot {
    int slot = -1;
    std::optional<std::int16_t> runtime_instruction_mode;
    std::optional<std::int16_t> validated_transition_mode;
    std::optional<std::int16_t> selected_std_action_key;
    std::optional<std::int16_t> subtype;
    std::optional<int> target_slot;
    std::optional<std::uint32_t> instruction_flags;
    CombatantVisualInstructionKnowledge knowledge =
        CombatantVisualInstructionKnowledge::Unknown;
    std::string provenance;
};

struct CombatantVisualKeyResolution {
    std::optional<std::int16_t> action_key;
    CombatantVisualKeySource source = CombatantVisualKeySource::Missing;
    CombatantVisualModelStatus status = CombatantVisualModelStatus::MissingInput;
    std::string provenance;
};

struct CombatantVisualTimelineState {
    bool installed = false;
    std::uint64_t epoch = 0;
    std::uint32_t owning_thread_visits = 0;
    CombatantVisualInstructionSnapshot instruction{};
    std::vector<int> published_record_indices;
};

struct CombatantVisualPublication {
    int slot = -1;
    int target_slot = -1;
    std::uint64_t epoch = 0;
    std::uint32_t owning_thread_visit = 0;
    int record_index = -1;
    CombatantVisualCommandKind kind = CombatantVisualCommandKind::Unknown;
    CombatantVisualModelStatus status = CombatantVisualModelStatus::Unsupported;
    CombatantVisualKeySource key_source = CombatantVisualKeySource::Missing;
    std::int16_t action_key = 0;
    const CombatantVisualCommandRecord* record = nullptr;
    std::string provenance;
};

struct CombatantVisualTimelineAdvanceResult {
    std::uint64_t epoch = 0;
    std::uint32_t owning_thread_visit = 0;
    CombatantVisualModelStatus status = CombatantVisualModelStatus::Matched;
    std::vector<CombatantVisualPublication> publications;
    std::vector<std::string> diagnostics;
};

enum class CombatantInstructionStdRowProducerStatus {
    DeferredState0,
    Idle,
    Published,
    Unchanged,
    MissingInput,
    Unsupported,
};

struct CombatantInstructionStdRowProducerCursor {
    int thread_state_0x19 = 0;
    bool has_observed_state1_input = false;
    int last_action_ordinal = -1;
    int last_instruction_revision = -1;
    std::uint64_t last_instruction_state_revision = 0;
    int last_selected_action_row_index = -1;
};

struct CombatantInstructionStdRowProducerRequest {
    int action_ordinal = -1;
    int slot = -1;
    int instruction_revision = 0;
    std::uint64_t instruction_state_revision = 0;
    bool selected_action_row_known = false;
    int selected_action_row_index = -1;
    std::optional<std::int16_t> selected_action_key;
    std::optional<std::int16_t> runtime_instruction_mode;
    std::optional<std::int16_t> subtype;
    std::optional<int> target_slot;
    std::optional<std::uint32_t> instruction_flags;
    CombatantVisualInstructionKnowledge knowledge =
        CombatantVisualInstructionKnowledge::Unknown;
    std::string provenance;
};

struct CombatantInstructionStdRowProducerResult {
    CombatantInstructionStdRowProducerStatus status =
        CombatantInstructionStdRowProducerStatus::MissingInput;
    CombatantVisualModelStatus visual_status =
        CombatantVisualModelStatus::MissingInput;
    bool install_epoch = false;
    CombatantInstructionStdRowProducerCursor cursor_after{};
    std::optional<CombatantVisualInstructionSnapshot> instruction;
    std::string provenance;
};

struct CombatantVisualActionViewRngPlan {
    bool mode0_rewrite_draw = false;
    bool mode0e_camera_draw = false;
    bool mode1_pathing_callback = false;
    bool supported_mode = false;
};

CombatantVisualKeyResolution resolve_combatant_visual_action_key(
    const CombatantVisualInstructionSnapshot& instruction);

void install_combatant_visual_instruction(
    CombatantVisualTimelineState& timeline,
    CombatantVisualInstructionSnapshot instruction);

bool combatant_visual_record_temporally_active(
    const CombatantVisualCommandRecord& record,
    std::uint32_t visual_frame);

bool combatant_visual_timeline_has_pending_publications(
    const CombatantVisualTimelineState& timeline,
    const CombatantVisualResource* resource);

CombatantVisualTimelineAdvanceResult advance_combatant_visual_timeline(
    CombatantVisualTimelineState& timeline,
    const CombatantVisualResource* resource);

CombatantInstructionStdRowProducerResult
visit_combatant_instruction_std_row_producer(
    const CombatantInstructionStdRowProducerCursor& cursor,
    const CombatantInstructionStdRowProducerRequest& request);

CombatantVisualActionViewRngPlan combatant_visual_action_view_rng_plan(
    std::int16_t payload_mode,
    std::int16_t effective_mode);

CombatantStdActionRowSelectionResult select_combatant_std_action_row(
    const std::vector<CombatantStdActionRow>& rows,
    const CombatantStdActionRowSelectionRequest& request);

CombatantStdMotionInitializationResult initialize_combatant_std_motion_state(
    const std::vector<CombatantStdActionRow>& rows);

Std0Table combatant_visual_selector_table(const CombatantVisualResource& resource);

const char* combatant_visual_command_kind_name(CombatantVisualCommandKind kind);
const char* combatant_visual_model_status_name(CombatantVisualModelStatus status);
const char* combatant_visual_key_source_name(CombatantVisualKeySource source);
const char* combatant_instruction_std_row_producer_status_name(
    CombatantInstructionStdRowProducerStatus status);
const char* combatant_std_action_row_selection_status_name(
    CombatantStdActionRowSelectionStatus status);
const char* combatant_std_motion_initialization_status_name(
    CombatantStdMotionInitializationStatus status);
const char* combatant_visual_dispatcher_rule_detail();

} // namespace savor::predict
