#include "BattleJobRunOptions.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <limits>
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

bool parse_u32_auto(const std::string& value, std::uint32_t& out) {
    char* end = nullptr;
    const auto parsed = std::strtoull(value.c_str(), &end, 0);
    if (end == value.c_str() || *end != '\0' || parsed > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    out = static_cast<std::uint32_t>(parsed);
    return true;
}

bool parse_fake_attack_count(const std::string& value, std::uint32_t& out) {
    if (!parse_u32_auto(value, out)) {
        return false;
    }
    return out <= 255u;
}

int outer_timeout_for_battle_run_ms(std::uint32_t battle_run_ms) {
    constexpr long long kMarginMs = 60000;
    const auto adjusted = static_cast<long long>(battle_run_ms) + kMarginMs;
    return static_cast<int>(std::min<long long>(adjusted, std::numeric_limits<int>::max()));
}

std::string normalize_policy_path(const std::filesystem::path& path) {
    auto normalized = path.lexically_normal().generic_string();
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    while (normalized.size() > 1 && normalized.back() == '/') {
        normalized.pop_back();
    }
    return normalized;
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
    out << std::put_time(&local, "%Y%m%d_%H%M%S") << "_" << std::setw(3) << std::setfill('0') << ms;
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

} // namespace

std::filesystem::path default_battle_job_run_root() {
    return std::filesystem::path("Analyses")
        / "battle_runs_first_battle"
        / "live_capture_runs"
        / timestamp_slug();
}

std::filesystem::path default_battle_job_worker_exe(const std::filesystem::path& executable_path) {
    if (executable_path.empty()) {
        return "SavorWorker.exe";
    }
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(executable_path, ec);
    const auto base = ec ? executable_path : absolute;
    const auto parent = base.has_parent_path() ? base.parent_path() : std::filesystem::path(".");
    return parent / "SavorWorker.exe";
}

bool is_mutable_debug_db_root(const std::filesystem::path& path) {
    return normalize_policy_path(path) == "d:/soasimdbdebug";
}

std::vector<std::string> validate_battle_job_run_options(const BattleJobRunOptions& options) {
    std::vector<std::string> errors;
    const bool has_turn = options.turn_job_id.has_value();
    const bool has_exec = options.exec_job_id.has_value();
    if (has_turn == has_exec) {
        errors.push_back("Specify exactly one of --turn-job-id or --exec-job-id.");
    }
    if (has_turn && *options.turn_job_id <= 0) {
        errors.push_back("--turn-job-id must be positive.");
    }
    if (has_exec && *options.exec_job_id <= 0) {
        errors.push_back("--exec-job-id must be positive.");
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
    if (options.timeout_ms <= 0) {
        errors.push_back("--timeout-ms must be positive.");
    }
    if (options.battle_run_ms.has_value() && *options.battle_run_ms == 0) {
        errors.push_back("--battle-run-ms must be positive.");
    }
    if (options.override_fake_attacks_this_turn.has_value()
        && *options.override_fake_attacks_this_turn > 255u) {
        errors.push_back("--override-fake-attacks must be in the range 0..255.");
    }
    return errors;
}

BattleJobRunParseResult parse_battle_job_run_tokens(
    const std::vector<std::string>& args,
    const std::filesystem::path& executable_path) {
    BattleJobRunParseResult result;
    result.options.run_root = default_battle_job_run_root();
    result.options.worker_exe_path = default_battle_job_worker_exe(executable_path);
    bool timeout_ms_specified = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        std::string value;
        if (arg == "--turn-job-id") {
            long long parsed = 0;
            if (require_value(args, i, arg, value, result.errors) && parse_ll(value, parsed)) {
                result.options.turn_job_id = parsed;
            } else {
                result.errors.push_back("--turn-job-id requires an integer.");
            }
        } else if (arg == "--exec-job-id") {
            long long parsed = 0;
            if (require_value(args, i, arg, value, result.errors) && parse_ll(value, parsed)) {
                result.options.exec_job_id = parsed;
            } else {
                result.errors.push_back("--exec-job-id requires an integer.");
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
                timeout_ms_specified = true;
            } else {
                result.errors.push_back("--timeout-ms requires an integer.");
            }
        } else if (arg == "--battle-run-ms") {
            std::uint32_t parsed = 0;
            if (require_value(args, i, arg, value, result.errors) && parse_u32_auto(value, parsed)) {
                result.options.battle_run_ms = parsed;
            } else {
                result.errors.push_back("--battle-run-ms requires a positive uint32 millisecond value.");
            }
        } else if (arg == "--override-start-rng-seed") {
            std::uint32_t parsed = 0;
            if (require_value(args, i, arg, value, result.errors) && parse_u32_auto(value, parsed)) {
                result.options.override_start_rng_seed = parsed;
            } else {
                result.errors.push_back("--override-start-rng-seed requires a uint32 seed in decimal or 0x hex.");
            }
        } else if (arg == "--override-fake-attacks") {
            std::uint32_t parsed = 0;
            if (require_value(args, i, arg, value, result.errors) && parse_fake_attack_count(value, parsed)) {
                result.options.override_fake_attacks_this_turn = parsed;
            } else {
                result.errors.push_back("--override-fake-attacks requires an integer in the range 0..255.");
            }
        } else if (arg == "--help" || arg == "-h") {
            result.help_requested = true;
        } else {
            result.errors.push_back("Unknown run-battle-job option: " + arg);
        }
    }

    if (result.options.battle_run_ms.has_value() && !timeout_ms_specified) {
        result.options.timeout_ms = std::max(
            result.options.timeout_ms,
            outer_timeout_for_battle_run_ms(*result.options.battle_run_ms));
    }

    if (!result.help_requested) {
        const auto validation_errors = validate_battle_job_run_options(result.options);
        result.errors.insert(result.errors.end(), validation_errors.begin(), validation_errors.end());
    }
    return result;
}

} // namespace savor::predict
