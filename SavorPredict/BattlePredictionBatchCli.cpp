#include "BattlePredictionBatchCli.h"

#include "ActionViewStdJsonCache.h"
#include "BattlePredictionProfileNames.h"
#include "CliResourceInputCompatibility.h"

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>

namespace savor::predict {
namespace {

bool parse_i64(std::string_view value, long long* parsed) {
    std::string text(value);
    char* end = nullptr;
    const auto result = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }
    *parsed = result;
    return true;
}

std::string trim(std::string_view value) {
    std::size_t first = 0;
    while (first < value.size()
        && std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first
        && std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

bool require_value(
    const std::vector<std::string>& args,
    std::size_t* index,
    std::string_view option,
    std::string* value,
    std::vector<std::string>* errors) {
    if (*index + 1 >= args.size()) {
        errors->push_back(std::string(option) + " requires a value.");
        return false;
    }
    *value = args[++*index];
    return true;
}

void append_exec_job_list(
    const std::filesystem::path& path,
    std::vector<long long>* ids,
    std::vector<std::string>* errors) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        errors->push_back(
            "Failed opening --exec-job-list: " + path.string());
        return;
    }
    std::string line;
    int line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        if (const auto comment = line.find('#');
            comment != std::string::npos) {
            line.resize(comment);
        }
        for (auto& ch : line) {
            if (ch == ',') {
                ch = ' ';
            }
        }
        std::istringstream tokens(line);
        std::string token;
        while (tokens >> token) {
            long long parsed = 0;
            if (!parse_i64(token, &parsed)) {
                errors->push_back(
                    "--exec-job-list contains a non-integer at line "
                    + std::to_string(line_number) + ": " + token);
            } else {
                ids->push_back(parsed);
            }
        }
    }
}

std::string timestamp_slug() {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() % 1000;
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream out;
    out << "prediction_" << std::put_time(&local, "%Y%m%d_%H%M%S")
        << "_" << std::setw(3) << std::setfill('0') << milliseconds;
    return out.str();
}

std::optional<std::filesystem::path> resolve_git_directory(
    std::filesystem::path start) {
    std::error_code ec;
    start = std::filesystem::absolute(start, ec);
    if (ec) {
        return std::nullopt;
    }
    if (!std::filesystem::is_directory(start, ec)) {
        start = start.parent_path();
    }
    for (auto current = start; !current.empty();) {
        const auto marker = current / ".git";
        if (std::filesystem::is_directory(marker, ec) && !ec) {
            return marker;
        }
        ec.clear();
        if (std::filesystem::is_regular_file(marker, ec) && !ec) {
            std::ifstream file(marker);
            std::string line;
            std::getline(file, line);
            constexpr std::string_view kPrefix = "gitdir:";
            if (line.starts_with(kPrefix)) {
                const auto relative = trim(
                    std::string_view(line).substr(kPrefix.size()));
                auto git_directory = std::filesystem::path(relative);
                if (git_directory.is_relative()) {
                    git_directory = current / git_directory;
                }
                return git_directory.lexically_normal();
            }
        }
        ec.clear();
        const auto parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return std::nullopt;
}

std::string read_first_line(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);
    return trim(line);
}

std::string resolve_git_reference(
    const std::filesystem::path& git_directory,
    std::string_view reference) {
    std::vector<std::filesystem::path> search_roots{git_directory};
    if (const auto common = read_first_line(git_directory / "commondir");
        !common.empty()) {
        auto common_directory = std::filesystem::path(common);
        if (common_directory.is_relative()) {
            common_directory = git_directory / common_directory;
        }
        search_roots.push_back(common_directory.lexically_normal());
    }
    for (const auto& root : search_roots) {
        if (const auto loose = read_first_line(root / reference);
            !loose.empty()) {
            return loose;
        }
        std::ifstream packed(root / "packed-refs");
        std::string line;
        while (std::getline(packed, line)) {
            if (line.empty() || line.front() == '#'
                || line.front() == '^') {
                continue;
            }
            const auto separator = line.find(' ');
            if (separator != std::string::npos
                && line.substr(separator + 1) == reference) {
                return line.substr(0, separator);
            }
        }
    }
    return {};
}

std::string resolve_git_commit(
    const std::filesystem::path& executable_path) {
    auto git_directory = resolve_git_directory(executable_path);
    if (!git_directory.has_value()) {
        git_directory = resolve_git_directory(
            std::filesystem::current_path());
    }
    if (!git_directory.has_value()) {
        return {};
    }
    const auto head = read_first_line(*git_directory / "HEAD");
    constexpr std::string_view kRefPrefix = "ref:";
    if (!head.starts_with(kRefPrefix)) {
        return head;
    }
    const auto reference = trim(
        std::string_view(head).substr(kRefPrefix.size()));
    return resolve_git_reference(*git_directory, reference);
}

std::filesystem::path normalized_absolute_path(
    const std::filesystem::path& path) {
    if (path.empty()) {
        return {};
    }
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec);
    return (ec ? path : absolute).lexically_normal();
}

} // namespace

BattlePredictionBatchCliParseResult parse_battle_prediction_batch_tokens(
    const std::vector<std::string>& args,
    const std::filesystem::path& executable_path) {
    BattlePredictionBatchCliParseResult result;
    cli_detail::DiscDumpRootOptions disc_dump_root_options;
    auto& batch = result.options.batch;
    batch.db_root = "D:/SavorPredictDB";
    batch.profile_name = std::string(kFirstBattleSoldiersProfileName);
    batch.scenario_name = std::string(kFirstBattleSoldiersProfileName);
    batch.run_name = timestamp_slug();
    std::error_code absolute_error;
    batch.executable_path =
        std::filesystem::absolute(executable_path, absolute_error);
    if (absolute_error) {
        batch.executable_path = executable_path;
    }
    batch.git_commit = resolve_git_commit(batch.executable_path);

    bool run_root_explicit = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        std::string value;
        if (arg == "--exec-job-id") {
            long long parsed = 0;
            if (require_value(
                    args, &i, arg, &value, &result.errors)
                && parse_i64(value, &parsed)) {
                batch.source_exec_job_ids.push_back(parsed);
            } else if (!value.empty()) {
                result.errors.push_back(
                    "--exec-job-id requires an integer.");
            }
        } else if (arg == "--exec-job-list") {
            if (require_value(
                    args, &i, arg, &value, &result.errors)) {
                append_exec_job_list(
                    value,
                    &batch.source_exec_job_ids,
                    &result.errors);
            }
        } else if (arg == "--db-root") {
            if (require_value(
                    args, &i, arg, &value, &result.errors)) {
                batch.db_root = value;
            }
        } else if (arg == "--run-root") {
            if (require_value(
                    args, &i, arg, &value, &result.errors)) {
                batch.run_root = value;
                run_root_explicit = true;
            }
        } else if (arg == "--run-name") {
            if (require_value(
                    args, &i, arg, &value, &result.errors)) {
                batch.run_name = value;
            }
        } else if (arg == "--scenario") {
            if (require_value(
                    args, &i, arg, &value, &result.errors)) {
                batch.scenario_name = value;
            }
        } else if (arg == "--profile") {
            if (require_value(
                    args, &i, arg, &value, &result.errors)) {
                batch.profile_name = value;
            }
        } else if (arg == "--action-view-std-json-dir") {
            if (require_value(
                    args, &i, arg, &value, &result.errors)) {
                batch.action_view_std_json_dir = value;
            }
        } else if (arg == "--disc-dump-root"
            || arg == "--std-disc-dump-root") {
            if (require_value(
                    args, &i, arg, &value, &result.errors)) {
                disc_dump_root_options.observe(
                    arg, value, result.errors);
            }
        } else if (arg == "--spice-file-parsing-exe") {
            if (require_value(
                    args, &i, arg, &value, &result.errors)) {
                result.options.spice_file_parsing_exe = value;
                cli_detail::add_spice_file_parsing_exe_warning(
                    result.warnings);
            }
        } else if (arg == "--allow-seed-candidate-fallback") {
            batch.allow_seed_candidate_fallback = true;
        } else if (arg == "--allow-profile-overrides") {
            batch.allow_profile_overrides = true;
        } else if (arg == "--emit-causal-diagnostics") {
            batch.emit_causal_diagnostics = true;
        } else if (arg == "--require-complete") {
            batch.require_complete = true;
        } else if (arg == "--preflight-only") {
            batch.preflight_only = true;
        } else if (arg == "--help" || arg == "-h") {
            result.help_requested = true;
        } else {
            result.errors.push_back(
                "Unknown predict-battle-jobs option: " + arg);
        }
    }
    disc_dump_root_options.finalize(
        result.options.disc_dump_root,
        result.errors,
        result.warnings);
    if (!run_root_explicit) {
        batch.run_root = std::filesystem::path(".codex-runs")
            / "predict"
            / batch.run_name;
    }
    if (batch.run_name.empty()) {
        result.errors.push_back("--run-name must not be empty.");
    } else if (batch.run_name == "." || batch.run_name == ".."
        || batch.run_name.find('/') != std::string::npos
        || batch.run_name.find('\\') != std::string::npos) {
        result.errors.push_back(
            "--run-name must be a single path-safe name.");
    }
    return result;
}

int run_battle_prediction_batch_cli(
    const BattlePredictionBatchCliOptions& supplied,
    std::ostream& out,
    std::ostream& err) {
    auto options = supplied;
    options.batch.db_root =
        normalized_absolute_path(options.batch.db_root);
    options.batch.run_root =
        normalized_absolute_path(options.batch.run_root);
    options.batch.executable_path =
        normalized_absolute_path(options.batch.executable_path);
    options.batch.action_view_std_json_dir =
        normalized_absolute_path(
            options.batch.action_view_std_json_dir);
    options.disc_dump_root =
        normalized_absolute_path(options.disc_dump_root);
    options.spice_file_parsing_exe =
        normalized_absolute_path(options.spice_file_parsing_exe);

    if (!cli_detail::ensure_first_battle_resource_inputs(
            options.disc_dump_root,
            options.batch.action_view_std_json_dir,
            options.resource_inputs,
            err)) {
        return 1;
    }
    options.batch.resource_inputs = options.resource_inputs;

    const auto& preliminary = options.batch;
    const auto preliminary_errors =
        validate_battle_prediction_batch_run_options(preliminary);
    if (!preliminary_errors.empty()) {
        for (const auto& validation_error : preliminary_errors) {
            err << validation_error << "\n";
        }
        return 2;
    }

    const auto result = run_battle_prediction_batch(
        options.batch, out, err);
    if (!result.manifest_path.empty()
        && std::filesystem::exists(result.manifest_path)) {
        out << "Prediction batch artifacts: "
            << result.options.run_root.string() << "\n";
    }
    return result.recommended_exit_code;
}

} // namespace savor::predict
