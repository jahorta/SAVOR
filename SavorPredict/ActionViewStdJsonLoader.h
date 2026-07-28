#pragma once

#include "ActionViewSelectorModel.h"
#include "CombatantVisualDispatcherModel.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace savor::predict {

struct SpiceStd0JsonLoadResult {
    Std0Table table;
    bool ok = false;
    int records_seen = 0;
    int records_imported = 0;
    std::vector<std::string> errors;
};

struct SpiceStdActionRowPrefixLoadResult {
    Std0Table table;
    bool ok = false;
    int rows_seen = 0;
    int rows_imported = 0;
    std::vector<std::string> errors;
};

struct SpiceStdVisualJsonLoadResult {
    CombatantVisualResource resource;
    bool ok = false;
    int records_seen = 0;
    int records_imported = 0;
    int visual_records_decoded = 0;
    std::vector<std::string> errors;
};

struct SpiceStdActionRowsLoadResult {
    std::vector<CombatantStdActionRow> rows;
    bool ok = false;
    int rows_seen = 0;
    int rows_imported = 0;
    std::vector<std::string> errors;
};

// SAVOR-owned projection of one SPICE STD entry-table record.  Direct SPICE
// consumers use this boundary so SPICE model types do not escape the adapter,
// while the JSON compatibility path shares the same payload decoder.
struct SpiceStdEntryProjection {
    int index = -1;
    std::int16_t location_code = -1;
    std::int16_t opcode = 0;
    int payload_size = 0;
    bool payload_in_bounds = false;
    std::vector<std::uint8_t> payload_bytes;
};

SpiceStd0JsonLoadResult project_spice_std0_table(
    std::span<const SpiceStdEntryProjection> records);

SpiceStdVisualJsonLoadResult project_spice_std_visual_resource(
    std::span<const SpiceStdEntryProjection> records);

SpiceStd0JsonLoadResult load_spice_std0_table_from_json_text(std::string_view json_text);

SpiceStd0JsonLoadResult load_spice_std0_table_from_json_file(
    const std::filesystem::path& path);

SpiceStdActionRowPrefixLoadResult load_spice_std_action_row_prefix_from_json_text(
    std::string_view json_text);

SpiceStdActionRowPrefixLoadResult load_spice_std_action_row_prefix_from_json_file(
    const std::filesystem::path& path);

SpiceStdVisualJsonLoadResult load_spice_std_visual_resource_from_json_text(
    std::string_view json_text);

SpiceStdVisualJsonLoadResult load_spice_std_visual_resource_from_json_file(
    const std::filesystem::path& path);

SpiceStdActionRowsLoadResult load_spice_std_action_rows_from_json_text(
    std::string_view json_text);

SpiceStdActionRowsLoadResult load_spice_std_action_rows_from_json_file(
    const std::filesystem::path& path);

const char* spice_std0_json_loader_rule_detail();

} // namespace savor::predict
