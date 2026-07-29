#include "BattleJobBatchRunOptions.h"
#include "CliResourceInputCompatibility.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

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

bool parse_exec_job_seed_spec(
    const std::string& value,
    BattleJobBatchRunRequest* request,
    std::vector<std::string>* errors) {
    const auto colon = value.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= value.size()) {
        errors->push_back("--exec-job-seed must be EXEC_ID:SEED[:FAKE_ATTACKS], with SEED decimal or 0x hex.");
        return false;
    }
    const auto second_colon = value.find(':', colon + 1);
    if (second_colon != std::string::npos
        && (second_colon + 1 >= value.size() || value.find(':', second_colon + 1) != std::string::npos)) {
        errors->push_back("--exec-job-seed must be EXEC_ID:SEED[:FAKE_ATTACKS].");
        return false;
    }
    long long exec_job_id = 0;
    std::uint32_t seed = 0;
    std::uint32_t fake_attacks = 0;
    if (!parse_ll(value.substr(0, colon), exec_job_id) || exec_job_id <= 0) {
        errors->push_back("--exec-job-seed requires a positive integer exec job id before ':'.");
        return false;
    }
    const auto seed_text = second_colon == std::string::npos
        ? value.substr(colon + 1)
        : value.substr(colon + 1, second_colon - colon - 1);
    if (!parse_u32_auto(seed_text, seed)) {
        errors->push_back("--exec-job-seed requires a uint32 seed after ':' in decimal or 0x hex.");
        return false;
    }
    if (second_colon != std::string::npos) {
        if (!parse_fake_attack_count(value.substr(second_colon + 1), fake_attacks)) {
            errors->push_back("--exec-job-seed fake attack override must be an integer in the range 0..255.");
            return false;
        }
        request->override_fake_attacks_this_turn = fake_attacks;
    }
    request->exec_job_id = exec_job_id;
    request->override_start_rng_seed = seed;
    return true;
}

bool parse_exec_job_fake_attacks_spec(
    const std::string& value,
    BattleJobBatchRunRequest* request,
    std::vector<std::string>* errors) {
    const auto colon = value.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= value.size()
        || value.find(':', colon + 1) != std::string::npos) {
        errors->push_back("--exec-job-fake-attacks must be EXEC_ID:FAKE_ATTACKS.");
        return false;
    }
    long long exec_job_id = 0;
    std::uint32_t fake_attacks = 0;
    if (!parse_ll(value.substr(0, colon), exec_job_id) || exec_job_id <= 0) {
        errors->push_back("--exec-job-fake-attacks requires a positive integer exec job id before ':'.");
        return false;
    }
    if (!parse_fake_attack_count(value.substr(colon + 1), fake_attacks)) {
        errors->push_back("--exec-job-fake-attacks requires an integer in the range 0..255 after ':'.");
        return false;
    }
    request->exec_job_id = exec_job_id;
    request->override_fake_attacks_this_turn = fake_attacks;
    return true;
}

void append_exec_job_seed_list_file(
    const std::filesystem::path& path,
    std::vector<BattleJobBatchRunRequest>* requests,
    std::vector<std::string>* errors) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        errors->push_back("Failed opening --exec-job-seed-list: " + path.string());
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
        BattleJobBatchRunRequest request;
        if (!parse_exec_job_seed_spec(line, &request, errors)) {
            errors->push_back(
                "--exec-job-seed-list contains an invalid run spec at line " + std::to_string(line_number));
            continue;
        }
        requests->push_back(request);
    }
}

void append_exec_job_fake_attacks_list_file(
    const std::filesystem::path& path,
    std::vector<BattleJobBatchRunRequest>* requests,
    std::vector<std::string>* errors) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        errors->push_back("Failed opening --exec-job-fake-attacks-list: " + path.string());
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
        BattleJobBatchRunRequest request;
        if (!parse_exec_job_fake_attacks_spec(line, &request, errors)) {
            errors->push_back(
                "--exec-job-fake-attacks-list contains an invalid run spec at line "
                + std::to_string(line_number));
            continue;
        }
        requests->push_back(request);
    }
}

} // namespace

std::filesystem::path default_battle_job_batch_run_root() {
    return std::filesystem::path("Analyses")
        / "battle_runs_first_battle"
        / "live_capture_runs"
        / timestamp_slug();
}

std::vector<BattleJobBatchRunRequest> resolved_battle_job_batch_requests(const BattleJobBatchRunOptions& options) {
    std::vector<BattleJobBatchRunRequest> requests;
    requests.reserve(options.exec_job_ids.size() + options.seeded_exec_job_requests.size());
    for (const auto exec_job_id : options.exec_job_ids) {
        requests.push_back({
            .exec_job_id = exec_job_id,
            .override_start_rng_seed = options.override_start_rng_seed,
            .override_fake_attacks_this_turn = options.override_fake_attacks_this_turn,
        });
    }
    requests.insert(
        requests.end(),
        options.seeded_exec_job_requests.begin(),
        options.seeded_exec_job_requests.end());
    if (options.override_fake_attacks_this_turn.has_value()) {
        for (auto& request : requests) {
            if (!request.override_fake_attacks_this_turn.has_value()) {
                request.override_fake_attacks_this_turn = options.override_fake_attacks_this_turn;
            }
        }
    }
    return requests;
}

std::vector<long long> unique_battle_job_batch_source_exec_job_ids(const BattleJobBatchRunOptions& options) {
    const auto requests = resolved_battle_job_batch_requests(options);
    std::vector<long long> ids;
    std::set<long long> seen;
    ids.reserve(requests.size());
    for (const auto& request : requests) {
        if (seen.insert(request.exec_job_id).second) {
            ids.push_back(request.exec_job_id);
        }
    }
    return ids;
}

std::vector<std::string> validate_battle_job_batch_run_options(const BattleJobBatchRunOptions& options) {
    std::vector<std::string> errors;
    const auto requests = resolved_battle_job_batch_requests(options);
    if (requests.empty()) {
        errors.push_back("Specify at least one --exec-job-id, --exec-job-list, --exec-job-seed, or --exec-job-seed-list.");
    }
    std::set<std::tuple<long long, std::uint64_t, std::uint64_t>> seen;
    for (const auto& request : requests) {
        if (request.exec_job_id <= 0) {
            errors.push_back("Batch exec job ids must be positive.");
            break;
        }
        const auto seed_key = request.override_start_rng_seed.has_value()
            ? static_cast<std::uint64_t>(*request.override_start_rng_seed)
            : (std::uint64_t{1} << 32);
        const auto fake_key = request.override_fake_attacks_this_turn.has_value()
            ? static_cast<std::uint64_t>(*request.override_fake_attacks_this_turn)
            : (std::uint64_t{1} << 32);
        if (!seen.insert({ request.exec_job_id, seed_key, fake_key }).second) {
            errors.push_back("Duplicate batch run request for exec job "
                + std::to_string(request.exec_job_id)
                + (request.override_start_rng_seed.has_value()
                    ? " with the same override seed."
                    : " with no override seed.")
                + (request.override_fake_attacks_this_turn.has_value()
                    ? " and the same fake attack override."
                    : " and no fake attack override."));
            break;
        }
        if (request.override_fake_attacks_this_turn.has_value()
            && *request.override_fake_attacks_this_turn > 255u) {
            errors.push_back("Batch fake attack overrides must be in the range 0..255.");
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
    if (!options.action_view_std_json_dir.empty()
        && !std::filesystem::is_directory(options.action_view_std_json_dir)) {
        errors.push_back("--action-view-std-json-dir must name an existing directory: "
            + options.action_view_std_json_dir.string());
    }
    if (options.poll_ms <= 0) {
        errors.push_back("--poll-ms must be positive.");
    }
    if (options.max_workers <= 0) {
        errors.push_back("--max-workers must be positive.");
    }
    if (options.override_fake_attacks_this_turn.has_value()
        && *options.override_fake_attacks_this_turn > 255u) {
        errors.push_back("--override-fake-attacks must be in the range 0..255.");
    }
    if (options.probe_mode != ProbeMode::Capture && !options.capture_profile_path.empty()) {
        errors.push_back("--capture-profile is only valid with --probe-mode capture.");
    }
    return errors;
}

BattleJobBatchRunParseResult parse_battle_job_batch_run_tokens(
    const std::vector<std::string>& args,
    const std::filesystem::path& executable_path) {
    BattleJobBatchRunParseResult result;
    result.options.run_root = default_battle_job_batch_run_root();
    result.options.worker_exe_path = default_battle_job_worker_exe(executable_path);
    cli_detail::DiscDumpRootOptions disc_dump_root_options;

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
        } else if (arg == "--exec-job-seed") {
            if (require_value(args, i, arg, value, result.errors)) {
                BattleJobBatchRunRequest request;
                if (parse_exec_job_seed_spec(value, &request, &result.errors)) {
                    result.options.seeded_exec_job_requests.push_back(request);
                }
            }
        } else if (arg == "--exec-job-seed-list") {
            if (require_value(args, i, arg, value, result.errors)) {
                append_exec_job_seed_list_file(
                    value,
                    &result.options.seeded_exec_job_requests,
                    &result.errors);
            }
        } else if (arg == "--exec-job-fake-attacks") {
            if (require_value(args, i, arg, value, result.errors)) {
                BattleJobBatchRunRequest request;
                if (parse_exec_job_fake_attacks_spec(value, &request, &result.errors)) {
                    result.options.seeded_exec_job_requests.push_back(request);
                }
            }
        } else if (arg == "--exec-job-fake-attacks-list") {
            if (require_value(args, i, arg, value, result.errors)) {
                append_exec_job_fake_attacks_list_file(
                    value,
                    &result.options.seeded_exec_job_requests,
                    &result.errors);
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
        } else if (arg == "--probe-mode") {
            if (require_value(args, i, arg, value, result.errors)) {
                if (const auto mode = parse_probe_mode(value); mode.has_value()) {
                    result.options.probe_mode = *mode;
                } else {
                    result.errors.push_back(
                        "--probe-mode must be capture, progress-only, or control-only.");
                }
            }
        } else if (arg == "--probe-cpu-core") {
            if (require_value(args, i, arg, value, result.errors)) {
                if (const auto core = parse_probe_cpu_core(value); core.has_value()) {
                    result.options.probe_cpu_core = *core;
                } else {
                    result.errors.push_back(
                        "--probe-cpu-core must be default, jit, or interpreter.");
                }
            }
        } else if (arg == "--action-view-std-json-dir") {
            if (require_value(args, i, arg, value, result.errors)) {
                result.options.action_view_std_json_dir = value;
            }
        } else if (arg == "--disc-dump-root"
            || arg == "--std-disc-dump-root") {
            if (require_value(args, i, arg, value, result.errors)) {
                disc_dump_root_options.observe(arg, value, result.errors);
            }
        } else if (arg == "--spice-file-parsing-exe") {
            if (require_value(args, i, arg, value, result.errors)) {
                result.options.spice_file_parsing_exe = value;
                cli_detail::add_spice_file_parsing_exe_warning(
                    result.warnings);
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
        } else if (arg == "--max-workers") {
            int parsed = 0;
            if (require_value(args, i, arg, value, result.errors) && parse_int(value, parsed)) {
                result.options.max_workers = parsed;
            } else {
                result.errors.push_back("--max-workers requires an integer.");
            }
        } else if (arg == "--wait-for-workers-ready") {
            result.options.wait_for_workers_ready = true;
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
            result.errors.push_back("Unknown run-battle-jobs option: " + arg);
        }
    }

    disc_dump_root_options.finalize(
        result.options.disc_dump_root,
        result.errors,
        result.warnings);
    if (!result.help_requested) {
        const auto validation_errors = validate_battle_job_batch_run_options(result.options);
        result.errors.insert(result.errors.end(), validation_errors.begin(), validation_errors.end());
    }
    return result;
}

} // namespace savor::predict
