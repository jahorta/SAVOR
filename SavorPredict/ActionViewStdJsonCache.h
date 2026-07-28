#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace savor::predict {

struct ActionViewStdJsonCacheOptions {
    std::filesystem::path db_root = "D:/SavorPredictDB";
    std::filesystem::path explicit_std_json_dir;
    std::filesystem::path std_disc_dump_root;
    std::filesystem::path spice_file_parsing_exe;
};

struct ActionViewStdJsonCacheResolution {
    std::filesystem::path resolved_std_json_dir;
    std::filesystem::path cache_dir;
    std::filesystem::path manifest_path;
    std::filesystem::path disc_dump_root;
    std::filesystem::path spice_file_parsing_exe;
    bool used_explicit_dir = false;
    bool cache_complete_before = false;
    bool manifest_present = false;
    bool manifest_verified = false;
    bool manifest_written = false;
    bool generation_attempted = false;
    bool generation_succeeded = false;
    bool available = false;
    bool fatal_error = false;
    std::uint32_t manifest_schema_version = 0;
    int spice_exit_code = -1;
    std::vector<std::string> missing_files_before;
    std::vector<std::string> missing_files_after;
    std::vector<std::string> hash_mismatches;
    std::vector<std::string> diagnostics;
    std::string spice_output;
};

std::vector<std::string> required_first_battle_action_view_std_json_files();
std::string action_view_std_json_manifest_filename();
bool write_action_view_std_json_manifest(
    const std::filesystem::path& std_json_dir,
    std::string* error = nullptr);
std::filesystem::path default_action_view_std_json_cache_dir(const std::filesystem::path& db_root);
std::filesystem::path default_action_view_std_disc_dump_root();

ActionViewStdJsonCacheResolution resolve_action_view_std_json_cache(
    const ActionViewStdJsonCacheOptions& options);

std::string summarize_action_view_std_json_cache_resolution(
    const ActionViewStdJsonCacheResolution& resolution);

} // namespace savor::predict
