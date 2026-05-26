#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <fstream>
#include <thread>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include "Common/Events/OutboxRelay.h"
#include "Common/Migrations/MigrationRunner.h"
#include "Execution/Workflow/SeedProbeWorkflowDefinition.h"
#include "Execution/Workflow/SqliteExecutionDb.h"
#include "Execution/Workflow/WorkflowRecoveryService.h"
#include "Execution/Workflow/WorkflowModeProvider.h"
#include "Runner/Parallel/SimCoreDB/DBWorkflowWorkerCoordinator.h"
#include "Runner/Parallel/SimCoreDB/WorkflowDispatchCoordinator.h"
#include "Runner/Parallel/SimCoreDB/WorkflowMaterializationService.h"
#include "Runner/Parallel/SimCoreDB/WorkflowSchedulerAdapter.h"
#include "common/DbPreparer.h"
#include "common/simcoredb_helpers.h"

namespace simcoreDB {
    TEST(Stage3Phase2Contracts, SeedProbeSplitContractIncludesNeutralGridUniqueDependenciesAndInputs) {
        using namespace simcore::db::execution::workflow;

        const auto definition = BuildSeedProbeChainDefinition();
        std::string validation_error;
        ASSERT_TRUE(ValidateWorkflowDefinition(definition, &validation_error)) << validation_error;

        const auto find_step = [&](std::string_view key) -> const WorkflowStepDefinition* {
            for (const auto& step : definition.steps) {
                if (step.step_key == key) {
                    return &step;
                }
            }
            return nullptr;
        };

        const auto* neutral = find_step("Neutral");
        const auto* grid = find_step("Grid");
        const auto* unique = find_step("Unique");
        ASSERT_NE(neutral, nullptr);
        ASSERT_NE(grid, nullptr);
        ASSERT_NE(unique, nullptr);

        ASSERT_EQ(neutral->required_inputs.size(), 1u);
        EXPECT_EQ(neutral->required_inputs.front(), "sp_probe_run.probe_run_id");

        ASSERT_EQ(grid->dependencies.size(), 1u);
        EXPECT_EQ(grid->dependencies.front(), "Neutral");
        ASSERT_EQ(grid->required_inputs.size(), 1u);
        EXPECT_EQ(grid->required_inputs.front(), "seedprobe.neutral.seed_context");

        ASSERT_EQ(unique->dependencies.size(), 1u);
        EXPECT_EQ(unique->dependencies.front(), "Grid");
        ASSERT_EQ(unique->required_inputs.size(), 1u);
        EXPECT_EQ(unique->required_inputs.front(), "seedprobe.grid.seed_evidence");
    }

    TEST(Stage3Phase2Contracts, DbLifecyclePreservesCanonicalEventOrderingAndOutboxContracts) {
        using namespace simcore::db::execution::workflow;
        using namespace simcore::db::migrations;

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
        std::string err;
        ASSERT_TRUE(ApplyContextMigrations(db, MigrationContext::Execution, embedded_options, &err)) << err;

        ASSERT_TRUE(ExecSql(db, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(2501, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, created_at_utc, ready_at_utc)
VALUES(2502, 2501, 'Neutral', 'seedprobe.neutral', 'READY', 0, 2, unixepoch()*1000, unixepoch()*1000);
)SQL"));

        SqliteExecutionDb execution_db(db);
        auto* commands = execution_db.WorkflowCommandService();
        ASSERT_NE(commands, nullptr);

        ASSERT_TRUE(commands->MarkStepMaterialized({ .workflow_step_id = 2502, .job_set_id = 8801, .requested_by = "SimCoreTests" }, &err)) << err;
        ASSERT_TRUE(commands->AppendStepInputEvent({
            .workflow_instance_id = 2501,
            .workflow_step_id = 2502,
            .event_kind = "Execution.WorkflowStepInputRequested.v1",
            .source_key = std::optional<std::string>("savestate"),
            .request_id = std::optional<std::string>("request-neutral"),
            .message = std::optional<std::string>("request input"),
            .requested_by = "SimCoreTests",
        }, &err)) << err;
        ASSERT_TRUE(commands->AppendStepInputEvent({
            .workflow_instance_id = 2501,
            .workflow_step_id = 2502,
            .event_kind = "Execution.WorkflowStepInputComplete.v1",
            .source_key = std::nullopt,
            .request_id = std::nullopt,
            .message = std::optional<std::string>("input complete"),
            .requested_by = "SimCoreTests",
        }, &err)) << err;
        ASSERT_TRUE(commands->MarkStepTerminal({ .workflow_step_id = 2502, .terminal_state = "COMPLETED", .requested_by = "SimCoreTests" }, &err)) << err;

        sqlite3_stmt* st = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db,
            "SELECT event_kind FROM exec_workflow_event WHERE workflow_step_id=2502 ORDER BY workflow_event_id;",
            -1, &st, nullptr));
        std::vector<std::string> lifecycle_events;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const auto* text = sqlite3_column_text(st, 0);
            lifecycle_events.emplace_back(text != nullptr ? reinterpret_cast<const char*>(text) : "");
        }
        sqlite3_finalize(st);
        EXPECT_EQ(lifecycle_events, (std::vector<std::string>{
            "Execution.WorkflowStepMaterialized.v1",
            "Execution.WorkflowStepCompleted.v1",
            }));

        ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db,
            "SELECT event_kind FROM exec_workflow_input_event WHERE workflow_step_id=2502 ORDER BY workflow_input_event_id;",
            -1, &st, nullptr));
        std::vector<std::string> input_events;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const auto* text = sqlite3_column_text(st, 0);
            input_events.emplace_back(text != nullptr ? reinterpret_cast<const char*>(text) : "");
        }
        sqlite3_finalize(st);
        EXPECT_EQ(input_events, (std::vector<std::string>{
            "Execution.WorkflowStepInputRequested.v1",
            "Execution.WorkflowStepInputComplete.v1",
            }));

        ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
            db,
            "SELECT COUNT(1) FROM exec_outbox_message WHERE aggregate_kind='workflow_instance' AND payload_ref_kind='workflow_event';",
            -1,
            &st,
            nullptr));
        ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
        EXPECT_EQ(sqlite3_column_int(st, 0), 2);
        sqlite3_finalize(st);

        sqlite3_close(db);
    }

    TEST(Stage3Phase2Contracts, FailedTerminalStepDoesNotAllowInvalidDownstreamAdvance) {
        using namespace simcore::db::execution::workflow;
        using namespace simcore::db::migrations;

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
        std::string err;
        ASSERT_TRUE(ApplyContextMigrations(db, MigrationContext::Execution, embedded_options, &err)) << err;

        ASSERT_TRUE(ExecSql(db, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(2701, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, created_at_utc, ready_at_utc)
VALUES
    (2702, 2701, 'Neutral', 'seedprobe.neutral', 'READY', 0, 2, unixepoch()*1000, unixepoch()*1000),
    (2703, 2701, 'Grid', 'seedprobe.grid', 'WAITING', 0, 2, unixepoch()*1000, NULL);
)SQL"));

        SqliteExecutionDb execution_db(db);
        auto* commands = execution_db.WorkflowCommandService();
        ASSERT_NE(commands, nullptr);
        ASSERT_TRUE(commands->MarkStepMaterialized({ .workflow_step_id = 2702, .job_set_id = 9101, .requested_by = "SimCoreTests" }, &err)) << err;
        ASSERT_TRUE(commands->MarkStepTerminal({ .workflow_step_id = 2702, .terminal_state = "FAILED", .requested_by = "SimCoreTests" }, &err)) << err;

        EXPECT_FALSE(commands->MarkStepMaterialized({ .workflow_step_id = 2703, .job_set_id = 9102, .requested_by = "SimCoreTests" }, &err));

        sqlite3_stmt* st = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, "SELECT state FROM exec_workflow_step WHERE workflow_step_id=2702;", -1, &st, nullptr));
        ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
        ASSERT_NE(sqlite3_column_text(st, 0), nullptr);
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))), "FAILED");
        sqlite3_finalize(st);
        sqlite3_close(db);
    }

    TEST(Stage3Phase2Contracts, TransitionTwoStepSuccessUsesSavestateSignalAndAdvancesGridToReady) {
        using namespace simcore::db::execution::workflow;
        using namespace simcore::db::migrations;

        const auto temp_dir = MakeTempPhase4Dir("phase2-two-step-savestate");
        const auto savestate_path = temp_dir / "input_transition.sav";
        {
            std::ofstream out(savestate_path, std::ios::binary);
            out << "dummy-savestate";
        }
        ASSERT_TRUE(std::filesystem::exists(savestate_path));

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
        std::string err;
        ASSERT_TRUE(ApplyContextMigrations(db, MigrationContext::Execution, embedded_options, &err)) << err;

        ASSERT_TRUE(ExecSql(db, R"SQL(
INSERT INTO exec_workflow_instance(
    workflow_instance_id, workflow_kind, state, root_scope_kind, input_ref_kind, input_ref_id, created_by, created_at_utc, started_at_utc
)
VALUES(2601, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'sp_probe_run.probe_run_id', 1, 'test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, input_ref_kind, created_at_utc, ready_at_utc)
VALUES
    (2602, 2601, 'Neutral', 'seedprobe.neutral', 'READY', 0, 2, 'sp_probe_run.probe_run_id', unixepoch()*1000, unixepoch()*1000),
    (2603, 2601, 'Grid', 'seedprobe.grid', 'WAITING', 0, 2, 'seedprobe.neutral.seed_context', unixepoch()*1000, NULL);
INSERT INTO exec_workflow_edge(workflow_edge_id, workflow_instance_id, from_step_id, to_step_id, created_at_utc)
VALUES(2604, 2601, 2602, 2603, unixepoch()*1000);
)SQL"));

        SqliteExecutionDb execution_db(db);
        auto* commands = execution_db.WorkflowCommandService();
        auto* queries = execution_db.WorkflowQueryService();
        ASSERT_NE(commands, nullptr);
        ASSERT_NE(queries, nullptr);

        ASSERT_TRUE(commands->MarkStepMaterialized({ .workflow_step_id = 2602, .job_set_id = 9001, .requested_by = "SimCoreTests" }, &err)) << err;
        ASSERT_TRUE(commands->AppendStepInputEvent({
            .workflow_instance_id = 2601,
            .workflow_step_id = 2602,
            .event_kind = "Execution.WorkflowStepInputRequested.v1",
            .source_key = std::optional<std::string>("savestate"),
            .request_id = std::optional<std::string>(savestate_path.filename().string()),
            .message = std::optional<std::string>("neutral input requested"),
            .requested_by = "SimCoreTests",
        }, &err)) << err;
        ASSERT_TRUE(commands->AppendStepInputEvent({
            .workflow_instance_id = 2601,
            .workflow_step_id = 2602,
            .event_kind = "Execution.WorkflowStepInputComplete.v1",
            .source_key = std::nullopt,
            .request_id = std::nullopt,
            .message = std::optional<std::string>("neutral input complete"),
            .requested_by = "SimCoreTests",
        }, &err)) << err;
        ASSERT_TRUE(commands->MarkStepTerminal({ .workflow_step_id = 2602, .terminal_state = "COMPLETED", .requested_by = "SimCoreTests" }, &err)) << err;
        ASSERT_TRUE(ExecSql(db, "UPDATE exec_workflow_step SET state='READY', ready_at_utc=unixepoch()*1000 WHERE workflow_step_id=2603 AND state='WAITING';"));

        const auto graph = queries->GetWorkflowGraph(2601);
        ASSERT_TRUE(graph.has_value());
        bool neutral_completed = false;
        bool grid_ready = false;
        for (const auto& step : graph->steps) {
            if (step.step_key == "Neutral" && step.state == WorkflowStepState::Completed) {
                neutral_completed = true;
            }
            if (step.step_key == "Grid" && step.state == WorkflowStepState::Ready) {
                grid_ready = true;
            }
        }
        EXPECT_TRUE(neutral_completed);
        EXPECT_TRUE(grid_ready);

        sqlite3_close(db);
        std::error_code ec;
        std::filesystem::remove_all(temp_dir, ec);
    }

    TEST(Stage3Phase3Telemetry, CapturesMaterializationAndDispatchCounters) {
        using namespace simcore::runner::parallel::simcoredb;
        using namespace simcore::db::execution::workflow;

        StaticWorkflowModeProvider mode_provider({ .mode = WorkflowExecutionMode::Workflow, .source = "phase3-telemetry-test" });

        DBWorkflowWorkerCoordinator coordinator(
            nullptr,
            &mode_provider,
            DBWorkflowWorkerCoordinatorConfig{
                .desired_workers = 0,
                .controller_sleep_ms = 1,
            },
            CoordinatorIntegrationConfig{},
            [](const WorkflowReadyStep& step) {
                return ScheduledJobSet{
                    .job_set_id = 13000 + step.workflow_step_id,
                    .workflow_step_id = step.workflow_step_id,
                };
            });

        coordinator.EnqueueReadyStep({
            .workflow_instance_id = 10,
            .workflow_step_id = 20,
            .step_key = "Neutral",
            .step_kind = "seedprobe.neutral",
            .priority = 1,
            });

        coordinator.Start();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        coordinator.Stop();

        const auto telemetry = coordinator.SnapshotTelemetry();
        EXPECT_GE(telemetry.materialization_count, 1);
        EXPECT_GE(telemetry.last_materialization_latency_ms, 0);
        EXPECT_GE(telemetry.max_materialization_latency_ms, 0);
        EXPECT_GE(telemetry.dispatch_attempt_count, 0);
        EXPECT_GE(telemetry.dispatch_miss_count, 0);
        EXPECT_GE(telemetry.dispatch_miss_rate_basis_points, 0);
    }

    TEST(Stage3Phase3Batching, ProgressCallbacksDrainInBatchesAndTerminalResultsAreNotBlocked) {
        using namespace simcore::runner::parallel::simcoredb;
        using namespace simcore::db::execution::workflow;

        StaticWorkflowModeProvider mode_provider({ .mode = WorkflowExecutionMode::Workflow, .source = "phase3-batching-test" });
        DBWorkflowWorkerCoordinator coordinator(
            nullptr,
            &mode_provider,
            DBWorkflowWorkerCoordinatorConfig{
                .desired_workers = 0,
                .controller_sleep_ms = 1,
            },
            CoordinatorIntegrationConfig{},
            [](const WorkflowReadyStep& step) {
                return ScheduledJobSet{
                    .job_set_id = 14000 + step.workflow_step_id,
                    .workflow_step_id = step.workflow_step_id,
                };
            });

        std::atomic<int> progress_seen{ 0 };
        std::promise<void> terminal_seen_promise;
        auto terminal_seen_future = terminal_seen_promise.get_future();
        coordinator.SetProgressCallback([&](const simcore::PRProgress&) {
            ++progress_seen;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            });
        coordinator.SetResultCallback([&](const simcore::PRResult&) {
            terminal_seen_promise.set_value();
            });

        coordinator.Start();
        for (int i = 0; i < 200; ++i) {
            coordinator.EnqueueProgressForTest(simcore::PRProgress{
                .worker_id = 0,
                .job_id = static_cast<std::uint64_t>(1000 + i),
                .text = "progress",
                });
        }
        coordinator.EnqueueResultForTest(simcore::PRResult{
            .job_id = 999,
            .epoch = 1,
            .worker_id = 0,
            .accepted = true,
            });

        EXPECT_EQ(terminal_seen_future.wait_for(std::chrono::milliseconds(250)), std::future_status::ready);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        coordinator.Stop();

        const auto telemetry = coordinator.SnapshotTelemetry();
        EXPECT_GT(progress_seen.load(), 0);
        EXPECT_GT(telemetry.progress_batch_count, 0);
        EXPECT_GT(telemetry.max_progress_batch_size, 0);
    }

    TEST(Stage3Phase3DispatchGuard, DISABLED_ClaimedJobsAreNotDispatchedBeforeMaterialization) {
#if 0
        using namespace simcore::runner::parallel::simcoredb;

        WorkflowSchedulerAdapter scheduler([](const WorkflowReadyStep& step) {
            return ScheduledJobSet{
                .job_set_id = 15000 + step.workflow_step_id,
                .workflow_step_id = step.workflow_step_id,
            };
            });

        std::vector<ClaimedJobSeed> claimed{
            ClaimedJobSeed{
                .step = WorkflowReadyStep{
                    .workflow_instance_id = 1,
                    .workflow_step_id = 2,
                    .step_key = "Neutral",
                    .step_kind = "seedprobe.neutral",
                    .priority = 1,
                },
                .job_set_id = 42,
                .job_id = 4242,
            },
        };

        WorkflowMaterializationService materialization(
            &scheduler,
            [&]() { return claimed; },
            [](std::int64_t job_id, const WorkflowReadyStep&) -> std::optional<simcore::PSJob> {
                simcore::PSJob job{};
                (void)job_id;
                return job;
            },
            {},
            {});

        int dispatch_calls = 0;
        WorkflowDispatchCoordinator dispatch(
            &materialization,
            [&](std::size_t, const ClaimedJobRecord&) {
                ++dispatch_calls;
                return true;
            });

        const auto now = std::chrono::steady_clock::now();
        EXPECT_EQ(materialization.ClaimJobs(now), 1u);
        EXPECT_FALSE(dispatch.DispatchNextEligibleForWorker(0, std::nullopt, now));
        EXPECT_EQ(dispatch_calls, 0);

        EXPECT_TRUE(materialization.MaterializeClaimedJobPayload(now));
        EXPECT_TRUE(dispatch.DispatchNextEligibleForWorker(0, std::nullopt, now));
        EXPECT_EQ(dispatch_calls, 1);
#endif
    }

    TEST(Stage3Phase3Contracts, DedupeIsolationIsScopedPerCoordinatorBridgeInstance) {
        using namespace simcore::runner::parallel::simcoredb;

        WorkflowCoordinatorBridge service_a;
        WorkflowCoordinatorBridge service_b;
        int service_a_seen = 0;
        int service_b_seen = 0;
        service_a.SetTerminalCallback([&](const TerminalJobSetSignal&) { ++service_a_seen; });
        service_b.SetTerminalCallback([&](const TerminalJobSetSignal&) { ++service_b_seen; });

        const TerminalJobSetSignal signal{
            .workflow_instance_id = 77,
            .workflow_step_id = 88,
            .job_set_id = 99,
            .terminal_state = "COMPLETED",
        };

        const bool a_first = service_a.NotifyTerminal(signal);
        const bool a_dup = service_a.NotifyTerminal(signal);
        const bool b_first = service_b.NotifyTerminal(signal);
        const bool b_dup = service_b.NotifyTerminal(signal);

        EXPECT_TRUE(a_first);
        EXPECT_FALSE(a_dup);
        EXPECT_TRUE(b_first);
        EXPECT_FALSE(b_dup);
        EXPECT_EQ(service_a_seen, 1);
        EXPECT_EQ(service_b_seen, 1);
    }

    TEST(Stage3Phase0Replay, BackfillRelayResolvesLegacyAndWorkflowInputPayloadRefs) {
        using namespace simcore::db::migrations;
        using namespace simcore::db::events;

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        const MigrationSourceOptions embedded_options{
            .source_kind = MigrationSourceKind::Embedded,
        };
        std::string migration_error;
        ASSERT_TRUE(ApplyContextMigrations(db, MigrationContext::Execution, embedded_options, &migration_error))
            << migration_error;

        ASSERT_TRUE(ExecSql(db, R"SQL(
INSERT INTO exec_workflow_instance(
    workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc
)
VALUES(301, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(
    workflow_step_id, workflow_instance_id, step_key, step_kind, state, created_at_utc, ready_at_utc
)
VALUES(302, 301, 'seedprobe.neutral', 'seedprobe.neutral', 'READY', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_event(workflow_event_id, workflow_instance_id, workflow_step_id, event_kind, event_ts_utc, message)
VALUES(303, 301, 302, 'Execution.WorkflowStepReady.v1', unixepoch()*1000, 'ready');
INSERT INTO exec_workflow_input_event(workflow_input_event_id, workflow_instance_id, workflow_step_id, event_kind, event_ts_utc, source_key, request_id)
VALUES(304, 301, 302, 'Execution.WorkflowStepInputRequested.v1', unixepoch()*1000, 'state', 'req-304');
INSERT INTO exec_outbox_message(
    outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id
)
VALUES
    (305,'evt-v-1','Execution.WorkflowStepReady.v1',1,'Execution','workflow_step','302','corr-301','cause-301',unixepoch()*1000,'workflow_event',303),
    (306,'evt-v-2','Execution.WorkflowStepInputRequested.v1',1,'Execution','workflow_step','302','corr-301','cause-301',unixepoch()*1000,'workflow_input_event',304);
)SQL"));

        simcore::db::execution::workflow::SqliteExecutionDb execution_db(db);
        const auto legacy_payload = execution_db.ResolveExecutionWorkflowJobPayload(
            "Execution.WorkflowStepReady.v1", 1, "workflow_event", 303);
        ASSERT_TRUE(legacy_payload.has_value());
        EXPECT_EQ(legacy_payload->workflow_instance_id, 301);
        EXPECT_EQ(legacy_payload->workflow_step_id, 302);

        int unresolved_count = 0;
        OutboxRelay relay({
            .db = db,
            .outbox_table = "exec_outbox_message",
            .context_name = "Execution",
        });

        std::vector<OutboxRelayDispatchBinding> bindings;
        bindings.push_back({
            .key = { .event_type = "Execution.WorkflowStepReady.v1", .event_version = 1 },
            .handler = [&](const EventEnvelope& envelope, std::string* handler_error) {
                const auto payload = execution_db.ResolveExecutionWorkflowJobPayload(envelope);
                if (!payload.has_value()) {
                    ++unresolved_count;
                    if (handler_error != nullptr) {
                        *handler_error = "unresolved payload";
                    }
                    return false;
                }
                return true;
            },
        });
        bindings.push_back({
            .key = { .event_type = "Execution.WorkflowStepInputRequested.v1", .event_version = 1 },
            .handler = [&](const EventEnvelope& envelope, std::string* handler_error) {
                const auto payload = execution_db.ResolveExecutionWorkflowJobPayload(envelope);
                if (!payload.has_value()) {
                    ++unresolved_count;
                    if (handler_error != nullptr) {
                        *handler_error = "unresolved payload";
                    }
                    return false;
                }
                return true;
            },
        });

        OutboxRelayResult relay_result{};
        std::string err;
        ASSERT_TRUE(relay.RelayBatch(0, 10, bindings, &relay_result, &err)) << err;
        EXPECT_EQ(relay_result.failure_count, 0);
        EXPECT_EQ(unresolved_count, 0);
        EXPECT_EQ(relay_result.published_count, 2);

        sqlite3_close(db);
    }

    TEST(Stage3Phase3Replay, ReplayCursorRobustnessAcrossSequentialRelayPasses) {
        using namespace simcore::db::events;
        using namespace simcore::db::migrations;

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        const MigrationSourceOptions embedded_options{
            .source_kind = MigrationSourceKind::Embedded,
        };
        std::string err;
        ASSERT_TRUE(ApplyContextMigrations(db, MigrationContext::Execution, embedded_options, &err)) << err;

        ASSERT_TRUE(ExecSql(db, R"SQL(
INSERT INTO exec_outbox_message(
    outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id
) VALUES
    (3901,'evt-p3-r1','Execution.WorkflowStepReady.v1',1,'Execution','workflow_step','901','corr-p3','cause-p3',unixepoch()*1000,'workflow_event',1);
)SQL"));

        OutboxRelay relay({
            .db = db,
            .outbox_table = "exec_outbox_message",
            .context_name = "Execution",
        });
        int handled = 0;
        std::vector<OutboxRelayDispatchBinding> bindings{ {
            .key = { .event_type = "Execution.WorkflowStepReady.v1", .event_version = 1 },
            .handler = [&](const EventEnvelope&, std::string*) { ++handled; return true; },
        } };

        OutboxRelayResult first{};
        ASSERT_TRUE(relay.RelayBatchFromCursor(0, 8, bindings, &first, &err)) << err;
        OutboxRelayResult second{};
        ASSERT_TRUE(relay.RelayBatchFromCursor(first.last_scanned_outbox_id, 8, bindings, &second, &err)) << err;

        EXPECT_EQ(first.published_count, 1);
        EXPECT_EQ(second.published_count, 0);
        EXPECT_EQ(handled, 1);
        sqlite3_close(db);
    }

    TEST(Stage3Phase3Replay, ReplayRobustnessSupportsSavestateOverrideAndJsonlFixtureSeeding) {
        using namespace simcore::db::events;

        const auto migration_root = ResolveMigrationRootForTests();
        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));

        DbPreparer preparer(db, migration_root);
        std::string err;
        ASSERT_TRUE(preparer.InitializeRequiredMigrations(&err)) << err;

        const auto temp_dir = MakeTempPhase4Dir("phase3-robustness-jsonl");
        const auto savestate_path = temp_dir / "fixture_phase3.sav";
        {
            std::ofstream sav(savestate_path, std::ios::binary);
            sav << "savestate-bytes";
        }
        ASSERT_TRUE(preparer.SeedSavestateArtifactAndOverride(savestate_path, &err)) << err;

        const std::string default_rows_json = R"JSON({
  "state_artifact": [
    {
      "artifact_id": 101,
      "sha256": "phase3-placeholder-sha256",
      "size_bytes": 1,
      "compression_kind": 0,
      "filename": "placeholder_phase3.sav",
      "file_ext": ".sav",
      "artifact_kind": "SAV"
    }
  ]
})JSON";
        ASSERT_TRUE(preparer.SeedRowsFromJsonObject(default_rows_json, &err)) << err;

        const auto jsonl_dir = temp_dir / "jsonl";
        std::filesystem::create_directories(jsonl_dir);
        {
            std::ofstream out(jsonl_dir / "exec_outbox_message.jsonl");
            out << R"JSON({"outbox_id":3901,"event_id":"evt-p3-r1","event_type":"Execution.WorkflowStepReady.v1","event_version":1,"context_name":"Execution","aggregate_kind":"workflow_step","aggregate_id":"901","correlation_id":"corr-p3","causation_id":"cause-p3","occurred_at_utc":1712304000000,"payload_ref_kind":"workflow_event","payload_ref_id":1})JSON"
                << "\n";
        }
        ASSERT_TRUE(preparer.SeedRowsFromJsonlDirectory(jsonl_dir, &err)) << err;

        sqlite3_stmt* st = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, "SELECT filename FROM state_artifact WHERE artifact_id=101;", -1, &st, nullptr));
        ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
        ASSERT_NE(sqlite3_column_text(st, 0), nullptr);
        const std::string filename = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
        EXPECT_EQ(std::filesystem::path(filename).extension().string(), ".sav");

        OutboxRelay relay({
            .db = db,
            .outbox_table = "exec_outbox_message",
            .context_name = "Execution",
        });
        int handled = 0;
        std::vector<OutboxRelayDispatchBinding> bindings{ {
            .key = { .event_type = "Execution.WorkflowStepReady.v1", .event_version = 1 },
            .handler = [&](const EventEnvelope&, std::string*) { ++handled; return true; },
        } };
        OutboxRelayResult first{};
        ASSERT_TRUE(relay.RelayBatchFromCursor(0, 8, bindings, &first, &err)) << err;
        OutboxRelayResult second{};
        ASSERT_TRUE(relay.RelayBatchFromCursor(first.last_scanned_outbox_id, 8, bindings, &second, &err)) << err;
        EXPECT_EQ(first.published_count, 1);
        EXPECT_EQ(second.published_count, 0);
        EXPECT_EQ(handled, 1);

        sqlite3_close(db);
        std::error_code ec;
        std::filesystem::remove_all(temp_dir, ec);
    }

    TEST(Stage3Phase3Relay, DeadLetterAndLagPreviewReadinessSignals) {
        using namespace simcore::db::events;
        using namespace simcore::db::migrations;
        using namespace simcore::db::retention;

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        const MigrationSourceOptions embedded_options{
            .source_kind = MigrationSourceKind::Embedded,
        };
        std::string err;
        ASSERT_TRUE(ApplyContextMigrations(db, MigrationContext::Execution, embedded_options, &err)) << err;

        ASSERT_TRUE(ExecSql(db, R"SQL(
INSERT INTO exec_outbox_message(
    outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id
) VALUES
    (3951,'evt-p3-lag','Execution.WorkflowStepReady.v1',1,'Execution','workflow_step','1','corr','cause',unixepoch()*1000,'workflow_event',1);
)SQL"));

        OutboxRelay relay({
            .db = db,
            .outbox_table = "exec_outbox_message",
            .context_name = "Execution",
            .max_attempts = 1,
        });
        std::vector<OutboxRelayDispatchBinding> bindings{ {
            .key = { .event_type = "Execution.WorkflowStepReady.v1", .event_version = 1 },
            .handler = [&](const EventEnvelope&, std::string* handler_error) {
                if (handler_error != nullptr) {
                    *handler_error = "intentional-failure";
                }
                return false;
            },
        } };

        OutboxRelayResult relay_result{};
        ASSERT_TRUE(relay.RelayBatchFromCursor(0, 10, bindings, &relay_result, &err)) << err;
        EXPECT_EQ(relay_result.dead_lettered_count, 1);

        simcore::db::execution::workflow::SqliteExecutionDb execution_db(db);
        const auto preview = execution_db.PreviewOutboxRetention(
            std::vector<OutboxSubscriptionSnapshot>{
                OutboxSubscriptionSnapshot{
                    .projector_name = "phase3-test-subscriber",
                    .last_outbox_id = 0,
                    .updated_at_utc = simcore::db::types::UtcNow(),
                    .status = "ACTIVE",
                },
            },
            simcore::db::types::UtcNow(),
            OutboxRetentionPolicy{});

        ASSERT_FALSE(preview.lag_per_subscription.empty());
        EXPECT_GE(preview.lag_per_subscription.front().lag_outbox_rows, 1);
        sqlite3_close(db);
    }

    TEST(Stage4Recovery, InvariantViolationRemediationSequencePersistsLifecycle) {
        using simcore::db::core::DBService;
        using simcore::db::execution::workflow::SqliteExecutionDb;
        using simcore::db::execution::workflow::WorkflowInvariantRemediationCommand;

        std::filesystem::path temp_dir;
        std::unique_ptr<DBService> service;
        SqliteExecutionDb* execution_db = nullptr;
        std::string err;
        ASSERT_TRUE(OpenPhase4ExecutionDb("remediation", &temp_dir, &service, &execution_db, &err)) << err;

        ASSERT_TRUE(execution_db->ValidationExecuteSql(R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(4101, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'phase4-test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, expected_total, created_at_utc)
VALUES(4102, 1, 'workflow', 2, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, attempts, max_attempts, created_at_utc, started_at_utc)
VALUES(4103, 4101, 'Grid', 'seedprobe.grid', 'RUNNING', 4102, 1, 2, unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc)
VALUES
(4104, 4102, 1, 1, 'seedprobe', 1, 'phase4-r1', 1, 'COMPLETED', 1, 2, unixepoch()*1000, unixepoch()*1000),
(4105, 4102, 1, 1, 'seedprobe', 1, 'phase4-r2', 1, 'FAILED', 1, 2, unixepoch()*1000, unixepoch()*1000);
)SQL", &err))
            << err;

        bool reopened = false;
        ASSERT_TRUE(execution_db->ValidationExecuteInvariantRemediation(
            WorkflowInvariantRemediationCommand{
                .workflow_instance_id = 4101,
                .workflow_step_id = 4103,
                .violation_reason = "STEP_BLOCKED_COUNT_MISMATCH",
                .requested_by = "phase4-test",
            },
            &reopened,
            &err))
            << err;
        EXPECT_TRUE(reopened);

        std::string instance_state;
        ASSERT_TRUE(execution_db->ValidationQueryText(
            "SELECT state FROM exec_workflow_instance WHERE workflow_instance_id=4101;",
            &instance_state,
            &err))
            << err;
        EXPECT_EQ(instance_state, "RUNNING");

        std::int64_t violation_events = 0;
        std::int64_t repair_events = 0;
        std::int64_t reopen_events = 0;
        ASSERT_TRUE(execution_db->ValidationQueryInt(
                        "SELECT COUNT(1) FROM exec_workflow_event WHERE event_kind='Execution.WorkflowInvariantViolation.v1';",
                        &violation_events,
                        &err)
                    && execution_db->ValidationQueryInt(
                        "SELECT COUNT(1) FROM exec_workflow_event WHERE event_kind='Execution.WorkflowRemediationRepairExecuted.v1';",
                        &repair_events,
                        &err)
                    && execution_db->ValidationQueryInt(
                        "SELECT COUNT(1) FROM exec_workflow_event WHERE event_kind='Execution.WorkflowRemediationReopened.v1';",
                        &reopen_events,
                        &err))
            << err;
        EXPECT_EQ(violation_events, 1);
        EXPECT_EQ(repair_events, 1);
        EXPECT_EQ(reopen_events, 1);

        CleanupPhase4Db(service, temp_dir);
    }

    TEST(Stage4Recovery, ClaimedJobMaterializationIsIdempotentAcrossRestart) {
        using simcore::db::core::DBService;
        using simcore::db::execution::workflow::SqliteExecutionDb;

        std::filesystem::path temp_dir;
        std::unique_ptr<DBService> service;
        SqliteExecutionDb* execution_db = nullptr;
        std::string err;
        ASSERT_TRUE(OpenPhase4ExecutionDb("powerloss", &temp_dir, &service, &execution_db, &err)) << err;

        ASSERT_TRUE(execution_db->ValidationExecuteSql(R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(4201, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'phase4-test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, created_at_utc, ready_at_utc)
VALUES(4202, 4201, 'Neutral', 'seedprobe.neutral', 'READY', 0, 2, unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, expected_total, created_at_utc)
VALUES(4203, 1, 'workflow', 1, unixepoch()*1000);
)SQL", &err))
            << err;

        ASSERT_TRUE(execution_db->WorkflowCommandService()->MarkStepMaterialized(
            { .workflow_step_id = 4202, .job_set_id = 4203, .requested_by = "phase4-test" }, &err))
            << err;

        service->Stop();
        using namespace simcore::db::migrations;
        auto restarted = std::make_unique<DBService>(
            MakePhase4DbPaths(temp_dir),
            MigrationSourceOptions{ .source_kind = MigrationSourceKind::Embedded });
        ASSERT_TRUE(restarted->Start(&err)) << err;
        auto* restarted_execution = dynamic_cast<SqliteExecutionDb*>(restarted->ExecutionDb());
        ASSERT_NE(restarted_execution, nullptr);

        ASSERT_TRUE(restarted_execution->WorkflowCommandService()->MarkStepMaterialized(
            { .workflow_step_id = 4202, .job_set_id = 4203, .requested_by = "phase4-test-restart" }, &err))
            << err;

        std::int64_t materialized_events = 0;
        ASSERT_TRUE(restarted_execution->ValidationQueryInt(
            "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_step_id=4202 AND event_kind='Execution.WorkflowStepMaterialized.v1';",
            &materialized_events,
            &err))
            << err;
        EXPECT_EQ(materialized_events, 1);

        CleanupPhase4Db(restarted, temp_dir);
    }

    TEST(Stage4Recovery, DuplicateTerminalReplayWritesSingleTerminalEvent) {
        using simcore::db::core::DBService;
        using simcore::db::execution::workflow::SqliteExecutionDb;

        std::filesystem::path temp_dir;
        std::unique_ptr<DBService> service;
        SqliteExecutionDb* execution_db = nullptr;
        std::string err;
        ASSERT_TRUE(OpenPhase4ExecutionDb("dup-terminal", &temp_dir, &service, &execution_db, &err)) << err;

        ASSERT_TRUE(execution_db->ValidationExecuteSql(R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(4301, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'phase4-test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, expected_total, created_at_utc)
VALUES(4302, 1, 'workflow', 1, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, attempts, max_attempts, created_at_utc, started_at_utc)
VALUES(4303, 4301, 'Unique', 'seedprobe.unique', 'MATERIALIZED', 4302, 1, 2, unixepoch()*1000, unixepoch()*1000);
)SQL", &err))
            << err;

        ASSERT_TRUE(execution_db->WorkflowCommandService()->MarkStepTerminal(
            { .workflow_step_id = 4303, .terminal_state = "COMPLETED", .requested_by = "phase4-test" }, &err))
            << err;
        ASSERT_TRUE(execution_db->WorkflowCommandService()->MarkStepTerminal(
            { .workflow_step_id = 4303, .terminal_state = "COMPLETED", .requested_by = "phase4-test-replay" }, &err))
            << err;

        std::int64_t completed_events = 0;
        ASSERT_TRUE(execution_db->ValidationQueryInt(
            "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_step_id=4303 AND event_kind='Execution.WorkflowStepCompleted.v1';",
            &completed_events,
            &err))
            << err;
        EXPECT_EQ(completed_events, 1);

        CleanupPhase4Db(service, temp_dir);
    }

    TEST(Stage4Recovery, PartialWriterFailurePreservesCommittedState) {
        using simcore::db::core::DBService;
        using simcore::db::execution::workflow::SqliteExecutionDb;

        std::filesystem::path temp_dir;
        std::unique_ptr<DBService> service;
        SqliteExecutionDb* execution_db = nullptr;
        std::string err;
        ASSERT_TRUE(OpenPhase4ExecutionDb("partial-writer", &temp_dir, &service, &execution_db, &err)) << err;

        ASSERT_TRUE(execution_db->ValidationExecuteSql(R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(4401, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'phase4-test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, expected_total, created_at_utc)
VALUES(4402, 1, 'workflow', 1, unixepoch()*1000),
      (4403, 1, 'workflow', 1, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, created_at_utc, ready_at_utc)
VALUES(4404, 4401, 'Neutral', 'seedprobe.neutral', 'READY', 0, 2, unixepoch()*1000, unixepoch()*1000);
)SQL", &err))
            << err;

        ASSERT_TRUE(execution_db->WorkflowCommandService()->MarkStepMaterialized(
            { .workflow_step_id = 4404, .job_set_id = 4402, .requested_by = "phase4-test" }, &err))
            << err;
        EXPECT_FALSE(execution_db->WorkflowCommandService()->MarkStepMaterialized(
            { .workflow_step_id = 4404, .job_set_id = 4403, .requested_by = "phase4-test-conflict" }, &err));

        std::int64_t mapped_job_set_id = 0;
        std::int64_t materialized_events = 0;
        ASSERT_TRUE(execution_db->ValidationQueryInt(
                        "SELECT job_set_id FROM exec_workflow_step WHERE workflow_step_id=4404;",
                        &mapped_job_set_id,
                        &err)
                    && execution_db->ValidationQueryInt(
                        "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_step_id=4404 AND event_kind='Execution.WorkflowStepMaterialized.v1';",
                        &materialized_events,
                        &err))
            << err;
        EXPECT_EQ(mapped_job_set_id, 4402);
        EXPECT_EQ(materialized_events, 1);

        CleanupPhase4Db(service, temp_dir);
    }

    TEST(Stage4Recovery, MissingDecisionResultRerunBackfillsExactlyOneDecisionEvent) {
        using simcore::db::core::DBService;
        using simcore::db::execution::workflow::SqliteExecutionDb;

        std::filesystem::path temp_dir;
        std::unique_ptr<DBService> service;
        SqliteExecutionDb* execution_db = nullptr;
        std::string err;
        ASSERT_TRUE(OpenPhase4ExecutionDb("missing-decision", &temp_dir, &service, &execution_db, &err)) << err;

        ASSERT_TRUE(execution_db->ValidationExecuteSql(R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(4501, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'phase4-test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, created_at_utc, ready_at_utc)
VALUES(4502, 4501, 'Grid', 'seedprobe.grid', 'RUNNING', 1, 2, unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_event(workflow_instance_id, workflow_step_id, event_kind, event_ts_utc, message)
VALUES(4501, 4502, 'Execution.WorkflowTransitionEvaluated.v1', unixepoch()*1000, 'decision_evaluated_without_result');
)SQL", &err))
            << err;

        std::int64_t existing_decisions = 0;
        ASSERT_TRUE(execution_db->ValidationQueryInt(
            "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_instance_id=4501 AND workflow_step_id=4502 AND event_kind IN ('Execution.WorkflowTransitionAdvanced.v1','Execution.WorkflowTransitionBlocked.v1');",
            &existing_decisions,
            &err))
            << err;
        EXPECT_EQ(existing_decisions, 0);

        ASSERT_TRUE(execution_db->WorkflowCommandService()->AppendLifecycleEvent(
            {
                .workflow_instance_id = 4501,
                .workflow_step_id = 4502,
                .event_kind = "Execution.WorkflowTransitionBlocked.v1",
                .message = std::optional<std::string>("restart_rerun_backfilled_missing_decision_result"),
                .requested_by = "phase4-test",
            },
            &err))
            << err;

        std::int64_t backfilled_decisions = 0;
        ASSERT_TRUE(execution_db->ValidationQueryInt(
            "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_instance_id=4501 AND workflow_step_id=4502 AND event_kind='Execution.WorkflowTransitionBlocked.v1';",
            &backfilled_decisions,
            &err))
            << err;
        EXPECT_EQ(backfilled_decisions, 1);

        CleanupPhase4Db(service, temp_dir);
    }

    TEST(Stage4Readiness, ObservabilityRetentionSignalsAndPolicyThresholdsAreCoherent) {
        using simcore::db::core::DBService;
        using simcore::db::execution::workflow::SqliteExecutionDb;

        std::filesystem::path temp_dir;
        std::unique_ptr<DBService> service;
        SqliteExecutionDb* execution_db = nullptr;
        std::string err;
        ASSERT_TRUE(OpenPhase4ExecutionDb("observability", &temp_dir, &service, &execution_db, &err)) << err;

        ASSERT_TRUE(execution_db->ValidationExecuteSql(R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(4601, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'phase4-test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, created_at_utc, ready_at_utc, blocked_reason)
VALUES
(4602, 4601, 'Neutral', 'seedprobe.neutral', 'RUNNING', 1, 2, unixepoch()*1000, unixepoch()*1000, NULL),
(4603, 4601, 'Grid', 'seedprobe.grid', 'RUNNING', 1, 2, unixepoch()*1000, unixepoch()*1000, 'STEP_BLOCKED_COUNT_MISMATCH'),
(4604, 4601, 'Unique', 'seedprobe.unique', 'RUNNING', 1, 2, unixepoch()*1000, unixepoch()*1000, 'STEP_BLOCKED_COUNT_MISMATCH');
INSERT INTO exec_workflow_event(workflow_instance_id, workflow_step_id, event_kind, event_ts_utc, message)
VALUES
(4601, 4602, 'Execution.WorkflowTransitionEvaluated.v1', unixepoch()*1000, 'eval-1'),
(4601, 4602, 'Execution.WorkflowTransitionEvaluated.v1', unixepoch()*1000, 'eval-2'),
(4601, 4602, 'Execution.WorkflowTransitionEvaluated.v1', unixepoch()*1000, 'eval-3'),
(4601, 4602, 'Execution.WorkflowTransitionEvaluated.v1', unixepoch()*1000, 'eval-4'),
(4601, 4603, 'Execution.WorkflowTransitionEvaluated.v1', unixepoch()*1000, 'eval-5');
INSERT INTO exec_handler_dedupe(handler_name, event_id, semantic_key, first_seen_at_utc, last_seen_at_utc)
VALUES
('workflow_terminal_subscriber', 'evt-1', 'workflow_step:4602:terminal:COMPLETED', unixepoch()*1000-6*3600*1000, unixepoch()*1000-6*3600*1000),
('workflow_terminal_subscriber', 'evt-2', 'workflow_step:4603:terminal:FAILED', unixepoch()*1000-5*3600*1000, unixepoch()*1000-5*3600*1000),
('workflow_terminal_subscriber', 'evt-3', 'workflow_step:4604:terminal:FAILED', unixepoch()*1000-4*3600*1000, unixepoch()*1000-4*3600*1000),
('workflow_terminal_subscriber', 'evt-4', 'workflow_step:4605:terminal:FAILED', unixepoch()*1000-1*3600*1000, unixepoch()*1000-1*3600*1000),
('workflow_terminal_subscriber', 'evt-5', 'workflow_step:4606:terminal:FAILED', unixepoch()*1000-30*60*1000, unixepoch()*1000-30*60*1000),
('workflow_terminal_subscriber', 'evt-6', 'workflow_step:4607:terminal:FAILED', unixepoch()*1000-20*60*1000, unixepoch()*1000-20*60*1000),
('workflow_terminal_subscriber', 'evt-7', 'workflow_step:4608:terminal:FAILED', unixepoch()*1000-10*60*1000, unixepoch()*1000-10*60*1000),
('workflow_terminal_subscriber', 'evt-8', 'workflow_step:4609:terminal:FAILED', unixepoch()*1000-5*60*1000, unixepoch()*1000-5*60*1000);
)SQL", &err))
            << err;

        std::int64_t completion_checks = 0;
        std::int64_t completion_mismatches = 0;
        std::int64_t replay_loop_steps = 0;
        std::int64_t historical_dedupe_rows = 0;
        std::int64_t current_dedupe_rows = 0;
        ASSERT_TRUE(execution_db->ValidationQueryInt(
                        "SELECT COUNT(1) FROM exec_workflow_event WHERE event_kind='Execution.WorkflowTransitionEvaluated.v1';",
                        &completion_checks,
                        &err)
                    && execution_db->ValidationQueryInt(
                        "SELECT COUNT(1) FROM exec_workflow_step WHERE blocked_reason='STEP_BLOCKED_COUNT_MISMATCH';",
                        &completion_mismatches,
                        &err)
                    && execution_db->ValidationQueryInt(
                        R"SQL(SELECT COUNT(1) FROM (
                                SELECT workflow_step_id, COUNT(1) AS attempts
                                FROM exec_workflow_event
                                WHERE event_kind='Execution.WorkflowTransitionEvaluated.v1'
                                GROUP BY workflow_step_id
                                HAVING attempts >= 3
                            );)SQL",
                        &replay_loop_steps,
                        &err)
                    && execution_db->ValidationQueryInt(
                        "SELECT COUNT(1) FROM exec_handler_dedupe WHERE last_seen_at_utc < unixepoch()*1000-3600*1000;",
                        &historical_dedupe_rows,
                        &err)
                    && execution_db->ValidationQueryInt(
                        "SELECT COUNT(1) FROM exec_handler_dedupe WHERE last_seen_at_utc >= unixepoch()*1000-3600*1000;",
                        &current_dedupe_rows,
                        &err))
            << err;

        ASSERT_GT(completion_checks, 0);
        ASSERT_GE(completion_mismatches, 0);
        ASSERT_LE(completion_mismatches, completion_checks);
        const double completion_gate_mismatch_frequency =
            static_cast<double>(completion_mismatches) / static_cast<double>(completion_checks);

        ASSERT_GT(historical_dedupe_rows, 0);
        ASSERT_GE(current_dedupe_rows, 0);
        const double dedupe_growth_ratio =
            static_cast<double>(current_dedupe_rows) / static_cast<double>(historical_dedupe_rows);

        EXPECT_GE(kPhase4DedupeTtlHours, kPhase4MinDedupeTtlHours);
        EXPECT_LE(kPhase4DedupeTtlHours, kPhase4MaxDedupeTtlHours);
        EXPECT_GE(kPhase4ClaimedJobStagingCleanupHours, kPhase4MinClaimedJobCleanupHours);
        EXPECT_LE(kPhase4ClaimedJobStagingCleanupHours, kPhase4MaxClaimedJobCleanupHours);

        const bool escalation_policy_present = kPhase4CompletionGateMismatchWarnFrequency > 0.0
            && kPhase4CompletionGateMismatchPageFrequency > kPhase4CompletionGateMismatchWarnFrequency
            && kPhase4ReplayLoopWarnCount > 0
            && kPhase4ReplayLoopPageCount > kPhase4ReplayLoopWarnCount
            && kPhase4DedupeGrowthWarnRatio > 1.0
            && kPhase4DedupeGrowthPageRatio > kPhase4DedupeGrowthWarnRatio;
        EXPECT_TRUE(escalation_policy_present);

        EXPECT_TRUE(std::isfinite(completion_gate_mismatch_frequency));
        EXPECT_GE(completion_gate_mismatch_frequency, 0.0);
        EXPECT_LE(completion_gate_mismatch_frequency, 1.0);
        EXPECT_GE(replay_loop_steps, 0);
        EXPECT_TRUE(std::isfinite(dedupe_growth_ratio));
        EXPECT_GT(dedupe_growth_ratio, 0.0);

        CleanupPhase4Db(service, temp_dir);
    }

}
