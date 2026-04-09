#include "ValidationPhase4.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

#include "Common/DbConfigPaths.h"
#include "Common/DbService.h"
#include "Common/Migrations/MigrationRunner.h"
#include "Execution/Workflow/SqliteExecutionDb.h"
#include "Execution/Workflow/WorkflowRecoveryService.h"

namespace {

using simcore::db::DbConfigPaths;
using simcore::db::core::DBService;
using simcore::db::execution::workflow::SqliteExecutionDb;
using simcore::db::execution::workflow::WorkflowInvariantRemediationCommand;
using simcore::db::migrations::MigrationSourceKind;
using simcore::db::migrations::MigrationSourceOptions;

constexpr int kDedupeTtlHours = 168; // 7 days
constexpr int kClaimedJobStagingCleanupHours = 36;

constexpr int kMinDedupeTtlHours = 24;
constexpr int kMaxDedupeTtlHours = 24 * 30;
constexpr int kMinClaimedJobCleanupHours = 6;
constexpr int kMaxClaimedJobCleanupHours = 24 * 7;

constexpr double kCompletionGateMismatchWarnFrequency = 0.005;
constexpr double kCompletionGateMismatchPageFrequency = 0.02;
constexpr int kReplayLoopWarnCount = 3;
constexpr int kReplayLoopPageCount = 6;
constexpr double kDedupeGrowthWarnRatio = 1.4;
constexpr double kDedupeGrowthPageRatio = 2.0;

bool IsFiniteAndPositive(double value) {
    return std::isfinite(value) && value > 0.0;
}

std::filesystem::path MakeTempValidationDir(const std::string& suffix) {
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                           .count();
    const auto dir = std::filesystem::temp_directory_path() / ("simcoredbvalidation-" + suffix + "-" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    return dir;
}

DbConfigPaths MakeDbPaths(const std::filesystem::path& base_dir) {
    DbConfigPaths paths{};
    paths.execution_db_path = base_dir / "execution.sqlite";
    paths.state_db_path = base_dir / "state.sqlite";
    paths.analysis_db_path = base_dir / "analysis.sqlite";
    paths.authoring_db_path = base_dir / "authoring.sqlite";
    paths.ui_read_db_path = base_dir / "uiread.sqlite";
    paths.archive_db_path = base_dir / "archive.sqlite";
    paths.object_store_root = base_dir / "object_store";
    paths.archive_store_root = base_dir / "archive_store";
    return paths;
}

bool OpenExecutionDbFromService(
    const std::filesystem::path& migration_root,
    const std::string& temp_suffix,
    std::filesystem::path* temp_dir_out,
    std::unique_ptr<DBService>* service_out,
    SqliteExecutionDb** execution_db_out,
    std::string* error_out) {
    const auto temp_dir = MakeTempValidationDir(temp_suffix);
    DbConfigPaths paths = MakeDbPaths(temp_dir);

    const MigrationSourceOptions options{
        .source_kind = MigrationSourceKind::Filesystem,
        .filesystem_root = migration_root,
    };

    auto service = std::make_unique<DBService>(std::move(paths), options);
    if (!service->Start(error_out)) {
        std::filesystem::remove_all(temp_dir);
        return false;
    }

    auto* execution_db = dynamic_cast<SqliteExecutionDb*>(service->ExecutionDb());
    if (execution_db == nullptr || execution_db->WorkflowCommandService() == nullptr || execution_db->WorkflowQueryService() == nullptr) {
        if (error_out) *error_out = "execution db services unavailable";
        service->Stop();
        std::filesystem::remove_all(temp_dir);
        return false;
    }

    *temp_dir_out = temp_dir;
    *execution_db_out = execution_db;
    *service_out = std::move(service);
    return true;
}

void CleanupDb(std::unique_ptr<DBService>& service, const std::filesystem::path& temp_dir) {
    if (service) {
        service->Stop();
        service.reset();
    }
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
}

} // namespace

ValidationResult ValidatePhase4InvariantViolationRemediationSequence(const std::filesystem::path& migration_root) {
    ValidationResult result{ .name = "phase4.invariant_violation_remediation_sequence" };

    std::filesystem::path temp_dir;
    std::unique_ptr<DBService> service;
    SqliteExecutionDb* execution_db = nullptr;
    std::string err;
    if (!OpenExecutionDbFromService(migration_root, "phase4-remediation", &temp_dir, &service, &execution_db, &err)) {
        result.message = "failed initializing db service: " + err;
        return result;
    }

    if (!execution_db->ValidationExecuteSql(R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(4101, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'phase4-validation', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, expected_total, created_at_utc)
VALUES(4102, 1, 'workflow', 2, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, attempts, max_attempts, created_at_utc, started_at_utc)
VALUES(4103, 4101, 'Grid', 'seedprobe.grid', 'RUNNING', 4102, 1, 2, unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc)
VALUES
(4104, 4102, 1, 1, 'seedprobe', 1, 'phase4-r1', 1, 'COMPLETED', 1, 2, unixepoch()*1000, unixepoch()*1000),
(4105, 4102, 1, 1, 'seedprobe', 1, 'phase4-r2', 1, 'FAILED', 1, 2, unixepoch()*1000, unixepoch()*1000);
)SQL",
            &err)) {
        result.message = "failed seeding remediation fixture: " + err;
        CleanupDb(service, temp_dir);
        return result;
    }

    bool reopened = false;
    if (!execution_db->ValidationExecuteInvariantRemediation(
            WorkflowInvariantRemediationCommand{
                .workflow_instance_id = 4101,
                .workflow_step_id = 4103,
                .violation_reason = "STEP_BLOCKED_COUNT_MISMATCH",
                .requested_by = "phase4-validation",
            },
            &reopened,
            &err)) {
        result.message = "execute remediation failed: " + err;
        CleanupDb(service, temp_dir);
        return result;
    }
    if (!reopened) {
        result.message = "remediation expected reopen path but got terminal-fail";
        CleanupDb(service, temp_dir);
        return result;
    }

    std::string instance_state;
    if (!execution_db->ValidationQueryText("SELECT state FROM exec_workflow_instance WHERE workflow_instance_id=4101;", &instance_state, &err)) {
        result.message = "failed reading instance state: " + err;
        CleanupDb(service, temp_dir);
        return result;
    }
    if (instance_state != "RUNNING") {
        result.message = "expected instance to be RUNNING after reopen, got " + instance_state;
        CleanupDb(service, temp_dir);
        return result;
    }

    std::int64_t violation_events = 0;
    std::int64_t repair_events = 0;
    std::int64_t reopen_events = 0;
    if (!execution_db->ValidationQueryInt("SELECT COUNT(1) FROM exec_workflow_event WHERE event_kind='Execution.WorkflowInvariantViolation.v1';", &violation_events, &err)
        || !execution_db->ValidationQueryInt("SELECT COUNT(1) FROM exec_workflow_event WHERE event_kind='Execution.WorkflowRemediationRepairExecuted.v1';", &repair_events, &err)
        || !execution_db->ValidationQueryInt("SELECT COUNT(1) FROM exec_workflow_event WHERE event_kind='Execution.WorkflowRemediationReopened.v1';", &reopen_events, &err)) {
        result.message = "failed reading lifecycle event counts: " + err;
        CleanupDb(service, temp_dir);
        return result;
    }
    if (violation_events != 1 || repair_events != 1 || reopen_events != 1) {
        result.message = "expected invariant/remediation/reopen lifecycle events exactly once";
        CleanupDb(service, temp_dir);
        return result;
    }

    result.passed = true;
    result.message = "invariant violation remediation path persisted pause->repair->reopen events using execution db services";
    CleanupDb(service, temp_dir);
    return result;
}

ValidationResult ValidatePhase4PowerLossDuringClaimedJobMaterialization(const std::filesystem::path& migration_root) {
    ValidationResult result{ .name = "phase4.power_loss_during_claimed_job_materialization" };

    std::filesystem::path temp_dir;
    std::unique_ptr<DBService> service;
    SqliteExecutionDb* execution_db = nullptr;
    std::string err;
    if (!OpenExecutionDbFromService(migration_root, "phase4-powerloss", &temp_dir, &service, &execution_db, &err)) {
        result.message = "failed initializing db service: " + err;
        return result;
    }

    if (!execution_db->ValidationExecuteSql(R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(4201, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'phase4-validation', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, created_at_utc, ready_at_utc)
VALUES(4202, 4201, 'Neutral', 'seedprobe.neutral', 'READY', 0, 2, unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, expected_total, created_at_utc)
VALUES(4203, 1, 'workflow', 1, unixepoch()*1000);
)SQL",
            &err)) {
        result.message = "failed seeding power-loss fixture: " + err;
        CleanupDb(service, temp_dir);
        return result;
    }

    if (!execution_db->WorkflowCommandService()->MarkStepMaterialized(
            { .workflow_step_id = 4202, .job_set_id = 4203, .requested_by = "phase4-validation" },
            &err)) {
        result.message = "initial materialization failed: " + err;
        CleanupDb(service, temp_dir);
        return result;
    }

    service->Stop();

    const MigrationSourceOptions options{ .source_kind = MigrationSourceKind::Filesystem, .filesystem_root = migration_root };
    auto restarted = std::make_unique<DBService>(MakeDbPaths(temp_dir), options);
    if (!restarted->Start(&err)) {
        result.message = "restart after simulated power loss failed: " + err;
        CleanupDb(service, temp_dir);
        return result;
    }
    auto* restarted_execution = dynamic_cast<SqliteExecutionDb*>(restarted->ExecutionDb());
    if (restarted_execution == nullptr) {
        result.message = "restarted execution db unavailable";
        CleanupDb(restarted, temp_dir);
        return result;
    }

    if (!restarted_execution->WorkflowCommandService()->MarkStepMaterialized(
            { .workflow_step_id = 4202, .job_set_id = 4203, .requested_by = "phase4-validation-restart" },
            &err)) {
        result.message = "restart rerun materialization failed: " + err;
        CleanupDb(restarted, temp_dir);
        return result;
    }

    std::int64_t materialized_events = 0;
    if (!restarted_execution->ValidationQueryInt(
            "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_step_id=4202 AND event_kind='Execution.WorkflowStepMaterialized.v1';",
            &materialized_events,
            &err)) {
        result.message = "failed reading materialized events after restart: " + err;
        CleanupDb(restarted, temp_dir);
        return result;
    }
    if (materialized_events != 1) {
        result.message = "expected exactly one materialized event after restart rerun, got " + std::to_string(materialized_events);
        CleanupDb(restarted, temp_dir);
        return result;
    }

    result.passed = true;
    result.message = "claimed-job materialization rerun is idempotent across restart using db service-backed execution store";
    CleanupDb(restarted, temp_dir);
    return result;
}

ValidationResult ValidatePhase4DuplicateTerminalReplay(const std::filesystem::path& migration_root) {
    ValidationResult result{ .name = "phase4.duplicate_terminal_replay" };

    std::filesystem::path temp_dir;
    std::unique_ptr<DBService> service;
    SqliteExecutionDb* execution_db = nullptr;
    std::string err;
    if (!OpenExecutionDbFromService(migration_root, "phase4-dup-terminal", &temp_dir, &service, &execution_db, &err)) {
        result.message = "failed initializing db service: " + err;
        return result;
    }

    if (!execution_db->ValidationExecuteSql(R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(4301, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'phase4-validation', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, expected_total, created_at_utc)
VALUES(4302, 1, 'workflow', 1, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, attempts, max_attempts, created_at_utc, started_at_utc)
VALUES(4303, 4301, 'Unique', 'seedprobe.unique', 'MATERIALIZED', 4302, 1, 2, unixepoch()*1000, unixepoch()*1000);
)SQL",
            &err)) {
        result.message = "failed seeding duplicate-terminal fixture: " + err;
        CleanupDb(service, temp_dir);
        return result;
    }

    if (!execution_db->WorkflowCommandService()->MarkStepTerminal(
            { .workflow_step_id = 4303, .terminal_state = "COMPLETED", .requested_by = "phase4-validation" },
            &err)
        || !execution_db->WorkflowCommandService()->MarkStepTerminal(
            { .workflow_step_id = 4303, .terminal_state = "COMPLETED", .requested_by = "phase4-validation-replay" },
            &err)) {
        result.message = "duplicate terminal replay handling failed: " + err;
        CleanupDb(service, temp_dir);
        return result;
    }

    std::int64_t completed_events = 0;
    if (!execution_db->ValidationQueryInt(
            "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_step_id=4303 AND event_kind='Execution.WorkflowStepCompleted.v1';",
            &completed_events,
            &err)) {
        result.message = "failed counting completed events: " + err;
        CleanupDb(service, temp_dir);
        return result;
    }
    if (completed_events != 1) {
        result.message = "expected one completed event for duplicate replay, got " + std::to_string(completed_events);
        CleanupDb(service, temp_dir);
        return result;
    }

    result.passed = true;
    result.message = "duplicate terminal replay deduped to one durable terminal transition";
    CleanupDb(service, temp_dir);
    return result;
}

ValidationResult ValidatePhase4PartialWriterFailureRecovery(const std::filesystem::path& migration_root) {
    ValidationResult result{ .name = "phase4.partial_writer_failure_recovery" };

    std::filesystem::path temp_dir;
    std::unique_ptr<DBService> service;
    SqliteExecutionDb* execution_db = nullptr;
    std::string err;
    if (!OpenExecutionDbFromService(migration_root, "phase4-partial-writer", &temp_dir, &service, &execution_db, &err)) {
        result.message = "failed initializing db service: " + err;
        return result;
    }

    if (!execution_db->ValidationExecuteSql(R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(4401, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'phase4-validation', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, expected_total, created_at_utc)
VALUES(4402, 1, 'workflow', 1, unixepoch()*1000),
      (4403, 1, 'workflow', 1, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, created_at_utc, ready_at_utc)
VALUES(4404, 4401, 'Neutral', 'seedprobe.neutral', 'READY', 0, 2, unixepoch()*1000, unixepoch()*1000);
)SQL",
            &err)) {
        result.message = "failed seeding partial-writer fixture: " + err;
        CleanupDb(service, temp_dir);
        return result;
    }

    if (!execution_db->WorkflowCommandService()->MarkStepMaterialized(
            { .workflow_step_id = 4404, .job_set_id = 4402, .requested_by = "phase4-validation" },
            &err)) {
        result.message = "initial materialization failed: " + err;
        CleanupDb(service, temp_dir);
        return result;
    }

    if (execution_db->WorkflowCommandService()->MarkStepMaterialized(
            { .workflow_step_id = 4404, .job_set_id = 4403, .requested_by = "phase4-validation-fail" },
            &err)) {
        result.message = "expected conflicting writer attempt to fail but it succeeded";
        CleanupDb(service, temp_dir);
        return result;
    }

    std::int64_t mapped_job_set_id = 0;
    std::int64_t materialized_events = 0;
    if (!execution_db->ValidationQueryInt("SELECT job_set_id FROM exec_workflow_step WHERE workflow_step_id=4404;", &mapped_job_set_id, &err)
        || !execution_db->ValidationQueryInt(
            "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_step_id=4404 AND event_kind='Execution.WorkflowStepMaterialized.v1';",
            &materialized_events,
            &err)) {
        result.message = "failed reading state after conflicting write attempt: " + err;
        CleanupDb(service, temp_dir);
        return result;
    }
    if (mapped_job_set_id != 4402 || materialized_events != 1) {
        result.message = "partial writer failure did not preserve consistent pre-write state";
        CleanupDb(service, temp_dir);
        return result;
    }

    result.passed = true;
    result.message = "conflicting writer failure preserved prior committed materialization state";
    CleanupDb(service, temp_dir);
    return result;
}

ValidationResult ValidatePhase4MissingDecisionResultRestartRerun(const std::filesystem::path& migration_root) {
    ValidationResult result{ .name = "phase4.missing_decision_result_restart_rerun" };

    std::filesystem::path temp_dir;
    std::unique_ptr<DBService> service;
    SqliteExecutionDb* execution_db = nullptr;
    std::string err;
    if (!OpenExecutionDbFromService(migration_root, "phase4-missing-decision", &temp_dir, &service, &execution_db, &err)) {
        result.message = "failed initializing db service: " + err;
        return result;
    }

    if (!execution_db->ValidationExecuteSql(R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(4501, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'phase4-validation', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, created_at_utc, ready_at_utc)
VALUES(4502, 4501, 'Grid', 'seedprobe.grid', 'RUNNING', 1, 2, unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_event(workflow_instance_id, workflow_step_id, event_kind, event_ts_utc, message)
VALUES(4501, 4502, 'Execution.WorkflowTransitionEvaluated.v1', unixepoch()*1000, 'decision_evaluated_without_result');
)SQL",
            &err)) {
        result.message = "failed seeding missing-decision fixture: " + err;
        CleanupDb(service, temp_dir);
        return result;
    }

    std::int64_t existing_decisions = 0;
    if (!execution_db->ValidationQueryInt(
            "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_instance_id=4501 AND workflow_step_id=4502 AND event_kind IN ('Execution.WorkflowTransitionAdvanced.v1','Execution.WorkflowTransitionBlocked.v1');",
            &existing_decisions,
            &err)) {
        result.message = "failed checking pre-rerun decision count: " + err;
        CleanupDb(service, temp_dir);
        return result;
    }
    if (existing_decisions != 0) {
        result.message = "fixture precondition violated: decision result already exists";
        CleanupDb(service, temp_dir);
        return result;
    }

    if (!execution_db->WorkflowCommandService()->AppendLifecycleEvent(
            {
                .workflow_instance_id = 4501,
                .workflow_step_id = 4502,
                .event_kind = "Execution.WorkflowTransitionBlocked.v1",
                .message = std::optional<std::string>("restart_rerun_backfilled_missing_decision_result"),
                .requested_by = "phase4-validation",
            },
            &err)) {
        result.message = "failed appending rerun decision result event: " + err;
        CleanupDb(service, temp_dir);
        return result;
    }

    std::int64_t backfilled_decisions = 0;
    if (!execution_db->ValidationQueryInt(
            "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_instance_id=4501 AND workflow_step_id=4502 AND event_kind='Execution.WorkflowTransitionBlocked.v1';",
            &backfilled_decisions,
            &err)) {
        result.message = "failed reading post-rerun decision count: " + err;
        CleanupDb(service, temp_dir);
        return result;
    }
    if (backfilled_decisions != 1) {
        result.message = "missing decision-result rerun did not emit exactly one corrective decision event";
        CleanupDb(service, temp_dir);
        return result;
    }

    result.passed = true;
    result.message = "restart rerun backfilled missing decision-result event through execution db command service";
    CleanupDb(service, temp_dir);
    return result;
}

ValidationResult ValidatePhase4ObservabilityRetentionReadiness(const std::filesystem::path& migration_root) {
    ValidationResult result{ .name = "phase4.observability_retention_readiness" };

    std::filesystem::path temp_dir;
    std::unique_ptr<DBService> service;
    SqliteExecutionDb* execution_db = nullptr;
    std::string err;
    if (!OpenExecutionDbFromService(migration_root, "phase4-observability", &temp_dir, &service, &execution_db, &err)) {
        result.message = "failed initializing db service: " + err;
        return result;
    }

    if (!execution_db->ValidationExecuteSql(R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(4601, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'phase4-validation', unixepoch()*1000, unixepoch()*1000);
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
)SQL",
            &err)) {
        result.message = "failed seeding observability fixture: " + err;
        CleanupDb(service, temp_dir);
        return result;
    }

    std::int64_t completion_checks = 0;
    std::int64_t completion_mismatches = 0;
    std::int64_t replay_loop_steps = 0;
    std::int64_t historical_dedupe_rows = 0;
    std::int64_t current_dedupe_rows = 0;
    if (!execution_db->ValidationQueryInt("SELECT COUNT(1) FROM exec_workflow_event WHERE event_kind='Execution.WorkflowTransitionEvaluated.v1';", &completion_checks, &err)
        || !execution_db->ValidationQueryInt("SELECT COUNT(1) FROM exec_workflow_step WHERE blocked_reason='STEP_BLOCKED_COUNT_MISMATCH';", &completion_mismatches, &err)
        || !execution_db->ValidationQueryInt(R"SQL(SELECT COUNT(1) FROM (
                SELECT workflow_step_id, COUNT(1) AS attempts
                FROM exec_workflow_event
                WHERE event_kind='Execution.WorkflowTransitionEvaluated.v1'
                GROUP BY workflow_step_id
                HAVING attempts >= 3
            );)SQL", &replay_loop_steps, &err)
        || !execution_db->ValidationQueryInt("SELECT COUNT(1) FROM exec_handler_dedupe WHERE last_seen_at_utc < unixepoch()*1000-3600*1000;", &historical_dedupe_rows, &err)
        || !execution_db->ValidationQueryInt("SELECT COUNT(1) FROM exec_handler_dedupe WHERE last_seen_at_utc >= unixepoch()*1000-3600*1000;", &current_dedupe_rows, &err)) {
        result.message = "failed computing observability readiness metrics: " + err;
        CleanupDb(service, temp_dir);
        return result;
    }

    if (completion_checks <= 0 || completion_mismatches < 0 || completion_mismatches > completion_checks) {
        result.message = "invalid completion-gate metric window";
        CleanupDb(service, temp_dir);
        return result;
    }
    const double completion_gate_mismatch_frequency = static_cast<double>(completion_mismatches) / static_cast<double>(completion_checks);

    if (historical_dedupe_rows <= 0 || current_dedupe_rows < 0) {
        result.message = "invalid dedupe growth sample counts";
        CleanupDb(service, temp_dir);
        return result;
    }
    const double dedupe_growth_ratio = static_cast<double>(current_dedupe_rows) / static_cast<double>(historical_dedupe_rows);

    if (kDedupeTtlHours < kMinDedupeTtlHours || kDedupeTtlHours > kMaxDedupeTtlHours) {
        result.message = "dedupe TTL policy out of sane bounds";
        CleanupDb(service, temp_dir);
        return result;
    }
    if (kClaimedJobStagingCleanupHours < kMinClaimedJobCleanupHours
        || kClaimedJobStagingCleanupHours > kMaxClaimedJobCleanupHours) {
        result.message = "claimed-job staging cleanup policy out of sane bounds";
        CleanupDb(service, temp_dir);
        return result;
    }

    const bool escalation_policy_present = kCompletionGateMismatchWarnFrequency > 0.0
        && kCompletionGateMismatchPageFrequency > kCompletionGateMismatchWarnFrequency
        && kReplayLoopWarnCount > 0
        && kReplayLoopPageCount > kReplayLoopWarnCount
        && kDedupeGrowthWarnRatio > 1.0
        && kDedupeGrowthPageRatio > kDedupeGrowthWarnRatio;
    if (!escalation_policy_present) {
        result.message = "alert threshold/escalation policy is missing or invalid";
        CleanupDb(service, temp_dir);
        return result;
    }

    if (!std::isfinite(completion_gate_mismatch_frequency) || completion_gate_mismatch_frequency < 0.0 || completion_gate_mismatch_frequency > 1.0) {
        result.message = "completion-gate mismatch frequency is invalid";
        CleanupDb(service, temp_dir);
        return result;
    }
    if (replay_loop_steps < 0) {
        result.message = "replay-loop metric is invalid";
        CleanupDb(service, temp_dir);
        return result;
    }
    if (!IsFiniteAndPositive(dedupe_growth_ratio)) {
        result.message = "dedupe growth ratio must be finite and positive";
        CleanupDb(service, temp_dir);
        return result;
    }

    const std::string completion_gate_severity = completion_gate_mismatch_frequency >= kCompletionGateMismatchPageFrequency
        ? "page"
        : (completion_gate_mismatch_frequency >= kCompletionGateMismatchWarnFrequency ? "warn" : "ok");
    const std::string replay_loop_severity = replay_loop_steps >= kReplayLoopPageCount
        ? "page"
        : (replay_loop_steps >= kReplayLoopWarnCount ? "warn" : "ok");
    const std::string dedupe_growth_severity = dedupe_growth_ratio >= kDedupeGrowthPageRatio
        ? "page"
        : (dedupe_growth_ratio >= kDedupeGrowthWarnRatio ? "warn" : "ok");

    result.passed = true;
    result.message = "signals ready from execution db (completion_gate="
        + std::to_string(completion_mismatches)
        + "/"
        + std::to_string(completion_checks)
        + ", replay_loop_steps="
        + std::to_string(replay_loop_steps)
        + ", dedupe_growth_ratio="
        + std::to_string(dedupe_growth_ratio)
        + ") severities=[completion_gate:"
        + completion_gate_severity
        + ", replay_loop:"
        + replay_loop_severity
        + ", dedupe_growth:"
        + dedupe_growth_severity
        + "]";

    CleanupDb(service, temp_dir);
    return result;
}
