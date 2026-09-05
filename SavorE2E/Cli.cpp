#include "Cli.h"

#include "Execution/WorkerLimits.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <sstream>
#include <vector>

namespace savor::e2e {
namespace {

constexpr auto kScenarioCatalog = std::to_array<E2eScenarioDescriptor>({
    {
        .name = "seedprobe",
        .kind = E2eScenarioKind::SeedProbe,
        .supported_entry_sources =
            EntrySourceBit(E2eScenarioEntrySource::ImportedSavestateFile)
            | EntrySourceBit(
                E2eScenarioEntrySource::PreparedSterilizedCheckpoint),
        .default_entry_source =
            E2eScenarioEntrySource::ImportedSavestateFile,
        .include_in_all = true,
    },
    {
        .name = "battle",
        .kind = E2eScenarioKind::Battle,
        .supported_entry_sources =
            EntrySourceBit(E2eScenarioEntrySource::FreshTasMovieValidation)
            | EntrySourceBit(
                E2eScenarioEntrySource::PreparedSterilizedCheckpoint)
            | EntrySourceBit(
                E2eScenarioEntrySource::TasMovieEstablishmentAttempt),
        .default_entry_source =
            E2eScenarioEntrySource::FreshTasMovieValidation,
        .must_run_alone = true,
        .requires_repeat_one = true,
    },
    {
        .name = "tasmovie_cutscene",
        .kind = E2eScenarioKind::TasMovieCutscene,
        .supported_entry_sources =
            EntrySourceBit(E2eScenarioEntrySource::FreshTasMovieValidation),
        .default_entry_source =
            E2eScenarioEntrySource::FreshTasMovieValidation,
        .must_run_alone = true,
        .requires_repeat_one = true,
    },
    {
        .name = "tasmovie_establish",
        .kind = E2eScenarioKind::TasMovieEstablish,
        .supported_entry_sources =
            EntrySourceBit(E2eScenarioEntrySource::FreshTasMovieValidation),
        .default_entry_source =
            E2eScenarioEntrySource::FreshTasMovieValidation,
        .must_run_alone = true,
        .requires_repeat_one = true,
        .requires_one_worker = true,
    },
    {
        .name = "tasmovie_validation",
        .kind = E2eScenarioKind::TasMovieValidation,
        .supported_entry_sources =
            EntrySourceBit(E2eScenarioEntrySource::FreshTasMovieValidation)
            | EntrySourceBit(
                E2eScenarioEntrySource::TasMovieEstablishmentAttempt),
        .default_entry_source =
            E2eScenarioEntrySource::FreshTasMovieValidation,
        .must_run_alone = true,
        .requires_repeat_one = true,
        .requires_one_worker = true,
    },
    {
        .name = "tasmovie_sterile",
        .kind = E2eScenarioKind::TasMovieSterile,
        .supported_entry_sources =
            EntrySourceBit(E2eScenarioEntrySource::FreshTasMovieValidation)
            | EntrySourceBit(
                E2eScenarioEntrySource::TasMovieEstablishmentAttempt),
        .default_entry_source =
            E2eScenarioEntrySource::FreshTasMovieValidation,
        .must_run_alone = true,
        .requires_repeat_one = true,
        .requires_one_worker = true,
    },
    {
        .name = "tasmovie_seedprobe",
        .kind = E2eScenarioKind::TasMovieSeedProbe,
        .supported_entry_sources =
            EntrySourceBit(E2eScenarioEntrySource::FreshTasMovieValidation)
            | EntrySourceBit(E2eScenarioEntrySource::ExistingDtmArtifact),
        .default_entry_source =
            E2eScenarioEntrySource::FreshTasMovieValidation,
        .must_run_alone = true,
        .requires_repeat_one = true,
    },
    {
        .name = "tasmovie_input_epoch_rewrite",
        .kind = E2eScenarioKind::TasMovieInputEpochRewrite,
        .supported_entry_sources =
            EntrySourceBit(E2eScenarioEntrySource::FreshTasMovieValidation),
        .default_entry_source =
            E2eScenarioEntrySource::FreshTasMovieValidation,
        .must_run_alone = true,
        .requires_repeat_one = true,
        .requires_one_worker = true,
    },
    {
        .name = "tasmovie_input_epoch_breakpoint_diagnostics",
        .kind = E2eScenarioKind::TasMovieInputEpochBreakpointDiagnostics,
        .supported_entry_sources =
            EntrySourceBit(E2eScenarioEntrySource::FreshTasMovieValidation),
        .default_entry_source =
            E2eScenarioEntrySource::FreshTasMovieValidation,
        .must_run_alone = true,
        .requires_repeat_one = true,
        .requires_one_worker = true,
    },
    {
        .name = "workflow_unit",
        .kind = E2eScenarioKind::WorkflowUnit,
        .supported_entry_sources = EntrySourceBit(
            E2eScenarioEntrySource::ExistingWorkspaceReference),
        .default_entry_source =
            E2eScenarioEntrySource::ExistingWorkspaceReference,
        .must_run_alone = true,
        .requires_repeat_one = true,
    },
});

bool IsSupportedScenario(const std::string_view scenario) {
    return scenario == "all" || FindE2eScenarioDescriptor(scenario) != nullptr;
}

std::vector<std::string> ExpandScenarioArguments(const std::vector<std::string>& requested_scenarios) {
    std::vector<std::string> expanded;
    for (const auto& requested : requested_scenarios) {
        if (requested == "all") {
            for (const auto& scenario : kScenarioCatalog) {
                if (scenario.include_in_all) {
                    expanded.push_back(std::string(scenario.name));
                }
            }
        } else {
            expanded.push_back(requested);
        }
    }
    return expanded;
}

bool HasAnyRtcArgument(const CliOptions& options) {
    return options.tasmovie_rtc.has_value()
        || options.tasmovie_rtc_min.has_value()
        || options.tasmovie_rtc_max.has_value();
}

bool IsCompleteExistingWorkspace(
    const std::filesystem::path& root,
    std::string* error_out) {
    if (root.empty() || !std::filesystem::is_directory(root)) {
        if (error_out) {
            *error_out = "existing-workspace mode requires an existing --workspace-root";
        }
        return false;
    }
    constexpr std::array<std::string_view, 6> database_names{
        "execution.db", "state.db", "analysis.db", "authoring.db",
        "ui_read.db", "archive.db",
    };
    for (const auto name : database_names) {
        const auto path = root / name;
        if (!std::filesystem::is_regular_file(path)) {
            if (error_out) {
                *error_out = "existing workspace is incomplete; missing "
                    + path.string();
            }
            return false;
        }
    }
    return true;
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
};

bool ResolveLoadProfile(const std::string& raw_value, LoadProfile* profile_out, std::string* error_out) {
    if (profile_out == nullptr) {
        return false;
    }
    const auto value = LowerAscii(raw_value);
    if (value == "low") {
        *profile_out = LoadProfile{ .samples_per_axis = 3 };
        return true;
    }
    if (value == "mid") {
        *profile_out = LoadProfile{ .samples_per_axis = 5 };
        return true;
    }
    if (value == "high") {
        *profile_out = LoadProfile{ .samples_per_axis = 20 };
        return true;
    }
    if (error_out != nullptr) {
        *error_out = "unknown --load-level: " + raw_value;
    }
    return false;
}

} // namespace

std::span<const E2eScenarioDescriptor> E2eScenarioCatalog() {
    return kScenarioCatalog;
}

const E2eScenarioDescriptor* FindE2eScenarioDescriptor(
    const std::string_view name) {
    const auto it = std::ranges::find(
        kScenarioCatalog, name, &E2eScenarioDescriptor::name);
    return it == kScenarioCatalog.end() ? nullptr : &*it;
}

E2eScenarioEntrySource SelectE2eScenarioEntrySource(
    const E2eScenarioDescriptor& descriptor,
    const CliOptions& options) {
    if (options.dtm_artifact_id.has_value()) {
        return E2eScenarioEntrySource::ExistingDtmArtifact;
    }
    if (options.source_savestate_id.has_value()) {
        return E2eScenarioEntrySource::PreparedSterilizedCheckpoint;
    }
    if (options.tasmovie_establishment_id.has_value()) {
        return E2eScenarioEntrySource::TasMovieEstablishmentAttempt;
    }
    return descriptor.default_entry_source;
}

std::string_view ToString(const E2eScenarioEntrySource source) {
    switch (source) {
    case E2eScenarioEntrySource::ImportedSavestateFile:
        return "ImportedSavestateFile";
    case E2eScenarioEntrySource::FreshTasMovieValidation:
        return "FreshTasMovieValidation";
    case E2eScenarioEntrySource::PreparedSterilizedCheckpoint:
        return "PreparedSterilizedCheckpoint";
    case E2eScenarioEntrySource::TasMovieEstablishmentAttempt:
        return "TasMovieEstablishmentAttempt";
    case E2eScenarioEntrySource::ExistingWorkspaceReference:
        return "ExistingWorkspaceReference";
    case E2eScenarioEntrySource::ExistingDtmArtifact:
        return "ExistingDtmArtifact";
    }
    return "Unknown";
}

bool EntrySourceRequiresFreshWorkspace(const E2eScenarioEntrySource source) {
    return source != E2eScenarioEntrySource::PreparedSterilizedCheckpoint
        && source != E2eScenarioEntrySource::TasMovieEstablishmentAttempt
        && source != E2eScenarioEntrySource::ExistingWorkspaceReference
        && source != E2eScenarioEntrySource::ExistingDtmArtifact;
}

std::filesystem::path ResolveWorkerExePath(const char* argv0) {
    const auto exe_path = std::filesystem::absolute(std::filesystem::path(argv0));
    return exe_path.parent_path() / "SavorWorker.exe";
}

void PrintUsage() {
    std::cout << "SavorE2E - real-worker end-to-end workflow harness\n\n";
    std::cout << "Usage:\n";
    std::cout << "  SavorE2E"
              << " --iso <path>"
              << " --dolphin-base-dir <path>"
              << " [--savestate-file <path>]"
              << " [--source-savestate-id <id>]"
              << " [--tas-establishment-id <id>]"
              << " [--workflow-unit <unit-kind>]"
              << " [--source-ref-kind <kind>]"
              << " [--source-ref-id <positive-id>]"
              << " [--dtm-artifact-id <positive-id>]"
              << " [--dtm-file <path>]"
              << " [--scenario seedprobe|battle|tasmovie_establish|tasmovie_validation|tasmovie_sterile|tasmovie_seedprobe|workflow_unit|all]"
              << " [--poll-ms <100..5000 - default 100>]"
              << " [--worker-count <1..30 - default 1>]"
              << " [--wait-for-workers-ready]"
              << " [--workspace-root <path>]"
              << " [--worker-dir-root <path>]"
              << " [--perf-report-dir <path>]"
              << " [--perf-snapshot-interval-ms <100..5000 - default 1000>]"
              << " [--repeat <count>]"
              << " [--load-level low|mid|high]"
              << " [--visual-worker *]"
              << " [--capture-seed-calls]"
              << " [--cutscene-delay]"
              << " [--breakpoint-diagnostics]"
              << " [--diagnostic-max-runs <1..5>]"
              << " [--visual-screenshot-dir <path>]"
              << " [--durable-lines <mode>]"
              << " [--tasmovie-rtc <value>]"
              << " [--tasmovie-rtc-min <value>]"
              << " [--tasmovie-rtc-max <value>]"
              << " [--seedprobe-min-value <0..255>]"
              << " [--seedprobe-max-value <0..255>]"
              << " [--seedprobe-samples-per-axis <count>]"
              << " [--seedprobe-combo-attempts-per-target <count>]"
              << " [--seedprobe-combo-sampler-tries <count>]"
              << " [--battle-fake-attack-min <count>]"
              << " [--battle-fake-attack-max <count>]"
              << "\n\n";
    std::cout << "Durable line modes: quiet, normal, verbose, all, or a comma list.\n";
    std::cout << "E2E perf mode requires Release builds and load-level low|mid|high; worker-count defaults to 15 and accepts 1..30.\n";
    std::cout << "Prepared checkpoint mode is selected by --source-savestate-id and preserves the complete existing workspace.\n";
    std::cout << "The workflow_unit scenario preserves an existing workspace and launches one exact static unit from --workflow-unit, --source-ref-kind, and --source-ref-id.\n";
    std::cout << "The tasmovie_establish scenario establishes the handcrafted root cursor only; it takes no RTC and must run alone.\n";
    std::cout << "The tasmovie_validation scenario requires one exact --tasmovie-rtc in 0..4294967295 and must run alone.\n";
    std::cout << "The tasmovie_sterile scenario composes validation and sterilization, requires an exact RTC and no external savestate, and must run alone.\n";
    std::cout << "The tasmovie_seedprobe scenario composes validation with SeedProbe, requires an exact RTC and no external savestate, and must run alone.\n";
    std::cout << "With --dtm-artifact-id, tasmovie_seedprobe preserves an existing workspace and reuses that qualified State DTM artifact.\n";
    std::cout << "The battle scenario validates the approved DTM, sterilizes its checkpoint, and in Fresh mode requires one exact --tasmovie-rtc with no external savestate.\n";
    std::cout << "With --source-savestate-id, battle starts from a prepared sterilized checkpoint.\n";
    std::cout << "With --tas-establishment-id, battle, tasmovie_validation, and tasmovie_sterile start from an existing root-cursor establishment attempt and do not reset the workspace.\n";
    std::cout << "Visual worker locks worker count to 1.\n";
    std::cout << "Categories: result,failure,warning,workflow,materialization,claim,dispatch,supersede,worker,adapter,db,debug\n\n";
    std::cout << "Scenarios: all";
    for (const auto& descriptor : E2eScenarioCatalog()) {
        std::cout << ", " << descriptor.name;
    }
    std::cout << "\nEntry sources:\n";
    for (const auto& descriptor : E2eScenarioCatalog()) {
        std::cout << "  " << descriptor.name << ": ";
        bool first = true;
        for (const auto source : {
                 E2eScenarioEntrySource::ImportedSavestateFile,
                 E2eScenarioEntrySource::FreshTasMovieValidation,
                 E2eScenarioEntrySource::PreparedSterilizedCheckpoint,
                 E2eScenarioEntrySource::TasMovieEstablishmentAttempt,
                 E2eScenarioEntrySource::ExistingWorkspaceReference}) {
            if ((descriptor.supported_entry_sources & EntrySourceBit(source)) == 0) {
                continue;
            }
            if (!first) std::cout << ", ";
            std::cout << ToString(source);
            if (source == descriptor.default_entry_source) std::cout << " (default)";
            first = false;
        }
        std::cout << '\n';
    }
    std::cout << "You may pass --scenario multiple times and they will run in order.\n\n";
}

bool ParseArgs(int argc, char** argv, CliOptions* options_out, std::string* error_out) {
    CliOptions options{};
    std::vector<std::string> requested_scenarios;
    bool worker_count_explicit = false;
    bool samples_per_axis_explicit = false;
    bool tasmovie_rtc_explicit = false;
    bool tasmovie_rtc_min_explicit = false;
    bool tasmovie_rtc_max_explicit = false;

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
        const auto require_int64 = [&](const char* flag, std::int64_t* out) {
            std::string v;
            if (!require_value(flag, &v)) return false;
            try {
                std::size_t consumed = 0;
                const auto parsed = std::stoll(v, &consumed, 0);
                if (consumed != v.size()) throw std::invalid_argument("trailing characters");
                *out = parsed;
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
        } else if (arg == "--poll-ms") {
            std::string v;
            if (!require_value("--poll-ms", &v)) return false;
            options.poll_ms = std::stoll(v);
        } else if (arg == "--worker-count") {
            std::string v;
            if (!require_value("--worker-count", &v)) return false;
            options.worker_count = std::stoll(v);
            worker_count_explicit = true;
        } else if (arg == "--wait-for-workers-ready") {
            options.wait_for_workers_ready = true;
        } else if (arg == "--savestate-file") {
            std::string v;
            if (!require_value("--savestate-file", &v)) return false;
            options.savestate_file = std::filesystem::path(v);
        } else if (arg == "--source-savestate-id") {
            std::string v;
            if (!require_value("--source-savestate-id", &v)) return false;
            try {
                options.source_savestate_id = std::stoll(v);
            } catch (const std::exception&) {
                if (error_out) *error_out = "invalid integer for --source-savestate-id: " + v;
                return false;
            }
        } else if (arg == "--tas-establishment-id") {
            std::string v;
            if (!require_value("--tas-establishment-id", &v)) return false;
            try {
                options.tasmovie_establishment_id = std::stoll(v);
            } catch (const std::exception&) {
                if (error_out) *error_out = "invalid integer for --tas-establishment-id: " + v;
                return false;
            }
        } else if (arg == "--workflow-unit") {
            std::string v;
            if (!require_value("--workflow-unit", &v)) return false;
            if (v.empty()) {
                if (error_out) *error_out = "--workflow-unit must not be empty";
                return false;
            }
            options.workflow_unit = std::move(v);
        } else if (arg == "--source-ref-kind") {
            std::string v;
            if (!require_value("--source-ref-kind", &v)) return false;
            if (v.empty()) {
                if (error_out) *error_out = "--source-ref-kind must not be empty";
                return false;
            }
            options.source_ref_kind = std::move(v);
        } else if (arg == "--source-ref-id") {
            std::int64_t v = 0;
            if (!require_int64("--source-ref-id", &v)) return false;
            options.source_ref_id = v;
        } else if (arg == "--dtm-artifact-id") {
            std::int64_t v = 0;
            if (!require_int64("--dtm-artifact-id", &v)) return false;
            options.dtm_artifact_id = v;
        } else if (arg == "--dtm-file") {
            std::string v;
            if (!require_value("--dtm-file", &v)) return false;
            options.dtm_file = std::filesystem::path(v);
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
        } else if (arg == "--tasmovie-rtc" || arg == "--rtc") {
            if (tasmovie_rtc_explicit) {
                if (error_out) *error_out = "exact TAS Movie RTC may be specified only once";
                return false;
            }
            std::int64_t v = 0;
            if (!require_int64(arg.c_str(), &v)) return false;
            options.tasmovie_rtc = v;
            tasmovie_rtc_explicit = true;
        } else if (arg == "--tasmovie-rtc-min" || arg == "--rtc-min") {
            if (tasmovie_rtc_min_explicit) {
                if (error_out) *error_out = "TAS Movie RTC minimum may be specified only once";
                return false;
            }
            std::int64_t v = 0;
            if (!require_int64(arg.c_str(), &v)) return false;
            options.tasmovie_rtc_min = v;
            tasmovie_rtc_min_explicit = true;
        } else if (arg == "--tasmovie-rtc-max" || arg == "--rtc-max") {
            if (tasmovie_rtc_max_explicit) {
                if (error_out) *error_out = "TAS Movie RTC maximum may be specified only once";
                return false;
            }
            std::int64_t v = 0;
            if (!require_int64(arg.c_str(), &v)) return false;
            options.tasmovie_rtc_max = v;
            tasmovie_rtc_max_explicit = true;
        } else if (arg == "--seedprobe-samples-per-axis" || arg == "--samples-per-axis") {
            int v = 0;
            if (!require_int(arg.c_str(), &v)) return false;
            options.seedprobe_samples_per_axis = v;
            samples_per_axis_explicit = true;
        } else if (arg == "--seedprobe-min-value") {
            int v = 0;
            if (!require_int(arg.c_str(), &v)) return false;
            options.seedprobe_min_value = v;
        } else if (arg == "--seedprobe-max-value") {
            int v = 0;
            if (!require_int(arg.c_str(), &v)) return false;
            options.seedprobe_max_value = v;
        } else if (arg == "--seedprobe-combo-attempts-per-target" || arg == "--combo-attempts-per-target") {
            int v = 0;
            if (!require_int(arg.c_str(), &v)) return false;
            options.seedprobe_combo_attempts_per_target = v;
        } else if (arg == "--seedprobe-combo-sampler-tries") {
            int v = 0;
            if (!require_int(arg.c_str(), &v)) return false;
            options.seedprobe_combo_sampler_tries = v;
        } else if (arg == "--breakpoint-diagnostics") {
            options.breakpoint_diagnostics = true;
        } else if (arg == "--capture-seed-calls") {
            options.capture_seed_calls = true;
        } else if (arg == "--cutscene-delay") {
            options.cutscene_delay = true;
        } else if (arg == "--diagnostic-max-runs") {
            int v = 0;
            if (!require_int(arg.c_str(), &v)) return false;
            options.diagnostic_max_runs = v;
        } else if (arg == "--battle-fake-attack-min") {
            int v = 0;
            if (!require_int(arg.c_str(), &v)) return false;
            options.battle_fake_attack_min = v;
        } else if (arg == "--battle-fake-attack-max") {
            int v = 0;
            if (!require_int(arg.c_str(), &v)) return false;
            options.battle_fake_attack_max = v;
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
    }

    bool is_tasmovie_establish = false;
    bool is_tasmovie_validation = false;
    bool is_tasmovie_sterile = false;
    bool is_tasmovie_seedprobe = false;
    bool is_battle = false;
    bool is_tasmovie_cutscene = false;
    bool is_workflow_unit = false;
    bool needs_savestate = false;
    bool needs_dtm = false;
    std::vector<std::string> savestate_required_scenarios;
    std::vector<std::string> dtm_required_scenarios;

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

    bool prepared_checkpoint_mode = false;
    bool tas_establishment_mode = false;
    for (const auto& scenario : options.scenarios) {
        const auto* descriptor = FindE2eScenarioDescriptor(scenario);
        if (descriptor == nullptr) {
            if (error_out) *error_out = "unknown --scenario: " + scenario;
            return false;
        }
        const auto source = SelectE2eScenarioEntrySource(*descriptor, options);
        if ((descriptor->supported_entry_sources & EntrySourceBit(source)) == 0) {
            if (error_out) {
                *error_out = "scenario '" + scenario + "' does not support entry source "
                    + std::string(ToString(source));
            }
            return false;
        }
        if (descriptor->must_run_alone && options.scenarios.size() != 1) {
            if (error_out) *error_out = "scenario '" + scenario + "' must run alone";
            return false;
        }
        if (descriptor->requires_repeat_one && options.repeat != 1) {
            if (error_out) *error_out = "scenario '" + scenario + "' requires --repeat 1";
            return false;
        }
        if (descriptor->requires_one_worker && options.worker_count != 1) {
            if (error_out) *error_out = "scenario '" + scenario + "' requires exactly one worker";
            return false;
        }

        is_tasmovie_establish = is_tasmovie_establish
            || descriptor->kind == E2eScenarioKind::TasMovieEstablish;
        is_tasmovie_validation = is_tasmovie_validation
            || descriptor->kind == E2eScenarioKind::TasMovieValidation;
        is_tasmovie_sterile = is_tasmovie_sterile
            || descriptor->kind == E2eScenarioKind::TasMovieSterile;
        is_tasmovie_seedprobe = is_tasmovie_seedprobe
            || descriptor->kind == E2eScenarioKind::TasMovieSeedProbe;
        is_battle = is_battle || descriptor->kind == E2eScenarioKind::Battle;
        is_tasmovie_cutscene = is_tasmovie_cutscene
            || descriptor->kind == E2eScenarioKind::TasMovieCutscene;
        is_workflow_unit = is_workflow_unit
            || descriptor->kind == E2eScenarioKind::WorkflowUnit;

        switch (source) {
        case E2eScenarioEntrySource::ImportedSavestateFile:
            needs_savestate = true;
            savestate_required_scenarios.push_back(scenario);
            break;
        case E2eScenarioEntrySource::TasMovieEstablishmentAttempt:
            tas_establishment_mode = true;
            break;
        case E2eScenarioEntrySource::FreshTasMovieValidation:
            needs_dtm = true;
            dtm_required_scenarios.push_back(scenario);
            break;
        case E2eScenarioEntrySource::PreparedSterilizedCheckpoint:
            prepared_checkpoint_mode = true;
            break;
        case E2eScenarioEntrySource::ExistingWorkspaceReference:
            break;
        case E2eScenarioEntrySource::ExistingDtmArtifact:
            break;
        }
    }

    if (options.capture_seed_calls
        && (!is_tasmovie_cutscene || options.scenarios.size() != 1)) {
        if (error_out) {
            *error_out = "--capture-seed-calls is supported only by --scenario tasmovie_cutscene";
        }
        return false;
    }

    const bool has_workflow_unit_arguments = options.workflow_unit.has_value()
        || options.source_ref_kind.has_value()
        || options.source_ref_id.has_value();
    if (is_workflow_unit) {
        if (!options.workflow_unit || options.workflow_unit->empty()
            || !options.source_ref_kind || options.source_ref_kind->empty()
            || !options.source_ref_id || *options.source_ref_id <= 0) {
            if (error_out) {
                *error_out = "workflow_unit requires --workflow-unit, --source-ref-kind, and a positive --source-ref-id";
            }
            return false;
        }
        if (!options.workspace_root
            || !IsCompleteExistingWorkspace(*options.workspace_root, error_out)) {
            return false;
        }
        if (options.source_savestate_id || !options.savestate_file.empty()
            || !options.dtm_file.empty() || HasAnyRtcArgument(options)) {
            if (error_out) {
                *error_out = "workflow_unit rejects savestate, DTM, and RTC entry arguments";
            }
            return false;
        }
    } else if (has_workflow_unit_arguments) {
        if (error_out) {
            *error_out = "--workflow-unit, --source-ref-kind, and --source-ref-id require --scenario workflow_unit";
        }
        return false;
    }
    if (options.dtm_artifact_id.has_value()) {
        if (*options.dtm_artifact_id <= 0) {
            if (error_out) {
                *error_out = "--dtm-artifact-id must be positive";
            }
            return false;
        }
        if (!options.workspace_root
            || !IsCompleteExistingWorkspace(*options.workspace_root, error_out)) {
            return false;
        }
        if (!options.dtm_file.empty() || !options.savestate_file.empty()
            || options.source_savestate_id.has_value()
            || options.tasmovie_establishment_id.has_value()) {
            if (error_out) {
                *error_out = "DTM-artifact mode requires an existing workspace and rejects --dtm-file, --savestate-file, --source-savestate-id, and --tas-establishment-id";
            }
            return false;
        }
    }

    if (prepared_checkpoint_mode) {
        if (!options.source_savestate_id.has_value()
            || *options.source_savestate_id <= 0) {
            if (error_out) {
                *error_out = "prepared-checkpoint mode requires a positive --source-savestate-id";
            }
            return false;
        }
        if (!options.workspace_root.has_value()
            || !IsCompleteExistingWorkspace(*options.workspace_root, error_out)) {
            return false;
        }
        if (!options.savestate_file.empty() || !options.dtm_file.empty()
            || HasAnyRtcArgument(options)) {
            if (error_out) {
                *error_out = "prepared-checkpoint mode rejects --savestate-file, --dtm-file, and all RTC arguments";
            }
            return false;
        }
    }
    if (options.source_savestate_id.has_value()
        && options.tasmovie_establishment_id.has_value()) {
        if (error_out) {
            *error_out = "cannot combine --source-savestate-id with --tas-establishment-id";
        }
        return false;
    }
    if (tas_establishment_mode) {
        if (!options.tasmovie_establishment_id.has_value()
            || *options.tasmovie_establishment_id <= 0) {
            if (error_out) {
                *error_out = "tasmovie-establishment-attempt mode requires a positive --tas-establishment-id";
            }
            return false;
        }
        if (!options.workspace_root.has_value()
            || !IsCompleteExistingWorkspace(*options.workspace_root, error_out)) {
            return false;
        }
        if (!options.savestate_file.empty()) {
            if (error_out) {
                *error_out = "tasmovie-establishment-attempt mode requires an existing --workspace-root and rejects --savestate-file";
            }
            return false;
        }
        if (!options.dtm_file.empty()) {
            if (error_out) {
                *error_out =
                    "tasmovie-establishment-attempt mode rejects --dtm-file";
            }
            return false;
        }
    }
    if (is_tasmovie_establish && (options.tasmovie_rtc.has_value()
        || options.tasmovie_rtc_min.has_value()
        || options.tasmovie_rtc_max.has_value())) {
        if (error_out) *error_out = "tasmovie_establish does not accept RTC arguments";
        return false;
    }
    if (!prepared_checkpoint_mode
        && (is_tasmovie_validation || is_tasmovie_sterile || is_battle)
        && (!options.tasmovie_rtc.has_value()
            || options.tasmovie_rtc_min.has_value()
            || options.tasmovie_rtc_max.has_value())) {
        if (error_out) {
            *error_out = "the selected validation-backed scenario requires exactly one --tasmovie-rtc and does not accept RTC range arguments";
        }
        return false;
    }
    if (!prepared_checkpoint_mode && is_tasmovie_cutscene) {
        const bool exact = options.tasmovie_rtc.has_value()
            && !options.tasmovie_rtc_min.has_value()
            && !options.tasmovie_rtc_max.has_value();
        const bool range = !options.tasmovie_rtc.has_value()
            && options.tasmovie_rtc_min.has_value()
            && options.tasmovie_rtc_max.has_value();
        if (!exact && !range) {
            if (error_out) {
                *error_out = "tasmovie_cutscene requires either one --tasmovie-rtc or both --tasmovie-rtc-min and --tasmovie-rtc-max";
            }
            return false;
        }
        if (range) {
            const auto low = static_cast<std::uint64_t>(
                *options.tasmovie_rtc_min);
            const auto high = static_cast<std::uint64_t>(
                *options.tasmovie_rtc_max);
            if (high >= low && high - low + 1 > 32) {
                if (error_out)
                    *error_out = "tasmovie_cutscene RTC fan-out may contain at most 32 branches";
                return false;
            }
        }
    }
    if (!prepared_checkpoint_mode
        && (is_tasmovie_seedprobe || is_tasmovie_sterile)
        && (!options.tasmovie_rtc.has_value()
            || options.tasmovie_rtc_min.has_value()
            || options.tasmovie_rtc_max.has_value())) {
        if (error_out) {
            *error_out = "the selected validation-backed scenario requires exactly one --tasmovie-rtc and does not accept RTC range arguments";
        }
        return false;
    }
    if (!prepared_checkpoint_mode
        && (is_tasmovie_seedprobe || is_tasmovie_sterile || is_battle
            || is_tasmovie_cutscene) &&
        !options.savestate_file.empty()) {
        if (error_out) {
            *error_out = "validation-backed Battle scenarios do not accept --savestate-file; downstream phases use the sterilized validation checkpoint";
        }
        return false;
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
    if (options.iso_path.empty() || !std::filesystem::exists(options.iso_path)) {
        if (error_out) *error_out = "--iso is required and must exist";
        return false;
    }
    if (options.dolphin_base_dir.empty() || !std::filesystem::exists(options.dolphin_base_dir)) {
        if (error_out) *error_out = "--dolphin-base-dir is required and must exist";
        return false;
    }
    if (options.poll_ms < 100) {
        if (error_out) *error_out = "--poll-ms must be >= 100";
        return false;
    }    
    if (options.worker_count < 1
        || options.worker_count
            > static_cast<std::int64_t>(
                savor::runner::parallel::savordb::kMaximumWorkerCount)) {
        if (error_out) {
            *error_out = "--worker-count must be between 1 and "
                + std::to_string(
                    savor::runner::parallel::savordb::kMaximumWorkerCount);
        }
        return false;
    }
    if (options.poll_ms > 5000) {
        if (error_out) *error_out = "--poll-ms must be <= 5000";
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
    constexpr std::int64_t kMaxGameCubeRtc =
        static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max());
    if (options.tasmovie_rtc.has_value()
        && (*options.tasmovie_rtc < 0
            || *options.tasmovie_rtc > kMaxGameCubeRtc)) {
        if (error_out) *error_out = "--tasmovie-rtc must be between 0 and 4294967295";
        return false;
    }
    if (options.diagnostic_max_runs < 1 || options.diagnostic_max_runs > 5) {
        if (error_out) *error_out = "--diagnostic-max-runs must be between 1 and 5";
        return false;
    }
    if (options.tasmovie_rtc_min.has_value()
        && (*options.tasmovie_rtc_min < 0
            || *options.tasmovie_rtc_min > kMaxGameCubeRtc)) {
        if (error_out) *error_out = "--tasmovie-rtc-min must be between 0 and 4294967295";
        return false;
    }
    if (options.tasmovie_rtc_max.has_value()
        && (*options.tasmovie_rtc_max < 0
            || *options.tasmovie_rtc_max > kMaxGameCubeRtc)) {
        if (error_out) *error_out = "--tasmovie-rtc-max must be between 0 and 4294967295";
        return false;
    }
    if (options.tasmovie_rtc_min.has_value()
        && options.tasmovie_rtc_max.has_value()
        && *options.tasmovie_rtc_min > *options.tasmovie_rtc_max) {
        if (error_out) *error_out = "--tasmovie-rtc-min must be <= --tasmovie-rtc-max";
        return false;
    }
    if (options.seedprobe_samples_per_axis.has_value() && *options.seedprobe_samples_per_axis <= 0) {
        if (error_out) *error_out = "--seedprobe-samples-per-axis must be > 0";
        return false;
    }
    const auto seedprobe_min = options.seedprobe_min_value.value_or(48);
    const auto seedprobe_max = options.seedprobe_max_value.value_or(207);
    if (seedprobe_min < 0 || seedprobe_min > 255) {
        if (error_out) *error_out = "--seedprobe-min-value must be between 0 and 255";
        return false;
    }
    if (seedprobe_max < 0 || seedprobe_max > 255) {
        if (error_out) *error_out = "--seedprobe-max-value must be between 0 and 255";
        return false;
    }
    if (seedprobe_min > seedprobe_max) {
        if (error_out) *error_out = "--seedprobe-min-value must be <= --seedprobe-max-value";
        return false;
    }
    if (options.seedprobe_combo_attempts_per_target.has_value() && *options.seedprobe_combo_attempts_per_target <= 0) {
        if (error_out) *error_out = "--seedprobe-combo-attempts-per-target must be > 0";
        return false;
    }
    if (options.seedprobe_combo_sampler_tries.has_value() && *options.seedprobe_combo_sampler_tries <= 0) {
        if (error_out) *error_out = "--seedprobe-combo-sampler-tries must be > 0";
        return false;
    }
    const auto fake_attack_min = options.battle_fake_attack_min.value_or(0);
    const auto fake_attack_max = options.battle_fake_attack_max.value_or(0);
    if (fake_attack_min < 0) {
        if (error_out) *error_out = "--battle-fake-attack-min must be >= 0";
        return false;
    }
    if (fake_attack_max < 0) {
        if (error_out) *error_out = "--battle-fake-attack-max must be >= 0";
        return false;
    }
    if (fake_attack_min > fake_attack_max) {
        if (error_out) *error_out = "--battle-fake-attack-min must be <= --battle-fake-attack-max";
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

TasMovieRtcRange ResolveTasMovieRtcRange(
    const CliOptions& options,
    std::int64_t default_value) {
    TasMovieRtcRange range{};
    if (options.tasmovie_rtc_min.has_value() || options.tasmovie_rtc_max.has_value()) {
        range.low = options.tasmovie_rtc_min.value_or(options.tasmovie_rtc_max.value_or(default_value));
        range.high = options.tasmovie_rtc_max.value_or(range.low);
    } else if (options.tasmovie_rtc.has_value()) {
        range.low = *options.tasmovie_rtc;
        range.high = *options.tasmovie_rtc;
    } else {
        range.low = default_value;
        range.high = default_value;
    }
    if (range.high < range.low) {
        range.high = range.low;
    }
    return range;
}

} // namespace savor::e2e
