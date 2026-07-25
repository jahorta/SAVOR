#include "ActionViewSelectorModel.h"

#include <sstream>

namespace savor::predict {
namespace {

constexpr std::uint32_t kExcludedCombinedEntryId = 0x00030041U;
constexpr std::uint32_t kMode11InstructionGate = 0x02000000U;

bool action_key_requires_direct_secondary(std::int16_t action_key) {
    return action_key == 0x18 || action_key == 0x1d || action_key == 0x1e;
}

void append_branch(ActionViewSelectorResult& result, const char* text) {
    result.branch_path.emplace_back(text);
}

void append_branch_value(ActionViewSelectorResult& result, const char* label, int value) {
    std::ostringstream out;
    out << label << "=" << value;
    result.branch_path.push_back(out.str());
}

void append_helper_call(
    ActionViewSelectorResult& result,
    std::uint32_t call_site_pc,
    const char* callee,
    std::int16_t actor_slot,
    std::optional<std::int16_t> mode_arg,
    const char* role) {
    result.helper_calls.push_back({
        .call_site_pc = call_site_pc,
        .callee = callee,
        .actor_slot = actor_slot,
        .mode_arg = mode_arg,
        .role = role,
    });
    result.operations.push_back({
        .kind = ActionViewSelectorOperationKind::DispatchHelper,
        .call_site_pc = call_site_pc,
        .actor_slot = actor_slot,
        .record_mode = mode_arg,
        .role = role,
    });
}

void record_spawned_action_view_record_mode(
    ActionViewSelectorResult& result,
    const ActionViewSelectorInput& input,
    std::int16_t helper_param_2,
    std::uint32_t call_site_pc,
    const char* role) {
    const auto record_mode = action_view_record_mode_from_80053f38(
        input.instruction_field6_0x6,
        helper_param_2);
    result.spawned_action_view_record_mode_if_known = record_mode;
    result.spawned_action_view_record_modes.push_back(record_mode);
    result.operations.push_back({
        .kind = ActionViewSelectorOperationKind::PublishSyntheticRecord,
        .call_site_pc = call_site_pc,
        .actor_slot = input.current_actor_slot,
        .secondary_slot = input.current_secondary_slot,
        .record_mode = record_mode,
        .role = role,
    });
    append_branch_value(result, "spawned_action_view_record_mode", record_mode);
}

std::int16_t normalize_selector_state_for_request(
    const ActionViewSelectorInput& input,
    std::int8_t requested_mode,
    std::int16_t selector_state_before_request) {
    if (requested_mode == input.previous_effective_mode_0x2f) {
        return selector_state_before_request;
    }
    if (selector_state_before_request == 1) {
        return 1;
    }

    const auto field6 = input.instruction_field6_0x6;
    if (field6 == 0x2 || field6 == 0x7 || field6 == 0xb) {
        if (input.previous_actor_slot_0x2 != input.current_actor_slot) {
            return 0;
        }
        return selector_state_before_request;
    }

    if (field6 == 0x1e
        && input.instruction_field8_0x8 >= 0x24
        && input.instruction_field8_0x8 < 0x26
        && input.previous_actor_slot_0x2 != input.current_actor_slot) {
        return 0;
    }

    return 0;
}

void append_state_write(
    ActionViewSelectorResult& result,
    std::uint32_t call_site_pc,
    std::int16_t before,
    std::int16_t after,
    const char* role) {
    result.operations.push_back({
        .kind = ActionViewSelectorOperationKind::WriteSelectorState,
        .call_site_pc = call_site_pc,
        .actor_slot = result.selector_actor_slot_0x2_written.value_or(-1),
        .secondary_slot =
            result.selector_secondary_slot_0x4_written.value_or(-1),
        .selector_state_before = before,
        .selector_state_after = after,
        .role = role,
    });
}

} // namespace

std::uint32_t std0_combined_entry_id(std::int16_t location_code, std::int16_t opcode) {
    return (static_cast<std::uint32_t>(static_cast<std::uint16_t>(opcode)) << 16U)
        | static_cast<std::uint16_t>(location_code);
}

bool match_std_payload_action_key(
    std::int16_t payload_primary_action_key,
    std::int16_t payload_direct_gate_secondary_key,
    std::int16_t query_action_key,
    std::int16_t query_secondary_key) {
    if (payload_primary_action_key != query_action_key) {
        return false;
    }
    if (action_key_requires_direct_secondary(payload_primary_action_key)) {
        return payload_direct_gate_secondary_key == query_secondary_key;
    }
    return true;
}

Std0CountResult count_matching_std0_entries(
    const Std0Table* table,
    const Std0CountQuery& query) {
    Std0CountResult result;
    result.used_combined_id_filter = query.location_code >= 0 && query.opcode >= 0;

    if (table == nullptr) {
        result.reached_sentinel = true;
        return result;
    }

    const auto required_combined_id = std0_combined_entry_id(query.location_code, query.opcode);
    for (const auto& entry : table->entries) {
        if (entry.location_code < 0) {
            result.reached_sentinel = true;
            break;
        }
        ++result.scanned_entries;
        if (!entry.has_payload) {
            continue;
        }
        if (!match_std_payload_action_key(
                entry.payload.primary_action_key,
                entry.payload.direct_gate_secondary_key,
                query.action_key,
                query.secondary_key)) {
            continue;
        }

        const auto combined_id = std0_combined_entry_id(entry.location_code, entry.opcode);
        if (combined_id == kExcludedCombinedEntryId) {
            continue;
        }
        if (result.used_combined_id_filter && combined_id != required_combined_id) {
            continue;
        }
        ++result.count;
    }

    if (!result.reached_sentinel && !table->includes_sentinel) {
        result.reached_sentinel = true;
    }
    return result;
}

Std0CountQuery mode0e_action_view_count_query() {
    return {
        .action_key = 4,
        .secondary_key = -1,
        .location_code = 0x2a,
        .opcode = 3,
    };
}

Std0CountQuery mode3_action_view_count_query() {
    return {
        .action_key = 5,
        .secondary_key = -1,
        .location_code = 0x2a,
        .opcode = 3,
    };
}

Std0CountQuery mode5_action_view_count_query(
    std::int16_t instruction_field6_0x6,
    std::int16_t instruction_field8_0x8) {
    return {
        .action_key = instruction_field6_0x6,
        .secondary_key = instruction_field8_0x8,
        .location_code = 0x2e,
        .opcode = 3,
    };
}

std::int16_t action_view_record_mode_from_80053f38(
    std::int16_t instruction_field6_0x6,
    std::int16_t helper_param_2) {
    if (helper_param_2 != 0) {
        return 0x11;
    }

    switch (instruction_field6_0x6) {
    case 4:
    case 8:
        return 0xe;
    case 5:
    case 0x1d:
        return 1;
    case 0x0c:
        return 0x0f;
    case 0x11:
        return 0x10;
    default:
        return 2;
    }
}

std::int8_t action_view_requested_mode_from_field6(
    std::int16_t instruction_field6_0x6,
    std::int16_t instruction_field8_0x8,
    bool helper_800153e0_result) {
    switch (instruction_field6_0x6) {
    case 6:
        return 0;
    case 29:
        (void)instruction_field8_0x8;
        return helper_800153e0_result ? 1 : 5;
    case 4:
    case 8:
    case 9:
        return 2;
    case 5:
    case 10:
        return 3;
    case 2:
    case 7:
    case 12:
        return 4;
    default:
        return 1;
    }
}

ActionViewSelectorResult select_action_view_mode(const ActionViewSelectorInput& input) {
    ActionViewSelectorResult result;
    result.selector_actor_slot_0x2_written = input.current_actor_slot;
    result.selector_secondary_slot_0x4_written =
        input.current_secondary_slot;
    result.operations.push_back({
        .kind = ActionViewSelectorOperationKind::WriteRole,
        .call_site_pc = 0x80012f88U,
        .actor_slot = input.current_actor_slot,
        .secondary_slot = input.current_secondary_slot,
        .role = "FUN_8001D41C resolved current action-view roles",
    });

    result.requested_mode = action_view_requested_mode_from_field6(
        input.instruction_field6_0x6,
        input.instruction_field8_0x8,
        input.helper_800153e0_result);
    append_branch_value(result, "requested_mode", result.requested_mode);

    if (input.current_actor_slot < 0) {
        result.status = ActionViewSelectorStatus::MissingInput;
        append_branch(result, "missing_current_actor");
        return result;
    }

    const bool actor_changed =
        input.previous_actor_slot_0x2 != input.current_actor_slot;
    auto selector_state_before_request = input.previous_selector_state_0x30;
    const bool role_reversed = actor_changed
        && input.actor_lookup_8001d41c_nonzero.value_or(false);
    if (!actor_changed) {
        append_branch(result, "entry_actor_unchanged");
    } else if (!input.actor_lookup_8001d41c_nonzero.has_value()) {
        result.status = ActionViewSelectorStatus::MissingInput;
        append_branch(result, "entry_actor_changed_lookup_missing");
        return result;
    } else {
        selector_state_before_request = role_reversed ? 2 : 0;
        append_branch(result, role_reversed
            ? "entry_actor_changed_lookup_nonzero_state2"
            : "entry_actor_changed_lookup_zero_state0");
    }

    auto selector_state = role_reversed
        ? static_cast<std::int16_t>(2)
        : normalize_selector_state_for_request(
            input,
            result.requested_mode,
            selector_state_before_request);
    append_branch_value(result, "normalized_selector_state", selector_state);

    auto dispatch_effective_mode = input.previous_effective_mode_0x2f;
    if (selector_state == 0
        || (selector_state == 1
            && result.requested_mode != input.previous_effective_mode_0x2f)
        || (role_reversed
            && result.requested_mode != input.previous_effective_mode_0x2f)) {
        dispatch_effective_mode = result.requested_mode;
        append_branch_value(result, "effective_mode_written_0x2f", dispatch_effective_mode);
        result.operations.push_back({
            .kind = ActionViewSelectorOperationKind::WriteEffectiveMode,
            .call_site_pc = 0x800130fcU,
            .actor_slot = input.current_actor_slot,
            .secondary_slot = input.current_secondary_slot,
            .effective_mode = dispatch_effective_mode,
            .selector_state_before = selector_state,
            .selector_state_after = selector_state,
            .role = "selector requested category changed",
        });
    }

    if ((selector_state == 0 || selector_state == 1)
        && !input.actor_instruction_flags_0xf0.has_value()) {
        result.status = ActionViewSelectorStatus::MissingInput;
        result.dispatch_effective_mode_0x2f = dispatch_effective_mode;
        result.selector_state_0x30 = selector_state;
        append_branch(result, "missing_actor_instruction_flags_0xf0");
        return result;
    }

    auto working_flags = input.actor_instruction_flags_0xf0;
    if (selector_state == 0) {
        if (actor_changed && !role_reversed) {
            append_helper_call(
                result,
                0x8001318cU,
                "FUN_80053f38",
                input.current_actor_slot,
                1,
                "state0_actor_changed_spawn_mode1");
            record_spawned_action_view_record_mode(
                result,
                input,
                1,
                0x8001318cU,
                "state0_actor_changed_spawn_mode1");
            append_branch(result, "state0_actor_changed_call_80053f38_mode1");
            const auto flags_before = *working_flags;
            *working_flags |= kMode11InstructionGate;
            result.operations.push_back({
                .kind = ActionViewSelectorOperationKind::SetMode11Gate,
                .call_site_pc = 0x80054024U,
                .actor_slot = input.current_actor_slot,
                .secondary_slot = input.current_secondary_slot,
                .flags_before = flags_before,
                .flags_after = *working_flags,
                .selector_state_before = selector_state,
                .selector_state_after = selector_state,
                .role = "SpawnSyntheticActionViewRecord_80053F38 mode 1 "
                        "synchronously set IW+0xF0 bit 0x02000000",
            });
        }
        result.operations.push_back({
            .kind = ActionViewSelectorOperationKind::GateRecheck,
            .call_site_pc = 0x80013198U,
            .actor_slot = input.current_actor_slot,
            .secondary_slot = input.current_secondary_slot,
            .flags_before = *working_flags,
            .flags_after = *working_flags,
            .selector_state_before = selector_state,
            .selector_state_after =
                (*working_flags & kMode11InstructionGate) != 0 ? 1 : 2,
            .role = "state-0 same-call IW+0xF0 gate reload",
        });
        const auto state_before = selector_state;
        selector_state =
            (*working_flags & kMode11InstructionGate) != 0 ? 1 : 2;
        append_state_write(
            result,
            selector_state == 1 ? 0x80013190U : 0x800131a8U,
            state_before,
            selector_state,
            selector_state == 1
                ? "mode-11 gate retained selector state 1"
                : "clear mode-11 gate advanced selector state 2");
        append_branch_value(result, "state0_transition_result", selector_state);
    } else if (selector_state == 1) {
        result.operations.push_back({
            .kind = ActionViewSelectorOperationKind::GateRecheck,
            .call_site_pc = 0x80013198U,
            .actor_slot = input.current_actor_slot,
            .secondary_slot = input.current_secondary_slot,
            .flags_before = *working_flags,
            .flags_after = *working_flags,
            .selector_state_before = 1,
            .selector_state_after =
                (*working_flags & kMode11InstructionGate) != 0 ? 1 : 2,
            .role = "state-1 IW+0xF0 gate reload",
        });
        if ((*working_flags & kMode11InstructionGate) == 0) {
            selector_state = 2;
            append_state_write(
                result,
                0x800131a8U,
                1,
                2,
                "cleared mode-11 gate advanced selector state 1 to 2");
            append_branch(result, "state1_gate_clear_advanced_state2");
        } else {
            append_branch(result, "state1_gate_set_retained_state1");
        }
    }

    result.actor_instruction_flags_0xf0_after = working_flags;
    result.dispatch_effective_mode_0x2f = dispatch_effective_mode;
    result.selector_state_0x30 = selector_state;
    append_branch_value(result, "dispatch_effective_mode", result.dispatch_effective_mode_0x2f);

    switch (dispatch_effective_mode) {
    case 0:
    case 4:
        append_branch(result, dispatch_effective_mode == 0
            ? "effective_mode_0_path"
            : "effective_mode_4_falls_through_to_mode0_path");
        if (selector_state == 3) {
            result.helper_family_selected = true;
            append_branch(result, "state3_helper_family_selected");
        } else if (selector_state == 2) {
            result.synthetic_call_80053f38_selected = true;
            append_helper_call(
                result,
                0x8001321cU,
                "FUN_80053f38",
                input.current_actor_slot,
                0,
                dispatch_effective_mode == 0
                    ? "mode0_state2_spawn_mode0"
                    : "mode4_state2_spawn_mode0");
            record_spawned_action_view_record_mode(
                result,
                input,
                0,
                0x8001321cU,
                dispatch_effective_mode == 0
                    ? "mode0_state2_spawn_mode0"
                    : "mode4_state2_spawn_mode0");
            append_state_write(
                result,
                0x80013220U,
                2,
                3,
                "mode 0/4 category publication advanced selector to state 3");
            result.selector_state_0x30 = 3;
            append_branch(result, "state2_call_80053f38_mode0_then_state3");
        } else {
            append_branch(result, "mode0_or_4_path_exit_without_query");
        }
        return result;

    case 1:
        append_branch(result, "effective_mode_1_path");
        if (selector_state == 3) {
            result.helper_family_selected = true;
            append_branch(result, "state3_helper_family_selected");
        } else if (selector_state == 2) {
            result.call_80032bbc_selected = true;
            append_helper_call(
                result,
                0x8001329cU,
                "FUN_80032bbc",
                input.current_actor_slot,
                0,
                "mode1_state2_call_mode0");
            append_state_write(
                result,
                0x800132a0U,
                2,
                3,
                "mode 1 helper dispatch advanced selector to state 3");
            result.selector_state_0x30 = 3;
            append_branch(result, "state2_call_80032bbc_mode0_then_state3");
        } else {
            append_branch(result, "mode1_path_exit_without_query");
        }
        return result;

    case 2:
        append_branch(result, "effective_mode_2_path");
        if (selector_state == 3) {
            result.helper_family_selected = true;
            append_branch(result, "state3_helper_family_selected");
            result.call_80032bbc_selected = true;
            append_helper_call(
                result,
                0x8001338cU,
                "FUN_80032bbc",
                input.current_actor_slot,
                1,
                "mode2_state3_tail_call_mode1");
            append_branch(result, "effective_mode2_tail_call_80032bbc_mode1");
            return result;
        }
        if (selector_state > 3) {
            result.call_80032bbc_selected = true;
            append_helper_call(
                result,
                0x8001338cU,
                "FUN_80032bbc",
                input.current_actor_slot,
                1,
                "mode2_state_gt3_tail_call_mode1");
            append_branch(result, "state_gt3_tail_call_80032bbc_mode1");
            return result;
        }
        if (selector_state < 2) {
            result.call_80032bbc_selected = true;
            append_helper_call(
                result,
                0x8001338cU,
                "FUN_80032bbc",
                input.current_actor_slot,
                1,
                "mode2_state_lt2_tail_call_mode1");
            append_branch(result, "state_lt2_tail_call_80032bbc_mode1");
            return result;
        }

        append_branch(result, "selector_state_2_mode0e_gate");
        result.mode0e_query_reached = true;
        if (!input.selected_aux_table.has_value()) {
            result.status = ActionViewSelectorStatus::MissingInput;
            result.unsupported_without_aux_table = true;
            append_branch(result, "missing_selected_aux_table");
            return result;
        }

        result.mode0e_count = count_matching_std0_entries(
            &*input.selected_aux_table,
            mode0e_action_view_count_query());
        append_branch_value(result, "mode0e_count", result.mode0e_count->count);
        result.mode0e_synthetic_call_selected = result.mode0e_count->count == 0;
        if (result.mode0e_synthetic_call_selected) {
            result.synthetic_call_80053f38_selected = true;
            append_helper_call(
                result,
                0x80013334U,
                "FUN_80053f38",
                input.current_actor_slot,
                0,
                "mode2_count_zero_spawn_mode0");
            record_spawned_action_view_record_mode(
                result,
                input,
                0,
                0x80013334U,
                "mode2_count_zero_spawn_mode0");
        }
        append_branch(result, result.mode0e_synthetic_call_selected
            ? "mode0e_synthetic_call_selected"
            : "mode0e_synthetic_call_suppressed_by_std0_match");

        append_state_write(
            result,
            0x80013358U,
            2,
            3,
            "mode 2 category gate advanced selector to state 3");
        result.selector_state_0x30 = 3;
        result.call_80032bbc_selected = true;
        append_helper_call(
            result,
            0x8001338cU,
            "FUN_80032bbc",
            input.current_actor_slot,
            1,
            "mode2_tail_call_mode1");
        append_branch(result, "selector_state_written_3_after_gate");
        append_branch(result, "effective_mode2_tail_call_80032bbc_mode1");
        return result;

    case 3:
        append_branch(result, "effective_mode_3_path");
        if (selector_state == 3) {
            result.helper_family_selected = true;
            append_branch(result, "state3_helper_family_selected");
            return result;
        }
        if (selector_state != 2) {
            append_branch(result, "mode3_path_exit_without_query");
            return result;
        }
        result.mode3_query_reached = true;
        append_branch(result, "selector_state_2_mode3_aux_gate");
        if (!input.selected_aux_table.has_value()) {
            result.status = ActionViewSelectorStatus::MissingInput;
            result.unsupported_without_aux_table = true;
            append_branch(result, "missing_selected_aux_table");
            return result;
        }
        result.mode3_count = count_matching_std0_entries(
            &*input.selected_aux_table,
            mode3_action_view_count_query());
        append_branch_value(result, "mode3_count", result.mode3_count->count);
        result.mode3_synthetic_call_selected = result.mode3_count->count == 0;
        if (result.mode3_synthetic_call_selected) {
            result.synthetic_call_80053f38_selected = true;
            append_helper_call(
                result,
                0x800133e4U,
                "FUN_80053f38",
                input.current_actor_slot,
                0,
                "mode3_count_zero_spawn_mode0");
            record_spawned_action_view_record_mode(
                result,
                input,
                0,
                0x800133e4U,
                "mode3_count_zero_spawn_mode0");
        }
        append_branch(result, result.mode3_synthetic_call_selected
            ? "mode3_synthetic_call_selected"
            : "mode3_synthetic_call_suppressed_by_std0_match");
        append_state_write(
            result,
            0x80013408U,
            2,
            3,
            "mode 3 category gate advanced selector to state 3");
        result.selector_state_0x30 = 3;
        append_branch(result, "selector_state_written_3_after_mode3_gate");
        return result;

    case 5:
        append_branch(result, "effective_mode_5_path");
        if (selector_state == 3) {
            result.call_80032bbc_selected = true;
            result.helper_family_selected = true;
            append_helper_call(
                result,
                0x800134a4U,
                "FUN_80032bbc",
                input.current_actor_slot,
                0,
                "mode5_state3_call_mode0");
            append_branch(result, "state3_call_80032bbc_mode0");
            append_branch(result, "state3_helper_family_selected");
            return result;
        }
        if (selector_state == 4) {
            result.helper_family_selected = true;
            append_branch(result, "state4_helper_family_selected");
            return result;
        }
        if (selector_state != 2) {
            append_branch(result, "mode5_path_exit_without_query");
            return result;
        }
        result.mode5_query_reached = true;
        append_branch(result, "selector_state_2_mode5_aux_gate");
        if (!input.selected_aux_table.has_value()) {
            result.status = ActionViewSelectorStatus::MissingInput;
            result.unsupported_without_aux_table = true;
            append_branch(result, "missing_selected_aux_table");
            return result;
        }
        result.mode5_count = count_matching_std0_entries(
            &*input.selected_aux_table,
            mode5_action_view_count_query(
                input.instruction_field6_0x6,
                input.instruction_field8_0x8));
        append_branch_value(result, "mode5_count", result.mode5_count->count);
        result.mode5_count_selected_state4 = result.mode5_count->count != 0;
        if (result.mode5_count_selected_state4) {
            append_state_write(
                result,
                0x80013474U,
                2,
                4,
                "mode 5 matching auxiliary row advanced selector to state 4");
            result.selector_state_0x30 = 4;
            append_branch(result, "mode5_count_nonzero_selector_state_written_4");
        } else {
            append_state_write(
                result,
                0x80013490U,
                2,
                3,
                "mode 5 zero-count helper path advanced selector to state 3");
            result.selector_state_0x30 = 3;
            result.call_80032bbc_selected = true;
            result.helper_family_selected = true;
            append_helper_call(
                result,
                0x800134a4U,
                "FUN_80032bbc",
                input.current_actor_slot,
                0,
                "mode5_count_zero_call_mode0");
            append_branch(result, "mode5_count_zero_selector_state_written_3");
            append_branch(result, "mode5_count_zero_call_80032bbc_mode0");
            append_branch(result, "mode5_count_zero_helper_family_selected");
        }
        return result;

    default:
        append_branch(result, "effective_mode_exit_without_dispatch_body");
        return result;
    }

    return result;
}

const char* action_view_selector_model_rule_detail() {
    return "Disassembly-backed model of FUN_80012f58 through the mode-0xe gate: "
           "the 8001d41c return initializes actor-change selector state 0x30 "
           "to 2 for role reversal or 0 for the ordinary actor; "
           "field6_0x6 is mapped through jump table 802db024 to a requested mode; "
           "an ordinary state-0 actor change publishes mode 0x11, synchronously "
           "sets IW+0xF0 bit 0x02000000, rereads it, and retains state 1; "
           "state 1 accepts a later requested mode but suppresses category "
           "publication until the current actor gate clears and advances state 2; "
           "FUN_80053f38 writes action-view record mode from its own table "
           "802df24c: param_2 != 0 selects 0x11, field6 4/8 selects 0xe, "
           "5/0x1d selects 1, 0xc selects 0xf, 0x11 selects 0x10, and "
           "default selects 2; "
           "controller state transitions enforce exactly-once category dispatch; "
           "the effective mode 2 / selector state 2 path loads "
           "action_thread->0x24->0x10->0x30 and calls "
           "STD::CountMatchingStd0Entries_80009030(root, 4, -1, 0x2a, 3); "
           "a zero count selects the synthetic 80053f38 mode-0xe call, and a "
           "nonzero count suppresses it; sibling effective mode 3 and 5 paths "
           "use the same aux table with predicates (5, -1, 0x2a, 3) and "
           "(field6, field8, 0x2e, 3).";
}

} // namespace savor::predict
