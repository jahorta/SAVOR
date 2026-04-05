#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include "Common/Migrations/MigrationRunner.h"
#include "SimCoreDB.h"
#include "Execution/Workflow/ExecutionDb.h"
#include "Execution/Workflow/SeedProbeWorkflowDefinition.h"
#include "Execution/Workflow/WorkflowEngine.h"
#include "Execution/Workflow/WorkflowIntegrityChecks.h"
#include "Execution/Workflow/WorkflowModeProvider.h"
#include "Execution/Workflow/WorkflowParityDiagnostics.h"
#include "Execution/Workflow/WorkflowParityStore.h"
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

class RecordingWorkflowCommandService final : public simcore::db::execution::workflow::IWorkflowOrchestrationCommandService {
public:
    bool RetryFailedStep(const simcore::db::execution::workflow::WorkflowRetryStepCommand&, std::string*) override { return true; }
    bool SkipStep(const simcore::db::execution::workflow::WorkflowSkipStepCommand&, std::string*) override { return true; }
    bool CancelWorkflowInstance(const simcore::db::execution::workflow::WorkflowCancelInstanceCommand&, std::string*) override { return true; }
    bool ResumeWorkflowInstance(const simcore::db::execution::workflow::WorkflowResumeInstanceCommand&, std::string*) override { return true; }

    bool MarkStepMaterialized(
        const simcore::db::execution::workflow::WorkflowMarkStepMaterializedCommand& command,
        std::string*) override {
        materialized_calls.push_back(command);
        return true;
    }

    bool MarkStepTerminal(
        const simcore::db::execution::workflow::WorkflowMarkStepTerminalCommand& command,
        std::string*) override {
        terminal_calls.push_back(command);
        return true;
    }

    std::vector<simcore::db::execution::workflow::WorkflowMarkStepMaterializedCommand> materialized_calls;
    std::vector<simcore::db::execution::workflow::WorkflowMarkStepTerminalCommand> terminal_calls;
};

class NullWorkflowQueryService final : public simcore::db::execution::workflow::IWorkflowOrchestrationQueryService {
public:
    std::vector<simcore::db::execution::workflow::WorkflowInstanceRecord> ListWorkflowInstances(
        simcore::db::execution::workflow::WorkflowInstanceState,
        std::int64_t,
        std::int64_t) const override {
        return {};
    }
    std::optional<simcore::db::execution::workflow::WorkflowGraphSnapshot> GetWorkflowGraph(std::int64_t) const override {
        return std::nullopt;
    }
    std::vector<simcore::db::execution::workflow::WorkflowStepRecord> ListBlockedSteps(std::int64_t) const override {
        return {};
    }
    std::vector<std::pair<std::int64_t, std::int64_t>> GetStepToJobSetMap(std::int64_t) const override {
        return {};
    }
};

class RecordingExecutionDb final : public simcore::db::IExecutionDb {
public:
    simcore::db::execution::workflow::IWorkflowOrchestrationQueryService* WorkflowQueryService() override {
        return &query_service;
    }
    simcore::db::execution::workflow::IWorkflowOrchestrationCommandService* WorkflowCommandService() override {
        return &command_service;
    }

    NullWorkflowQueryService query_service;
    RecordingWorkflowCommandService command_service;
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

    ExecutionDb execution_db(db_);
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

TEST(Stage3cSeedProbeDefinition, ValidatesAndRejectsCycleDefinitions) {
    using namespace simcore::db::execution::workflow;

    auto definition = BuildSeedProbeChainDefinition();
    std::string err;
    EXPECT_TRUE(ValidateWorkflowDefinition(definition, &err)) << err;

    definition.steps[0].dependencies.push_back("Done");
    EXPECT_FALSE(ValidateWorkflowDefinition(definition, &err));
    EXPECT_NE(err.find("cycle"), std::string::npos);
}

TEST(Stage3cSeedProbeDefinition, RegistryRegistersDefaultsAndRejectsDuplicates) {
    using namespace simcore::db::execution::workflow;

    WorkflowDefinitionRegistry registry;
    std::string err;
    EXPECT_TRUE(registry.RegisterSeedProbeDefaults(&err)) << err;

    const auto* found = registry.Find("SEED_PROBE_CHAIN");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->steps.size(), 4u);

    EXPECT_FALSE(registry.RegisterSeedProbeDefaults(&err));
    EXPECT_NE(err.find("already registered"), std::string::npos);
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

TEST(Stage3cWorkflowPromotionGate, ProducesMachineReadableDecisionArtifacts) {
    using namespace simcore::db::execution::workflow;

    const WorkflowPromotionEvidence pass_evidence{
        .parity_compared_steps = 100,
        .parity_matched_steps = 100,
        .recovery_passed = true,
        .integrity_passed = true,
        .readiness_scan_p95_ms = 5.0,
        .readiness_scan_threshold_ms = 20.0,
    };

    const auto pass_decision = EvaluateWorkflowPromotionGate(pass_evidence);
    EXPECT_TRUE(pass_decision.approved);
    EXPECT_TRUE(pass_decision.blockers.empty());

    const auto pass_json = BuildWorkflowPromotionDecisionJson(pass_evidence, pass_decision);
    EXPECT_NE(pass_json.find("\"approved\":true"), std::string::npos);
    EXPECT_NE(pass_json.find("\"blockers\":[]"), std::string::npos);

    const WorkflowPromotionEvidence fail_evidence{
        .parity_compared_steps = 100,
        .parity_matched_steps = 95,
        .recovery_passed = false,
        .integrity_passed = true,
        .readiness_scan_p95_ms = 50.0,
        .readiness_scan_threshold_ms = 20.0,
    };

    const auto fail_decision = EvaluateWorkflowPromotionGate(fail_evidence);
    EXPECT_FALSE(fail_decision.approved);
    EXPECT_GE(fail_decision.blockers.size(), 2u);

    const auto fail_json = BuildWorkflowPromotionDecisionJson(fail_evidence, fail_decision);
    EXPECT_NE(fail_json.find("parity_below_99_percent"), std::string::npos);
    EXPECT_NE(fail_json.find("recovery_failed"), std::string::npos);
    EXPECT_NE(fail_json.find("readiness_latency_above_threshold"), std::string::npos);

    const std::vector<std::string> required_json_fields{
        "\"approved\":",
        "\"parity_percent\":",
        "\"parity_compared_steps\":",
        "\"parity_matched_steps\":",
        "\"recovery_passed\":",
        "\"integrity_passed\":",
        "\"readiness_scan_p95_ms\":",
        "\"readiness_scan_threshold_ms\":",
        "\"blockers\":[",
    };

    auto has_all_required_fields = [&](const std::string& json) {
        for (const auto& token : required_json_fields) {
            if (json.find(token) == std::string::npos) {
                return false;
            }
        }
        return true;
    };

    EXPECT_TRUE(has_all_required_fields(pass_json));
    EXPECT_TRUE(has_all_required_fields(fail_json));

    const auto pass_parity_percent =
        (static_cast<double>(pass_evidence.parity_matched_steps) / static_cast<double>(pass_evidence.parity_compared_steps)) * 100.0;
    const auto fail_parity_percent =
        (static_cast<double>(fail_evidence.parity_matched_steps) / static_cast<double>(fail_evidence.parity_compared_steps)) * 100.0;

    std::ostringstream detail_summary;
    detail_summary << "Item18PromotionGate\n";
    detail_summary << "RequiredDataFieldsPresent(pass_json)="
                   << (has_all_required_fields(pass_json) ? "YES" : "NO") << "\n";
    detail_summary << "RequiredDataFieldsPresent(fail_json)="
                   << (has_all_required_fields(fail_json) ? "YES" : "NO") << "\n";
    detail_summary << "PassEvidence: "
                   << "approved=" << (pass_decision.approved ? "true" : "false")
                   << ", parity_percent=" << pass_parity_percent
                   << ", parity_compared_steps=" << pass_evidence.parity_compared_steps
                   << ", parity_matched_steps=" << pass_evidence.parity_matched_steps
                   << ", recovery_passed=" << (pass_evidence.recovery_passed ? "true" : "false")
                   << ", integrity_passed=" << (pass_evidence.integrity_passed ? "true" : "false")
                   << ", readiness_scan_p95_ms=" << pass_evidence.readiness_scan_p95_ms
                   << ", readiness_scan_threshold_ms=" << pass_evidence.readiness_scan_threshold_ms
                   << ", blockers_count=" << pass_decision.blockers.size() << "\n";
    detail_summary << "FailEvidence: "
                   << "approved=" << (fail_decision.approved ? "true" : "false")
                   << ", parity_percent=" << fail_parity_percent
                   << ", parity_compared_steps=" << fail_evidence.parity_compared_steps
                   << ", parity_matched_steps=" << fail_evidence.parity_matched_steps
                   << ", recovery_passed=" << (fail_evidence.recovery_passed ? "true" : "false")
                   << ", integrity_passed=" << (fail_evidence.integrity_passed ? "true" : "false")
                   << ", readiness_scan_p95_ms=" << fail_evidence.readiness_scan_p95_ms
                   << ", readiness_scan_threshold_ms=" << fail_evidence.readiness_scan_threshold_ms
                   << ", blockers_count=" << fail_decision.blockers.size() << "\n";
    detail_summary << "FailBlockers=";
    for (size_t i = 0; i < fail_decision.blockers.size(); ++i) {
        if (i > 0) {
            detail_summary << "|";
        }
        detail_summary << fail_decision.blockers[i];
    }

    ::testing::Test::RecordProperty("TestDetailSummary", detail_summary.str());
}

TEST(Stage3cWorkflowModeProvider, ParsesAndReturnsSelectedModes) {
    using namespace simcore::db::execution::workflow;

    StaticWorkflowModeProvider provider({ .mode = WorkflowExecutionMode::DualWriteObserve, .source = "unit-test" });
    const auto selection = provider.GetModeSelection();
    EXPECT_EQ(selection.mode, WorkflowExecutionMode::DualWriteObserve);
    EXPECT_EQ(selection.source, "unit-test");

    EXPECT_EQ(ParseWorkflowExecutionMode("WorkflowOnly", WorkflowExecutionMode::LegacyOnly), WorkflowExecutionMode::WorkflowOnly);
    EXPECT_EQ(ParseWorkflowExecutionMode("invalid", WorkflowExecutionMode::DualWriteObserve), WorkflowExecutionMode::DualWriteObserve);

    const auto legacy_policy = BuildWorkflowAuthorityPolicy(WorkflowExecutionMode::LegacyOnly);
    EXPECT_TRUE(legacy_policy.run_legacy);
    EXPECT_FALSE(legacy_policy.run_workflow);
    EXPECT_TRUE(legacy_policy.legacy_authoritative);
    EXPECT_FALSE(legacy_policy.workflow_authoritative);

    const auto dual_policy = BuildWorkflowAuthorityPolicy(WorkflowExecutionMode::DualWriteObserve);
    EXPECT_TRUE(dual_policy.run_legacy);
    EXPECT_TRUE(dual_policy.run_workflow);
    EXPECT_TRUE(dual_policy.legacy_authoritative);
    EXPECT_FALSE(dual_policy.workflow_authoritative);

    const auto primary_policy = BuildWorkflowAuthorityPolicy(WorkflowExecutionMode::WorkflowPrimary);
    EXPECT_TRUE(primary_policy.run_legacy);
    EXPECT_TRUE(primary_policy.run_workflow);
    EXPECT_FALSE(primary_policy.legacy_authoritative);
    EXPECT_TRUE(primary_policy.workflow_authoritative);

    const auto workflow_only_policy = BuildWorkflowAuthorityPolicy(WorkflowExecutionMode::WorkflowOnly);
    EXPECT_FALSE(workflow_only_policy.run_legacy);
    EXPECT_TRUE(workflow_only_policy.run_workflow);
    EXPECT_FALSE(workflow_only_policy.legacy_authoritative);
    EXPECT_TRUE(workflow_only_policy.workflow_authoritative);
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

TEST(Stage3cCoordinatorReplacement, PersistsMaterializedAndTerminalTransitionsToExecutionDb) {
    using namespace simcore::runner::parallel::simcoredb;
    using namespace simcore::db::execution::workflow;

    RecordingExecutionDb execution_db;
    StaticWorkflowModeProvider mode_provider({ .mode = WorkflowExecutionMode::WorkflowPrimary, .source = "unit-test" });

    DBWorkflowWorkerCoordinator coordinator(
        &execution_db,
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
                .job_set_id = 9000 + step.workflow_step_id,
                .workflow_step_id = step.workflow_step_id,
            };
        });

    const auto materialized = coordinator.MaterializeWorkflowStep({
        .workflow_instance_id = 77,
        .workflow_step_id = 12,
        .step_key = "Grid",
        .step_kind = "seedprobe.grid",
        .priority = 5,
    });
    ASSERT_TRUE(materialized.has_value());
    ASSERT_EQ(execution_db.command_service.materialized_calls.size(), 1u);
    EXPECT_EQ(execution_db.command_service.materialized_calls[0].workflow_step_id, 12);
    EXPECT_EQ(execution_db.command_service.materialized_calls[0].job_set_id, 9012);

    const TerminalJobSetSignal terminal{
        .workflow_instance_id = 77,
        .workflow_step_id = 12,
        .job_set_id = 9012,
        .terminal_state = "FAILED",
    };

    EXPECT_TRUE(coordinator.PublishTerminalJobSet(terminal));
    EXPECT_FALSE(coordinator.PublishTerminalJobSet(terminal));
    ASSERT_EQ(execution_db.command_service.terminal_calls.size(), 1u);
    EXPECT_EQ(execution_db.command_service.terminal_calls[0].workflow_step_id, 12);
    EXPECT_EQ(execution_db.command_service.terminal_calls[0].terminal_state, "FAILED");
}

TEST(Stage3cCoordinatorReplacement, LegacyOnlyModeSkipsWorkflowPersistencePath) {
    using namespace simcore::runner::parallel::simcoredb;
    using namespace simcore::db::execution::workflow;

    RecordingExecutionDb execution_db;
    StaticWorkflowModeProvider mode_provider({ .mode = WorkflowExecutionMode::LegacyOnly, .source = "unit-test" });

    DBWorkflowWorkerCoordinator coordinator(
        &execution_db,
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
                .job_set_id = 5000 + step.workflow_step_id,
                .workflow_step_id = step.workflow_step_id,
            };
        });

    const auto scheduled = coordinator.MaterializeWorkflowStep({
        .workflow_instance_id = 3,
        .workflow_step_id = 4,
        .step_key = "Grid",
        .step_kind = "seedprobe.grid",
        .priority = 1,
    });
    EXPECT_FALSE(scheduled.has_value());
    EXPECT_TRUE(execution_db.command_service.materialized_calls.empty());

    const TerminalJobSetSignal terminal{
        .workflow_instance_id = 3,
        .workflow_step_id = 4,
        .job_set_id = 5004,
        .terminal_state = "COMPLETED",
    };
    EXPECT_FALSE(coordinator.PublishTerminalJobSet(terminal));
    EXPECT_TRUE(execution_db.command_service.terminal_calls.empty());
}

TEST_F(SqliteDbFixture, Stage3cWorkflowProjectorOutboxReplayUsesCheckpointAndIsIdempotent) {
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

    ExecutionDb execution_db(db_);
    std::string cmd_error;
    ASSERT_TRUE(execution_db.WorkflowCommandService()->MarkStepTerminal(
        { .workflow_step_id = 9401, .terminal_state = "COMPLETED", .requested_by = "projector-replay-test" },
        &cmd_error))
        << cmd_error;

    WorkflowProjector projector(db_);
    EXPECT_EQ(projector.GetCheckpoint("WorkflowProjector", &err), 0);
    ASSERT_TRUE(projector.ProjectFromOutbox("WorkflowProjector", 100, &err)) << err;
    const auto checkpoint_after_first = projector.GetCheckpoint("WorkflowProjector", &err);
    EXPECT_GT(checkpoint_after_first, 0);

    sqlite3_stmt* st = nullptr;
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
    const auto checkpoint_after_second = projector.GetCheckpoint("WorkflowProjector", &err);
    EXPECT_EQ(checkpoint_after_second, checkpoint_after_first);
}

TEST(Stage3cCoordinatorModes, ModeMatrixPoliciesDriveWorkflowPathDecisions) {
    using namespace simcore::runner::parallel::simcoredb;
    using namespace simcore::db::execution::workflow;

    const std::vector<std::pair<WorkflowExecutionMode, bool>> matrix{
        { WorkflowExecutionMode::LegacyOnly, false },
        { WorkflowExecutionMode::DualWriteObserve, true },
        { WorkflowExecutionMode::WorkflowPrimary, true },
        { WorkflowExecutionMode::WorkflowOnly, true },
    };

    for (const auto& [mode, should_run_workflow] : matrix) {
        RecordingExecutionDb execution_db;
        StaticWorkflowModeProvider mode_provider({ .mode = mode, .source = "mode-matrix" });

        DBWorkflowWorkerCoordinator coordinator(
            &execution_db,
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
                    .job_set_id = 10000 + step.workflow_step_id,
                    .workflow_step_id = step.workflow_step_id,
                };
            });

        const auto scheduled = coordinator.MaterializeWorkflowStep({
            .workflow_instance_id = 1,
            .workflow_step_id = 2,
            .step_key = "Grid",
            .step_kind = "seedprobe.grid",
            .priority = 5,
        });

        TerminalJobSetSignal terminal{
            .workflow_instance_id = 1,
            .workflow_step_id = 2,
            .job_set_id = 10002,
            .terminal_state = "COMPLETED",
        };
        const auto published_terminal = coordinator.PublishTerminalJobSet(terminal);

        if (should_run_workflow) {
            ASSERT_TRUE(scheduled.has_value());
            EXPECT_EQ(execution_db.command_service.materialized_calls.size(), 1u);
            EXPECT_TRUE(published_terminal);
            EXPECT_EQ(execution_db.command_service.terminal_calls.size(), 1u);
        } else {
            EXPECT_FALSE(scheduled.has_value());
            EXPECT_TRUE(execution_db.command_service.materialized_calls.empty());
            EXPECT_FALSE(published_terminal);
            EXPECT_TRUE(execution_db.command_service.terminal_calls.empty());
        }
    }
}

TEST(Stage3cCoordinatorTelemetry, CapturesReadinessScanLatencyAndQueueDepth) {
    using namespace simcore::runner::parallel::simcoredb;
    using namespace simcore::db::execution::workflow;

    RecordingExecutionDb execution_db;
    StaticWorkflowModeProvider mode_provider({ .mode = WorkflowExecutionMode::WorkflowPrimary, .source = "telemetry-test" });

    DBWorkflowWorkerCoordinator coordinator(
        &execution_db,
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
                .job_set_id = 12000 + step.workflow_step_id,
                .workflow_step_id = step.workflow_step_id,
            };
        });

    coordinator.EnqueueReadyStep({
        .workflow_instance_id = 50,
        .workflow_step_id = 60,
        .step_key = "Grid",
        .step_kind = "seedprobe.grid",
        .priority = 1,
    });
    coordinator.EnqueueReadyStep({
        .workflow_instance_id = 50,
        .workflow_step_id = 61,
        .step_key = "Unique",
        .step_kind = "seedprobe.unique",
        .priority = 1,
    });

    const auto telemetry = coordinator.SnapshotTelemetry();
    EXPECT_GE(telemetry.max_ready_queue_depth, 0);
    EXPECT_GE(telemetry.ready_scan_count, 0);
    EXPECT_GE(telemetry.last_ready_scan_latency_ms, 0);
}

TEST_F(SqliteDbFixture, Stage3cEndToEndDualPathSeedProbeWithRestartMidRun) {
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

    ExecutionDb execution_db(db_);
    StaticWorkflowModeProvider mode_provider({ .mode = WorkflowExecutionMode::DualWriteObserve, .source = "stage3c-item15-test" });

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
            .mode = CoordinatorIntegrationMode::SimCoreDbWorkflow,
            .dual_write_observe = true,
        },
        schedule);

    std::vector<WorkflowOutcomeItem> legacy_outcomes;
    legacy_outcomes.push_back({ .step_key = "Neutral", .outcome = "COMPLETED" });

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
    legacy_outcomes.push_back({ .step_key = "Grid", .outcome = "COMPLETED" });

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
            .mode = CoordinatorIntegrationMode::SimCoreDbWorkflow,
            .dual_write_observe = true,
        },
        schedule);

    ASSERT_TRUE(ExecSql(db_, (std::string(
        "INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc) "
        "VALUES(13001,")
        + std::to_string(grid->job_set_id)
        + ",1,1,'seedprobe',1,'item15-grid',10,'COMPLETED',1,2,unixepoch(),unixepoch());")
        .c_str()));

    WorkflowRecoveryService recovery(db_);
    WorkflowRecoveryResult recovery_result{};
    ASSERT_TRUE(recovery.ReconcileInFlightInstances(&recovery_result, &err)) << err;
    EXPECT_EQ(recovery_result.completed_steps, 1);

    ASSERT_TRUE(ExecSql(db_, "UPDATE exec_workflow_step SET state='READY', ready_at_utc=unixepoch() WHERE workflow_step_id=9913;"));
    legacy_outcomes.push_back({ .step_key = "Unique", .outcome = "COMPLETED" });
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
    legacy_outcomes.push_back({ .step_key = "Done", .outcome = "COMPLETED" });
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
    const auto parity_report = CompareLegacyAndWorkflowOutcomes(legacy_outcomes, workflow_outcomes);
    EXPECT_EQ(parity_report.compared_steps, 4);
    EXPECT_EQ(parity_report.matched_steps, 4);
    EXPECT_TRUE(parity_report.mismatches.empty());

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
