#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "Cli.h"
#include "Common/DbService.h"
#include "Common/Performance/DbPerfReport.h"
#include "DbSetup.h"
#include "BattlePhasesRealWorkerScenario.h"
#include "SeedProbeRealWorkerScenario.h"
#include "ScenarioEntry.h"
#include "TasMovieRealWorkerScenario.h"

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
        << " wait_for_workers_ready="
        << (options.wait_for_workers_ready ? "true" : "false")
        << " repeat=" << options.repeat
        << " samples_per_axis=" << options.seedprobe_samples_per_axis.value_or(0)
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

    const bool reset_workspace = std::ranges::any_of(
        options.scenarios, [&](const std::string& scenario_name) {
            const auto* descriptor = FindE2eScenarioDescriptor(scenario_name);
            return descriptor != nullptr && EntrySourceRequiresFreshWorkspace(
                SelectE2eScenarioEntrySource(*descriptor, options));
        });
    if (reset_workspace) {
        std::filesystem::path reset_root;
        std::string reset_error;
        if (!ResetScenarioWorkspace(options, &reset_root, &reset_error)) {
            std::cerr << "[FAIL] resetting E2E scenario workspace - "
                      << reset_error << "\n";
            return 1;
        }
        std::cout << "[e2e-db-reset] workspace=" << reset_root.string() << "\n";
    }

    std::cout
        << "[worker-startup-policy] wait_for_workers_ready="
        << (options.wait_for_workers_ready ? 1 : 0) << "\n";

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
            const auto* descriptor = FindE2eScenarioDescriptor(scenario_name);
            if (descriptor == nullptr) {
                scenario_error = "unknown --scenario: " + scenario_name;
                std::cerr << scenario_error << "\n";
                exit_code = 2;
                break;
            }

            const bool requires_workflow_boundary = true;
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
            ResolvedE2eScenarioEntry entry{};
            scenario_error.clear();
            if (!ResolveE2eScenarioEntry(
                    *descriptor, options, &service, &entry,
                    &scenario_error)) {
                failed += 1;
                std::cerr << "[FAIL] resolving entry for '" << scenario_name
                          << "' - " << scenario_error << "\n";
                exit_code = 1;
                break;
            }
            submitted += 1;
            std::cout << "Running scenario '" << scenario_name << "' repeat=" << (repeat_index + 1)
                      << "/" << options.repeat
                      << " poll=" << options.poll_ms << "ms"
                      << " entry_source=" << ToString(entry.source);
            if (entry.savestate_id) {
                std::cout << " entry_savestate=" << *entry.savestate_id;
            }
            std::cout << "\n";

            scenario_error.clear();
            bool scenario_passed = false;
            switch (descriptor->kind) {
            case E2eScenarioKind::SeedProbe:
                scenario_passed = RunSeedProbeWorkflowGraphRealWorkerSmoke(
                    options, entry, argv[0], &service, &scenario_error);
                break;
            case E2eScenarioKind::Battle:
                scenario_passed = RunBattleWorkflowGraphRealWorkerScenario(
                    options, entry, argv[0], &service, &scenario_error);
                break;
            case E2eScenarioKind::TasMovie:
                scenario_passed = RunTasMovieRealWorkerSmoke(
                    options, entry, argv[0], &service, &scenario_error);
                break;
            case E2eScenarioKind::TasMovieWithValidation:
                scenario_passed = RunTasMovieWithValidationRealWorkerSmoke(
                    options, entry, argv[0], &service, &scenario_error);
                break;
            case E2eScenarioKind::TasMovieSeedProbe:
                scenario_passed = RunTasMovieSeedProbeRealWorkerSmoke(
                    options, entry, argv[0], &service, &scenario_error);
                break;
            }
            if (scenario_passed && !RequalifyPreparedScenarioEntry(
                    entry, &service, &scenario_error)) {
                scenario_passed = false;
            }
            if (!scenario_passed) {
                std::cerr << "[FAIL] " << scenario_name << " - " << scenario_error << "\n";
            }
            std::cout << "[scenario-result] scenario=" << scenario_name
                      << " execution="
                      << (scenario_passed ? "SUCCEEDED" : "FAILED")
                      << " invariants="
                      << (scenario_passed ? "PASS" : "FAIL")
                      << " trajectory=HUMAN_REVIEW_REQUIRED"
                      << '\n';

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
        savor::db::perf::WriteSummaryJson(*options.perf_report_dir, report);
        savor::db::perf::WriteMarkdownReport(*options.perf_report_dir, report);
        std::cout << "Perf report: " << (*options.perf_report_dir / "perf-report.md").string() << "\n"
                  << "Perf summary: " << (*options.perf_report_dir / "perf-summary.json").string() << "\n";
    }

    service.Stop();
    return exit_code;
}
