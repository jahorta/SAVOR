#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace savor::predict {

struct SpiceStdJsonExportRequest {
    std::filesystem::path spice_file_parsing_exe;
    std::filesystem::path bchara_dir;
    std::filesystem::path output_dir;
};

struct SpiceStdJsonExportResult {
    int exit_code = -1;
    std::string output;
    std::string error;
};

using SpiceStdJsonExportRunner =
    std::function<SpiceStdJsonExportResult(const SpiceStdJsonExportRequest&)>;

struct ActionViewStdJsonCacheOptions {
    std::filesystem::path db_root = "D:/SavorPredictDB";
    std::filesystem::path explicit_std_json_dir;
    std::filesystem::path std_disc_dump_root;
    std::filesystem::path spice_file_parsing_exe;
};

struct ActionViewStdJsonCacheResolution {
    std::filesystem::path resolved_std_json_dir;
    std::filesystem::path cache_dir;
    std::filesystem::path disc_dump_root;
    std::filesystem::path spice_file_parsing_exe;
    bool used_explicit_dir = false;
    bool cache_complete_before = false;
    bool generation_attempted = false;
    bool generation_succeeded = false;
    bool available = false;
    bool fatal_error = false;
    int spice_exit_code = -1;
    std::vector<std::string> missing_files_before;
    std::vector<std::string> missing_files_after;
    std::vector<std::string> diagnostics;
    std::string spice_output;
};

std::vector<std::string> required_first_battle_action_view_std_json_files();
std::filesystem::path default_action_view_std_json_cache_dir(const std::filesystem::path& db_root);
std::filesystem::path default_action_view_std_disc_dump_root();
std::filesystem::path default_spice_file_parsing_exe();

ActionViewStdJsonCacheResolution resolve_action_view_std_json_cache(
    const ActionViewStdJsonCacheOptions& options,
    SpiceStdJsonExportRunner runner = {});

std::string summarize_action_view_std_json_cache_resolution(
    const ActionViewStdJsonCacheResolution& resolution);

} // namespace savor::predict
