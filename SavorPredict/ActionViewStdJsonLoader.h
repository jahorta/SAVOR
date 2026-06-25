#pragma once

#include "ActionViewSelectorModel.h"

#include <filesystem>
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

SpiceStd0JsonLoadResult load_spice_std0_table_from_json_text(std::string_view json_text);

SpiceStd0JsonLoadResult load_spice_std0_table_from_json_file(
    const std::filesystem::path& path);

SpiceStdActionRowPrefixLoadResult load_spice_std_action_row_prefix_from_json_text(
    std::string_view json_text);

SpiceStdActionRowPrefixLoadResult load_spice_std_action_row_prefix_from_json_file(
    const std::filesystem::path& path);

const char* spice_std0_json_loader_rule_detail();

} // namespace savor::predict
