#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include "Common/Migrations/MigrationRunner.h"
#include "Common/DbService.h"
#include "Common/Events/EventCatalog.h"
#include "Common/Events/EventPayloadDispatch.h"
#include "Common/Events/EventPayloadValidation.h"
#include "Common/Events/EventTypeFormat.h"
#include "Common/Events/OutboxRelay.h"
#include "SimCoreDB.h"
#include "Analysis/SqliteAnalysisDb.h"
#include "Archive/SqliteArchiveDb.h"
#include "Archive/ArchivePackageService.h"
#include "Archive/RehydrateExecutor.h"
#include "UIRead/SqliteUiReadDb.h"
#include "Runner/Parallel/SimCoreDB/ArchiveWorkflowCommands.h"
#include "Execution/Workflow/SqliteExecutionDb.h"
#include "Execution/Jobs/JobEventOrchestration.h"
#include "Execution/Workflow/SeedProbeWorkflowDefinition.h"
#include "Execution/Workflow/WorkflowEngine.h"
#include "Execution/Workflow/WorkflowIntegrityChecks.h"
#include "Execution/Workflow/WorkflowModeProvider.h"
#include "Execution/Workflow/WorkflowParityDiagnostics.h"
#include "Execution/Workflow/WorkflowParityStore.h"
#include "Execution/Workflow/WorkflowProjector.h"
#include "Execution/Workflow/WorkflowRecoveryService.h"
#include "Execution/Workflow/AdapterChainOrchestrator.h"
#include "Execution/Workflow/WorkflowTerminalOutboxSubscriber.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbePhaseRegistration.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeNeutralAdapters.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeGridAdapters.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeUniqueAdapters.h"
#include "UIRead/Projectors/ProjectorContract.h"
#include "Runner/Parallel/SimCoreDB/WorkflowCoordinatorBridge.h"
#include "Runner/Parallel/SimCoreDB/DBWorkflowWorkerCoordinator.h"
#include "Runner/Parallel/SimCoreDB/WorkflowSchedulerAdapter.h"
#include "Runner/Parallel/SimCoreDB/StepInputAggregationService.h"

#include "common/RecordingExecutionDb.h"
#include "common/SqliteDbFixture.h"
#include "common/simcoredb_helpers.h"

namespace {


TEST_F(SqliteDbFixture, EmbeddedMigrationsApplyOncePerContextAndTrackVersion) {
    using namespace simcore::db::migrations;

    const MigrationSourceOptions embedded_options{
        .source_kind = MigrationSourceKind::Embedded,
    };

    for (const auto context : ListAllMigrationContexts()) {
        std::string err;
        EXPECT_TRUE(ApplyContextMigrations(db_, context, embedded_options, &err)) << err;

        const auto version = GetCurrentContextSchemaVersion(db_, context, &err);
        ASSERT_TRUE(version.has_value()) << err;
        EXPECT_GT(*version, 0) << "Expected non-zero version for context " << ToString(context);

        const auto entries = LoadContextMigrations(context, embedded_options);
        ASSERT_FALSE(entries.empty());

        bool applied = false;
        EXPECT_TRUE(HasMigrationBeenApplied(db_, context, entries.front().name, &applied, &err)) << err;
        EXPECT_TRUE(applied) << "Expected first migration to be marked applied for context " << ToString(context);

        // Second apply should be a no-op and still succeed.
        EXPECT_TRUE(ApplyContextMigrations(db_, context, embedded_options, &err)) << err;
    }
}

TEST_F(SqliteDbFixture, Stage3bWorkflowMigrationsCreateExecutionAndUiReadTables) {
    using namespace simcore::db::migrations;

    const MigrationSourceOptions embedded_options{
        .source_kind = MigrationSourceKind::Embedded,
    };

    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Archive, embedded_options, &err)) << err;

    EXPECT_TRUE(TableExists(db_, "exec_workflow_instance"));
    EXPECT_TRUE(TableExists(db_, "exec_workflow_step"));
    EXPECT_TRUE(TableExists(db_, "exec_workflow_edge"));
    EXPECT_TRUE(TableExists(db_, "exec_workflow_event"));
    EXPECT_TRUE(TableExists(db_, "exec_workflow_input_event"));

    EXPECT_TRUE(TableExists(db_, "ui_workflow_instance"));
    EXPECT_TRUE(TableExists(db_, "ui_workflow_step"));
    EXPECT_TRUE(TableExists(db_, "ui_workflow_edge"));
    EXPECT_TRUE(TableExists(db_, "ui_workflow_alert"));

    EXPECT_TRUE(TableExists(db_, "ar_archive_item_kind_catalog"));

    EXPECT_TRUE(ColumnExists(db_, "exec_workflow_step", "guard_kind"));
    EXPECT_TRUE(ColumnExists(db_, "exec_workflow_step", "guard_value"));
    EXPECT_TRUE(ColumnExists(db_, "exec_workflow_step", "job_set_id"));
    EXPECT_TRUE(ColumnExists(db_, "exec_workflow_instance", "failure_text"));
    EXPECT_TRUE(ColumnExists(db_, "ui_workflow_step", "job_failed_count"));
    EXPECT_TRUE(ColumnExists(db_, "ui_workflow_alert", "is_active"));

    EXPECT_TRUE(IndexExists(db_, "ix_exec_workflow_instance_state_created"));
    EXPECT_TRUE(IndexExists(db_, "ix_exec_workflow_step_instance_state_priority_ready"));
    EXPECT_TRUE(IndexExists(db_, "ix_exec_workflow_step_job_set"));
    EXPECT_TRUE(IndexExists(db_, "ix_exec_workflow_edge_instance_to"));
    EXPECT_TRUE(IndexExists(db_, "ix_exec_workflow_input_event_step_ts"));
    EXPECT_TRUE(IndexExists(db_, "ix_exec_workflow_input_event_instance_step"));
    EXPECT_TRUE(IndexExists(db_, "ix_exec_outbox_payload_ref"));
    EXPECT_TRUE(IndexExists(db_, "ix_exec_outbox_replay_cursor"));
    EXPECT_TRUE(IndexExists(db_, "ix_ui_workflow_instance_state_created"));
    EXPECT_TRUE(IndexExists(db_, "ix_ui_workflow_step_instance_state"));
    EXPECT_TRUE(IndexExists(db_, "ix_ui_workflow_alert_active"));

    const auto step_table_sql = TableCreateSql(db_, "exec_workflow_step");
    EXPECT_NE(step_table_sql.find("CHECK(state IN ('WAITING','READY','MATERIALIZED','RUNNING','COMPLETED','FAILED','SKIPPED'))"), std::string::npos);
    EXPECT_NE(step_table_sql.find("CONSTRAINT uq_exec_workflow_step_instance_step_key UNIQUE (workflow_instance_id, step_key)"), std::string::npos);
    EXPECT_NE(step_table_sql.find("CONSTRAINT uq_exec_workflow_step_job_set_id UNIQUE (job_set_id)"), std::string::npos);

    const auto instance_table_sql = TableCreateSql(db_, "exec_workflow_instance");
    EXPECT_NE(instance_table_sql.find("CHECK(state IN ('PENDING','RUNNING','COMPLETED','FAILED','CANCELED'))"), std::string::npos);
    EXPECT_NE(instance_table_sql.find("CHECK(root_scope_kind IN ('job_set','run','manual'))"), std::string::npos);
}

TEST_F(SqliteDbFixture, Stage3cReadinessGuardRequiresStage3bWorkflowSchemaVersion) {
    using namespace simcore::db::migrations;

    const MigrationSourceOptions embedded_options{
        .source_kind = MigrationSourceKind::Embedded,
    };

    sqlite3* readiness_db = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &readiness_db));

    std::string reason;
    EXPECT_FALSE(simcore::db::Stage3cWorkflowSliceReady(readiness_db, &reason));
    EXPECT_NE(reason.find("Execution schema version"), std::string::npos);

    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(readiness_db, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(readiness_db, MigrationContext::UIRead, embedded_options, &err)) << err;

    EXPECT_TRUE(simcore::db::Stage3cWorkflowSliceReady(readiness_db, &reason)) << reason;
    EXPECT_EQ(reason, "OK");

    sqlite3_close(readiness_db);
}

TEST_F(SqliteDbFixture, Stage3cExecutionDbServicesResolveAndRoundTripQueryCommand) {
    using namespace simcore::db::migrations;
    using namespace simcore::db::execution::workflow;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };

    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(1001, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch());
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(9501, 1, 'workflow', unixepoch());
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, priority, attempts, max_attempts, created_at_utc)
VALUES
  (2001,1001,'Neutral','seedprobe.neutral','FAILED',10,0,2,unixepoch()),
  (2002,1001,'Grid','seedprobe.grid','WAITING',8,0,2,unixepoch()),
  (2003,1001,'Unique','seedprobe.unique','WAITING',7,0,2,unixepoch()),
  (2004,1001,'Done','seedprobe.done','WAITING',0,0,1,unixepoch());
INSERT INTO exec_workflow_edge(workflow_edge_id, workflow_instance_id, from_step_id, to_step_id, created_at_utc)
VALUES (3001,1001,2001,2002,unixepoch()),(3002,1001,2002,2003,unixepoch()),(3003,1001,2003,2004,unixepoch());
)SQL"));

    simcore::db::execution::workflow::SqliteExecutionDb execution_db(db_);
    ASSERT_NE(execution_db.WorkflowQueryService(), nullptr);
    ASSERT_NE(execution_db.WorkflowCommandService(), nullptr);

    const auto graph = execution_db.WorkflowQueryService()->GetWorkflowGraph(1001);
    ASSERT_TRUE(graph.has_value());
    EXPECT_EQ(graph->steps.size(), 4);

    std::string command_error;
    EXPECT_TRUE(execution_db.WorkflowCommandService()->RetryFailedStep({ .workflow_step_id = 2001, .requested_by = "test" }, &command_error)) << command_error;
    EXPECT_TRUE(execution_db.WorkflowCommandService()->MarkStepMaterialized(
        { .workflow_step_id = 2001, .job_set_id = 9501, .requested_by = "test" },
        &command_error))
        << command_error;
    EXPECT_TRUE(execution_db.WorkflowCommandService()->MarkStepTerminal(
        { .workflow_step_id = 2001, .terminal_state = "COMPLETED", .requested_by = "test" },
        &command_error))
        << command_error;
    // Idempotent duplicate terminal callback.
    EXPECT_TRUE(execution_db.WorkflowCommandService()->MarkStepTerminal(
        { .workflow_step_id = 2001, .terminal_state = "COMPLETED", .requested_by = "test" },
        &command_error))
        << command_error;

    const auto map = execution_db.WorkflowQueryService()->GetStepToJobSetMap(1001);
    ASSERT_EQ(map.size(), 1u);
    EXPECT_EQ(map[0].first, 2001);
    EXPECT_EQ(map[0].second, 9501);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT state, attempts FROM exec_workflow_step WHERE workflow_step_id=2001;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))), "COMPLETED");
    EXPECT_EQ(sqlite3_column_int(st, 1), 1);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_instance_id=1001 AND event_kind='Execution.WorkflowStepMaterialized.v1';",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 1);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_instance_id=1001 AND event_kind='Execution.WorkflowStepCompleted.v1';",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 1);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cWorkflowQueryListsReadyStepsWithoutGraphFanout) {
    using namespace simcore::db::migrations;
    using namespace simcore::db::execution::workflow;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_at_utc)
VALUES
  (1101, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', unixepoch()),
  (1102, 'SEED_PROBE_CHAIN', 'PENDING', 'manual', unixepoch());
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, priority, ready_at_utc, attempts, max_attempts, created_at_utc)
VALUES
  (2101, 1101, 'Grid', 'seedprobe.grid', 'READY', 8, unixepoch()-5, 0, 2, unixepoch()),
  (2102, 1101, 'Unique', 'seedprobe.unique', 'READY', 10, unixepoch()-10, 0, 2, unixepoch()),
  (2103, 1101, 'Done', 'seedprobe.done', 'WAITING', 0, NULL, 0, 1, unixepoch()),
  (2104, 1102, 'Grid', 'seedprobe.grid', 'READY', 9, unixepoch()-20, 0, 2, unixepoch());
)SQL"));

    simcore::db::execution::workflow::SqliteExecutionDb execution_db(db_);
    const auto ready_steps = execution_db.WorkflowQueryService()->ListReadySteps(10);
    ASSERT_EQ(ready_steps.size(), 2u);
    EXPECT_EQ(ready_steps[0].workflow_step_id, 2102);
    EXPECT_EQ(ready_steps[1].workflow_step_id, 2101);
}

TEST_F(SqliteDbFixture, Stage3cWorkflowProjectorProjectsUiReadRows) {
    using namespace simcore::db::migrations;
    using namespace simcore::db::execution::workflow;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(4001, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch());
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(9001, 1, 'workflow', unixepoch());
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES(9101, 9001, 1, 1, 'seedprobe', 1, 'wf-step-4001', 10, 'COMPLETED', 1, 2, unixepoch());
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc, completed_at_utc)
VALUES(5001, 4001, 'Neutral', 'seedprobe.neutral', 'COMPLETED', 9001, 10, 1, 2, unixepoch(), unixepoch());
INSERT INTO exec_workflow_edge(workflow_edge_id, workflow_instance_id, from_step_id, to_step_id, created_at_utc)
VALUES(6001, 4001, 5001, 5001, unixepoch());
)SQL"));

    WorkflowProjector projector(db_);
    ASSERT_TRUE(projector.ProjectInstance(4001, &err)) << err;

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT state, blocked_step_count, failed_step_count FROM ui_workflow_instance WHERE workflow_instance_id=4001;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))), "RUNNING");
    EXPECT_EQ(sqlite3_column_int(st, 1), 0);
    EXPECT_EQ(sqlite3_column_int(st, 2), 0);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cWorkflowProjectorProjectsAndClearsUiAlerts) {
    using namespace simcore::db::migrations;
    using namespace simcore::db::execution::workflow;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(4100, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch());
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, blocked_reason, priority, attempts, max_attempts, created_at_utc, failed_at_utc)
VALUES
  (5101, 4100, 'Grid', 'seedprobe.grid', 'WAITING', 'waiting_on_dependency', 8, 0, 2, unixepoch(), NULL),
  (5102, 4100, 'Unique', 'seedprobe.unique', 'FAILED', NULL, 7, 1, 2, unixepoch(), unixepoch());
)SQL"));

    WorkflowProjector projector(db_);
    ASSERT_TRUE(projector.ProjectInstance(4100, &err)) << err;

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM ui_workflow_alert WHERE workflow_instance_id=4100 AND is_active=1;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 2);
    sqlite3_finalize(st);

    ASSERT_TRUE(ExecSql(db_, R"SQL(
UPDATE exec_workflow_step SET blocked_reason=NULL, state='COMPLETED', completed_at_utc=unixepoch()
WHERE workflow_step_id=5101;
UPDATE exec_workflow_step SET state='COMPLETED', completed_at_utc=unixepoch(), failed_at_utc=NULL
WHERE workflow_step_id=5102;
)SQL"));

    ASSERT_TRUE(projector.ProjectInstance(4100, &err)) << err;

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM ui_workflow_alert WHERE workflow_instance_id=4100 AND is_active=1;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 0);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM ui_workflow_alert WHERE workflow_instance_id=4100 AND is_active=0 AND cleared_at_utc IS NOT NULL;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 2);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cRecoveryServiceReconcilesInFlightRunningSteps) {
    using namespace simcore::db::migrations;
    using namespace simcore::db::execution::workflow;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_at_utc)
VALUES(7001, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', unixepoch());
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(8001, 1, 'workflow', unixepoch());
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc)
VALUES(8101, 8001, 1, 1, 'seedprobe', 1, 'recovery-step', 10, 'FAILED', 1, 2, unixepoch(), unixepoch());
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES(7101, 7001, 'Grid', 'seedprobe.grid', 'RUNNING', 8001, 10, 1, 2, unixepoch());
)SQL"));

    WorkflowRecoveryService recovery(db_);
    WorkflowRecoveryResult result{};
    ASSERT_TRUE(recovery.ReconcileInFlightInstances(&result, &err)) << err;
    EXPECT_EQ(result.failed_steps, 1);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT state FROM exec_workflow_step WHERE workflow_step_id=7101;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))), "FAILED");
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM exec_outbox_message WHERE payload_ref_kind='workflow_event' AND event_type='Execution.WorkflowStepFailed.v1';",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 1);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cRecoveryServiceUsesJobSetAggregateTerminalState) {
    using namespace simcore::db::migrations;
    using namespace simcore::db::execution::workflow;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_at_utc)
VALUES
  (7011, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', unixepoch()),
  (7012, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', unixepoch()),
  (7013, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', unixepoch());
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES
  (8011, 1, 'workflow', unixepoch()),
  (8012, 1, 'workflow', unixepoch()),
  (8013, 1, 'workflow', unixepoch());
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES
  (7111, 7011, 'Grid', 'seedprobe.grid', 'RUNNING', 8011, 10, 1, 2, unixepoch()),
  (7112, 7012, 'Grid', 'seedprobe.grid', 'RUNNING', 8012, 10, 1, 2, unixepoch()),
  (7113, 7013, 'Grid', 'seedprobe.grid', 'RUNNING', 8013, 10, 1, 2, unixepoch());
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc)
VALUES
  (8111, 8011, 1, 1, 'seedprobe', 1, 'agg-success-1', 10, 'SUCCEEDED', 1, 2, unixepoch(), unixepoch()),
  (8112, 8011, 1, 1, 'seedprobe', 1, 'agg-success-2', 10, 'SUCCEEDED_WINNER', 1, 2, unixepoch(), unixepoch()),
  (8121, 8012, 1, 1, 'seedprobe', 1, 'agg-fail-1', 10, 'SUCCEEDED', 1, 2, unixepoch(), unixepoch()),
  (8122, 8012, 1, 1, 'seedprobe', 1, 'agg-fail-2', 10, 'FAILED', 1, 2, unixepoch(), unixepoch()),
  (8131, 8013, 1, 1, 'seedprobe', 1, 'agg-running', 10, 'RUNNING', 1, 2, unixepoch(), NULL);
)SQL"));

    WorkflowRecoveryService recovery(db_);
    WorkflowRecoveryResult result{};
    ASSERT_TRUE(recovery.ReconcileInFlightInstances(&result, &err)) << err;
    EXPECT_EQ(result.completed_steps, 1);
    EXPECT_EQ(result.failed_steps, 1);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT workflow_step_id, state FROM exec_workflow_step WHERE workflow_step_id IN (7111,7112,7113) ORDER BY workflow_step_id;",
        -1,
        &st,
        nullptr));

    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int64(st, 0), 7111);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 1))), "COMPLETED");
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int64(st, 0), 7112);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 1))), "FAILED");
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int64(st, 0), 7113);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 1))), "RUNNING");
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cWorkflowIntegrityChecksDetectViolations) {
    using namespace simcore::db::migrations;
    using namespace simcore::db::execution::workflow;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_at_utc)
VALUES(7201, 'SEED_PROBE_CHAIN', 'COMPLETED', 'manual', unixepoch());
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, priority, attempts, max_attempts, created_at_utc)
VALUES
  (7301, 7201, 'Grid', 'seedprobe.grid', 'RUNNING', 5, 0, 2, unixepoch()),
  (7302, 7201, 'Unique', 'seedprobe.unique', 'COMPLETED', 5, 1, 2, unixepoch());
INSERT INTO exec_workflow_edge(workflow_edge_id, workflow_instance_id, from_step_id, to_step_id, created_at_utc)
VALUES(7401, 7201, 999999, 7302, unixepoch());
)SQL"));

    WorkflowIntegrityReport report{};
    ASSERT_TRUE(RunWorkflowIntegrityChecks(db_, &report, &err)) << err;
    EXPECT_GT(report.dangling_edge_count, 0);
    EXPECT_GT(report.missing_job_set_link_count, 0);
    EXPECT_GT(report.non_terminal_step_in_completed_instance_count, 0);
    EXPECT_GT(report.non_terminal_step_in_terminal_instance_count, 0);
    EXPECT_FALSE(report.IsClean());
}

TEST_F(SqliteDbFixture, Stage3cWorkflowIntegrityChecksPassForCleanRows) {
    using namespace simcore::db::migrations;
    using namespace simcore::db::execution::workflow;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_at_utc)
VALUES(8201, 'SEED_PROBE_CHAIN', 'COMPLETED', 'manual', unixepoch());
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(8301, 1, 'workflow', unixepoch()),
      (8302, 1, 'workflow', unixepoch());
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc, completed_at_utc)
VALUES
  (8401, 8201, 'Grid', 'seedprobe.grid', 'COMPLETED', 8301, 5, 1, 2, unixepoch(), unixepoch()),
  (8402, 8201, 'Unique', 'seedprobe.unique', 'SKIPPED', 8302, 5, 1, 2, unixepoch(), unixepoch());
INSERT INTO exec_workflow_edge(workflow_edge_id, workflow_instance_id, from_step_id, to_step_id, created_at_utc)
VALUES(8501, 8201, 8401, 8402, unixepoch());
)SQL"));

    WorkflowIntegrityReport report{};
    ASSERT_TRUE(RunWorkflowIntegrityChecks(db_, &report, &err)) << err;
    EXPECT_TRUE(report.IsClean());
    EXPECT_EQ(report.non_terminal_step_in_terminal_instance_count, 0);
    EXPECT_EQ(report.completed_step_missing_completion_ts_count, 0);
}

TEST_F(SqliteDbFixture, Stage3cWorkflowParityStorePersistsAndListsSummaryRows) {
    using namespace simcore::db::execution::workflow;

    const auto report = CompareLegacyAndWorkflowOutcomes(
        {
            WorkflowOutcomeItem{ .step_key = "Neutral", .outcome = "COMPLETED" },
            WorkflowOutcomeItem{ .step_key = "Grid", .outcome = "COMPLETED" },
        },
        {
            WorkflowOutcomeItem{ .step_key = "Neutral", .outcome = "FAILED" },
            WorkflowOutcomeItem{ .step_key = "Unique", .outcome = "COMPLETED" },
        });

    WorkflowParityStore store(db_);
    std::string err;
    ASSERT_TRUE(store.PersistReport("run-42", 4200, report, &err)) << err;

    const auto summaries = store.ListSummaries(&err);
    ASSERT_FALSE(summaries.empty()) << err;
    EXPECT_EQ(summaries[0].run_ref, "run-42");
    EXPECT_EQ(summaries[0].workflow_instance_id, 4200);
    EXPECT_EQ(summaries[0].compared_steps, 3);
    EXPECT_EQ(summaries[0].matched_steps, 0);
    EXPECT_EQ(summaries[0].mismatch_steps, 3);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM exec_workflow_parity_mismatch WHERE run_ref='run-42' AND workflow_instance_id=4200;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 3);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cTerminalSubscriberProcessesTerminalJobEventsAsynchronously) {
    using namespace simcore::db::execution::workflow;

    const simcore::db::migrations::MigrationSourceOptions embedded_options{ .source_kind = simcore::db::migrations::MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(simcore::db::migrations::ApplyContextMigrations(db_, simcore::db::migrations::MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(1, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc, expected_total)
VALUES(10, 1, 'workflow', unixepoch()*1000, 1);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES(100, 1, 'Neutral', 'seedprobe.neutral', 'MATERIALIZED', 10, 0, 0, 1, unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES(1000, 10, 1, 1, 'seedprobe_spec', 44, 'fp-1', 0, 'QUEUED', 0, 1, unixepoch()*1000);
)SQL"));

    simcore::db::execution::jobs::SqliteJobEventCommandService job_events(db_);
    std::string job_error;
    ASSERT_TRUE(job_events.AppendLifecycleEvent(
        {
            .kind = simcore::db::execution::jobs::JobLifecycleEventKind::JobCompleted,
            .job_id = 1000,
        },
        &job_error)) << job_error;

    simcore::db::execution::programdb::ProgramKindRegistry registry;
    simcore::db::execution::programdb::ProgramKindDescriptor descriptor{};
    descriptor.program_kind = 1;
    descriptor.program_name = "seedprobe.neutral";
    descriptor.workflow_transition = std::make_shared<AlwaysAdvanceTransitionHandler>();
    ASSERT_TRUE(registry.RegisterForStepKind("seedprobe.neutral", descriptor));
    StepCompletionGateService gate;
    AdapterChainOrchestrator orchestrator(&registry, &gate);

    SqliteWorkflowOrchestrationCommandService command_service(db_);
    WorkflowTerminalOutboxSubscriber subscriber(db_, &orchestrator, &command_service);
    WorkflowTerminalOutboxSubscriberResult result{};
    std::string sub_error;
    ASSERT_TRUE(subscriber.ConsumeFromCursor(0, 32, &result, &sub_error)) << sub_error;
    EXPECT_GE(result.handled_count, 1);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT state, blocked_reason FROM exec_workflow_step WHERE workflow_step_id=100;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), "COMPLETED");
    EXPECT_EQ(sqlite3_column_type(st, 1), SQLITE_NULL);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3fWorkflowProjectorOutboxReplayUsesSubscriptionCursorAndIsIdempotent) {
    using namespace simcore::db::migrations;
    using namespace simcore::db::execution::workflow;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(9201, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch());
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(9301, 1, 'workflow', unixepoch());
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES(9401, 9201, 'Neutral', 'seedprobe.neutral', 'MATERIALIZED', 9301, 10, 1, 2, unixepoch());
)SQL"));

    simcore::db::execution::workflow::SqliteExecutionDb execution_db(db_);
    std::string cmd_error;
    ASSERT_TRUE(execution_db.WorkflowCommandService()->MarkStepTerminal(
        { .workflow_step_id = 9401, .terminal_state = "COMPLETED", .requested_by = "projector-replay-test" },
        &cmd_error))
        << cmd_error;

    WorkflowProjector projector(db_);
    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) "
        "FROM ui_projection_subscription "
        "WHERE projector_name='WorkflowProjector' "
        "AND source_context='Execution' "
        "AND source_outbox_table='exec_outbox_message';",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 0);
    sqlite3_finalize(st);

    ASSERT_TRUE(projector.ProjectFromOutbox("WorkflowProjector", 100, &err)) << err;

    std::int64_t cursor_after_first = 0;
    st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT last_outbox_id, COALESCE(last_event_id, ''), status, COALESCE(last_error, '') "
        "FROM ui_projection_subscription "
        "WHERE projector_name='WorkflowProjector' "
        "AND source_context='Execution' "
        "AND source_outbox_table='exec_outbox_message';",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    cursor_after_first = sqlite3_column_int64(st, 0);
    EXPECT_GT(cursor_after_first, 0);
    ASSERT_NE(sqlite3_column_text(st, 1), nullptr);
    EXPECT_FALSE(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 1))).empty());
    ASSERT_NE(sqlite3_column_text(st, 2), nullptr);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 2))), "ACTIVE");
    ASSERT_NE(sqlite3_column_text(st, 3), nullptr);
    EXPECT_TRUE(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 3))).empty());
    sqlite3_finalize(st);

    st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM ui_workflow_step WHERE workflow_instance_id=9201;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 1);
    sqlite3_finalize(st);

    ASSERT_TRUE(projector.ProjectFromOutbox("WorkflowProjector", 100, &err)) << err;
    st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT last_outbox_id "
        "FROM ui_projection_subscription "
        "WHERE projector_name='WorkflowProjector' "
        "AND source_context='Execution' "
        "AND source_outbox_table='exec_outbox_message';",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int64(st, 0), cursor_after_first);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cOutboxRelayRoundTripsOccurredAtUtcEpochMilliseconds) {
    using namespace simcore::db::events;
    using namespace simcore::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    constexpr std::int64_t kOccurredAtUtcEpochMillis = 1735689600123; // 2025-01-01T00:00:00.123Z
    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_outbox_message(
    outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id
)
VALUES(
    6001,'evt-workflow-ms-roundtrip','Execution.WorkflowStepCompleted.v1',1,'Execution','workflow_instance','42','42','42',1735689600123,'workflow_event',4201
);
)SQL"));

    OutboxRelay relay({
        .db = db_,
        .outbox_table = "exec_outbox_message",
        .context_name = "Execution",
        .aggregate_kind = "workflow_instance",
        .payload_ref_kind = "workflow_event",
        .max_attempts = 3,
    });

    std::int64_t observed_occurred_at_utc_epoch_millis = -1;
    std::vector<OutboxRelayDispatchBinding> bindings;
    bindings.push_back(OutboxRelayDispatchBinding{
        .key = { .event_type = "Execution.WorkflowStepCompleted.v1", .event_version = 1 },
        .handler = [&observed_occurred_at_utc_epoch_millis](const EventEnvelope& envelope, std::string*) {
            observed_occurred_at_utc_epoch_millis = envelope.occurred_at_utc.time_since_epoch().count();
            return true;
        },
    });

    OutboxRelayResult result{};
    ASSERT_TRUE(relay.RelayBatch(0, 100, bindings, &result, &err)) << err;
    EXPECT_EQ(result.published_count, 1);
    EXPECT_EQ(observed_occurred_at_utc_epoch_millis, kOccurredAtUtcEpochMillis);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT occurred_at_utc, published_at_utc FROM exec_outbox_message WHERE outbox_id=6001;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int64(st, 0), kOccurredAtUtcEpochMillis);
    EXPECT_EQ(sqlite3_column_type(st, 1), SQLITE_INTEGER);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3fProjectionSubscriptionSkipsDuplicateEventIdsAndAdvancesCursor) {
    using namespace simcore::db::events;
    using namespace simcore::db::migrations;
    using namespace simcore::db::uiread::projectors;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_outbox_message(
    outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,occurred_at_utc,payload_ref_kind,payload_ref_id
)
VALUES(
    7001,'evt-projector-contract-dedup','Execution.JobQueued.v1',1,'Execution','job','42',unixepoch()*1000,'job',42
);
INSERT INTO ui_projection_handled_event(projector_name,event_id,last_outbox_id,handled_at_utc)
VALUES('ProjectorSubscription.exec|Execution|exec_outbox_message','evt-projector-contract-dedup',6999,unixepoch()*1000);
)SQL"));

    int handler_calls = 0;
    std::vector<OutboxRelayDispatchBinding> bindings;
    bindings.push_back(OutboxRelayDispatchBinding{
        .key = { .event_type = "Execution.JobQueued.v1", .event_version = 1 },
        .handler = [&handler_calls](const EventEnvelope&, std::string*) {
            handler_calls += 1;
            return true;
        },
    });

    ASSERT_TRUE(RunProjectorRelay(
        db_,
        "ProjectorSubscription.exec",
        "Execution",
        "exec_outbox_message",
        {
            .db = db_,
            .outbox_table = "exec_outbox_message",
            .context_name = "Execution",
            .aggregate_kind = "",
            .payload_ref_kind = "",
            .max_attempts = 5,
        },
        bindings,
        100,
        &err))
        << err;

    EXPECT_EQ(handler_calls, 0);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COALESCE(last_event_id, ''), last_outbox_id, status, COALESCE(last_error, '') "
        "FROM ui_projection_subscription "
        "WHERE projector_name='ProjectorSubscription.exec' "
        "AND source_context='Execution' "
        "AND source_outbox_table='exec_outbox_message';",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    ASSERT_NE(sqlite3_column_text(st, 0), nullptr);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))), "evt-projector-contract-dedup");
    EXPECT_EQ(sqlite3_column_int64(st, 1), 7001);
    ASSERT_NE(sqlite3_column_text(st, 2), nullptr);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 2))), "ACTIVE");
    ASSERT_NE(sqlite3_column_text(st, 3), nullptr);
    EXPECT_TRUE(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 3))).empty());
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cWorkflowProjectorOutboxRelayFailureIncrementsAttemptsAndDeadLetters) {
    using namespace simcore::db::migrations;
    using namespace simcore::db::execution::workflow;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_outbox_message(
    outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,occurred_at_utc,payload_ref_kind,payload_ref_id
)
VALUES(
    5001,'evt-workflow-relay-missing-handler','Execution.WorkflowStepBlocked.v1',1,'Execution','workflow_instance','999',unixepoch()*1000,'workflow_event',9001
);
)SQL"));

    WorkflowProjector projector(db_);
    ASSERT_FALSE(projector.ProjectFromOutbox("WorkflowProjector", 100, &err, 2));
    EXPECT_EQ(err, "relay batch contained 1 failing event(s)");

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT attempt_count, last_error, published_at_utc FROM exec_outbox_message WHERE outbox_id=5001;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 1);
    ASSERT_NE(sqlite3_column_text(st, 1), nullptr);
    const std::string first_error(reinterpret_cast<const char*>(sqlite3_column_text(st, 1)));
    EXPECT_EQ(first_error.find("unsupported Execution event_type"), 0u);
    EXPECT_EQ(sqlite3_column_type(st, 2), SQLITE_NULL);
    sqlite3_finalize(st);

    ASSERT_TRUE(projector.ProjectFromOutbox("WorkflowProjector", 100, &err, 2)) << err;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT attempt_count, last_error FROM exec_outbox_message WHERE outbox_id=5001;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 2);
    ASSERT_NE(sqlite3_column_text(st, 1), nullptr);
    const std::string dead_letter_error(reinterpret_cast<const char*>(sqlite3_column_text(st, 1)));
    EXPECT_EQ(dead_letter_error.find("dead-letter: unsupported Execution event_type"), 0u);
    sqlite3_finalize(st);

    ASSERT_TRUE(projector.ProjectFromOutbox("WorkflowProjector", 100, &err, 2)) << err;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT attempt_count FROM exec_outbox_message WHERE outbox_id=5001;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 2);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cEndToEndWorkflowSeedProbeWithRestartMidRun) {
    using namespace simcore::db::migrations;
    using namespace simcore::db::execution::workflow;
    using namespace simcore::runner::parallel::simcoredb;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(9901, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'stage3c-e2e', unixepoch(), unixepoch());
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, priority, attempts, max_attempts, ready_at_utc, created_at_utc)
VALUES
  (9911,9901,'Neutral','seedprobe.neutral','READY',10,0,2,unixepoch(),unixepoch()),
  (9912,9901,'Grid','seedprobe.grid','WAITING',8,0,2,NULL,unixepoch()),
  (9913,9901,'Unique','seedprobe.unique','WAITING',7,0,2,NULL,unixepoch()),
  (9914,9901,'Done','seedprobe.done','WAITING',1,0,1,NULL,unixepoch());
INSERT INTO exec_workflow_edge(workflow_edge_id, workflow_instance_id, from_step_id, to_step_id, created_at_utc)
VALUES
  (9921,9901,9911,9912,unixepoch()),
  (9922,9901,9912,9913,unixepoch()),
  (9923,9901,9913,9914,unixepoch());
)SQL"));

    simcore::db::execution::workflow::SqliteExecutionDb execution_db(db_);
    StaticWorkflowModeProvider mode_provider({ .mode = WorkflowExecutionMode::Workflow, .source = "stage3c-item15-test" });

    std::int64_t next_job_set_id = 12000;
    auto schedule = [&](const WorkflowReadyStep& step) {
        const auto job_set_id = ++next_job_set_id;
        const std::string sql =
            "INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc) VALUES("
            + std::to_string(job_set_id) + ", 1, 'stage3c-item15', unixepoch());";
        const bool ok = ExecSql(db_, sql.c_str());
        EXPECT_TRUE(ok);
        return ScheduledJobSet{
            .job_set_id = job_set_id,
            .workflow_step_id = step.workflow_step_id,
        };
    };

    DBWorkflowWorkerCoordinator coordinator(
        &execution_db,
        &mode_provider,
        DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = 0,
        },
        CoordinatorIntegrationConfig{

        },
        schedule);

    auto insert_completed_job = [&](std::int64_t job_id, std::int64_t job_set_id, const char* fingerprint) {
        const std::string sql =
            "INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc) VALUES("
            + std::to_string(job_id)
            + ","
            + std::to_string(job_set_id)
            + ",1,1,'seedprobe',1,'"
            + fingerprint
            + "',10,'COMPLETED',1,2,unixepoch(),unixepoch());";
        ASSERT_TRUE(ExecSql(db_, sql.c_str()));
    };

    const auto neutral = coordinator.MaterializeWorkflowStep({
        .workflow_instance_id = 9901,
        .workflow_step_id = 9911,
        .step_key = "Neutral",
        .step_kind = "seedprobe.neutral",
        .priority = 10,
    });
    ASSERT_TRUE(neutral.has_value());
    EXPECT_TRUE(coordinator.PublishTerminalJobSet({
        .workflow_instance_id = 9901,
        .workflow_step_id = 9911,
        .job_set_id = neutral->job_set_id,
        .terminal_state = "COMPLETED",
    }));

    ASSERT_TRUE(ExecSql(db_, "UPDATE exec_workflow_step SET state='READY', ready_at_utc=unixepoch() WHERE workflow_step_id=9912;"));

    const auto grid = coordinator.MaterializeWorkflowStep({
        .workflow_instance_id = 9901,
        .workflow_step_id = 9912,
        .step_key = "Grid",
        .step_kind = "seedprobe.grid",
        .priority = 8,
    });
    ASSERT_TRUE(grid.has_value());

    // Simulate restart in the middle: process exits after materialization, before terminal callback.
    DBWorkflowWorkerCoordinator after_restart(
        &execution_db,
        &mode_provider,
        DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = 0,
        },
        CoordinatorIntegrationConfig{

        },
        schedule);

    insert_completed_job(13001, grid->job_set_id, "item15-grid");

    WorkflowRecoveryService recovery(db_);
    WorkflowRecoveryResult recovery_result{};
    ASSERT_TRUE(recovery.ReconcileInFlightInstances(&recovery_result, &err)) << err;
    EXPECT_EQ(recovery_result.completed_steps, 1);

    ASSERT_TRUE(ExecSql(db_, "UPDATE exec_workflow_step SET state='READY', ready_at_utc=unixepoch() WHERE workflow_step_id=9913;"));
    const auto unique = after_restart.MaterializeWorkflowStep({
        .workflow_instance_id = 9901,
        .workflow_step_id = 9913,
        .step_key = "Unique",
        .step_kind = "seedprobe.unique",
        .priority = 7,
    });
    ASSERT_TRUE(unique.has_value());
    EXPECT_TRUE(after_restart.PublishTerminalJobSet({
        .workflow_instance_id = 9901,
        .workflow_step_id = 9913,
        .job_set_id = unique->job_set_id,
        .terminal_state = "COMPLETED",
    }));
    EXPECT_FALSE(after_restart.PublishTerminalJobSet({
        .workflow_instance_id = 9901,
        .workflow_step_id = 9913,
        .job_set_id = unique->job_set_id,
        .terminal_state = "COMPLETED",
    }));

    ASSERT_TRUE(ExecSql(db_, "UPDATE exec_workflow_step SET state='READY', ready_at_utc=unixepoch() WHERE workflow_step_id=9914;"));
    const auto done = after_restart.MaterializeWorkflowStep({
        .workflow_instance_id = 9901,
        .workflow_step_id = 9914,
        .step_key = "Done",
        .step_kind = "seedprobe.done",
        .priority = 1,
    });
    ASSERT_TRUE(done.has_value());
    EXPECT_TRUE(after_restart.PublishTerminalJobSet({
        .workflow_instance_id = 9901,
        .workflow_step_id = 9914,
        .job_set_id = done->job_set_id,
        .terminal_state = "COMPLETED",
    }));

    insert_completed_job(13000, neutral->job_set_id, "item15-neutral");
    insert_completed_job(13002, unique->job_set_id, "item15-unique");
    insert_completed_job(13003, done->job_set_id, "item15-done");

    WorkflowRecoveryResult final_recovery_result{};
    ASSERT_TRUE(recovery.ReconcileInFlightInstances(&final_recovery_result, &err)) << err;
    EXPECT_EQ(final_recovery_result.completed_steps, 3);

    ASSERT_TRUE(ExecSql(db_, "UPDATE exec_workflow_instance SET state='COMPLETED', completed_at_utc=unixepoch() WHERE workflow_instance_id=9901;"));

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM exec_workflow_step WHERE workflow_instance_id=9901 AND state='COMPLETED';",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 4);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_instance_id=9901 AND event_kind='Execution.WorkflowStepMaterialized.v1';",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 4);
    sqlite3_finalize(st);

    std::vector<WorkflowOutcomeItem> workflow_outcomes{
        { .step_key = "Neutral", .outcome = "COMPLETED" },
        { .step_key = "Grid", .outcome = "COMPLETED" },
        { .step_key = "Unique", .outcome = "COMPLETED" },
        { .step_key = "Done", .outcome = "COMPLETED" },
    };
    EXPECT_EQ(workflow_outcomes.size(), 4);

    WorkflowProjector projector(db_);
    ASSERT_TRUE(projector.ProjectFromOutbox("WorkflowProjector", 1000, &err)) << err;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT state FROM ui_workflow_instance WHERE workflow_instance_id=9901;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))), "COMPLETED");
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3dExecutionJobCommandServiceEmitsEventsOneThroughEight) {
    using namespace simcore::db::execution::jobs;
    using namespace simcore::db::execution::workflow;
    using namespace simcore::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(501, 1, 'stage3d', unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES(601, 501, 1, 1, 'seed_probe', 10, 'fp-stage3d-601', 5, 'QUEUED', 0, 3, unixepoch()*1000);
)SQL"));

    simcore::db::execution::workflow::SqliteExecutionDb execution_db(db_);
    auto* job_commands = execution_db.JobCommandService();
    ASSERT_NE(job_commands, nullptr);

    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobSetCreated, .job_set_id = 501 }, &err)) << err;
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobQueued, .job_id = 601 }, &err)) << err;
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobClaimed, .job_id = 601, .claimed_by_token = std::string("worker-1"), .lease_expires_at_utc = 2000000 }, &err)) << err;
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobLeaseRenewed, .job_id = 601, .lease_expires_at_utc = 3000000 }, &err)) << err;
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobProgressed, .job_id = 601, .message = std::string("50%") }, &err)) << err;
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobCompleted, .job_id = 601 }, &err)) << err;
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobEventArchived, .job_id = 601, .message = std::string("archived") }, &err)) << err;
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobRestored, .job_id = 601, .message = std::string("restored") }, &err)) << err;

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT COUNT(1) FROM exec_outbox_message WHERE event_type LIKE 'Execution.Job%.v1';", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 8);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT COUNT(1) FROM exec_outbox_message WHERE payload_ref_kind='job' AND payload_ref_id=601;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 7);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3dExecutionPayloadResolverReadsJobSetAndJobPayloads) {
    using namespace simcore::db::execution::workflow;
    using namespace simcore::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(701, 1, 'payload', unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES(801, 701, 1, 1, 'seed_probe', 11, 'fp-stage3d-801', 5, 'QUEUED', 0, 2, unixepoch()*1000);
)SQL"));

    simcore::db::execution::workflow::SqliteExecutionDb execution_db(db_);

    const auto from_job_set = execution_db.ResolveExecutionWorkflowJobPayload(
        "Execution.JobSetCreated.v1", 1, "job_set", 701);
    ASSERT_TRUE(from_job_set.has_value());
    EXPECT_EQ(from_job_set->job_set_id, 701);
    EXPECT_EQ(from_job_set->job_id, 0);

    const auto from_job = execution_db.ResolveExecutionWorkflowJobPayload(
        "Execution.JobQueued.v1", 1, "job", 801);
    ASSERT_TRUE(from_job.has_value());
    EXPECT_EQ(from_job->job_set_id, 701);
    EXPECT_EQ(from_job->job_id, 801);

    simcore::db::events::EventEnvelope invalid{};
    invalid.event_type = "Execution.JobQueued.v1";
    invalid.event_version = 1;
    invalid.context_name = "Execution";
    invalid.aggregate_kind = "job";
    invalid.payload_ref_kind = "workflow_event";
    invalid.payload_ref_id = 801;
    EXPECT_FALSE(execution_db.ResolveExecutionWorkflowJobPayload(invalid).has_value());
}

TEST_F(SqliteDbFixture, Stage0ExecutionPayloadResolverReadsWorkflowInputEventPayloads) {
    using namespace simcore::db::execution::workflow;
    using namespace simcore::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(
    workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc
)
VALUES(9101, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'stage0-input', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(9102, 1, 'stage0-input', unixepoch()*1000);
INSERT INTO exec_workflow_step(
    workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, created_at_utc, ready_at_utc
)
VALUES(9103, 9101, 'seedprobe.grid', 'seedprobe.grid', 'READY', 9102, unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_input_event(
    workflow_input_event_id, workflow_instance_id, workflow_step_id, event_kind, source_key, request_id, event_ts_utc
)
VALUES(9104, 9101, 9103, 'Execution.WorkflowStepInputFragmentReady.v1', 'state', 'req-9104', unixepoch()*1000);
)SQL"));

    SqliteExecutionDb execution_db(db_);

    const auto from_input_event = execution_db.ResolveExecutionWorkflowJobPayload(
        "Execution.WorkflowStepInputFragmentReady.v1", 1, "workflow_input_event", 9104);
    ASSERT_TRUE(from_input_event.has_value());
    EXPECT_EQ(from_input_event->workflow_instance_id, 9101);
    EXPECT_EQ(from_input_event->workflow_step_id, 9103);
    EXPECT_EQ(from_input_event->job_set_id, 9102);
    EXPECT_EQ(from_input_event->job_id, 0);

    const auto legacy_from_workflow_event = execution_db.ResolveExecutionWorkflowJobPayload(
        "Execution.WorkflowStepCompleted.v1", 1, "workflow_event", 9104);
    EXPECT_FALSE(legacy_from_workflow_event.has_value());
}

TEST_F(SqliteDbFixture, Stage0ReplayBackfillValidationResolvesWorkflowAndWorkflowInputPayloadRefs) {
    using namespace simcore::db::events;
    using namespace simcore::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(
    workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc
)
VALUES(9201, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'stage0-replay', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(
    workflow_step_id, workflow_instance_id, step_key, step_kind, state, created_at_utc, ready_at_utc
)
VALUES(9202, 9201, 'seedprobe.neutral', 'seedprobe.neutral', 'READY', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_event(workflow_event_id, workflow_instance_id, workflow_step_id, event_kind, event_ts_utc, message)
VALUES(9203, 9201, 9202, 'Execution.WorkflowStepReady.v1', unixepoch()*1000, 'ready');
INSERT INTO exec_workflow_input_event(workflow_input_event_id, workflow_instance_id, workflow_step_id, event_kind, event_ts_utc, source_key, request_id)
VALUES(9204, 9201, 9202, 'Execution.WorkflowStepInputRequested.v1', unixepoch()*1000, 'state', 'req-9204');
INSERT INTO exec_outbox_message(
    outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id
)
VALUES
    (9205,'evt-stage0-replay-1','Execution.WorkflowStepReady.v1',1,'Execution','workflow_step','9202','corr-9201','cause-9201',unixepoch()*1000,'workflow_event',9203),
    (9206,'evt-stage0-replay-2','Execution.WorkflowStepInputRequested.v1',1,'Execution','workflow_step','9202','corr-9201','cause-9201',unixepoch()*1000,'workflow_input_event',9204);
)SQL"));

    simcore::db::execution::workflow::SqliteExecutionDb execution_db(db_);
    int unresolved_count = 0;

    OutboxRelay relay({
        .db = db_,
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
                if (handler_error) *handler_error = "unresolved payload";
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
                if (handler_error) *handler_error = "unresolved payload";
                return false;
            }
            return true;
        },
    });

    OutboxRelayResult result{};
    ASSERT_TRUE(relay.RelayBatch(0, 10, bindings, &result, &err)) << err;
    EXPECT_EQ(result.failure_count, 0);
    EXPECT_EQ(unresolved_count, 0);
    EXPECT_EQ(result.published_count, 2);
}

TEST_F(SqliteDbFixture, Stage3dAnalysisBattleCommandsEmitEventsThirtyThroughThirtySix) {
    using namespace simcore::db;
    using namespace simcore::db::analysis;
    using namespace simcore::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::AnalysisBattle, embedded_options, &err)) << err;

    SqliteAnalysisDb analysis_db(db_);

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));
    std::int64_t battle_set_id = 0;
    ASSERT_TRUE(analysis_db.CreateBattleSet(
        {
            .name = "stage3d-battle-set",
            .entry_savestate_id = 101,
            .battle_run_spec_id = 202,
            .explorer_settings_id = 303,
            .status = "ACTIVE",
            .created_at_utc = now,
            .event_id = "ab-event-30",
            .correlation_id = "ab-corr-1",
            .causation_id = "ab-cause-1",
        },
        &battle_set_id,
        &err))
        << err;
    ASSERT_GT(battle_set_id, 0);

    std::int64_t seed_candidate_id = 0;
    ASSERT_TRUE(analysis_db.AddBattleSeedCandidate(
        {
            .battle_set_id = battle_set_id,
            .source_unique_seed_id = 444,
            .seed_value = 555,
            .source_kind = "SP_UNIQUE",
            .candidate_status = "PENDING",
            .created_at_utc = now,
            .event_id = "ab-event-31",
            .correlation_id = "ab-corr-1",
            .causation_id = "ab-cause-2",
        },
        &seed_candidate_id,
        &err))
        << err;

    std::int64_t wave_id = 0;
    ASSERT_TRUE(analysis_db.CreateBattleTurnWave(
        {
            .battle_set_id = battle_set_id,
            .turn_index = 1,
            .seed_candidate_id = seed_candidate_id,
            .status = "RUNNING",
            .created_at_utc = now,
            .event_id = "ab-event-32",
            .correlation_id = "ab-corr-1",
            .causation_id = "ab-cause-3",
        },
        &wave_id,
        &err))
        << err;

    std::int64_t turn_job_id = 0;
    ASSERT_TRUE(analysis_db.RecordBattleTurnJob(
        {
            .wave_id = wave_id,
            .exec_job_id = 7001,
            .plan_id = 9001,
            .fake_attacks_this_turn = 2,
            .fake_attacks_used_before = 1,
            .job_state = "COMPLETED",
            .has_results = true,
            .battle_outcome = 1,
            .recorded_at_utc = now,
            .event_id = "ab-event-33",
            .correlation_id = "ab-corr-1",
            .causation_id = "ab-cause-4",
        },
        &turn_job_id,
        &err))
        << err;

    std::int64_t selection_pool_id = 0;
    ASSERT_TRUE(analysis_db.CreateBattleSelectionPool(
        {
            .battle_set_id = battle_set_id,
            .turn_index = 1,
            .pool_name = "pool-a",
            .criterion_kind = "MAX_VI",
            .created_at_utc = now,
            .event_id = "ab-event-34",
            .correlation_id = "ab-corr-1",
            .causation_id = "ab-cause-5",
        },
        &selection_pool_id,
        &err))
        << err;

    std::int64_t selection_decision_id = 0;
    ASSERT_TRUE(analysis_db.RecordBattleSelectionDecision(
        {
            .selection_pool_id = selection_pool_id,
            .turn_job_id = turn_job_id,
            .decision_kind = "WINNER",
            .decision_reason = std::string("best vi"),
            .created_at_utc = now,
            .event_id = "ab-event-35",
            .correlation_id = "ab-corr-1",
            .causation_id = "ab-cause-6",
        },
        &selection_decision_id,
        &err))
        << err;

    std::int64_t terminal_followup_id = 0;
    ASSERT_TRUE(analysis_db.UpsertBattleTerminalFollowup(
        {
            .turn_job_id = turn_job_id,
            .is_victory = true,
            .manual_followup_status = "RECORDED",
            .recorded_dtm_artifact_id = 777,
            .note = std::string("stage3d"),
            .updated_at_utc = now,
            .event_id = "ab-event-36",
            .correlation_id = "ab-corr-1",
            .causation_id = "ab-cause-7",
        },
        &terminal_followup_id,
        &err))
        << err;
    EXPECT_GT(terminal_followup_id, 0);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT COUNT(1) FROM ab_outbox_message;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 7);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT COUNT(1) FROM ab_outbox_message WHERE context_name='AnalysisBattle';", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 7);
    sqlite3_finalize(st);

    const auto payload_30 = analysis_db.ResolveBattlePayload(1, "battle_set", battle_set_id);
    ASSERT_TRUE(payload_30.has_value());
    EXPECT_EQ(payload_30->battle_set_id, battle_set_id);

    const auto payload_31 = analysis_db.ResolveBattlePayload(1, "seed_candidate", seed_candidate_id);
    ASSERT_TRUE(payload_31.has_value());
    EXPECT_EQ(payload_31->battle_set_id, battle_set_id);

    const auto payload_32 = analysis_db.ResolveBattlePayload(1, "turn_wave", wave_id);
    ASSERT_TRUE(payload_32.has_value());
    EXPECT_EQ(payload_32->wave_id, wave_id);

    const auto payload_33 = analysis_db.ResolveBattlePayload(1, "turn_job", turn_job_id);
    ASSERT_TRUE(payload_33.has_value());
    EXPECT_EQ(payload_33->turn_job_id, turn_job_id);

    const auto payload_34 = analysis_db.ResolveBattlePayload(1, "selection_pool", selection_pool_id);
    ASSERT_TRUE(payload_34.has_value());
    EXPECT_EQ(payload_34->battle_set_id, battle_set_id);

    const auto payload_35 = analysis_db.ResolveBattlePayload(1, "selection_decision", selection_decision_id);
    ASSERT_TRUE(payload_35.has_value());
    EXPECT_EQ(payload_35->battle_set_id, battle_set_id);
    EXPECT_EQ(payload_35->turn_job_id, turn_job_id);

    const auto payload_36 = analysis_db.ResolveBattlePayload(1, "terminal_followup", terminal_followup_id);
    ASSERT_TRUE(payload_36.has_value());
    EXPECT_EQ(payload_36->battle_set_id, battle_set_id);
    EXPECT_EQ(payload_36->turn_job_id, turn_job_id);
}

TEST_F(SqliteDbFixture, Stage3dAnalysisSeedProbeSetCreateEmitsEventTwentyThree) {
    using namespace simcore::db;
    using namespace simcore::db::analysis;
    using namespace simcore::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::AnalysisSeedProbe, embedded_options, &err)) << err;

    SqliteAnalysisDb analysis_db(db_);
    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));

    std::int64_t probe_set_id = 0;
    ASSERT_TRUE(analysis_db.CreateSeedProbeSet(
        {
            .name = "stage3d-probe-set",
            .probe_flavor = "BATTLE_PRE",
            .breakpoint_policy_name = "bp-default",
            .segment_source_kind = "FILE",
            .created_at_utc = now,
            .event_id = "sp-event-23",
            .correlation_id = "sp-corr-1",
            .causation_id = "sp-cause-1",
        },
        &probe_set_id,
        &err))
        << err;
    EXPECT_GT(probe_set_id, 0);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT COUNT(1) FROM sp_outbox_message WHERE event_type='AnalysisSeedProbe.SetCreated.v1';", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 1);
    sqlite3_finalize(st);

    const auto payload = analysis_db.ResolveSeedProbePayload(1, "probe_set", probe_set_id);
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(payload->probe_set_id, probe_set_id);
}

TEST_F(SqliteDbFixture, Stage3dArchiveCommandsEmitEventsFortyFourThroughFortyEight) {
    using namespace simcore::db;
    using namespace simcore::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Archive, embedded_options, &err)) << err;

    SqliteArchiveDb archive_db(db_);
    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));

    std::int64_t archive_package_id = 0;
    ASSERT_TRUE(archive_db.CreateArchivePackage(
        {
            .source_context = "Execution",
            .source_root_job_set_id = 9001,
            .created_at_utc = now,
            .schema_version = 1,
            .event_catalog_version = 1,
            .time_range_start_utc = now,
            .time_range_end_utc = now,
            .manifest_path = "manifest.json",
            .checksum_status = "PENDING",
            .event_id = "ar-event-44",
            .correlation_id = "ar-corr-1",
            .causation_id = "ar-cause-1",
        },
        &archive_package_id,
        &err))
        << err;
    ASSERT_GT(archive_package_id, 0);

    std::int64_t archive_item_id = 0;
    ASSERT_TRUE(archive_db.AddArchiveItem(
        {
            .archive_package_id = archive_package_id,
            .item_kind = "exec_job_event",
            .item_count = 4,
            .blob_path = std::string("jobs.jsonl"),
            .checksum = std::string("abc123"),
            .indexed_at_utc = now,
            .event_id = "ar-event-45",
            .correlation_id = "ar-corr-1",
            .causation_id = "ar-cause-2",
        },
        &archive_item_id,
        &err))
        << err;
    ASSERT_GT(archive_item_id, 0);

    std::int64_t rehydrate_request_id = 0;
    ASSERT_TRUE(archive_db.RequestRehydrate(
        {
            .archive_package_id = archive_package_id,
            .status = "REQUESTED",
            .requested_at_utc = now,
            .target_namespace = "test",
            .event_id = "ar-event-46",
            .correlation_id = "ar-corr-1",
            .causation_id = "ar-cause-3",
        },
        &rehydrate_request_id,
        &err))
        << err;

    ASSERT_TRUE(archive_db.CompleteRehydrate(
        {
            .rehydrate_request_id = rehydrate_request_id,
            .status = "COMPLETED",
            .completed_at_utc = now,
            .entity_mappings = {
                { .entity_kind = "job", .old_id = "12", .new_id = "1012" },
            },
            .event_id = "ar-event-47",
            .correlation_id = "ar-corr-1",
            .causation_id = "ar-cause-4",
        },
        &err))
        << err;

    std::int64_t failed_request_id = 0;
    ASSERT_TRUE(archive_db.RequestRehydrate(
        {
            .archive_package_id = archive_package_id,
            .status = "REQUESTED",
            .requested_at_utc = now,
            .target_namespace = "test",
            .event_id = "ar-event-46b",
            .correlation_id = "ar-corr-1",
            .causation_id = "ar-cause-5",
        },
        &failed_request_id,
        &err))
        << err;
    ASSERT_TRUE(archive_db.FailRehydrate(
        {
            .rehydrate_request_id = failed_request_id,
            .status = "FAILED",
            .completed_at_utc = now,
            .error_text = "simulated failure",
            .event_id = "ar-event-48",
            .correlation_id = "ar-corr-1",
            .causation_id = "ar-cause-6",
        },
        &err))
        << err;

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT COUNT(1) FROM ar_outbox_message;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 6);
    sqlite3_finalize(st);

    const auto payload_package = archive_db.ResolveArchivePayload(1, "archive_package", archive_package_id);
    ASSERT_TRUE(payload_package.has_value());
    EXPECT_EQ(payload_package->archive_package_id, archive_package_id);

    const auto payload_item = archive_db.ResolveArchivePayload(1, "archive_item", archive_item_id);
    ASSERT_TRUE(payload_item.has_value());
    EXPECT_EQ(payload_item->archive_item_id, archive_item_id);
}

TEST_F(SqliteDbFixture, Stage4ArchiveOperatorCommandsPackageCountsAndChecksumValidation) {
    using namespace simcore::db;
    using namespace simcore::db::migrations;
    using namespace simcore::runner::parallel::simcoredb;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Archive, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc) VALUES(100,1,'root',1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc)
VALUES(200,100,1,1,'workflow',100,'fp-200',0,'COMPLETED',0,1,1000,2000);
INSERT INTO exec_job_event(job_event_id, job_id, event_kind, event_ts_utc, message) VALUES(300,200,'done',2000,'ok');
)SQL"));

    const auto temp_root = std::filesystem::temp_directory_path() / ("soasim-archive-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(temp_root));

    simcore::db::execution::workflow::SqliteExecutionDb execution_db(db_);
    SqliteUiReadDb ui_read_db(db_);
    SqliteArchiveDb archive_db(db_);
    simcore::db::archive::SqliteArchivePackageService package_service(
        db_,
        &execution_db,
        &ui_read_db,
        &archive_db,
        DbConfigPaths{ .archive_store_root = temp_root });

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));
    const auto package = package_service.CreatePackage({
        .source_root_job_set_id = 100,
        .created_at_utc = now,
        .event_id = "stage4-package-1",
        .correlation_id = "stage4-corr",
        .causation_id = "stage4-cause",
    });
    ASSERT_TRUE(package.success) << package.error.value_or("unknown error");

    simcore::db::archive::SqliteRehydrateExecutor rehydrate_executor(db_, db_, &archive_db, temp_root);
    ArchiveWorkflowCommands commands(db_, &archive_db, &package_service, &rehydrate_executor);

    const auto verify_pass = commands.PackageVerify({ .archive_package_id = package.archive_package_id });
    EXPECT_TRUE(verify_pass.success);
    EXPECT_GT(verify_pass.manifest_row_total, 0);
    EXPECT_EQ(verify_pass.manifest_row_total, verify_pass.archive_item_row_total);

    const auto jobs_file = package.package_root / "data" / "jobs.jsonl";
    ASSERT_TRUE(std::filesystem::exists(jobs_file));
    {
        std::ofstream out(jobs_file, std::ios::out | std::ios::trunc);
        out << "{\"job_id\":200}\n";
    }

    const auto verify_fail = commands.PackageVerify({ .archive_package_id = package.archive_package_id });
    EXPECT_FALSE(verify_fail.success);
    EXPECT_FALSE(verify_fail.blocking_reasons.empty());
    EXPECT_NE(std::find_if(
        verify_fail.blocking_reasons.begin(),
        verify_fail.blocking_reasons.end(),
        [](const std::string& r) { return r.find("checksum_mismatch") != std::string::npos; }),
        verify_fail.blocking_reasons.end());

    std::filesystem::remove_all(temp_root);
}

TEST_F(SqliteDbFixture, Stage4ArchiveOperatorCommandsRehydrateNoCollisionAndRoundtripAndCleanup) {
    using namespace simcore::db;
    using namespace simcore::db::migrations;
    using namespace simcore::runner::parallel::simcoredb;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Archive, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc) VALUES(100,1,'root',1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc)
VALUES(200,100,1,1,'workflow',100,'fp-200',0,'COMPLETED',0,1,1000,2000);
INSERT INTO exec_job_event(job_event_id, job_id, event_kind, event_ts_utc, message) VALUES(300,200,'done',2000,'ok');
)SQL"));

    const auto temp_root = std::filesystem::temp_directory_path() / ("soasim-rehydrate-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(temp_root));

    simcore::db::execution::workflow::SqliteExecutionDb execution_db(db_);
    SqliteUiReadDb ui_read_db(db_);
    SqliteArchiveDb archive_db(db_);
    simcore::db::archive::SqliteArchivePackageService package_service(
        db_,
        &execution_db,
        &ui_read_db,
        &archive_db,
        DbConfigPaths{ .archive_store_root = temp_root });

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));
    const auto package = package_service.CreatePackage({
        .source_root_job_set_id = 100,
        .created_at_utc = now,
        .event_id = "stage4-roundtrip-1",
        .correlation_id = "stage4-roundtrip",
        .causation_id = "stage4-roundtrip",
    });
    ASSERT_TRUE(package.success) << package.error.value_or("unknown error");

    ASSERT_TRUE(ExecSql(
        db_,
        "INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc) "
        "VALUES(201,100,1,1,'workflow',101,'fp-live',0,'READY',0,1,3000);"));
    ASSERT_TRUE(ExecSql(db_, "DELETE FROM exec_job_event WHERE job_id=200; DELETE FROM exec_job WHERE job_id=200; DELETE FROM exec_job_set WHERE job_set_id=100;"));

    simcore::db::archive::SqliteRehydrateExecutor rehydrate_executor(db_, db_, &archive_db, temp_root);
    ArchiveWorkflowCommands commands(db_, &archive_db, &package_service, &rehydrate_executor);

    const auto rehydrate_summary = commands.RehydrateExecute({
        .archive_package_id = package.archive_package_id,
        .now_utc = now,
        .target_namespace = "stage4",
        .event_id_prefix = "stage4-rh",
    });
    const auto join_messages = [](const std::vector<std::string>& values) {
        std::ostringstream out;
        for (size_t i = 0; i < values.size(); ++i) {
            if (i > 0) out << " | ";
            out << values[i];
        }
        return out.str();
    };

    std::string rehydrate_status;
    std::string rehydrate_error_text;
    {
        sqlite3_stmt* status_st = nullptr;
        ASSERT_EQ(
            SQLITE_OK,
            sqlite3_prepare_v2(
                db_,
                "SELECT status, COALESCE(error_text, '') FROM ar_rehydrate_request ORDER BY rehydrate_request_id DESC LIMIT 1;",
                -1,
                &status_st,
                nullptr));
        if (sqlite3_step(status_st) == SQLITE_ROW) {
            const auto* status = reinterpret_cast<const char*>(sqlite3_column_text(status_st, 0));
            const auto* err_text = reinterpret_cast<const char*>(sqlite3_column_text(status_st, 1));
            if (status != nullptr) rehydrate_status = status;
            if (err_text != nullptr) rehydrate_error_text = err_text;
        }
        sqlite3_finalize(status_st);
    }

    ASSERT_TRUE(rehydrate_summary.success)
        << "RehydrateExecute failed"
        << "\nsummary.errors=" << join_messages(rehydrate_summary.errors)
        << "\nsummary.blocking_reasons=" << join_messages(rehydrate_summary.blocking_reasons)
        << "\nlatest_request.status=" << rehydrate_status
        << "\nlatest_request.error_text=" << rehydrate_error_text;
    ASSERT_EQ(rehydrate_summary.request_ids.size(), 1u);

    sqlite3_stmt* st = nullptr;

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT old_id, new_id FROM ar_rehydrate_map WHERE rehydrate_request_id=?1 AND entity_kind='job';", -1, &st, nullptr));
    sqlite3_bind_int64(st, 1, rehydrate_summary.request_ids.front());
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    const auto old_id = sqlite3_column_int64(st, 0);
    const auto new_id = sqlite3_column_int64(st, 1);
    EXPECT_EQ(200, old_id);
    EXPECT_NE(200, new_id);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT fingerprint FROM exec_job WHERE job_id=?1;", -1, &st, nullptr));
    sqlite3_bind_int64(st, 1, new_id);
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    const auto* new_fingerprint_text = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    ASSERT_NE(nullptr, new_fingerprint_text);
    const std::string new_fingerprint = new_fingerprint_text;
    EXPECT_FALSE(new_fingerprint.empty());
    EXPECT_NE("fp-200", new_fingerprint);
    EXPECT_NE("fp-live", new_fingerprint);
    sqlite3_finalize(st);

    const auto cleanup = commands.RehydrateCleanup({ .rehydrate_request_id = rehydrate_summary.request_ids.front() });
    EXPECT_TRUE(cleanup.success);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT COUNT(1) FROM ar_rehydrate_request WHERE rehydrate_request_id=?1;", -1, &st, nullptr));
    sqlite3_bind_int64(st, 1, rehydrate_summary.request_ids.front());
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(0, sqlite3_column_int(st, 0));
    sqlite3_finalize(st);

    std::filesystem::remove_all(temp_root);
}

TEST_F(SqliteDbFixture, Stage4ArchiveOperatorCommandsArchivePreviewRespectsSubscriberFloorSafety) {
    using namespace simcore::db;
    using namespace simcore::db::migrations;
    using namespace simcore::runner::parallel::simcoredb;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Archive, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc) VALUES(100,1,'root',1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc)
VALUES(200,100,1,1,'workflow',100,'fp-200',0,'COMPLETED',0,1,1000,2000);
INSERT INTO exec_outbox_message(outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,occurred_at_utc,payload_ref_kind,payload_ref_id,published_at_utc)
VALUES(10,'evt-10','Execution.JobQueued.v1',1,'Execution','job','200',1000,'job',200,1100),
      (20,'evt-20','Execution.JobCompleted.v1',1,'Execution','job','200',2000,'job',200,2100);
INSERT INTO ui_projection_subscription(projector_name,source_context,source_outbox_table,last_outbox_id,last_event_id,updated_at_utc,status,last_error)
VALUES('WorkflowProjector','Execution','exec_outbox_message',15,'evt-15',3000,'ACTIVE','');
)SQL"));

    const auto temp_root = std::filesystem::temp_directory_path() / ("soasim-floor-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(temp_root));

    simcore::db::execution::workflow::SqliteExecutionDb execution_db(db_);
    SqliteUiReadDb ui_read_db(db_);
    SqliteArchiveDb archive_db(db_);
    simcore::db::archive::SqliteArchivePackageService package_service(
        db_,
        &execution_db,
        &ui_read_db,
        &archive_db,
        DbConfigPaths{ .archive_store_root = temp_root });
    simcore::db::archive::SqliteRehydrateExecutor rehydrate_executor(db_, db_, &archive_db, temp_root);
    ArchiveWorkflowCommands commands(db_, &archive_db, &package_service, &rehydrate_executor);

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));
    const auto preview = commands.ArchivePreview({
        .older_than_utc = types::UtcTimePoint(std::chrono::milliseconds(1712305000000)),
        .now_utc = now,
        .max_candidates = 10,
        .outbox_policy = { .paused_or_error_block_threshold = std::chrono::hours(2), .block_when_no_active_subscriptions = true },
    });

    EXPECT_TRUE(preview.success);
    EXPECT_EQ(preview.ui_safe_floor_outbox_id, 15);
    ASSERT_TRUE(preview.retention_safe_floor_outbox_id.has_value());
    EXPECT_EQ(preview.retention_safe_floor_outbox_id.value(), 15);

    std::filesystem::remove_all(temp_root);
}

}
