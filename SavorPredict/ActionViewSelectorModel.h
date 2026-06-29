#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

struct Std0PayloadGateFields {
    std::int16_t primary_action_key = 0;
    std::int16_t generic_secondary_key = 0;
    std::int16_t direct_gate_secondary_key = 0;
};

struct Std0EntryRecord {
    std::int16_t location_code = -1;
    std::int16_t opcode = 0;
    Std0PayloadGateFields payload{};
    bool has_payload = false;
};

struct Std0Table {
    std::vector<Std0EntryRecord> entries;
    bool includes_sentinel = false;
};

struct Std0CountQuery {
    std::int16_t action_key = 0;
    std::int16_t secondary_key = -1;
    std::int16_t location_code = -1;
    std::int16_t opcode = -1;
};

struct Std0CountResult {
    int count = 0;
    int scanned_entries = 0;
    bool reached_sentinel = false;
    bool used_combined_id_filter = false;
};

struct ActionViewSelectorInput {
    std::int16_t instruction_field6_0x6 = 0;
    std::int16_t instruction_field8_0x8 = 0;
    std::int8_t previous_effective_mode_0x2f = 0;
    std::int16_t previous_selector_state_0x30 = 0;
    std::int16_t previous_actor_slot_0x2 = -1;
    std::int16_t current_actor_slot = -1;
    std::int16_t current_secondary_slot = -1;
    bool instruction_flags_bit6_set = false;
    bool helper_800153e0_result = false;
    std::optional<bool> actor_lookup_8001d41c_nonzero;
    std::optional<Std0Table> selected_aux_table;
};

struct ActionViewSelectorHelperCall {
    std::uint32_t call_site_pc = 0;
    std::string callee;
    std::int16_t actor_slot = -1;
    std::optional<std::int16_t> mode_arg;
    std::string role;
};

struct ActionViewSelectorResult {
    std::int8_t requested_mode = 1;
    std::int8_t dispatch_effective_mode_0x2f = 0;
    std::int16_t selector_state_0x30 = 0;
    std::optional<std::int16_t> selector_actor_slot_0x2_written;
    std::optional<std::int16_t> selector_secondary_slot_0x4_written;
    std::optional<std::int16_t> spawned_action_view_record_mode_if_known;
    std::optional<Std0CountResult> mode0e_count;
    std::optional<Std0CountResult> mode3_count;
    std::optional<Std0CountResult> mode5_count;
    bool mode0e_query_reached = false;
    bool mode3_query_reached = false;
    bool mode5_query_reached = false;
    bool mode0e_synthetic_call_selected = false;
    bool mode3_synthetic_call_selected = false;
    bool mode5_count_selected_state4 = false;
    bool synthetic_call_80053f38_selected = false;
    bool call_80032bbc_selected = false;
    bool helper_family_selected = false;
    bool unsupported_without_aux_table = false;
    std::vector<ActionViewSelectorHelperCall> helper_calls;
    std::vector<std::string> branch_path;
};

std::uint32_t std0_combined_entry_id(std::int16_t location_code, std::int16_t opcode);

bool match_std_payload_action_key(
    std::int16_t payload_primary_action_key,
    std::int16_t payload_direct_gate_secondary_key,
    std::int16_t query_action_key,
    std::int16_t query_secondary_key);

Std0CountResult count_matching_std0_entries(
    const Std0Table* table,
    const Std0CountQuery& query);

Std0CountQuery mode0e_action_view_count_query();
Std0CountQuery mode3_action_view_count_query();
Std0CountQuery mode5_action_view_count_query(
    std::int16_t instruction_field6_0x6,
    std::int16_t instruction_field8_0x8);

std::int16_t action_view_record_mode_from_80053f38(
    std::int16_t instruction_field6_0x6,
    std::int16_t helper_param_2);

std::int8_t action_view_requested_mode_from_field6(
    std::int16_t instruction_field6_0x6,
    std::int16_t instruction_field8_0x8,
    bool helper_800153e0_result);

ActionViewSelectorResult select_action_view_mode(const ActionViewSelectorInput& input);

const char* action_view_selector_model_rule_detail();

} // namespace savor::predict
