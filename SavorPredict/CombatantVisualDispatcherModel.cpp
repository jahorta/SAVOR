#include "CombatantVisualDispatcherModel.h"

#include <algorithm>
#include <bit>
#include <sstream>

namespace savor::predict {
namespace {

bool already_published(const CombatantVisualTimelineState& timeline, int record_index) {
    return std::find(
        timeline.published_record_indices.begin(),
        timeline.published_record_indices.end(),
        record_index) != timeline.published_record_indices.end();
}

std::uint32_t decoded_visual_frame(std::uint32_t raw_frame) {
    // Captured STD camera timing uses a four-bit fixed-point frame encoding.
    return raw_frame >= 16U && (raw_frame % 16U) == 0U
        ? raw_frame >> 4U
        : raw_frame;
}

CombatantVisualModelStatus status_for_key_source(CombatantVisualKeySource source) {
    switch (source) {
    case CombatantVisualKeySource::RuntimeInstruction:
    case CombatantVisualKeySource::ValidatedTransition:
        return CombatantVisualModelStatus::Matched;
    case CombatantVisualKeySource::SelectedStdAction:
        return CombatantVisualModelStatus::Provisional;
    case CombatantVisualKeySource::Missing:
        return CombatantVisualModelStatus::MissingInput;
    }
    return CombatantVisualModelStatus::MissingInput;
}

} // namespace

CombatantVisualKeyResolution resolve_combatant_visual_action_key(
    const CombatantVisualInstructionSnapshot& instruction) {
    CombatantVisualKeyResolution result;
    if (instruction.runtime_instruction_mode.has_value()
        && instruction.knowledge != CombatantVisualInstructionKnowledge::Unknown) {
        result.action_key = instruction.runtime_instruction_mode;
        result.source = CombatantVisualKeySource::RuntimeInstruction;
    } else if (instruction.validated_transition_mode.has_value()) {
        result.action_key = instruction.validated_transition_mode;
        result.source = CombatantVisualKeySource::ValidatedTransition;
    } else if (instruction.selected_std_action_key.has_value()) {
        result.action_key = instruction.selected_std_action_key;
        result.source = CombatantVisualKeySource::SelectedStdAction;
    }
    result.status = status_for_key_source(result.source);
    if (result.source == CombatantVisualKeySource::RuntimeInstruction
        && instruction.knowledge == CombatantVisualInstructionKnowledge::Provisional) {
        result.status = CombatantVisualModelStatus::Provisional;
    }
    result.provenance = instruction.provenance;
    if (!result.provenance.empty()) {
        result.provenance += "; ";
    }
    result.provenance += std::string("visual_key_source=")
        + combatant_visual_key_source_name(result.source);
    return result;
}

void install_combatant_visual_instruction(
    CombatantVisualTimelineState& timeline,
    CombatantVisualInstructionSnapshot instruction) {
    timeline.installed = true;
    ++timeline.epoch;
    timeline.owning_thread_visits = 0;
    timeline.instruction = std::move(instruction);
    timeline.published_record_indices.clear();
}

bool combatant_visual_record_temporally_active(
    const CombatantVisualCommandRecord& record,
    std::uint32_t visual_frame) {
    if (record.kind != CombatantVisualCommandKind::SystemCamera
        || !record.system_camera.has_value()) {
        return true;
    }
    const auto& camera = *record.system_camera;
    const auto start_frame = decoded_visual_frame(camera.start_frame);
    const auto end_frame = decoded_visual_frame(camera.end_frame);
    if (visual_frame < start_frame) {
        return false;
    }
    return end_frame == 0 || visual_frame <= end_frame;
}

CombatantVisualTimelineAdvanceResult advance_combatant_visual_timeline(
    CombatantVisualTimelineState& timeline,
    const CombatantVisualResource* resource) {
    CombatantVisualTimelineAdvanceResult result;
    result.epoch = timeline.epoch;
    result.owning_thread_visit = timeline.owning_thread_visits;

    if (!timeline.installed) {
        result.status = CombatantVisualModelStatus::MissingInput;
        result.diagnostics.push_back("visual timeline has no installed instruction epoch");
        return result;
    }
    if (resource == nullptr) {
        result.status = CombatantVisualModelStatus::MissingInput;
        result.diagnostics.push_back("visual timeline has no bound resource");
        ++timeline.owning_thread_visits;
        return result;
    }

    const auto key = resolve_combatant_visual_action_key(timeline.instruction);
    result.status = key.status;
    if (!key.action_key.has_value()) {
        result.diagnostics.push_back("visual instruction did not resolve an action key");
        ++timeline.owning_thread_visits;
        return result;
    }

    const auto secondary = timeline.instruction.subtype.value_or(-1);
    for (const auto& record : resource->records) {
        if (record.location_code < 0 || already_published(timeline, record.index)) {
            continue;
        }
        if (record.kind != CombatantVisualCommandKind::SetCommand
            && record.kind != CombatantVisualCommandKind::SystemCamera) {
            continue;
        }
        if (!record.gate_fields_known) {
            result.diagnostics.push_back(
                "record " + std::to_string(record.index) + " has no decoded gate fields");
            continue;
        }
        if (!match_std_payload_action_key(
                record.gate_fields.primary_action_key,
                record.gate_fields.direct_gate_secondary_key,
                *key.action_key,
                secondary)) {
            continue;
        }
        if (!combatant_visual_record_temporally_active(
                record,
                timeline.owning_thread_visits)) {
            continue;
        }

        CombatantVisualPublication publication;
        publication.slot = timeline.instruction.slot;
        publication.target_slot = timeline.instruction.target_slot.value_or(-1);
        publication.epoch = timeline.epoch;
        publication.owning_thread_visit = timeline.owning_thread_visits;
        publication.record_index = record.index;
        publication.kind = record.kind;
        publication.status = key.status;
        publication.key_source = key.source;
        publication.action_key = *key.action_key;
        publication.record = &record;
        publication.provenance = key.provenance
            + "; resource=" + resource->binding.resource_stem
            + "; record_index=" + std::to_string(record.index);
        result.publications.push_back(std::move(publication));
        timeline.published_record_indices.push_back(record.index);
    }

    ++timeline.owning_thread_visits;
    return result;
}

CombatantVisualActionViewRngPlan combatant_visual_action_view_rng_plan(
    std::int16_t payload_mode,
    std::int16_t effective_mode) {
    CombatantVisualActionViewRngPlan result;
    result.mode0_rewrite_draw = payload_mode == 0;
    result.mode0e_camera_draw = effective_mode == 0x0e;
    result.mode1_pathing_callback = effective_mode == 1;
    result.supported_mode = effective_mode == 0
        || effective_mode == 1
        || effective_mode == 0x0e;
    return result;
}

CombatantStdActionRowSelectionResult select_combatant_std_action_row(
    const std::vector<CombatantStdActionRow>& rows,
    const CombatantStdActionRowSelectionRequest& request) {
    CombatantStdActionRowSelectionResult result;
    result.requested_action_id = request.action_id;
    if (rows.empty() || request.action_id < 0) {
        result.provenance = rows.empty()
            ? "selected-action-row producer has no imported STD action rows"
            : "selected-action-row producer has no requested action ID";
        return result;
    }

    const auto normalize_action = [](std::int16_t action_id) {
        return action_id == 0x16 ? static_cast<std::int16_t>(0x15) : action_id;
    };
    const auto requires_secondary_key = [](std::int16_t action_id) {
        return action_id == 0x18 || action_id == 0x1d || action_id == 0x1e;
    };
    const auto find_row = [&](std::int16_t action_id)
        -> std::optional<CombatantStdActionRow> {
        for (const auto& row : rows) {
            if (row.row_type == 3) {
                break;
            }
            if (row.action_id != action_id) {
                continue;
            }
            if (requires_secondary_key(action_id)) {
                if (!request.secondary_key.has_value()
                    || row.secondary_key != *request.secondary_key) {
                    continue;
                }
            }
            return row;
        }
        return std::nullopt;
    };

    const auto normalized = normalize_action(request.action_id);
    if (const auto row = find_row(normalized); row.has_value()) {
        result.status = CombatantStdActionRowSelectionStatus::Matched;
        result.row = row;
        result.selected_action_id = normalized;
        result.provenance =
            "STD action-row scan selected the first matching row before the type-3 terminator";
        return result;
    }
    if (requires_secondary_key(normalized) && !request.secondary_key.has_value()) {
        result.status = CombatantStdActionRowSelectionStatus::MissingInput;
        result.provenance =
            "selected STD action requires the instruction secondary key";
        return result;
    }

    std::optional<std::int16_t> fallback_action;
    if (request.allow_transition_fallback) {
        if (normalized == 8 || normalized == 5) {
            fallback_action = 4;
        } else if (normalized == 0x13) {
            fallback_action = 6;
        }
    }
    if (fallback_action.has_value()) {
        if (const auto row = find_row(*fallback_action); row.has_value()) {
            result.status = CombatantStdActionRowSelectionStatus::Provisional;
            result.row = row;
            result.selected_action_id = *fallback_action;
            result.used_transition_fallback = true;
            result.provenance =
                "STD transition producer applied the validated action-row fallback mapping";
            return result;
        }
    }

    result.status = CombatantStdActionRowSelectionStatus::Unsupported;
    result.provenance =
        "imported STD action rows do not contain a modeled selection for this action";
    return result;
}

CombatantStdMotionInitializationResult initialize_combatant_std_motion_state(
    const std::vector<CombatantStdActionRow>& rows) {
    CombatantStdMotionInitializationResult result;
    if (rows.empty()) {
        result.provenance =
            "STD motion-state producer has no imported action rows";
        return result;
    }

    const CombatantStdActionRow* base_row = nullptr;
    for (const auto& row : rows) {
        if (row.row_type == 3) {
            break;
        }
        if (row.row_type == 0) {
            base_row = &row;
            break;
        }
    }
    if (base_row == nullptr) {
        result.status = CombatantStdMotionInitializationStatus::Unsupported;
        result.provenance =
            "STD motion-state producer found no type-0 initialization row before the terminator";
        return result;
    }

    const auto alternate = select_combatant_std_action_row(
        rows,
        CombatantStdActionRowSelectionRequest{
            .action_id = 1,
            .secondary_key = -1,
            .allow_transition_fallback = false,
        });
    if (!alternate.row.has_value()) {
        result.status = alternate.status
                == CombatantStdActionRowSelectionStatus::MissingInput
            ? CombatantStdMotionInitializationStatus::MissingInput
            : CombatantStdMotionInitializationStatus::Unsupported;
        result.base_row_index = base_row->index;
        result.provenance =
            "STD motion-state producer could not resolve the action-1 alternate row; "
            + alternate.provenance;
        return result;
    }

    constexpr float kInitialMotionScale = 1.5f;
    const float base_source = std::bit_cast<float>(
        base_row->transition_gate_divisor_bits);
    const float alternate_source = std::bit_cast<float>(
        alternate.row->transition_gate_divisor_bits);
    result.status = CombatantStdMotionInitializationStatus::Matched;
    result.base_row_index = base_row->index;
    result.alternate_row_index = alternate.row->index;
    result.base_speed = base_source * kInitialMotionScale;
    result.alternate_uses_base = alternate_source == 0.0f;
    result.alternate_speed = result.alternate_uses_base
        ? result.base_speed
        : alternate_source * kInitialMotionScale;
    result.turn_speed_degrees = std::bit_cast<float>(
        base_row->motion_progress_step_bits);
    result.base_speed_bits = std::bit_cast<std::uint32_t>(result.base_speed);
    result.alternate_speed_bits = std::bit_cast<std::uint32_t>(
        result.alternate_speed);
    result.turn_speed_bits = base_row->motion_progress_step_bits;
    result.provenance =
        "FUN_80020060 first type-0 row initializes IW+0x12C/+0x128; "
        "action-1 row lookup initializes IW+0x130 and a zero source reuses IW+0x12C";
    return result;
}

Std0Table combatant_visual_selector_table(const CombatantVisualResource& resource) {
    if (!resource.selector_table.entries.empty()) {
        return resource.selector_table;
    }
    Std0Table table;
    table.includes_sentinel = resource.includes_sentinel;
    table.entries.reserve(resource.records.size());
    for (const auto& record : resource.records) {
        Std0EntryRecord entry;
        entry.location_code = record.location_code;
        entry.opcode = record.opcode;
        entry.payload = record.gate_fields;
        entry.has_payload = record.gate_fields_known;
        table.entries.push_back(entry);
    }
    return table;
}

const char* combatant_visual_command_kind_name(CombatantVisualCommandKind kind) {
    switch (kind) {
    case CombatantVisualCommandKind::Unknown: return "Unknown";
    case CombatantVisualCommandKind::SetCommand: return "SetCommand";
    case CombatantVisualCommandKind::SystemCamera: return "SystemCamera";
    case CombatantVisualCommandKind::SyntheticActionView: return "SyntheticActionView";
    }
    return "Unknown";
}

const char* combatant_visual_model_status_name(CombatantVisualModelStatus status) {
    switch (status) {
    case CombatantVisualModelStatus::Matched: return "Matched";
    case CombatantVisualModelStatus::Provisional: return "Provisional";
    case CombatantVisualModelStatus::MissingInput: return "MissingInput";
    case CombatantVisualModelStatus::Unsupported: return "Unsupported";
    }
    return "Unsupported";
}

const char* combatant_visual_key_source_name(CombatantVisualKeySource source) {
    switch (source) {
    case CombatantVisualKeySource::Missing: return "missing";
    case CombatantVisualKeySource::RuntimeInstruction: return "runtime_instruction";
    case CombatantVisualKeySource::ValidatedTransition: return "validated_transition";
    case CombatantVisualKeySource::SelectedStdAction: return "selected_std_action";
    }
    return "missing";
}

const char* combatant_std_action_row_selection_status_name(
    CombatantStdActionRowSelectionStatus status) {
    switch (status) {
    case CombatantStdActionRowSelectionStatus::Matched: return "Matched";
    case CombatantStdActionRowSelectionStatus::Provisional: return "Provisional";
    case CombatantStdActionRowSelectionStatus::MissingInput: return "MissingInput";
    case CombatantStdActionRowSelectionStatus::Unsupported: return "Unsupported";
    }
    return "Unsupported";
}

const char* combatant_std_motion_initialization_status_name(
    CombatantStdMotionInitializationStatus status) {
    switch (status) {
    case CombatantStdMotionInitializationStatus::Matched: return "Matched";
    case CombatantStdMotionInitializationStatus::MissingInput: return "MissingInput";
    case CombatantStdMotionInitializationStatus::Unsupported: return "Unsupported";
    }
    return "Unsupported";
}

const char* combatant_visual_dispatcher_rule_detail() {
    return "Each instruction installation, including a same-mode reapplication, creates a new "
           "visual epoch. The owning combatant advances the epoch once per packed-thread visit. "
           "SET COMMAND and SYSTEM CAMERA rows are matched with the validated STD action-key "
           "predicate and publish at most once per epoch. SYSTEM CAMERA start/end fields gate "
           "the provisional visual-frame cursor. Runtime instruction and validated transition "
           "keys take precedence over a producer-selected STD action key; missing keys remain "
           "MissingInput.";
}

} // namespace savor::predict
