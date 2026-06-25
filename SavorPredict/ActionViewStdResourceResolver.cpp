#include "ActionViewStdResourceResolver.h"

#include "ActionViewStdJsonLoader.h"

#include <algorithm>
#include <cctype>

namespace savor::predict {
namespace {

std::string lowercase_ascii(std::string_view value) {
    std::string out(value.begin(), value.end());
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string strip_std_extension(std::string_view value) {
    std::string out(value.begin(), value.end());
    const auto dot = out.find_last_of('.');
    if (dot != std::string::npos) {
        out.resize(dot);
    }
    return out;
}

void add_error(ActionViewStdResourceResolution& result, std::string error) {
    result.errors.push_back(std::move(error));
}

} // namespace

const char* action_view_std_materialization_source_name(
    ActionViewStdMaterializationSource source) {
    switch (source) {
    case ActionViewStdMaterializationSource::Unknown:
        return "unknown";
    case ActionViewStdMaterializationSource::Cache:
        return "cache";
    case ActionViewStdMaterializationSource::TransientHandoff:
        return "transient_handoff";
    case ActionViewStdMaterializationSource::FreshLoad:
        return "fresh_load";
    default:
        return "unknown";
    }
}

std::optional<std::string> first_battle_action_view_resource_stem_for_slot(int actor_slot) {
    switch (actor_slot) {
    case 0:
        return std::string("ma000");
    case 1:
        return std::string("MA001");
    case 4:
    case 5:
        return std::string("MB000");
    default:
        return std::nullopt;
    }
}

std::optional<std::uint32_t> first_battle_action_view_std0_cache_key_for_slot(
    int actor_slot) {
    switch (actor_slot) {
    case 0:
        return 0x00989680u;
    case 1:
        return 0x00989681u;
    case 4:
    case 5:
        return 0x00989A68u;
    default:
        return std::nullopt;
    }
}

std::optional<int> first_battle_action_view_std0_cache_slot_for_slot(int actor_slot) {
    switch (actor_slot) {
    case 0:
        return 0;
    case 1:
        return 1;
    case 4:
    case 5:
        return 2;
    default:
        return std::nullopt;
    }
}

std::string action_view_std0_companion_filename_for_std_resource(
    std::string_view std_resource_name) {
    const auto stem = lowercase_ascii(strip_std_extension(std_resource_name));
    return stem + "0.std";
}

ActionViewStdResourceResolution resolve_first_battle_action_view_std0_table_for_slot(
    int actor_slot,
    const std::filesystem::path& spice_std_json_dir) {
    ActionViewStdResourceResolution result;
    result.actor_slot = actor_slot;
    result.table_contents_source_data_equivalent = true;

    const auto stem = first_battle_action_view_resource_stem_for_slot(actor_slot);
    if (!stem.has_value()) {
        add_error(result, "unsupported first-battle actor slot " + std::to_string(actor_slot));
        return result;
    }

    result.materialization_source = ActionViewStdMaterializationSource::Cache;
    result.first_battle_cache_key =
        first_battle_action_view_std0_cache_key_for_slot(actor_slot);
    result.first_battle_cache_slot =
        first_battle_action_view_std0_cache_slot_for_slot(actor_slot);
    result.runtime_loaded_resource_plus_0x30_is_aux_root = true;

    result.resource_stem = *stem;
    result.std_filename = result.resource_stem + ".std";
    result.std0_filename = action_view_std0_companion_filename_for_std_resource(result.std_filename);
    result.std_json_path = spice_std_json_dir / (result.std_filename + ".json");
    result.std0_json_path = spice_std_json_dir / (result.std0_filename + ".json");

    const auto loaded = load_spice_std0_table_from_json_file(result.std0_json_path);
    if (!loaded.ok) {
        result.errors = loaded.errors;
        if (result.errors.empty()) {
            add_error(result, "failed to load " + result.std0_json_path.string());
        }
        return result;
    }

    result.companion_table = loaded.table;
    result.table = loaded.table;

    if (std::filesystem::exists(result.std_json_path)) {
        const auto prefix = load_spice_std_action_row_prefix_from_json_file(result.std_json_path);
        if (prefix.ok) {
            Std0Table runtime_table;
            runtime_table.entries = prefix.table.entries;
            runtime_table.entries.insert(
                runtime_table.entries.end(),
                loaded.table.entries.begin(),
                loaded.table.entries.end());
            runtime_table.includes_sentinel = loaded.table.includes_sentinel;
            result.table = std::move(runtime_table);
            result.runtime_aux_table_has_action_row_prefix = true;
            result.runtime_aux_table_prefix_rows = static_cast<int>(prefix.table.entries.size());
        }
    }

    result.mode0e_count = count_matching_std0_entries(
        &result.table,
        mode0e_action_view_count_query());
    result.ok = true;
    return result;
}

const char* action_view_std_resource_resolver_rule_detail() {
    return "First-battle action-view STD resolver maps actor slots 0, 1, 4, and 5 "
           "to ma000, MA001, and MB000 resource stems, derives the companion _0_STD "
           "filename as lowercase(stem)+'0.std', imports the SPICE entry_table JSON, "
           "and, when the primary action_rows JSON is present, prepends the first "
           "action row as the runtime aux-table prefix observed before companion rows. "
           "The first-battle runtime chain is validated as combatant payload +0x10 "
           "loaded resource, with loaded_resource+0x30 used as the selector aux root; "
           "live capture links those roots to the STD0 cache keys 0x00989680, "
           "0x00989681, and 0x00989A68 for ma000, MA001, and MB000 respectively. "
           "Heap aux-root addresses are therefore runtime locations, not predictor "
           "inputs; table contents are source-data equivalent for predictor counting.";
}

} // namespace savor::predict
