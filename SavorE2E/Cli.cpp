#include "Cli.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <exception>
#include <iostream>
#include <string_view>
#include <sstream>
#include <vector>

namespace savor::e2e {
namespace {

constexpr auto kAllScenarioOrder = std::to_array<std::string_view>({
    "tasmovie",
    "seedprobe",
    "battle",
    "tasmovie_seedprobe",
    "tasmovie_seedprobe_battle",
    "tasmovie_seedprobe_battle_override",
    "tasmovie_battle",
});

struct ScenarioRequirement {
    bool requires_dtm_file = false;
    bool requires_savestate_file = false;
    bool requires_savestate_file_for_seedprobe_placeholder = false;
};

ScenarioRequirement GetScenarioRequirement(const std::string_view scenario) {
    if (scenario == "seedprobe") {
        return {.requires_savestate_file = true};
    }
    if (scenario == "tasmovie") {
        return {.requires_dtm_file = true};
    }
    if (scenario == "battle") {
        return {.requires_savestate_file = true};
    }
    if (scenario == "tasmovie_seedprobe") {
        return {
            .requires_dtm_file = true,
            .requires_savestate_file = true,
            .requires_savestate_file_for_seedprobe_placeholder = true,
        };
    }
    if (scenario == "tasmovie_seedprobe_battle") {
        return {.requires_dtm_file = true};
    }
    if (scenario == "tasmovie_seedprobe_battle_override" || scenario == "tasmovie_battle") {
        return {
            .requires_dtm_file = true,
            .requires_savestate_file = true,
            .requires_savestate_file_for_seedprobe_placeholder = true,
        };
    }
    return {};
}

bool IsSupportedScenario(const std::string_view scenario) {
    if (scenario == "all") {
        return true;
    }
    for (const auto& supported : kAllScenarioOrder) {
        if (scenario == supported) {
            return true;
        }
    }
    return false;
}

bool IsTasMovieScenario(const std::string_view scenario) {
    return scenario == "tasmovie"
        || scenario == "tasmovie_seedprobe"
        || scenario == "tasmovie_seedprobe_battle"
        || scenario == "tasmovie_seedprobe_battle_override"
        || scenario == "tasmovie_battle";
}

bool IsTasMovieSeedProbeScenario(const std::string_view scenario) {
    return scenario == "tasmovie_seedprobe";
}

std::vector<std::string> ExpandScenarioArguments(const std::vector<std::string>& requested_scenarios) {
    std::vector<std::string> expanded;
    for (const auto& requested : requested_scenarios) {
        if (requested == "all") {
            for (const auto& scenario : kAllScenarioOrder) {
                expanded.push_back(std::string(scenario));
            }
        } else {
            expanded.push_back(requested);
        }
    }
    return expanded;
}

std::vector<std::string> RemoveDuplicateScenarios(
    const std::vector<std::string>& scenarios,
    std::vector<std::string>* duplicates_out) {
    std::vector<std::string> deduplicated;
    for (const auto& scenario : scenarios) {
        if (std::find(deduplicated.begin(), deduplicated.end(), scenario) == deduplicated.end()) {
            deduplicated.push_back(scenario);
        } else if (duplicates_out != nullptr) {
            duplicates_out->push_back(scenario);
        }
    }
    return deduplicated;
}

std::string LowerAscii(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool ApplyDurableLineToken(const std::string& raw_token, std::uint32_t* mask, std::string* error_out) {
    if (mask == nullptr) {
        return false;
    }
    const auto token = LowerAscii(raw_token);
    if (token.empty()) {
        return true;
    }

    if (token == "quiet" || token == "none") {
        *mask = kDurableLineQuietMask;
        return true;
    }
    if (token == "normal" || token == "default") {
        *mask = kDurableLineNormalMask;
        return true;
    }
    if (token == "verbose") {
        *mask = kDurableLineVerboseMask;
        return true;
    }
    if (token == "all") {
        *mask = kDurableLineAllMask;
        return true;
    }

    const auto add = [&](DurableLineCategory category) {
        *mask |= DurableLineBit(category);
    };
    if (token == "result" || token == "results") {
        add(DurableLineCategory::Result);
    } else if (token == "failure" || token == "failures" || token == "error" || token == "errors") {
        add(DurableLineCategory::Failure);
    } else if (token == "warning" || token == "warnings") {
        add(DurableLineCategory::Warning);
    } else if (token == "workflow" || token == "terminal") {
        add(DurableLineCategory::Workflow);
    } else if (token == "materialization" || token == "materialize" || token == "summary") {
        add(DurableLineCategory::Materialization);
    } else if (token == "claim" || token == "claims") {
        add(DurableLineCategory::Claim);
    } else if (token == "dispatch" || token == "dispatches") {
        add(DurableLineCategory::Dispatch);
    } else if (token == "supersede" || token == "superseded") {
        add(DurableLineCategory::Supersede);
    } else if (token == "worker" || token == "workers") {
        add(DurableLineCategory::Worker);
    } else if (token == "adapter" || token == "adapters") {
        add(DurableLineCategory::Adapter);
    } else if (token == "db" || token == "database") {
        add(DurableLineCategory::Db);
    } else if (token == "debug" || token == "diagnostic" || token == "diagnostics") {
        add(DurableLineCategory::Debug);
    } else {
        if (error_out) *error_out = "unknown durable line category: " + raw_token;
        return false;
    }
    return true;
}

bool ParseDurableLineMask(const std::string& value, std::uint32_t* mask_out, std::string* error_out) {
    if (mask_out == nullptr) {
        return false;
    }

    const auto mode = LowerAscii(value);
    if (mode == "quiet" || mode == "none") {
        *mask_out = kDurableLineQuietMask;
        return true;
    }
    if (mode == "normal" || mode == "default") {
        *mask_out = kDurableLineNormalMask;
        return true;
    }
    if (mode == "verbose") {
        *mask_out = kDurableLineVerboseMask;
        return true;
    }
    if (mode == "all") {
        *mask_out = kDurableLineAllMask;
        return true;
    }

    std::uint32_t mask = 0;
    std::string token;
    std::stringstream ss(value);
    while (std::getline(ss, token, ',')) {
        token.erase(token.begin(), std::find_if(token.begin(), token.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
        token.erase(std::find_if(token.rbegin(), token.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(), token.end());
        if (!ApplyDurableLineToken(token, &mask, error_out)) {
            return false;
        }
    }
    *mask_out = mask;
    return true;
}

struct LoadProfile {
    int samples_per_axis = 3;
    int fake_attack_low = 0;
    int fake_attack_high = 2;
};

bool ResolveLoadProfile(const std::string& raw_value, LoadProfile* profile_out, std::string* error_out) {
    if (profile_out == nullptr) {
        return false;
    }
    const auto value = LowerAscii(raw_value);
    if (value == "low") {
        *profile_out = LoadProfile{ .samples_per_axis = 3, .fake_attack_low = 0, .fake_attack_high = 2 };
        return true;
    }
    if (value == "mid") {
        *profile_out = LoadProfile{ .samples_per_axis = 5, .fake_attack_low = 0, .fake_attack_high = 10 };
        return true;
    }
    if (value == "high") {
        *profile_out = LoadProfile{ .samples_per_axis = 20, .fake_attack_low = 0, .fake_attack_high = 20 };
        return true;
    }
    if (error_out != nullptr) {
        *error_out = "unknown --load-level: " + raw_value;
    }
    return false;
}

} // namespace

std::filesystem::path ResolveWorkerExePath(const char* argv0) {
    const auto exe_path = std::filesystem::absolute(std::filesystem::path(argv0));
    return exe_path.parent_path() / "SavorWorker.exe";
}

std::filesystem::path ResolveMigrationRoot(const std::optional<std::filesystem::path>& explicit_root) {
    if (explicit_root.has_value() && !explicit_root->empty() && std::filesystem::exists(*explicit_root / "Execution")) {
        return *explicit_root;
    }

    const std::filesystem::path candidates[] = {
        std::filesystem::path("SavorDb") / "migration",
        std::filesystem::path("..") / "SavorDb" / "migration",
        std::filesystem::path("..") / ".." / "SavorDb" / "migration",
        std::filesystem::path("migration"),
    };

    for (const auto& path : candidates) {
        if (std::filesystem::exists(path / "Execution")) {
            return path;
        }
    }

    return candidates[0];
}

void PrintUsage() {
    std::cout << "SavorE2E - real-worker end-to-end workflow harness\n\n";
    std::cout << "Usage:\n";
    std::cout << "  SavorE2E"
              << " --iso <path>"
              << " --dolphin-base-dir <path>"
              << " [--savestate-file <path>]"
              << " [--dtm-file <path>]"
              << " [--scenario seedprobe|all]"
              << " [--timeout-ms <100..800000000 - default 30000>]"
              << " [--poll-ms <100..5000 - default 100>]"
              << " [--worker-count <1..30 - default 1>"
              << " [--migration-root <path>]"
              << " [--workspace-root <path>]"
              << " [--worker-dir-root <path>]"
              << " [--perf-report-dir <path>]"
              << " [--perf-snapshot-interval-ms <100..5000 - default 1000>]"
              << " [--repeat <count>]"
              << " [--load-level low|mid|high]"
              << " [--visual-worker *]"
              << " [--visual-screenshot-dir <path>]"
              << " [--durable-lines <mode>]"
              << " [--tasmovie-headroom <x10>]"
              << " [--tasmovie-rtc <value>]"
              << " [--seedprobe-samples-per-axis <count>]"
              << " [--seedprobe-combo-attempts-per-target <count>]"
              << " [--battle-fake-attack-low <count>]"
              << " [--battle-fake-attack-high <count>]\n\n";
    std::cout << "Durable line modes: quiet, normal, verbose, all, or a comma list.\n";
    std::cout << "E2E perf mode requires Release builds, worker-count 15, and load-level low|mid|high.\n";
    std::cout << "TAS rtc sets both rtc_low and rtc_high. TAS headroom is the existing x10 value.\n";
    std::cout << "Visual worker locks worker count to 1.\n";
    std::cout << "Categories: result,failure,warning,workflow,materialization,claim,dispatch,supersede,worker,adapter,db,debug\n\n";
    std::cout << "Scenarios: all, seedprobe, tasmovie, battle, "
              << "tasmovie_seedprobe, tasmovie_seedprobe_battle, "
              << "tasmovie_seedprobe_battle_override, tasmovie_battle\n";
    std::cout << "You may pass --scenario multiple times and they will run in order.\n\n";
}

bool ParseArgs(int argc, char** argv, CliOptions* options_out, std::string* error_out) {
    CliOptions options{};
    std::vector<std::string> requested_scenarios;
    bool worker_count_explicit = false;
    bool samples_per_axis_explicit = false;
    bool fake_attack_low_explicit = false;
    bool fake_attack_high_explicit = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto require_value = [&](const char* flag, std::string* out) {
            if (i + 1 >= argc) {
                if (error_out) *error_out = std::string("missing value for ") + flag;
                return false;
            }
            *out = argv[++i];
            return true;
        };
        const auto require_int = [&](const char* flag, int* out) {
            std::string v;
            if (!require_value(flag, &v)) return false;
            try {
                *out = std::stoi(v);
            } catch (const std::exception&) {
                if (error_out) *error_out = std::string("invalid integer for ") + flag + ": " + v;
                return false;
            }
            return true;
        };

        if (arg == "--scenario") {
            std::string scenario;
            if (!require_value("--scenario", &scenario)) return false;
            if (!IsSupportedScenario(scenario)) {
                if (error_out) *error_out = "unknown --scenario: " + scenario;
                return false;
            }
            requested_scenarios.push_back(std::move(scenario));
        } else if (arg == "--timeout-ms") {
            std::string v;
            if (!require_value("--timeout-ms", &v)) return false;
            options.timeout_ms = std::stoll(v);
        } else if (arg == "--poll-ms") {
            std::string v;
            if (!require_value("--poll-ms", &v)) return false;
            options.poll_ms = std::stoll(v);
        } else if (arg == "--worker-count") {
            std::string v;
            if (!require_value("--worker-count", &v)) return false;
            options.worker_count = std::stoll(v);
            worker_count_explicit = true;
        } else if (arg == "--savestate-file") {
            std::string v;
            if (!require_value("--savestate-file", &v)) return false;
            options.savestate_file = std::filesystem::path(v);
        } else if (arg == "--dtm-file") {
            std::string v;
            if (!require_value("--dtm-file", &v)) return false;
            options.dtm_file = std::filesystem::path(v);
        } else if (arg == "--migration-root") {
            std::string v;
            if (!require_value("--migration-root", &v)) return false;
            options.migration_root = std::filesystem::path(v);
        } else if (arg == "--iso") {
            std::string v;
            if (!require_value("--iso", &v)) return false;
            options.iso_path = std::filesystem::path(v);
        } else if (arg == "--dolphin-base-dir") {
            std::string v;
            if (!require_value("--dolphin-base-dir", &v)) return false;
            options.dolphin_base_dir = std::filesystem::path(v);
        } else if (arg == "--workspace-root") {
            std::string v;
            if (!require_value("--workspace-root", &v)) return false;
            options.workspace_root = std::filesystem::path(v);
        } else if (arg == "--worker-dir-root") {
            std::string v;
            if (!require_value("--worker-dir-root", &v)) return false;
            options.worker_dir_root = std::filesystem::path(v);
        } else if (arg == "--perf-report-dir") {
            std::string v;
            if (!require_value("--perf-report-dir", &v)) return false;
            options.perf_report_dir = std::filesystem::path(v);
        } else if (arg == "--perf-snapshot-interval-ms") {
            std::string v;
            if (!require_value("--perf-snapshot-interval-ms", &v)) return false;
            options.perf_snapshot_interval_ms = std::stoll(v);
        } else if (arg == "--repeat") {
            int v = 0;
            if (!require_int("--repeat", &v)) return false;
            options.repeat = v;
        } else if (arg == "--load-level") {
            std::string v;
            if (!require_value("--load-level", &v)) return false;
            options.load_level = LowerAscii(v);
        } else if (arg == "--visual-worker") {
            options.visual_worker = true;
        } else if (arg == "--visual-screenshot-dir") {
            std::string v;
            if (!require_value("--visual-screenshot-dir", &v)) return false;
            options.visual_screenshot_dir = std::filesystem::path(v);
        } else if (arg == "--durable-lines" || arg == "--debug-durable-lines") {
            std::string v;
            if (!require_value(arg.c_str(), &v)) return false;
            if (!ParseDurableLineMask(v, &options.durable_line_mask, error_out)) return false;
        } else if (arg == "--tasmovie-headroom" || arg == "--tasmovie-headroom-x10" || arg == "--headroom") {
            int v = 0;
            if (!require_int(arg.c_str(), &v)) return false;
            options.tasmovie_headroom_x10 = v;
        } else if (arg == "--tasmovie-rtc" || arg == "--rtc") {
            int v = 0;
            if (!require_int(arg.c_str(), &v)) return false;
            options.tasmovie_rtc = v;
        } else if (arg == "--seedprobe-samples-per-axis" || arg == "--samples-per-axis") {
            int v = 0;
            if (!require_int(arg.c_str(), &v)) return false;
            options.seedprobe_samples_per_axis = v;
            samples_per_axis_explicit = true;
        } else if (arg == "--seedprobe-combo-attempts-per-target" || arg == "--combo-attempts-per-target") {
            int v = 0;
            if (!require_int(arg.c_str(), &v)) return false;
            options.seedprobe_combo_attempts_per_target = v;
        } else if (arg == "--battle-fake-attack-low" || arg == "--fake-attack-low") {
            int v = 0;
            if (!require_int(arg.c_str(), &v)) return false;
            options.battle_fake_attack_low = v;
            fake_attack_low_explicit = true;
        } else if (arg == "--battle-fake-attack-high" || arg == "--fake-attack-high") {
            int v = 0;
            if (!require_int(arg.c_str(), &v)) return false;
            options.battle_fake_attack_high = v;
            fake_attack_high_explicit = true;
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage();
            std::exit(0);
        } else {
            if (error_out) *error_out = "unknown argument: " + arg;
            return false;
        }
    }

    if (requested_scenarios.empty()) {
        requested_scenarios = {"seedprobe"};
    }

    std::vector<std::string> ignored_duplicate_scenarios;
    options.scenarios = RemoveDuplicateScenarios(
        ExpandScenarioArguments(requested_scenarios),
        &ignored_duplicate_scenarios);
    for (const auto& scenario : ignored_duplicate_scenarios) {
        std::cerr << "[warn] duplicate scenario '" << scenario << "' ignored\n";
    }
    if (!options.scenarios.empty()) {
        options.scenario = options.scenarios.front();
    }

    const bool perf_mode = options.perf_report_dir.has_value();
    if (perf_mode) {
#ifdef NDEBUG
        if (options.load_level.empty()) {
            options.load_level = "low";
        }
        if (!worker_count_explicit) {
            options.worker_count = 15;
        } else if (options.worker_count != 15) {
            if (error_out) *error_out = "E2E perf mode requires --worker-count 15";
            return false;
        }
#else
        if (error_out) *error_out = "E2E perf mode must be run from a Release SavorE2E build";
        return false;
#endif
    }

    if (!options.load_level.empty()) {
        LoadProfile profile{};
        if (!ResolveLoadProfile(options.load_level, &profile, error_out)) {
            return false;
        }
        if (perf_mode || !samples_per_axis_explicit) {
            options.seedprobe_samples_per_axis = profile.samples_per_axis;
        }
        if (perf_mode || !fake_attack_low_explicit) {
            options.battle_fake_attack_low = profile.fake_attack_low;
        }
        if (perf_mode || !fake_attack_high_explicit) {
            options.battle_fake_attack_high = profile.fake_attack_high;
        }
    }

    bool is_tasmovie = false;
    bool is_tasmovie_seedprobe = false;
    bool needs_savestate = false;
    bool needs_dtm = false;
    std::vector<std::string> savestate_required_scenarios;
    std::vector<std::string> dtm_required_scenarios;
    std::vector<std::string> placeholder_savestate_required_scenarios;

    const auto join = [](const std::vector<std::string>& names) {
        std::ostringstream oss;
        for (size_t i = 0; i < names.size(); ++i) {
            if (i > 0) {
                oss << ", ";
            }
            oss << names[i];
        }
        return oss.str();
    };

    for (const auto& scenario : options.scenarios) {
        is_tasmovie = is_tasmovie || IsTasMovieScenario(scenario);
        is_tasmovie_seedprobe = is_tasmovie_seedprobe || IsTasMovieSeedProbeScenario(scenario);
        const auto req = GetScenarioRequirement(scenario);
        needs_savestate = needs_savestate || req.requires_savestate_file;
        needs_dtm = needs_dtm || req.requires_dtm_file;

        if (req.requires_savestate_file) {
            savestate_required_scenarios.push_back(scenario);
        }
        if (req.requires_dtm_file) {
            dtm_required_scenarios.push_back(scenario);
        }
        if (req.requires_savestate_file_for_seedprobe_placeholder) {
            placeholder_savestate_required_scenarios.push_back(scenario);
        }
    }

    if (needs_savestate && options.savestate_file.empty()) {
        if (error_out) *error_out = "--savestate-file is required for: " + join(savestate_required_scenarios);
        return false;
    }
    if (!options.savestate_file.empty() && !std::filesystem::exists(options.savestate_file)) {
        if (error_out) *error_out = "savestate file does not exist: " + options.savestate_file.string();
        return false;
    }
    if (needs_dtm && options.dtm_file.empty()) {
        if (error_out) *error_out = "--dtm-file is required for: " + join(dtm_required_scenarios);
        return false;
    }
    if (!options.dtm_file.empty() && !std::filesystem::exists(options.dtm_file)) {
        if (error_out) *error_out = "DTM file does not exist: " + options.dtm_file.string();
        return false;
    }
    if (is_tasmovie_seedprobe && options.savestate_file.empty()) {
        if (error_out) *error_out = "--savestate-file is required as the placeholder SeedProbe run state for: "
                                   + join(placeholder_savestate_required_scenarios);
        return false;
    }
    if (options.iso_path.empty() || !std::filesystem::exists(options.iso_path)) {
        if (error_out) *error_out = "--iso is required and must exist";
        return false;
    }
    if (options.dolphin_base_dir.empty() || !std::filesystem::exists(options.dolphin_base_dir)) {
        if (error_out) *error_out = "--dolphin-base-dir is required and must exist";
        return false;
    }
    if (options.timeout_ms < 100 || options.poll_ms < 100) {
        if (error_out) *error_out = "--timeout-ms and --poll-ms must be > 100";
        return false;
    }    
    if (options.timeout_ms > 800000000) {
        if (error_out) *error_out = "--timeout-ms must be <= 800000000";
        return false;
    }
    if (options.poll_ms > 5000) {
        if (error_out) *error_out = "--timeout-ms and --poll-ms must be <= 5000";
        return false;
    }
    if (options.perf_snapshot_interval_ms < 100 || options.perf_snapshot_interval_ms > 5000) {
        if (error_out) *error_out = "--perf-snapshot-interval-ms must be between 100 and 5000";
        return false;
    }
    if (options.repeat <= 0) {
        if (error_out) *error_out = "--repeat must be > 0";
        return false;
    }
    if (perf_mode && options.visual_worker) {
        if (error_out) *error_out = "E2E perf mode does not support --visual-worker";
        return false;
    }
    if (options.tasmovie_headroom_x10.has_value() && (*options.tasmovie_headroom_x10 < 0 || *options.tasmovie_headroom_x10 > 255)) {
        if (error_out) *error_out = "--tasmovie-headroom must be between 0 and 255";
        return false;
    }
    if (options.tasmovie_rtc.has_value() && (*options.tasmovie_rtc < 0 || *options.tasmovie_rtc > 255)) {
        if (error_out) *error_out = "--tasmovie-rtc must be between 0 and 255";
        return false;
    }
    if (options.seedprobe_samples_per_axis.has_value() && *options.seedprobe_samples_per_axis <= 0) {
        if (error_out) *error_out = "--seedprobe-samples-per-axis must be > 0";
        return false;
    }
    if (options.seedprobe_combo_attempts_per_target.has_value() && *options.seedprobe_combo_attempts_per_target <= 0) {
        if (error_out) *error_out = "--seedprobe-combo-attempts-per-target must be > 0";
        return false;
    }
    if (options.battle_fake_attack_low.has_value() && *options.battle_fake_attack_low < 0) {
        if (error_out) *error_out = "--battle-fake-attack-low must be >= 0";
        return false;
    }
    if (options.battle_fake_attack_high.has_value() && *options.battle_fake_attack_high < 0) {
        if (error_out) *error_out = "--battle-fake-attack-high must be >= 0";
        return false;
    }
    if (options.battle_fake_attack_low.has_value()
        && options.battle_fake_attack_high.has_value()
        && *options.battle_fake_attack_low > *options.battle_fake_attack_high) {
        if (error_out) *error_out = "--battle-fake-attack-low must be <= --battle-fake-attack-high";
        return false;
    }

    if (options.visual_worker) {
        options.worker_count = 1;
    }

    if (!options.worker_dir_root.has_value() && options.workspace_root.has_value()) {
		options.worker_dir_root = std::filesystem::path(*options.workspace_root) / ".workers";
    }

    *options_out = std::move(options);
    return true;
}

} // namespace savor::e2e
