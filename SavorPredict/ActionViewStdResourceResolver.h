#pragma once

#include "ActionViewSelectorModel.h"
#include "BattlePredictorResourceBundle.h"

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace savor::predict {

enum class ActionViewStdMaterializationSource {
    Unknown,
    ResourceBundle,
    Cache,
    TransientHandoff,
    FreshLoad,
};

const char* action_view_std_materialization_source_name(
    ActionViewStdMaterializationSource source);

struct ActionViewStdResourceResolution {
    bool ok = false;
    int actor_slot = -1;
    std::string resource_stem;
    std::string std_filename;
    std::string std0_filename;
    std::filesystem::path std_source_path;
    std::filesystem::path std0_source_path;
    std::filesystem::path std_json_path;
    std::filesystem::path std0_json_path;
    ActionViewStdMaterializationSource materialization_source =
        ActionViewStdMaterializationSource::Unknown;
    std::optional<std::uint32_t> first_battle_cache_key;
    std::optional<int> first_battle_cache_slot;
    bool runtime_loaded_resource_plus_0x30_is_aux_root = false;
    bool table_contents_source_data_equivalent = false;
    bool runtime_aux_table_has_action_row_prefix = false;
    int runtime_aux_table_prefix_rows = 0;
    Std0Table companion_table;
    Std0Table table;
    std::optional<Std0CountResult> mode0e_count;
    std::vector<std::string> errors;
};

std::optional<std::string> first_battle_action_view_resource_stem_for_slot(int actor_slot);

std::optional<std::uint32_t> first_battle_action_view_std0_cache_key_for_slot(
    int actor_slot);

std::optional<int> first_battle_action_view_std0_cache_slot_for_slot(int actor_slot);

std::string action_view_std0_companion_filename_for_std_resource(
    std::string_view std_resource_name);

ActionViewStdResourceResolution resolve_first_battle_action_view_std0_table_for_slot(
    int actor_slot,
    const BattlePredictorResourceBundle& resource_inputs);

// Explicit legacy compatibility loader. New predictor/checkpoint paths should
// resolve the immutable BattlePredictorResourceBundle overload above.
ActionViewStdResourceResolution resolve_first_battle_action_view_std0_table_for_slot(
    int actor_slot,
    const std::filesystem::path& spice_std_json_dir);

const char* action_view_std_resource_resolver_rule_detail();

} // namespace savor::predict
