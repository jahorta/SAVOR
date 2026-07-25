#include "ActionViewGateCheckpointModel.h"

#include "ActionViewStdResourceResolver.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <utility>

namespace savor::predict {

namespace {

constexpr int kExpectedQueryArg0 = 4;
constexpr int kExpectedQueryArg1 = -1;
constexpr int kExpectedQueryArg2 = 0x2a;
constexpr int kExpectedQueryArg3 = 3;
constexpr int kExpectedSelectedRecordMode = 0;
constexpr const char* kMode0eOwner = "mode0e_action_view_camera";
constexpr const char* kMode0FallbackOwner = "mode0_action_view_camera_fallback";
constexpr const char* kAttackHitOwner = "attack_hit_dodge";

struct KnownStd0Candidate {
    int actor_slot = -1;
    std::string resource_stem;
    std::string std_filename;
    std::string std0_filename;
    std::string std0_json_path;
    std::string materialization_source;
    Std0Table table;
};

std::optional<KnownStd0Candidate> expected_candidate_for_actor_slot(
    int actor_slot,
    const std::filesystem::path& spice_std_json_dir);

std::optional<int> parse_field_int(const CheckpointEvent& event, const char* field_name) {
    const auto found = event.fields.find(field_name);
    if (found == event.fields.end()) {
        return std::nullopt;
    }

    const auto& text = found->second;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(text.c_str(), &end, 16);
        if (end == text.c_str() || *end != '\0') {
            return std::nullopt;
        }
        if (parsed <= static_cast<unsigned long>(std::numeric_limits<int>::max())) {
            return static_cast<int>(parsed);
        }
        if (parsed <= static_cast<unsigned long>(std::numeric_limits<std::uint32_t>::max())) {
            return static_cast<int>(static_cast<std::int32_t>(parsed));
        }
        return std::nullopt;
    }

    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 0);
    if (end == text.c_str() || *end != '\0') {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

std::optional<int> parse_first_field_int(
    const CheckpointEvent& event,
    std::initializer_list<const char*> field_names) {
    for (const auto* field_name : field_names) {
        if (const auto value = parse_field_int(event, field_name); value.has_value()) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<int> action_sequence_id_from_event(const CheckpointEvent& event) {
    return parse_first_field_int(
        event,
        {"action_sequence_id", "action_sequence", "attack_sequence", "attack_index", "action_index", "sequence_id"});
}

std::optional<std::string> parse_first_field_string(
    const CheckpointEvent& event,
    std::initializer_list<const char*> field_names) {
    for (const auto* field_name : field_names) {
        const auto found = event.fields.find(field_name);
        if (found != event.fields.end() && !found->second.empty()) {
            return found->second;
        }
    }
    return std::nullopt;
}

std::optional<bool> parse_field_bool(const CheckpointEvent& event, const char* field_name) {
    const auto found = event.fields.find(field_name);
    if (found == event.fields.end()) {
        return std::nullopt;
    }

    std::string lowered = found->second;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (lowered == "1" || lowered == "true" || lowered == "yes") {
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no") {
        return false;
    }
    return std::nullopt;
}

std::optional<bool> parse_first_field_bool(
    const CheckpointEvent& event,
    std::initializer_list<const char*> field_names) {
    for (const auto* field_name : field_names) {
        if (const auto value = parse_field_bool(event, field_name); value.has_value()) {
            return value;
        }
    }
    return std::nullopt;
}

bool owner_is(const CheckpointEvent& event, const char* owner) {
    return event.known_rng_owner == owner;
}

bool is_gate_checkpoint(const CheckpointEvent& event) {
    return event.pc == "80012F58"
        || event.function == "FUN_80009030"
        || event.checkpoint == "action_view_gate"
        || event.checkpoint == "action_view_query"
        || event.checkpoint == "action_view_query_result";
}

bool is_legacy_gate_checkpoint(const CheckpointEvent& event) {
    return event.checkpoint == "action_view_gate";
}

bool is_query_call_checkpoint(const CheckpointEvent& event) {
    return event.checkpoint == "action_view_query"
        || event.checkpoint.find("query_call") != std::string::npos;
}

bool is_query_result_checkpoint(const CheckpointEvent& event) {
    return event.checkpoint == "action_view_query_result"
        || event.checkpoint.find("query_result") != std::string::npos;
}

std::optional<unsigned int> parse_pc_u32(const std::string& pc) {
    if (pc.empty()) {
        return std::nullopt;
    }
    std::string text = pc;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.erase(0, 2);
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 16);
    if (end == text.c_str() || *end != '\0') {
        return std::nullopt;
    }
    if (parsed > static_cast<unsigned long>(std::numeric_limits<unsigned int>::max())) {
        return std::nullopt;
    }
    return static_cast<unsigned int>(parsed);
}

bool is_action_view_helper_call_checkpoint(const CheckpointEvent& event) {
    if (event.checkpoint == "action_view_spawn"
        || event.checkpoint == "action_view_helper"
        || event.checkpoint.find("action_view_spawn") != std::string::npos
        || event.checkpoint.find("action_view_helper") != std::string::npos) {
        return true;
    }
    const auto pc = parse_pc_u32(event.pc);
    if (!pc.has_value()) {
        return false;
    }
    switch (*pc) {
    case 0x8001318cU:
    case 0x8001321cU:
    case 0x8001329cU:
    case 0x80013334U:
    case 0x8001338cU:
    case 0x800133e4U:
    case 0x800134a4U:
        return event.function == "FUN_80053f38" || event.function == "FUN_80032bbc";
    default:
        return false;
    }
}

bool is_dispatch_checkpoint(const CheckpointEvent& event) {
    return event.pc == "80051424"
        || event.checkpoint == "action_view_dispatch_state"
        || event.function == "UpdateActionViewRecord";
}

bool has_query_args(const ActionViewGateCheckpointEvent& event) {
    return event.query_arg0.has_value()
        && event.query_arg1.has_value()
        && event.query_arg2.has_value()
        && event.query_arg3.has_value();
}

bool query_args_match(const ActionViewGateCheckpointEvent& event) {
    return has_query_args(event)
        && *event.query_arg0 == kExpectedQueryArg0
        && *event.query_arg1 == kExpectedQueryArg1
        && *event.query_arg2 == kExpectedQueryArg2
        && *event.query_arg3 == kExpectedQueryArg3;
}

bool query_args_match_expected(
    const ActionViewGateCheckpointEvent& event,
    const Std0CountQuery& expected) {
    return has_query_args(event)
        && *event.query_arg0 == expected.action_key
        && *event.query_arg1 == expected.secondary_key
        && *event.query_arg2 == expected.location_code
        && *event.query_arg3 == expected.opcode;
}

std::optional<unsigned int> parse_first_field_u32(
    const CheckpointEvent& event,
    std::initializer_list<const char*> field_names) {
    for (const auto* field_name : field_names) {
        const auto found = event.fields.find(field_name);
        if (found == event.fields.end()) {
            continue;
        }
        std::string text = found->second;
        int base = 0;
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            base = 16;
            text.erase(0, 2);
        }
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(text.c_str(), &end, base == 0 ? 0 : base);
        if (end == text.c_str() || *end != '\0') {
            continue;
        }
        if (parsed <= static_cast<unsigned long>(std::numeric_limits<unsigned int>::max())) {
            return static_cast<unsigned int>(parsed);
        }
    }
    return std::nullopt;
}

std::string aux_row_field_name(int row, const char* suffix) {
    std::ostringstream out;
    out << "aux_row" << std::setw(2) << std::setfill('0') << row << "_" << suffix;
    return out.str();
}

std::optional<std::int16_t> parse_aux_row_i16(
    const CheckpointEvent& event,
    int row,
    const char* suffix) {
    const auto value = parse_field_int(event, aux_row_field_name(row, suffix).c_str());
    if (!value.has_value()) {
        return std::nullopt;
    }
    return static_cast<std::int16_t>(*value);
}

std::optional<Std0Table> sampled_aux_table_from_event(const CheckpointEvent& event) {
    constexpr int kMaxSampledRows = 32;
    Std0Table table;

    for (int row = 0; row < kMaxSampledRows; ++row) {
        const auto location = parse_aux_row_i16(event, row, "location_code");
        if (!location.has_value()) {
            break;
        }

        Std0EntryRecord record;
        record.location_code = *location;
        record.opcode = parse_aux_row_i16(event, row, "opcode").value_or(0);
        if (const auto primary = parse_aux_row_i16(event, row, "payload_primary");
            primary.has_value()) {
            record.payload.primary_action_key = *primary;
            record.payload.generic_secondary_key =
                parse_aux_row_i16(event, row, "payload_secondary").value_or(0);
            record.payload.direct_gate_secondary_key =
                parse_aux_row_i16(event, row, "payload_direct_secondary").value_or(0);
            record.has_payload = true;
        }
        table.entries.push_back(record);
        if (record.location_code < 0) {
            table.includes_sentinel = true;
            break;
        }
    }

    if (table.entries.empty()) {
        return std::nullopt;
    }
    return table;
}

std::optional<Std0CountQuery> selector_expected_query_from_result(
    const ActionViewSelectorResult& selector,
    const ActionViewSelectorInput& input) {
    if (selector.mode0e_query_reached) {
        return mode0e_action_view_count_query();
    }
    if (selector.mode3_query_reached) {
        return mode3_action_view_count_query();
    }
    if (selector.mode5_query_reached) {
        return mode5_action_view_count_query(
            input.instruction_field6_0x6,
            input.instruction_field8_0x8);
    }
    return std::nullopt;
}

std::optional<ActionViewSelectorInput> selector_input_from_gate_event(
    const ActionViewGateCheckpointEvent& event) {
    const auto field6 = event.actor_field6_0x6.has_value()
        ? event.actor_field6_0x6
        : event.source_field6_0x6;
    if (!field6.has_value()
        || !event.gate_category_0x2f.has_value()
        || !event.gate_state_0x30.has_value()) {
        return std::nullopt;
    }

    ActionViewSelectorInput input;
    input.instruction_field6_0x6 = static_cast<std::int16_t>(*field6);
    input.instruction_field8_0x8 = static_cast<std::int16_t>(event.actor_subtype_0x8.value_or(0));
    input.previous_effective_mode_0x2f = static_cast<std::int8_t>(*event.gate_category_0x2f);
    input.previous_selector_state_0x30 = static_cast<std::int16_t>(*event.gate_state_0x30);
    input.current_actor_slot = static_cast<std::int16_t>(event.active_slot.value_or(-1));
    input.current_secondary_slot = static_cast<std::int16_t>(event.target_slot.value_or(-1));
    input.previous_actor_slot_0x2 = static_cast<std::int16_t>(
        event.gate_active_slot_0x02.value_or(input.current_actor_slot));
    input.actor_instruction_flags_0xf0 = event.instruction_flags_0xf0;
    if (input.previous_actor_slot_0x2 == input.current_actor_slot) {
        input.actor_lookup_8001d41c_nonzero = false;
    }
    return input;
}

std::optional<Std0Table> expected_aux_table_for_event(
    const ActionViewGateCheckpointEvent& event,
    const ActionViewGateCheckpointOptions& options) {
    if (!event.active_slot.has_value()) {
        return std::nullopt;
    }
    const auto expected = expected_candidate_for_actor_slot(*event.active_slot, options.action_view_std_json_dir);
    if (!expected.has_value()) {
        return std::nullopt;
    }
    return expected->table;
}

bool helper_call_matches_prediction(
    const ActionViewSelectorHelperCall& expected,
    const ActionViewGateCheckpointEvent& observed) {
    if (!observed.helper_call_site_pc.has_value()
        || expected.call_site_pc != *observed.helper_call_site_pc) {
        return false;
    }
    if (observed.helper_callee.has_value() && expected.callee != *observed.helper_callee) {
        return false;
    }
    if (observed.helper_actor_slot.has_value()
        && expected.actor_slot != *observed.helper_actor_slot) {
        return false;
    }
    if (observed.helper_mode_arg.has_value()) {
        return expected.mode_arg.has_value() && *expected.mode_arg == *observed.helper_mode_arg;
    }
    return true;
}

const ActionViewSelectorHelperCall* find_predicted_helper_call(
    const ActionViewSelectorResult& result,
    const ActionViewGateCheckpointEvent& observed) {
    const auto exact = std::find_if(
        result.helper_calls.begin(),
        result.helper_calls.end(),
        [&](const ActionViewSelectorHelperCall& expected) {
            return helper_call_matches_prediction(expected, observed);
        });
    if (exact != result.helper_calls.end()) {
        return &*exact;
    }
    if (!observed.helper_call_site_pc.has_value()) {
        return nullptr;
    }
    const auto same_pc = std::find_if(
        result.helper_calls.begin(),
        result.helper_calls.end(),
        [&](const ActionViewSelectorHelperCall& expected) {
            return expected.call_site_pc == *observed.helper_call_site_pc;
        });
    return same_pc == result.helper_calls.end() ? nullptr : &*same_pc;
}

bool has_scheduler_chain(const ActionViewGateCheckpointEvent& event) {
    return event.action_child_thread.has_value()
        && event.child_payload.has_value()
        && event.nested_payload.has_value()
        && event.aux_list_root.has_value();
}

bool has_spicestd_payload_fields(const ActionViewDispatchCheckpointEvent& event) {
    return event.payload_primary_key.has_value()
        && event.payload_secondary_key.has_value()
        && event.payload_flags.has_value()
        && event.payload_start_frame.has_value()
        && event.payload_end_frame.has_value()
        && event.payload_hold.has_value()
        && event.payload_step.has_value()
        && event.payload_mode.has_value();
}

ActionViewGateCheckpointStatus classify_status(const ActionViewGateCheckpointSummary& summary) {
    if (summary.observed_gate_events == 0 && summary.observed_helper_call_events == 0) {
        return ActionViewGateCheckpointStatus::ObservedOnly;
    }
    const bool legacy_missing_fields =
        summary.legacy_gate_events > 0
        && (summary.events_with_aux_list_root < summary.legacy_gate_events
            || summary.events_with_query_args < summary.legacy_gate_events
            || summary.events_with_query_result < summary.legacy_gate_events
            || summary.events_with_selected_record_mode < summary.legacy_gate_events);
    const bool split_missing_fields =
        (summary.query_call_events > 0
            && summary.query_call_events_with_query_args != summary.query_call_events)
        || (summary.query_result_events > 0
            && summary.query_result_events_with_query_result != summary.query_result_events);
    if (legacy_missing_fields || split_missing_fields) {
        return ActionViewGateCheckpointStatus::MissingLiveGateFields;
    }
    if (summary.query_args_mismatch > 0 && summary.selector_model_comparisons == 0) {
        return ActionViewGateCheckpointStatus::QueryArgsMismatch;
    }
    if (summary.selector_query_args_mismatch > 0
        || (summary.selector_model_comparisons > 0
            && summary.selector_model_missing_expected_query > 0)
        || summary.selector_helper_call_mismatches > 0
        || summary.selector_helper_call_missing_expected > 0) {
        return ActionViewGateCheckpointStatus::SelectorModelMismatch;
    }
    if (summary.aux_table_count_mismatches_query_result > 0) {
        return ActionViewGateCheckpointStatus::AuxTableCountMismatch;
    }
    if (summary.selected_mode_mismatches > 0) {
        return ActionViewGateCheckpointStatus::SelectedModeMismatch;
    }
    if (summary.legacy_gate_events > 0
        && (summary.events_with_action_child_thread < summary.legacy_gate_events
            || summary.events_with_child_payload < summary.legacy_gate_events
            || summary.events_with_nested_payload < summary.legacy_gate_events
            || summary.events_with_child_thread_state < summary.legacy_gate_events
            || summary.events_with_scheduler_chain < summary.legacy_gate_events
            || summary.events_with_mode0_fallback_flag < summary.legacy_gate_events)) {
        return ActionViewGateCheckpointStatus::MissingSchedulerFields;
    }
    if (summary.action_sequence_order_mismatches > 0) {
        return ActionViewGateCheckpointStatus::ActionViewOrderMismatch;
    }
    return ActionViewGateCheckpointStatus::MatchesExpected;
}

void record_first_draw_for_sequence(
    std::map<int, int>& draws_by_sequence,
    const CheckpointEvent& event) {
    const auto sequence_id = action_sequence_id_from_event(event);
    if (!sequence_id.has_value() || !event.rng_draw_index_before.has_value()) {
        return;
    }
    draws_by_sequence.emplace(*sequence_id, *event.rng_draw_index_before);
}

bool same_optional_int(std::optional<int> lhs, std::optional<int> rhs) {
    return lhs.has_value() && rhs.has_value() && *lhs == *rhs;
}

bool std0_records_match(const Std0EntryRecord& sampled, const Std0EntryRecord& candidate) {
    if (sampled.location_code != candidate.location_code || sampled.opcode != candidate.opcode) {
        return false;
    }
    if (sampled.has_payload != candidate.has_payload) {
        return false;
    }
    if (!sampled.has_payload) {
        return true;
    }
    return sampled.payload.primary_action_key == candidate.payload.primary_action_key
        && sampled.payload.generic_secondary_key == candidate.payload.generic_secondary_key
        && sampled.payload.direct_gate_secondary_key == candidate.payload.direct_gate_secondary_key;
}

bool sampled_table_matches_candidate_at_offset(
    const Std0Table& sampled,
    const Std0Table& candidate,
    std::size_t sampled_offset) {
    if (sampled.entries.empty()
        || sampled_offset >= sampled.entries.size()
        || sampled.entries.size() - sampled_offset > candidate.entries.size()) {
        return false;
    }
    for (std::size_t i = sampled_offset; i < sampled.entries.size(); ++i) {
        const std::size_t candidate_index = i - sampled_offset;
        if (!std0_records_match(sampled.entries[i], candidate.entries[candidate_index])) {
            return false;
        }
        if (sampled.entries[i].location_code < 0) {
            return true;
        }
    }
    return true;
}

std::optional<int> sampled_table_candidate_row_offset(
    const Std0Table& sampled,
    const Std0Table& candidate) {
    if (sampled_table_matches_candidate_at_offset(sampled, candidate, 0)) {
        return 0;
    }

    // Live FUN_80009030 roots include a one-row runtime prefix before the
    // companion _0_STD entry rows. Keep this explicit so the trace can prove
    // both table identity and the materialized layout shape.
    if (sampled_table_matches_candidate_at_offset(sampled, candidate, 1)) {
        return 1;
    }
    return std::nullopt;
}

std::vector<KnownStd0Candidate> load_first_battle_std0_candidates(
    const std::filesystem::path& spice_std_json_dir) {
    std::vector<KnownStd0Candidate> candidates;
    if (spice_std_json_dir.empty()) {
        return candidates;
    }

    std::vector<std::string> seen_std0_filenames;
    for (const int actor_slot : {0, 1, 4, 5}) {
        auto resolved = resolve_first_battle_action_view_std0_table_for_slot(
            actor_slot,
            spice_std_json_dir);
        if (!resolved.ok) {
            continue;
        }
        if (std::find(seen_std0_filenames.begin(), seen_std0_filenames.end(), resolved.std0_filename)
            != seen_std0_filenames.end()) {
            continue;
        }
        seen_std0_filenames.push_back(resolved.std0_filename);
        candidates.push_back(KnownStd0Candidate{
            .actor_slot = actor_slot,
            .resource_stem = resolved.resource_stem,
            .std_filename = resolved.std_filename,
            .std0_filename = resolved.std0_filename,
            .std0_json_path = resolved.std0_json_path.string(),
            .materialization_source =
                action_view_std_materialization_source_name(resolved.materialization_source),
            .table = resolved.table,
        });
    }
    return candidates;
}

std::optional<KnownStd0Candidate> expected_candidate_for_actor_slot(
    int actor_slot,
    const std::filesystem::path& spice_std_json_dir) {
    if (spice_std_json_dir.empty()) {
        return std::nullopt;
    }
    auto resolved = resolve_first_battle_action_view_std0_table_for_slot(
        actor_slot,
        spice_std_json_dir);
    if (!resolved.ok) {
        return std::nullopt;
    }
    return KnownStd0Candidate{
        .actor_slot = actor_slot,
        .resource_stem = resolved.resource_stem,
        .std_filename = resolved.std_filename,
        .std0_filename = resolved.std0_filename,
        .std0_json_path = resolved.std0_json_path.string(),
        .materialization_source =
            action_view_std_materialization_source_name(resolved.materialization_source),
        .table = resolved.table,
    };
}

void attach_std0_identity(
    ActionViewGateCheckpointSummary& summary,
    ActionViewGateCheckpointEvent& observed,
    const Std0Table& sampled_aux_table,
    const std::vector<KnownStd0Candidate>& candidates,
    const ActionViewGateCheckpointOptions& options) {
    if (candidates.empty()) {
        return;
    }

    std::vector<const KnownStd0Candidate*> matches;
    std::optional<int> unique_match_offset;
    for (const auto& candidate : candidates) {
        const auto offset = sampled_table_candidate_row_offset(sampled_aux_table, candidate.table);
        if (offset.has_value()) {
            matches.push_back(&candidate);
            unique_match_offset = offset;
        }
    }
    observed.matched_std0_candidate_count = static_cast<int>(matches.size());
    if (matches.empty()) {
        return;
    }
    if (matches.size() == 1) {
        ++summary.aux_table_fingerprint_matches_known_std0;
        observed.matched_resource_stem = matches.front()->resource_stem;
        observed.matched_std_filename = matches.front()->std_filename;
        observed.matched_std0_filename = matches.front()->std0_filename;
        observed.matched_std0_json_path = matches.front()->std0_json_path;
        observed.matched_std0_materialization_source = matches.front()->materialization_source;
        observed.matched_std0_sample_row_offset = unique_match_offset;
    } else {
        ++summary.aux_table_fingerprint_ambiguous_known_std0;
    }

    if (!observed.active_slot.has_value()) {
        return;
    }
    const auto expected = expected_candidate_for_actor_slot(
        *observed.active_slot,
        options.action_view_std_json_dir);
    if (!expected.has_value()) {
        return;
    }
    observed.actor_slot_expected_std0_filename = expected->std0_filename;
    observed.actor_slot_expected_std0_sample_row_offset =
        sampled_table_candidate_row_offset(sampled_aux_table, expected->table);
    observed.actor_slot_expected_std0_matches_sample =
        observed.actor_slot_expected_std0_sample_row_offset.has_value();
    if (*observed.actor_slot_expected_std0_matches_sample) {
        ++summary.aux_table_fingerprint_matches_actor_slot_std0;
    } else {
        ++summary.aux_table_fingerprint_mismatches_actor_slot_std0;
    }
}

bool result_matches_query_call_context(
    const ActionViewGateCheckpointEvent& call,
    const ActionViewGateCheckpointEvent& result) {
    return result.query_result_event
        && same_optional_int(call.draw_index, result.draw_index)
        && same_optional_int(call.active_slot, result.active_slot)
        && same_optional_int(call.target_slot, result.target_slot)
        && same_optional_int(call.gate_category_0x2f, result.gate_category_0x2f)
        && same_optional_int(call.gate_state_0x30, result.gate_state_0x30);
}

} // namespace

ActionViewGateCheckpointSummary summarize_action_view_gate_checkpoints(
    const std::vector<CheckpointEvent>& events) {
    return summarize_action_view_gate_checkpoints(events, {});
}

ActionViewGateCheckpointSummary summarize_action_view_gate_checkpoints(
    const std::vector<CheckpointEvent>& events,
    const ActionViewGateCheckpointOptions& options) {
    ActionViewGateCheckpointSummary summary;
    std::map<int, int> mode0e_draw_by_sequence;
    std::map<int, int> attack_hit_draw_by_sequence;
    const auto known_std0_candidates =
        load_first_battle_std0_candidates(options.action_view_std_json_dir);

    for (const auto& event : events) {
        if (owner_is(event, kMode0eOwner)) {
            ++summary.observed_mode0e_camera_draws;
            record_first_draw_for_sequence(mode0e_draw_by_sequence, event);
            if (!summary.first_mode0e_draw_index.has_value()) {
                summary.first_mode0e_draw_index = event.rng_draw_index_before;
            }
            continue;
        }
        if (owner_is(event, kMode0FallbackOwner)) {
            ++summary.observed_mode0_fallback_draws;
            if (!summary.first_mode0_fallback_draw_index.has_value()) {
                summary.first_mode0_fallback_draw_index = event.rng_draw_index_before;
            }
            continue;
        }
        if (owner_is(event, kAttackHitOwner)) {
            ++summary.observed_attack_hit_draws;
            record_first_draw_for_sequence(attack_hit_draw_by_sequence, event);
            if (!summary.first_attack_hit_draw_index.has_value()) {
                summary.first_attack_hit_draw_index = event.rng_draw_index_before;
            }
            continue;
        }
        if (is_dispatch_checkpoint(event)) {
            ActionViewDispatchCheckpointEvent observed;
            observed.draw_index = event.rng_draw_index_before;
            observed.payload_primary_key = parse_first_field_int(event, {"payload_primary_0x00", "payload_primary"});
            observed.payload_secondary_key =
                parse_first_field_int(event, {"payload_secondary_0x02", "payload_secondary"});
            observed.payload_flags =
                parse_first_field_string(event, {"payload_flags_0x10", "payload_flags"});
            observed.payload_start_frame =
                parse_first_field_int(event, {"payload_start_frame_0x18", "payload_start_frame"});
            observed.payload_end_frame =
                parse_first_field_int(event, {"payload_end_frame_0x1c", "payload_end_frame"});
            observed.payload_hold =
                parse_first_field_int(event, {"payload_hold_0x1e", "payload_hold"});
            observed.payload_step =
                parse_first_field_int(event, {"payload_step_0x20", "payload_step"});
            observed.payload_mode =
                parse_first_field_int(event, {"payload_mode_0x22", "payload_mode", "selected_record_mode"});
            observed.saved_mode =
                parse_first_field_int(event, {"worksheet_saved_mode_0x110", "saved_mode"});
            observed.effective_mode =
                parse_first_field_int(event, {"worksheet_effective_mode_0x112", "effective_mode"});
            observed.worksheet_turn_timer =
                parse_first_field_int(event, {"worksheet_turn_timer_0x70", "turn_timer"});
            observed.instruction_flags =
                parse_first_field_string(event, {"instruction_flags_0xf0", "instruction_flags"});
            observed.global_camera_override =
                parse_first_field_string(event, {"global_camera_override_80347394", "global_camera_override"});
            observed.global_camera_flags =
                parse_first_field_string(event, {"global_camera_flags_803472F4", "global_camera_flags"});

            ++summary.observed_dispatch_events;
            if (observed.payload_mode.has_value()) {
                ++summary.dispatch_events_with_payload_mode;
                if (*observed.payload_mode == 0) {
                    ++summary.dispatch_serialized_mode0_events;
                }
            }
            if (observed.effective_mode.has_value()) {
                ++summary.dispatch_events_with_effective_mode;
                if (*observed.effective_mode == 0) {
                    ++summary.dispatch_effective_mode0_events;
                } else if (*observed.effective_mode == 0x0e) {
                    ++summary.dispatch_effective_mode0e_events;
                }
            }
            if (observed.payload_mode.has_value() && observed.effective_mode.has_value()) {
                if (*observed.payload_mode == 0 && *observed.effective_mode == 0x0e) {
                    ++summary.dispatch_mode0_to_mode0e_rewrites;
                } else if (*observed.payload_mode == 0 && *observed.effective_mode == 0) {
                    ++summary.dispatch_mode0_stays_mode0_events;
                }
            }
            if (has_spicestd_payload_fields(observed)) {
                ++summary.dispatch_events_with_spicestd_payload_fields;
            }
            summary.dispatch_events.push_back(std::move(observed));
            continue;
        }
        if (is_action_view_helper_call_checkpoint(event)) {
            ActionViewGateCheckpointEvent observed;
            observed.checkpoint = event.checkpoint;
            observed.helper_call_event = true;
            observed.helper_call_site_pc = parse_pc_u32(event.pc);
            observed.helper_callee = parse_first_field_string(event, {"helper_callee", "callee"});
            if (!observed.helper_callee.has_value()
                && (event.function == "FUN_80053f38" || event.function == "FUN_80032bbc")) {
                observed.helper_callee = event.function;
            }
            observed.draw_index = event.rng_draw_index_before;
            observed.action_sequence_id = action_sequence_id_from_event(event);
            observed.active_slot = event.active_slot.has_value()
                ? event.active_slot
                : parse_first_field_int(event, {"active_slot", "actor_slot"});
            observed.source_slot = parse_first_field_int(event, {"source_slot", "source_actor_slot"});
            observed.target_slot = event.target_slot.has_value()
                ? event.target_slot
                : parse_first_field_int(event, {"target_slot"});
            observed.source_field6_0x6 =
                parse_first_field_int(event, {"source_field6_0x6", "source_field6", "source_field6_6"});
            observed.actor_field6_0x6 =
                parse_first_field_int(event, {"actor_field6_0x6", "actor_field6", "actor_field6_6"});
            observed.actor_subtype_0x8 =
                parse_first_field_int(event, {"actor_subtype_0x8", "actor_field8_0x8", "instruction_field8_0x8"});
            observed.gate_category_0x2f =
                parse_first_field_int(
                    event,
                    {"gate_category_0x2f", "effective_mode_0x2f", "previous_effective_mode_0x2f"});
            observed.gate_state_0x30 =
                parse_first_field_int(event, {"gate_state_0x30", "selector_state_0x30"});
            observed.gate_active_slot_0x02 =
                parse_first_field_int(event, {"gate_active_slot_0x02", "previous_actor_slot_0x2"});
            observed.gate_target_slot_0x04 =
                parse_first_field_int(event, {"gate_target_slot_0x04", "previous_target_slot_0x4"});
            observed.instruction_flags_0xf0 =
                parse_first_field_u32(event, {"instruction_flags_0xf0", "instruction_flags"});
            observed.aux_list_root =
                parse_first_field_string(event, {"aux_list_root", "aux_root", "list_root"});
            observed.helper_actor_slot = parse_first_field_int(
                event,
                {"spawn_slot_arg", "helper_slot_arg", "call_slot_arg", "helper_actor_slot", "actor_slot_arg"});
            if (!observed.helper_actor_slot.has_value()) {
                observed.helper_actor_slot = observed.active_slot;
            }
            observed.helper_mode_arg = parse_first_field_int(
                event,
                {"spawn_mode_arg", "helper_mode_arg", "call_mode_arg", "mode_arg"});

            ++summary.observed_helper_call_events;
            if (const auto selector_input = selector_input_from_gate_event(observed); selector_input.has_value()) {
                ++summary.helper_call_events_with_selector_inputs;
                auto comparable_input = *selector_input;
                if (const auto sampled_aux_table = sampled_aux_table_from_event(event);
                    sampled_aux_table.has_value()) {
                    comparable_input.selected_aux_table = sampled_aux_table;
                } else if (const auto expected_table = expected_aux_table_for_event(observed, options);
                           expected_table.has_value()) {
                    comparable_input.selected_aux_table = expected_table;
                }

                observed.selector_result_without_table = select_action_view_mode(comparable_input);
                if (!observed.selector_result_without_table->unsupported_without_aux_table) {
                    ++summary.selector_helper_call_comparisons;
                    const auto* expected = find_predicted_helper_call(
                        *observed.selector_result_without_table,
                        observed);
                    if (expected == nullptr) {
                        ++summary.selector_helper_call_missing_expected;
                        observed.selector_helper_call_matches = false;
                    } else {
                        observed.selector_expected_helper_role = expected->role;
                        observed.selector_expected_helper_callee = expected->callee;
                        if (expected->mode_arg.has_value()) {
                            observed.selector_expected_helper_mode_arg = *expected->mode_arg;
                        }
                        observed.selector_expected_spawned_record_mode =
                            observed.selector_result_without_table->spawned_action_view_record_mode_if_known;
                        observed.selector_helper_call_matches =
                            helper_call_matches_prediction(*expected, observed);
                        if (*observed.selector_helper_call_matches) {
                            ++summary.selector_helper_call_matches;
                        } else {
                            ++summary.selector_helper_call_mismatches;
                        }
                    }
                }
            }
            summary.events.push_back(std::move(observed));
            continue;
        }
        if (!is_gate_checkpoint(event)) {
            continue;
        }

        ActionViewGateCheckpointEvent observed;
        observed.checkpoint = event.checkpoint;
        observed.legacy_gate_event = is_legacy_gate_checkpoint(event);
        observed.query_call_event = is_query_call_checkpoint(event);
        observed.query_result_event = is_query_result_checkpoint(event);
        observed.draw_index = event.rng_draw_index_before;
        observed.action_sequence_id = action_sequence_id_from_event(event);
        observed.active_slot = event.active_slot.has_value()
            ? event.active_slot
            : parse_first_field_int(event, {"active_slot", "actor_slot"});
        observed.source_slot = parse_first_field_int(event, {"source_slot", "source_actor_slot"});
        observed.target_slot = event.target_slot.has_value()
            ? event.target_slot
            : parse_first_field_int(event, {"target_slot"});
        observed.source_field6_0x6 =
            parse_first_field_int(event, {"source_field6_0x6", "source_field6", "source_field6_6"});
        observed.actor_field6_0x6 =
            parse_first_field_int(event, {"actor_field6_0x6", "actor_field6", "actor_field6_6"});
        observed.actor_subtype_0x8 =
            parse_first_field_int(event, {"actor_subtype_0x8", "actor_field8_0x8", "instruction_field8_0x8"});
        observed.gate_category_0x2f =
            parse_first_field_int(event, {"gate_category_0x2f", "effective_mode_0x2f", "previous_effective_mode_0x2f"});
        observed.gate_state_0x30 =
            parse_first_field_int(event, {"gate_state_0x30", "selector_state_0x30"});
        observed.gate_active_slot_0x02 =
            parse_first_field_int(event, {"gate_active_slot_0x02", "previous_actor_slot_0x2"});
        observed.gate_target_slot_0x04 =
            parse_first_field_int(event, {"gate_target_slot_0x04", "previous_target_slot_0x4"});
        observed.instruction_flags_0xf0 =
            parse_first_field_u32(event, {"instruction_flags_0xf0", "instruction_flags"});
        observed.aux_list_root =
            parse_first_field_string(event, {"aux_list_root", "aux_root", "list_root"});
        observed.query_arg0 = parse_first_field_int(event, {"query_arg0", "query_a"});
        observed.query_arg1 = parse_first_field_int(event, {"query_arg1", "query_b"});
        observed.query_arg2 = parse_first_field_int(event, {"query_arg2", "query_c"});
        observed.query_arg3 = parse_first_field_int(event, {"query_arg3", "query_d"});
        observed.query_result =
            parse_first_field_string(event, {"query_result", "aux_result", "selected_record_ptr"});
        observed.query_result_count = parse_first_field_int(event, {"query_result", "aux_count", "count_result"});
        observed.selected_record_mode =
            parse_first_field_int(event, {"selected_record_mode", "record_mode", "selected_mode"});
        observed.action_child_thread = parse_first_field_string(
            event,
            {"action_child_thread", "child_thread", "action_view_child_thread", "action_view_thread", "thread"});
        observed.child_payload =
            parse_first_field_string(event, {"child_payload", "thread_payload", "payload_0x24"});
        observed.nested_payload =
            parse_first_field_string(event, {"nested_payload", "payload_nested", "payload_0x10", "nested_payload_0x10"});
        observed.child_thread_state_byte =
            parse_first_field_int(event, {"child_thread_state_byte", "thread_state_byte", "thread_0x19"});
        observed.mode0_fallback_reached =
            parse_first_field_bool(event, {"mode0_fallback_reached", "fallback_reached", "mode0_fallback"});

        ++summary.observed_gate_events;
        if (observed.legacy_gate_event) {
            ++summary.legacy_gate_events;
        }
        if (observed.query_call_event) {
            ++summary.query_call_events;
        }
        if (observed.query_result_event) {
            ++summary.query_result_events;
        }
        if (!summary.first_gate_draw_index.has_value()) {
            summary.first_gate_draw_index = observed.draw_index;
        }
        if (observed.aux_list_root.has_value()) {
            ++summary.events_with_aux_list_root;
        }
        if (has_query_args(observed)) {
            ++summary.events_with_query_args;
            if (observed.query_call_event) {
                ++summary.query_call_events_with_query_args;
            }
            if (query_args_match(observed)) {
                ++summary.query_args_match;
            } else {
                ++summary.query_args_mismatch;
            }
        }
        if (const auto selector_input = selector_input_from_gate_event(observed); selector_input.has_value()) {
            ++summary.events_with_selector_inputs;
            if (observed.legacy_gate_event || observed.query_call_event || has_query_args(observed)) {
                observed.selector_result_without_table = select_action_view_mode(*selector_input);
                observed.selector_expected_query =
                    selector_expected_query_from_result(*observed.selector_result_without_table, *selector_input);
                ++summary.selector_model_comparisons;
                if (observed.selector_expected_query.has_value()) {
                    observed.selector_query_args_match =
                        query_args_match_expected(observed, *observed.selector_expected_query);
                    if (*observed.selector_query_args_match) {
                        ++summary.selector_query_args_match;
                    } else {
                        ++summary.selector_query_args_mismatch;
                    }
                } else {
                    ++summary.selector_model_missing_expected_query;
                }
            }
        }
        if (observed.query_call_event) {
            if (const auto sampled_aux_table = sampled_aux_table_from_event(event);
                sampled_aux_table.has_value()) {
                observed.sampled_aux_table_rows =
                    static_cast<int>(sampled_aux_table->entries.size());
                observed.sampled_aux_table_includes_sentinel =
                    sampled_aux_table->includes_sentinel;
                ++summary.events_with_aux_table_fingerprint;
                if (observed.selector_expected_query.has_value()) {
                    const auto count = count_matching_std0_entries(
                        &*sampled_aux_table,
                        *observed.selector_expected_query);
                    observed.sampled_aux_table_count = count.count;
                    ++summary.events_with_aux_table_count;
                }
                attach_std0_identity(
                    summary,
                    observed,
                    *sampled_aux_table,
                    known_std0_candidates,
                    options);
            }
        }
        if (observed.query_result.has_value()) {
            ++summary.events_with_query_result;
            if (observed.query_result_event) {
                ++summary.query_result_events_with_query_result;
            }
        }
        if (observed.selected_record_mode.has_value()) {
            ++summary.events_with_selected_record_mode;
            if (*observed.selected_record_mode == kExpectedSelectedRecordMode) {
                ++summary.selected_mode_matches;
            } else {
                ++summary.selected_mode_mismatches;
            }
        }
        if (observed.action_child_thread.has_value()) {
            ++summary.events_with_action_child_thread;
        }
        if (observed.child_payload.has_value()) {
            ++summary.events_with_child_payload;
        }
        if (observed.nested_payload.has_value()) {
            ++summary.events_with_nested_payload;
        }
        if (observed.child_thread_state_byte.has_value()) {
            ++summary.events_with_child_thread_state;
        }
        if (has_scheduler_chain(observed)) {
            ++summary.events_with_scheduler_chain;
        }
        if (observed.mode0_fallback_reached.has_value()) {
            ++summary.events_with_mode0_fallback_flag;
            if (*observed.mode0_fallback_reached) {
                ++summary.mode0_fallback_reached_events;
            }
        }
        summary.events.push_back(std::move(observed));
    }

    for (auto& event : summary.events) {
        if (!event.query_call_event || !event.sampled_aux_table_count.has_value()) {
            continue;
        }
        auto matched_result = std::find_if(
            summary.events.begin(),
            summary.events.end(),
            [&](const ActionViewGateCheckpointEvent& candidate) {
                return result_matches_query_call_context(event, candidate);
            });
        if (matched_result == summary.events.end()
            || !matched_result->query_result_count.has_value()) {
            ++summary.aux_table_count_missing_query_result;
            continue;
        }
        event.matched_query_result_count = matched_result->query_result_count;
        event.sampled_aux_table_count_matches_query_result =
            *event.sampled_aux_table_count == *matched_result->query_result_count;
        if (*event.sampled_aux_table_count_matches_query_result) {
            ++summary.aux_table_count_matches_query_result;
        } else {
            ++summary.aux_table_count_mismatches_query_result;
        }
    }

    for (auto& event : summary.events) {
        if (!event.draw_index.has_value()) {
            continue;
        }
        if (summary.first_mode0e_draw_index.has_value()
            && *event.draw_index < *summary.first_mode0e_draw_index) {
            ++summary.gate_events_before_first_mode0e;
        }
        if (summary.first_attack_hit_draw_index.has_value()
            && *event.draw_index < *summary.first_attack_hit_draw_index) {
            ++summary.gate_events_before_first_attack_hit;
        }
        if (event.action_sequence_id.has_value()) {
            ++summary.events_with_action_sequence_id;
            const auto mode0e_draw = mode0e_draw_by_sequence.find(*event.action_sequence_id);
            const auto attack_hit_draw = attack_hit_draw_by_sequence.find(*event.action_sequence_id);
            if (mode0e_draw != mode0e_draw_by_sequence.end()) {
                event.matched_mode0e_draw_index = mode0e_draw->second;
                event.gate_before_mode0e_draw = *event.draw_index < mode0e_draw->second;
            }
            if (attack_hit_draw != attack_hit_draw_by_sequence.end()) {
                event.matched_attack_hit_draw_index = attack_hit_draw->second;
                event.gate_before_attack_hit_draw = *event.draw_index < attack_hit_draw->second;
            }
            if (mode0e_draw != mode0e_draw_by_sequence.end()
                && attack_hit_draw != attack_hit_draw_by_sequence.end()) {
                event.mode0e_draw_before_attack_hit_draw =
                    mode0e_draw->second < attack_hit_draw->second;
            }

            if (event.gate_before_mode0e_draw.has_value()
                && event.gate_before_attack_hit_draw.has_value()
                && event.mode0e_draw_before_attack_hit_draw.has_value()) {
                ++summary.action_sequence_order_comparisons;
                if (*event.gate_before_mode0e_draw
                    && *event.gate_before_attack_hit_draw
                    && *event.mode0e_draw_before_attack_hit_draw) {
                    ++summary.action_sequence_order_matches;
                } else {
                    ++summary.action_sequence_order_mismatches;
                }
            } else {
                ++summary.action_sequence_order_missing_camera_or_hit;
            }
        }
    }

    if (summary.first_attack_hit_draw_index.has_value()) {
        for (const auto& event : events) {
            if (owner_is(event, kMode0eOwner)
                && event.rng_draw_index_before.has_value()
                && *event.rng_draw_index_before < *summary.first_attack_hit_draw_index) {
                ++summary.mode0e_draws_before_first_attack_hit;
            }
        }
    }

    if (summary.first_mode0e_draw_index.has_value()) {
        for (const auto& event : events) {
            if (owner_is(event, kMode0FallbackOwner)
                && event.rng_draw_index_before.has_value()
                && *event.rng_draw_index_before < *summary.first_mode0e_draw_index) {
                ++summary.mode0_fallback_draws_before_first_mode0e;
            }
        }
    }

    if (summary.first_attack_hit_draw_index.has_value()) {
        for (const auto& event : events) {
            if (owner_is(event, kMode0FallbackOwner)
                && event.rng_draw_index_before.has_value()
                && *event.rng_draw_index_before < *summary.first_attack_hit_draw_index) {
                ++summary.mode0_fallback_draws_before_first_attack_hit;
            }
        }
    }

    summary.status = classify_status(summary);
    return summary;
}

const char* action_view_gate_checkpoint_status_name(ActionViewGateCheckpointStatus status) {
    switch (status) {
    case ActionViewGateCheckpointStatus::ObservedOnly: return "ObservedOnly";
    case ActionViewGateCheckpointStatus::MatchesExpected: return "MatchesExpected";
    case ActionViewGateCheckpointStatus::MissingLiveGateFields: return "MissingLiveGateFields";
    case ActionViewGateCheckpointStatus::MissingSchedulerFields: return "MissingSchedulerFields";
    case ActionViewGateCheckpointStatus::QueryArgsMismatch: return "QueryArgsMismatch";
    case ActionViewGateCheckpointStatus::SelectorModelMismatch: return "SelectorModelMismatch";
    case ActionViewGateCheckpointStatus::AuxTableCountMismatch: return "AuxTableCountMismatch";
    case ActionViewGateCheckpointStatus::SelectedModeMismatch: return "SelectedModeMismatch";
    case ActionViewGateCheckpointStatus::Mode0FallbackReached: return "Mode0FallbackReached";
    case ActionViewGateCheckpointStatus::ActionViewOrderMismatch: return "ActionViewOrderMismatch";
    default: return "Unknown";
    }
}

const char* first_battle_action_view_gate_checkpoint_rule_detail() {
    return "first-battle action-view gate checkpoints should prove the aux-list root, "
           "FUN_80009030 query args (4, -1, 0x2a, 3), query result, and selected serialized "
           "mode 0 when a SpiceStd 0x0003002a record is present; dispatch checkpoints should "
           "then show whether UpdateActionViewRecord kept effective mode 0 or rewrote it to "
           "runtime mode 0xe before the shared attack hit draw";
}

} // namespace savor::predict
