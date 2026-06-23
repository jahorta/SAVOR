#include "BattleJobBatchRunOptions.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

namespace savor::predict {
namespace {

bool parse_ll(const std::string& value, long long& out) {
    char* end = nullptr;
    out = std::strtoll(value.c_str(), &end, 10);
    return end != value.c_str() && *end == '\0';
}

bool parse_int(const std::string& value, int& out) {
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<int>(parsed);
    return true;
}

std::string timestamp_slug() {
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &tt);
#else
    localtime_r(&tt, &local);
#endif
    std::ostringstream out;
    out << "batch_" << std::put_time(&local, "%Y%m%d_%H%M%S")
        << "_" << std::setw(3) << std::setfill('0') << ms;
    return out.str();
}

bool require_value(
    const std::vector<std::string>& args,
    std::size_t& index,
    const std::string& option,
    std::string& value,
    std::vector<std::string>& errors) {
    if (index + 1 >= args.size()) {
        errors.push_back(option + " requires a value.");
        return false;
    }
    value = args[++index];
    return true;
}

std::string trim(const std::string& value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    return value.substr(first, last - first);
}

void append_exec_job_list_file(
    const std::filesystem::path& path,
    std::vector<long long>* ids,
    std::vector<std::string>* errors) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        errors->push_back("Failed opening --exec-job-list: " + path.string());
        return;
    }
    std::string line;
    int line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        if (const auto hash = line.find('#'); hash != std::string::npos) {
            line.resize(hash);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        long long parsed = 0;
        if (!parse_ll(line, parsed)) {
            errors->push_back(
                "--exec-job-list contains a non-integer at line " + std::to_string(line_number));
            continue;
        }
        ids->push_back(parsed);
    }
}

} // namespace

std::filesystem::path default_battle_job_batch_run_root() {
    return std::filesystem::path("Analyses")
        / "battle_runs_first_battle"
        / "live_capture_runs"
        / timestamp_slug();
}

int resolved_battle_job_batch_timeout_ms(const BattleJobBatchRunOptions& options) {
    if (options.timeout_ms.has_value()) {
        return *options.timeout_ms;
    }
    const auto job_count = static_cast<long long>(std::max<std::size_t>(1, options.exec_job_ids.size()));
    const auto worker_count = static_cast<long long>(std::max(1, options.max_workers));
    const auto waves = (job_count + worker_count - 1) / worker_count;
    const auto timeout = 180000LL * waves;
    return static_cast<int>(std::min<long long>(timeout, std::numeric_limits<int>::max()));
}

std::vector<std::string> validate_battle_job_batch_run_options(const BattleJobBatchRunOptions& options) {
    std::vector<std::string> errors;
    if (options.exec_job_ids.empty()) {
        errors.push_back("Specify at least one --exec-job-id or --exec-job-list.");
    }
    std::set<long long> seen;
    for (const auto id : options.exec_job_ids) {
        if (id <= 0) {
            errors.push_back("--exec-job-id values must be positive.");
            break;
        }
        if (!seen.insert(id).second) {
            errors.push_back("Duplicate --exec-job-id value: " + std::to_string(id));
            break;
        }
    }
    if (options.db_root.empty()) {
        errors.push_back("--db-root is required.");
    } else if (is_mutable_debug_db_root(options.db_root)) {
        errors.push_back("Refusing to use D:/SoaSimDBDebug as a run DB root; copy to D:/SavorPredictDB first.");
    }
    if (options.run_root.empty()) {
        errors.push_back("--run-root is required.");
    }
    if (options.iso_path.empty()) {
        errors.push_back("--iso is required.");
    }
    if (options.dolphin_base_dir.empty()) {
        errors.push_back("--dolphin-base-dir is required.");
    }
    if (options.worker_exe_path.empty()) {
        errors.push_back("--worker-exe could not be resolved.");
    }
    if (options.poll_ms <= 0) {
        errors.push_back("--poll-ms must be positive.");
    }
    if (options.timeout_ms.has_value() && *options.timeout_ms <= 0) {
        errors.push_back("--timeout-ms must be positive.");
    }
    if (options.max_workers <= 0) {
        errors.push_back("--max-workers must be positive.");
    }
    return errors;
}

BattleJobBatchRunParseResult parse_battle_job_batch_run_tokens(
    const std::vector<std::string>& args,
    const std::filesystem::path& executable_path) {
    BattleJobBatchRunParseResult result;
    result.options.run_root = default_battle_job_batch_run_root();
    result.options.worker_exe_path = default_battle_job_worker_exe(executable_path);

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        std::string value;
        if (arg == "--exec-job-id") {
            long long parsed = 0;
            if (require_value(args, i, arg, value, result.errors) && parse_ll(value, parsed)) {
                result.options.exec_job_ids.push_back(parsed);
            } else {
                result.errors.push_back("--exec-job-id requires an integer.");
            }
        } else if (arg == "--exec-job-list") {
            if (require_value(args, i, arg, value, result.errors)) {
                append_exec_job_list_file(value, &result.options.exec_job_ids, &result.errors);
            }
        } else if (arg == "--db-root") {
            if (require_value(args, i, arg, value, result.errors)) {
                result.options.db_root = value;
            }
        } else if (arg == "--run-root") {
            if (require_value(args, i, arg, value, result.errors)) {
                result.options.run_root = value;
            }
        } else if (arg == "--iso") {
            if (require_value(args, i, arg, value, result.errors)) {
                result.options.iso_path = value;
            }
        } else if (arg == "--dolphin-base-dir") {
            if (require_value(args, i, arg, value, result.errors)) {
                result.options.dolphin_base_dir = value;
            }
        } else if (arg == "--worker-exe") {
            if (require_value(args, i, arg, value, result.errors)) {
                result.options.worker_exe_path = value;
            }
        } else if (arg == "--capture-profile") {
            if (require_value(args, i, arg, value, result.errors)) {
                result.options.capture_profile_path = value;
            }
        } else if (arg == "--sandbox-mode") {
            if (require_value(args, i, arg, value, result.errors)) {
                if (const auto mode = savor::dbutils::ParseSandboxMode(value); mode.has_value()) {
                    result.options.sandbox_mode = *mode;
                } else {
                    result.errors.push_back("--sandbox-mode must be minimal or full-copy.");
                }
            }
        } else if (arg == "--poll-ms") {
            int parsed = 0;
            if (require_value(args, i, arg, value, result.errors) && parse_int(value, parsed)) {
                result.options.poll_ms = parsed;
            } else {
                result.errors.push_back("--poll-ms requires an integer.");
            }
        } else if (arg == "--timeout-ms") {
            int parsed = 0;
            if (require_value(args, i, arg, value, result.errors) && parse_int(value, parsed)) {
                result.options.timeout_ms = parsed;
            } else {
                result.errors.push_back("--timeout-ms requires an integer.");
            }
        } else if (arg == "--max-workers") {
            int parsed = 0;
            if (require_value(args, i, arg, value, result.errors) && parse_int(value, parsed)) {
                result.options.max_workers = parsed;
            } else {
                result.errors.push_back("--max-workers requires an integer.");
            }
        } else if (arg == "--help" || arg == "-h") {
            result.help_requested = true;
        } else {
            result.errors.push_back("Unknown run-battle-jobs option: " + arg);
        }
    }

    if (!result.help_requested) {
        const auto validation_errors = validate_battle_job_batch_run_options(result.options);
        result.errors.insert(result.errors.end(), validation_errors.begin(), validation_errors.end());
    }
    return result;
}

} // namespace savor::predict
