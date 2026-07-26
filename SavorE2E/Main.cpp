#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <thread>

#include "Cli.h"
#include "BattleMacroProbeScenario.h"
#include "BattleEndResultsScenario.h"
#include "BattleSingleTurnScenario.h"
#include "Common/DbService.h"
#include "Common/Performance/DbPerfReport.h"
#include "DbSetup.h"
#include "NavigationContextScenario.h"
#include "SeedProbeRealWorkerScenario.h"
#include "TasMovieRealWorkerScenario.h"
#include "Worker/WorkerCapabilityPreflight.h"
#include "WorkerCoordinatorPerf.h"

namespace {

using Clock = std::chrono::steady_clock;

std::string JoinScenarios(const std::vector<std::string>& scenarios) {
    std::ostringstream out;
    for (std::size_t i = 0; i < scenarios.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << scenarios[i];
    }
    return out.str();
}

std::string E2EPerfConfiguration(const savor::e2e::CliOptions& options) {
    const auto rtc_range = savor::e2e::ResolveTasMovieRtcRange(options, 0);
    std::ostringstream out;
    out << "workers=" << options.worker_count
        << " repeat=" << options.repeat
        << " samples_per_axis=" << options.seedprobe_samples_per_axis.value_or(0)
        << " fake_attack_min=" << options.battle_fake_attack_low.value_or(0)
        << " fake_attack_max=" << options.battle_fake_attack_high.value_or(0)
        << " rtc_min=" << rtc_range.low
        << " rtc_max=" << rtc_range.high;
    return out.str();
}

} // namespace

int main(int argc, char** argv) {
    using namespace savor::e2e;
    using namespace savor::db::core;
    using namespace savor::db::migrations;

    CliOptions options{};
    std::string parse_error;
    if (!ParseArgs(argc, argv, &options, &parse_error)) {
        std::cerr << parse_error << "\n\n";
        PrintUsage();
        return 2;
    }

    const std::map<std::string, bool (*)(const CliOptions&, const char*, DBService*, std::string*)> scenarios{
        { "seedprobe", &RunSeedProbeWorkflowGraphRealWorkerSmoke },
        { "tasmovie", &RunTasMovieRealWorkerSmoke },
        { "tasmovie_seedprobe", &RunTasMovieSeedProbeRealWorkerSmoke },
        { "seedprobe_battle", &RunSeedProbeBattleRealWorkerScenario },
        { "battle", &RunBattleWorkflowGraphRealWorkerScenario },
        { "battle_macro_probe", &RunBattleMacroProbeScenario },
        { "battle_end", &RunBattleEndResultsScenario },
        { "battle_end_results", &RunBattleEndResultsScenario },
        { "navigation_context", &RunNavigationContextScenario },
        { "tasmovie_seedprobe_battle", &RunTasMovieSeedProbeBattleWorkflowGraphRealWorkerScenario },
        { "tasmovie_seedprobe_battle_override", &RunTasMovieSeedProbeBattleOverrideWorkflowGraphRealWorkerScenario },
        { "tasmovie_battle", &RunTasMovieBattleWorkflowGraphRealWorkerScenario },
    };

    const auto worker_preflight = savor::RunWorkerCapabilityPreflight(
        savor::WorkerCapabilityPreflightRequest{
            .worker_exe_path = ResolveWorkerExePath(argv[0]).string(),
            .log_directory = options.worker_dir_root
                ? (*options.worker_dir_root / "capability-preflight").string()
                : std::string{},
            .worker_id = 0,
            .timeout_ms = static_cast<std::uint32_t>(
                std::clamp<std::int64_t>(
                    options.timeout_ms,
                    1,
                    std::numeric_limits<std::uint32_t>::max())),
            .required_capabilities = savor::runtime::CapabilityMask(
                savor::runtime::WorkerCapability::ProgramInvocation),
        });
    if (!worker_preflight) {
        std::cerr << "[FAIL] " << worker_preflight.message << "\n";
        return 1;
    }

    const auto migration_root = ResolveMigrationRoot(options.migration_root);
    const auto db_paths = BuildDbPaths(options);
    DBService service(
        db_paths,
        MigrationSourceOptions{
            .source_kind = MigrationSourceKind::Filesystem,
            .filesystem_root = migration_root,
        });

    std::string db_error;
    if (!service.Start(&db_error)) {
        std::cerr << "[FAIL] starting DBService - " << db_error << "\n";
        return 1;
    }

    const bool perf_mode = options.perf_report_dir.has_value();
    WorkerCoordinatorPerfAccumulator worker_coordinator_perf;
    if (perf_mode) {
        options.worker_coordinator_perf = &worker_coordinator_perf;
    }
    std::ofstream snapshots;
    std::atomic<bool> stop_sampling{ false };
    const auto started_at = Clock::now();
    std::thread sampler;
    if (perf_mode) {
        std::filesystem::create_directories(*options.perf_report_dir);
        snapshots.open(*options.perf_report_dir / "perf-snapshots.jsonl", std::ios::binary);
        sampler = std::thread([&]() {
            while (!stop_sampling.load()) {
                const auto elapsed_ms = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at).count());
                savor::db::perf::WriteSnapshotLine(
                    snapshots,
                    "e2e-replay",
                    elapsed_ms,
                    service.SnapshotPerformance());
                std::this_thread::sleep_for(std::chrono::milliseconds{ options.perf_snapshot_interval_ms });
            }
        });
    }

    std::string scenario_error;
    std::int64_t submitted = 0;
    std::int64_t completed = 0;
    std::int64_t failed = 0;
    int exit_code = 0;
    for (int repeat_index = 0; repeat_index < options.repeat && exit_code == 0; ++repeat_index) {
        for (const auto& scenario_name : options.scenarios) {
            const auto it = scenarios.find(scenario_name);
            if (it == scenarios.end()) {
                scenario_error = "unknown --scenario: " + scenario_name;
                std::cerr << scenario_error << "\n";
                exit_code = 2;
                break;
            }

            const bool requires_workflow_boundary = scenario_name != "battle_macro_probe";
            if (requires_workflow_boundary) {
                std::string boundary_diagnostics;
                if (!CheckWorkflowQuiescence(service.ExecutionDb(), &boundary_diagnostics)) {
                    scenario_error =
                        "pre-scenario workflow boundary failed for '" + scenario_name
                        + "': " + boundary_diagnostics;
                    failed += 1;
                    std::cerr << "[FAIL] " << scenario_error << "\n";
                    exit_code = 1;
                    break;
                }
                std::cout
                    << "[PASS] pre-scenario workflow boundary '" << scenario_name
                    << "' repeat=" << (repeat_index + 1) << " is quiescent\n";
            }

            options.scenario = scenario_name;
            submitted += 1;
            std::cout << "Running scenario '" << scenario_name << "' repeat=" << (repeat_index + 1)
                      << "/" << options.repeat
                      << " timeout=" << options.timeout_ms
                      << "ms poll=" << options.poll_ms << "ms\n";

            scenario_error.clear();
            const bool scenario_passed =
                it->second(options, argv[0], &service, &scenario_error);
            if (!scenario_passed) {
                std::cerr << "[FAIL] " << scenario_name << " - " << scenario_error << "\n";
            }

            bool post_boundary_passed = true;
            std::string post_boundary_diagnostics;
            if (requires_workflow_boundary) {
                post_boundary_passed =
                    CheckWorkflowQuiescence(service.ExecutionDb(), &post_boundary_diagnostics);
                if (post_boundary_passed) {
                    std::cout
                        << "[PASS] post-scenario workflow boundary '" << scenario_name
                        << "' repeat=" << (repeat_index + 1) << " is quiescent\n";
                } else {
                    std::cerr
                        << "[FAIL] post-scenario workflow boundary for '" << scenario_name
                        << "' repeat=" << (repeat_index + 1) << " - "
                        << post_boundary_diagnostics << "\n";
                }
            }

            if (!scenario_passed || !post_boundary_passed) {
                failed += 1;
                if (!post_boundary_passed) {
                    if (!scenario_error.empty()) {
                        scenario_error += "; ";
                    }
                    scenario_error +=
                        "post-scenario workflow boundary failed: "
                        + post_boundary_diagnostics;
                }
                exit_code = 1;
                break;
            }
            completed += 1;
            std::cout << "[PASS] " << scenario_name << "\n";
        }
    }

    savor::db::perf::DrainUiReadProjection(service, std::chrono::seconds{ 5 }, &db_error);
    if (perf_mode) {
        stop_sampling = true;
        if (sampler.joinable()) {
            sampler.join();
        }
        const auto elapsed_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at).count());
        const auto snapshot = service.SnapshotPerformance();
        savor::db::perf::WriteSnapshotLine(snapshots, "e2e-replay", elapsed_ms, snapshot);
        snapshots.flush();

        savor::db::perf::PerfRunReport report{};
        report.scenario = "e2e-replay";
        report.measured_workload = "Release real-worker SavorE2E scenario replay: " + JoinScenarios(options.scenarios);
        report.first_error = scenario_error;
        report.load_level = options.load_level;
        report.configuration = E2EPerfConfiguration(options);
        report.databases = snapshot.databases;
        report.projection = snapshot.ui_read_projection;
        report.submitted = submitted;
        report.completed = completed;
        report.failed = failed + (exit_code == 2 ? 1 : 0);
        report.elapsed_ms = elapsed_ms;
        report.worker_count = static_cast<int>(options.worker_count);
        report.repeat_count = options.repeat;
        report.worker_coordinator = worker_coordinator_perf.BuildSummary();
        savor::db::perf::WriteSummaryJson(*options.perf_report_dir, report);
        savor::db::perf::WriteMarkdownReport(*options.perf_report_dir, report);
        std::cout << "Perf report: " << (*options.perf_report_dir / "perf-report.md").string() << "\n"
                  << "Perf summary: " << (*options.perf_report_dir / "perf-summary.json").string() << "\n";
    }

    service.Stop();
    return exit_code;
}
