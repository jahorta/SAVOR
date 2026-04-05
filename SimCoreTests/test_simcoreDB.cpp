#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include "Common/Migrations/MigrationRunner.h"
#include "SimCoreDB.h"
#include "Execution/Workflow/ExecutionDb.h"
#include "Execution/Workflow/SeedProbeWorkflowDefinition.h"
#include "Execution/Workflow/WorkflowEngine.h"
#include "Execution/Workflow/WorkflowModeProvider.h"
#include "Execution/Workflow/WorkflowParityDiagnostics.h"
#include "Execution/Workflow/WorkflowProjector.h"
#include "Execution/Workflow/WorkflowRecoveryService.h"
#include "Runner/Parallel/SimCoreDB/WorkflowCoordinatorBridge.h"
#include "Runner/Parallel/SimCoreDB/DBWorkflowWorkerCoordinator.h"
#include "Runner/Parallel/SimCoreDB/WorkflowSchedulerAdapter.h"

namespace {

class SqliteDbFixture : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db_));
    }

    void TearDown() override {
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }

    sqlite3* db_ = nullptr;
};

} // namespace

namespace {

bool TableExists(sqlite3* db, const char* table_name) {
    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1;";

    if (sqlite3_prepare_v2(db, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(st, 1, table_name, -1, SQLITE_STATIC);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_ROW;
}

bool ColumnExists(sqlite3* db, const char* table_name, const char* column_name) {
    sqlite3_stmt* st = nullptr;
    const std::string sql = "PRAGMA table_info(" + std::string(table_name) + ");";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    bool found = false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(st, 1);
        if (text && std::string(reinterpret_cast<const char*>(text)) == column_name) {
            found = true;
            break;
        }
    }

    sqlite3_finalize(st);
    return found;
}

bool IndexExists(sqlite3* db, const char* index_name) {
    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT 1 FROM sqlite_master WHERE type='index' AND name=?1 LIMIT 1;";

    if (sqlite3_prepare_v2(db, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(st, 1, index_name, -1, SQLITE_STATIC);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_ROW;
}

std::string TableCreateSql(sqlite3* db, const char* table_name) {
    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT sql FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1;";
    if (sqlite3_prepare_v2(db, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return {};
    }

    sqlite3_bind_text(st, 1, table_name, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return {};
    }

    const unsigned char* text = sqlite3_column_text(st, 0);
    const std::string result = text ? reinterpret_cast<const char*>(text) : "";
    sqlite3_finalize(st);
    return result;
}

} // namespace

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

    std::string reason;
    EXPECT_FALSE(simcore::db::Stage3cWorkflowSliceReady(db_, &reason));
    EXPECT_NE(reason.find("Execution schema version"), std::string::npos);

    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;

    EXPECT_TRUE(simcore::db::Stage3cWorkflowSliceReady(db_, &reason)) << reason;
    EXPECT_EQ(reason, "OK");
}

TEST(DbMigrateMigrationsIntegration, DISABLED_FilesystemSourceHasMigrationPerContext) {
    namespace fs = std::filesystem;
    using namespace simcore::db::migrations;

    const auto root = fs::weakly_canonical(fs::path("../../SimCoreDB/migration"));
    const MigrationSourceOptions filesystem_options{
        .source_kind = MigrationSourceKind::Filesystem,
        .filesystem_root = root,
    };

    for (const auto context : ListAllMigrationContexts()) {
        const auto entries = LoadContextMigrations(context, filesystem_options);
        ASSERT_FALSE(entries.empty()) << "Expected at least one migration in context " << ToString(context);
    }
}


namespace {

bool ExecSql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

} // namespace

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
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, priority, attempts, max_attempts, created_at_utc)
VALUES
  (2001,1001,'Neutral','seedprobe.neutral','FAILED',10,0,2,unixepoch()),
  (2002,1001,'Grid','seedprobe.grid','WAITING',8,0,2,unixepoch()),
  (2003,1001,'Unique','seedprobe.unique','WAITING',7,0,2,unixepoch()),
  (2004,1001,'Done','seedprobe.done','WAITING',0,0,1,unixepoch());
INSERT INTO exec_workflow_edge(workflow_edge_id, workflow_instance_id, from_step_id, to_step_id, created_at_utc)
VALUES (3001,1001,2001,2002,unixepoch()),(3002,1001,2002,2003,unixepoch()),(3003,1001,2003,2004,unixepoch());
)SQL"));

    ExecutionDb execution_db(db_);
    ASSERT_NE(execution_db.WorkflowQueryService(), nullptr);
    ASSERT_NE(execution_db.WorkflowCommandService(), nullptr);

    const auto graph = execution_db.WorkflowQueryService()->GetWorkflowGraph(1001);
    ASSERT_TRUE(graph.has_value());
    EXPECT_EQ(graph->steps.size(), 4);

    std::string command_error;
    EXPECT_TRUE(execution_db.WorkflowCommandService()->RetryFailedStep({ .workflow_step_id = 2001, .requested_by = "test" }, &command_error)) << command_error;

    const auto map = execution_db.WorkflowQueryService()->GetStepToJobSetMap(1001);
    EXPECT_TRUE(map.empty());

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT state, attempts FROM exec_workflow_step WHERE workflow_step_id=2001;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))), "READY");
    EXPECT_EQ(sqlite3_column_int(st, 1), 1);
    sqlite3_finalize(st);
}

TEST(Stage3cSeedProbeDefinition, ValidatesAndRejectsCycleDefinitions) {
    using namespace simcore::db::execution::workflow;

    auto definition = BuildSeedProbeChainDefinition();
    std::string err;
    EXPECT_TRUE(ValidateWorkflowDefinition(definition, &err)) << err;

    definition.steps[0].dependencies.push_back("Done");
    EXPECT_FALSE(ValidateWorkflowDefinition(definition, &err));
    EXPECT_NE(err.find("cycle"), std::string::npos);
}

TEST(Stage3cWorkflowEngine, ResolveReadinessAndRecoveryTransitions) {
    using namespace simcore::db::execution::workflow;

    WorkflowGraphSnapshot snapshot;
    snapshot.instance.workflow_instance_id = 44;
    snapshot.steps = {
        WorkflowStepRecord{ .workflow_step_id = 1, .workflow_instance_id = 44, .step_key = "Neutral", .step_kind = "seedprobe.neutral", .state = WorkflowStepState::Completed },
        WorkflowStepRecord{ .workflow_step_id = 2, .workflow_instance_id = 44, .step_key = "Grid", .step_kind = "seedprobe.grid", .state = WorkflowStepState::Waiting },
        WorkflowStepRecord{ .workflow_step_id = 3, .workflow_instance_id = 44, .step_key = "Unique", .step_kind = "seedprobe.unique", .state = WorkflowStepState::Materialized, .job_set_id = 555 },
        WorkflowStepRecord{ .workflow_step_id = 4, .workflow_instance_id = 44, .step_key = "Done", .step_kind = "seedprobe.done", .state = WorkflowStepState::Waiting },
    };

    const auto definition = BuildSeedProbeChainDefinition();
    const auto ready_result = ResolveReadiness(snapshot, definition, {});
    ASSERT_EQ(ready_result.transitions.size(), 1);
    EXPECT_EQ(ready_result.transitions[0].workflow_step_id, 2);
    EXPECT_EQ(ready_result.transitions[0].to, WorkflowStepState::Ready);

    const auto reconcile_result = ReconcileRunningSteps(snapshot, { { 555, "FAILED" } });
    ASSERT_EQ(reconcile_result.transitions.size(), 1);
    EXPECT_EQ(reconcile_result.transitions[0].workflow_step_id, 3);
    EXPECT_EQ(reconcile_result.transitions[0].to, WorkflowStepState::Failed);
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
}

TEST(Stage3cWorkflowParityDiagnostics, ClassifiesMissingAndMismatchedOutcomes) {
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

    EXPECT_EQ(report.compared_steps, 3);
    EXPECT_EQ(report.matched_steps, 0);
    EXPECT_EQ(report.mismatches.size(), 3);
}

TEST(Stage3cWorkflowModeProvider, ParsesAndReturnsSelectedModes) {
    using namespace simcore::db::execution::workflow;

    StaticWorkflowModeProvider provider({ .mode = WorkflowExecutionMode::DualWriteObserve, .source = "unit-test" });
    const auto selection = provider.GetModeSelection();
    EXPECT_EQ(selection.mode, WorkflowExecutionMode::DualWriteObserve);
    EXPECT_EQ(selection.source, "unit-test");

    EXPECT_EQ(ParseWorkflowExecutionMode("WorkflowOnly", WorkflowExecutionMode::LegacyOnly), WorkflowExecutionMode::WorkflowOnly);
    EXPECT_EQ(ParseWorkflowExecutionMode("invalid", WorkflowExecutionMode::DualWriteObserve), WorkflowExecutionMode::DualWriteObserve);
}

TEST(Stage3cCoordinatorBridge, DeduplicatesTerminalSignalsAndSchedulesReadySteps) {
    using namespace simcore::runner::parallel::simcoredb;

    int terminal_count = 0;
    WorkflowCoordinatorBridge bridge;
    bridge.SetTerminalCallback([&](const TerminalJobSetSignal&) {
        ++terminal_count;
    });

    const TerminalJobSetSignal signal{
        .workflow_instance_id = 1,
        .workflow_step_id = 2,
        .job_set_id = 3,
        .terminal_state = "FAILED",
    };

    EXPECT_TRUE(bridge.NotifyTerminal(signal));
    EXPECT_FALSE(bridge.NotifyTerminal(signal));
    EXPECT_EQ(terminal_count, 1);

    WorkflowSchedulerAdapter adapter([](const WorkflowReadyStep& step) {
        return ScheduledJobSet{ .job_set_id = 1234, .workflow_step_id = step.workflow_step_id };
    });
    const auto scheduled = adapter.MaterializeReadyStep({ .workflow_instance_id = 9, .workflow_step_id = 44, .step_key = "Grid", .step_kind = "seedprobe.grid", .priority = 10 });
    EXPECT_EQ(scheduled.job_set_id, 1234);
    EXPECT_EQ(scheduled.workflow_step_id, 44);
}

TEST(Stage3cCoordinatorReplacement, MaterializesAndPublishesThroughWorkflowBridge) {
    using namespace simcore::runner::parallel::simcoredb;
    using namespace simcore::db::execution::workflow;

    StaticWorkflowModeProvider mode_provider({ .mode = WorkflowExecutionMode::WorkflowPrimary, .source = "unit-test" });

    DBWorkflowWorkerCoordinator coordinator(
        nullptr,
        &mode_provider,
        DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = 0,
        },
        CoordinatorIntegrationConfig{
            .mode = CoordinatorIntegrationMode::SimCoreDbWorkflow,
            .dual_write_observe = true,
        },
        [](const WorkflowReadyStep& step) {
            return ScheduledJobSet{
                .job_set_id = 7777 + step.workflow_step_id,
                .workflow_step_id = step.workflow_step_id,
            };
        });

    int materialized_callbacks = 0;
    int terminal_callbacks = 0;
    coordinator.SetWorkflowMaterializationCallback([&](std::int64_t workflow_step_id, std::int64_t job_set_id) {
        ++materialized_callbacks;
        EXPECT_EQ(workflow_step_id, 22);
        EXPECT_EQ(job_set_id, 7799);
    });
    coordinator.SetWorkflowTerminalCallback([&](const TerminalJobSetSignal& signal) {
        ++terminal_callbacks;
        EXPECT_EQ(signal.workflow_step_id, 22);
        EXPECT_EQ(signal.terminal_state, "COMPLETED");
    });

    const auto materialized = coordinator.MaterializeWorkflowStep({
        .workflow_instance_id = 11,
        .workflow_step_id = 22,
        .step_key = "Grid",
        .step_kind = "seedprobe.grid",
        .priority = 10,
    });
    ASSERT_TRUE(materialized.has_value());
    EXPECT_EQ(materialized->job_set_id, 7799);
    EXPECT_EQ(materialized_callbacks, 1);

    const TerminalJobSetSignal terminal{
        .workflow_instance_id = 11,
        .workflow_step_id = 22,
        .job_set_id = 7799,
        .terminal_state = "COMPLETED",
    };

    EXPECT_TRUE(coordinator.PublishTerminalJobSet(terminal));
    EXPECT_FALSE(coordinator.PublishTerminalJobSet(terminal));
    EXPECT_EQ(terminal_callbacks, 1);

    const auto status = coordinator.SnapshotStatus();
    EXPECT_EQ(status.running_workers, 1u);
    EXPECT_EQ(status.pending_start_workers, 1u);
}
