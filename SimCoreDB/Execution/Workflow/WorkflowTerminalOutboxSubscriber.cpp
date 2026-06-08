#include "WorkflowTerminalOutboxSubscriber.h"

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string_view>

#include "../../Common/Events/OutboxRelay.h"

namespace simcore::db::execution::workflow {
namespace {

struct Statement {
    sqlite3_stmt* st = nullptr;
    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }
};

bool Prepare(sqlite3* db, const char* sql, Statement* out, std::string* error_out) {
    if (sqlite3_prepare_v2(db, sql, -1, &out->st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    return true;
}

std::int64_t NowUtc() {
    const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
    return now.time_since_epoch().count();
}

bool TryRecordHandlerDedupeEvent(
    sqlite3* db,
    std::string_view handler_name,
    std::string_view event_id,
    bool* inserted_out,
    std::string* error_out) {
    Statement st;
    if (!Prepare(
            db,
            "INSERT OR IGNORE INTO exec_handler_dedupe(handler_name, event_id, semantic_key, first_seen_at_utc, last_seen_at_utc) "
            "VALUES(?1, ?2, NULL, ?3, ?3);",
            &st,
            error_out)) {
        return false;
    }

    const auto now_utc = NowUtc();
    sqlite3_bind_text(st.st, 1, handler_name.data(), static_cast<int>(handler_name.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 2, event_id.data(), static_cast<int>(event_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 3, now_utc);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }

    const bool inserted = sqlite3_changes(db) == 1;
    if (!inserted) {
        Statement update;
        if (!Prepare(
                db,
                "UPDATE exec_handler_dedupe SET last_seen_at_utc=?3 WHERE handler_name=?1 AND event_id=?2;",
                &update,
                error_out)) {
            return false;
        }
        sqlite3_bind_text(update.st, 1, handler_name.data(), static_cast<int>(handler_name.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(update.st, 2, event_id.data(), static_cast<int>(event_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(update.st, 3, now_utc);
        if (sqlite3_step(update.st) != SQLITE_DONE) {
            if (error_out) *error_out = sqlite3_errmsg(db);
            return false;
        }
    }
    if (inserted_out != nullptr) {
        *inserted_out = inserted;
    }
    return true;
}

void DeleteHandlerDedupeEvent(sqlite3* db, std::string_view handler_name, std::string_view event_id) {
    Statement cleanup;
    if (!Prepare(
            db,
            "DELETE FROM exec_handler_dedupe WHERE handler_name=?1 AND event_id=?2;",
            &cleanup,
            nullptr)) {
        return;
    }
    sqlite3_bind_text(cleanup.st, 1, handler_name.data(), static_cast<int>(handler_name.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(cleanup.st, 2, event_id.data(), static_cast<int>(event_id.size()), SQLITE_TRANSIENT);
    sqlite3_step(cleanup.st);
}

bool LoadStepTerminalSnapshot(
    sqlite3* db,
    const char* sql,
    std::int64_t id,
    WorkflowTerminalOutboxSubscriber::StepTerminalContextSnapshot* snapshot_out,
    std::string* error_out) {
    Statement st;
    if (!Prepare(db, sql, &st, error_out)) {
        return false;
    }

    sqlite3_bind_int64(st.st, 1, id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        if (error_out) *error_out = "step snapshot not found";
        return false;
    }

    snapshot_out->workflow_instance_id = sqlite3_column_int64(st.st, 0);
    snapshot_out->workflow_step_id = sqlite3_column_int64(st.st, 1);
    snapshot_out->job_set_id = sqlite3_column_int64(st.st, 2);

    const auto* workflow_kind = sqlite3_column_text(st.st, 3);
    const auto* step_key = sqlite3_column_text(st.st, 4);
    const auto* step_kind = sqlite3_column_text(st.st, 5);
    snapshot_out->workflow_kind = workflow_kind ? reinterpret_cast<const char*>(workflow_kind) : "";
    snapshot_out->step_key = step_key ? reinterpret_cast<const char*>(step_key) : "";
    snapshot_out->step_kind = step_kind ? reinterpret_cast<const char*>(step_kind) : "";
    if (sqlite3_column_type(st.st, 6) == SQLITE_NULL) {
        snapshot_out->input_ref_kind = std::nullopt;
    } else {
        const auto* input_ref_kind = sqlite3_column_text(st.st, 6);
        snapshot_out->input_ref_kind = input_ref_kind ? std::optional<std::string>(reinterpret_cast<const char*>(input_ref_kind)) : std::nullopt;
    }
    if (sqlite3_column_type(st.st, 7) == SQLITE_NULL) {
        snapshot_out->input_ref_id = std::nullopt;
    } else {
        snapshot_out->input_ref_id = sqlite3_column_int64(st.st, 7);
    }
    if (sqlite3_column_type(st.st, 8) == SQLITE_NULL) {
        snapshot_out->output_ref_kind = std::nullopt;
    } else {
        const auto* output_ref_kind = sqlite3_column_text(st.st, 8);
        snapshot_out->output_ref_kind = output_ref_kind ? std::optional<std::string>(reinterpret_cast<const char*>(output_ref_kind)) : std::nullopt;
    }
    if (sqlite3_column_type(st.st, 9) == SQLITE_NULL) {
        snapshot_out->output_ref_id = std::nullopt;
    } else {
        snapshot_out->output_ref_id = sqlite3_column_int64(st.st, 9);
    }

    snapshot_out->completion.workflow_step_id = snapshot_out->workflow_step_id;
    snapshot_out->completion.job_set_id = snapshot_out->job_set_id;
    snapshot_out->completion.expected_total = sqlite3_column_int(st.st, 10);
    snapshot_out->completion.discovered_total = sqlite3_column_int(st.st, 11);
    snapshot_out->completion.terminal_total = sqlite3_column_int(st.st, 12);
    snapshot_out->completion.failed_total = sqlite3_column_int(st.st, 13);
    snapshot_out->failed_total = snapshot_out->completion.failed_total;

    return true;
}

} // namespace

WorkflowTerminalOutboxSubscriber::WorkflowTerminalOutboxSubscriber(
    sqlite3* db,
    const AdapterChainOrchestrator* orchestrator,
    IWorkflowOrchestrationCommandService* command_service)
    : db_(db)
    , orchestrator_(orchestrator)
    , command_service_(command_service)
    , recovery_service_(db) {
}

bool WorkflowTerminalOutboxSubscriber::ConsumeFromCursor(
    std::int64_t cursor_outbox_id,
    int max_batch_size,
    WorkflowTerminalOutboxSubscriberResult* result_out,
    std::string* error_out) const {
    if (db_ == nullptr || orchestrator_ == nullptr || command_service_ == nullptr) {
        if (error_out) *error_out = "subscriber dependencies are not configured";
        return false;
    }

    events::OutboxRelay relay({
        .db = db_,
        .outbox_table = "exec_outbox_message",
        .context_name = "Execution",
        .aggregate_kind = std::string{},
        .payload_ref_kind = std::string{},
        .max_attempts = 5,
    });

    const std::vector<events::OutboxRelayDispatchBinding> bindings{
        { { "Execution.JobCompleted.v1", 1 }, [this](const events::EventEnvelope& env, std::string* handler_error) {
               return HandleTerminalEvent(env, handler_error);
           } },
        { { "Execution.WorkflowStepCompleted.v1", 1 }, [this](const events::EventEnvelope& env, std::string* handler_error) {
               return HandleTerminalEvent(env, handler_error);
           } },
        { { "Execution.WorkflowStepFailed.v1", 1 }, [this](const events::EventEnvelope& env, std::string* handler_error) {
               return HandleTerminalEvent(env, handler_error);
           } },
    };

    events::OutboxRelayResult relay_result{};
    if (!relay.RelayBatchFromCursor(cursor_outbox_id, max_batch_size, bindings, &relay_result, error_out)) {
        return false;
    }

    if (result_out != nullptr) {
        result_out->last_scanned_outbox_id = relay_result.last_scanned_outbox_id;
        result_out->last_scanned_event_id = relay_result.last_scanned_event_id;
        result_out->scanned_count = relay_result.scanned_count;
        result_out->handled_count = relay_result.published_count;
    }
    return true;
}

bool WorkflowTerminalOutboxSubscriber::HandleTerminalEvent(const events::EventEnvelope& envelope, std::string* error_out) const {
    bool inserted = false;
    if (!TryRecordHandlerDedupeEvent(
            db_,
            "workflow_terminal_outbox_subscriber",
            envelope.event_id,
            &inserted,
            error_out)) {
        return false;
    }
    if (!inserted) {
        return true;
    }

    StepTerminalContextSnapshot snapshot{};
    if (envelope.event_type == "Execution.JobCompleted.v1") {
        if (!LoadStepTerminalSnapshotForJob(envelope.payload_ref_id, &snapshot, error_out)) {
            DeleteHandlerDedupeEvent(db_, "workflow_terminal_outbox_subscriber", envelope.event_id);
            return false;
        }
    } else if (envelope.event_type == "Execution.WorkflowStepCompleted.v1"
        || envelope.event_type == "Execution.WorkflowStepFailed.v1") {
        if (!LoadStepTerminalSnapshotForWorkflowEvent(envelope.payload_ref_id, &snapshot, error_out)) {
            DeleteHandlerDedupeEvent(db_, "workflow_terminal_outbox_subscriber", envelope.event_id);
            return false;
        }
    } else {
        return true;
    }

    if (!HandleStepTerminalSnapshot(snapshot, true, error_out)) {
        DeleteHandlerDedupeEvent(db_, "workflow_terminal_outbox_subscriber", envelope.event_id);
        return false;
    }

    return true;
}

bool WorkflowTerminalOutboxSubscriber::LoadStepTerminalSnapshotForJob(
    std::int64_t job_id,
    StepTerminalContextSnapshot* snapshot_out,
    std::string* error_out) const {
    constexpr const char* kSql =
        "WITH RECURSIVE job_set_ancestry(job_set_id, parent_job_set_id, depth) AS ("
        "  SELECT js.job_set_id, js.parent_job_set_id, 0 "
        "  FROM exec_job source "
        "  JOIN exec_job_set js ON js.job_set_id=source.job_set_id "
        "  WHERE source.job_id=?1 "
        "  UNION ALL "
        "  SELECT parent.job_set_id, parent.parent_job_set_id, job_set_ancestry.depth + 1 "
        "  FROM exec_job_set parent "
        "  JOIN job_set_ancestry ON parent.job_set_id=job_set_ancestry.parent_job_set_id "
        "  WHERE job_set_ancestry.parent_job_set_id IS NOT NULL "
        "    AND job_set_ancestry.depth < 64"
        "), "
        "step_root AS ("
        "  SELECT i.workflow_instance_id, s.workflow_step_id, s.job_set_id, i.workflow_kind, s.step_key, s.step_kind, "
        "s.input_ref_kind, s.input_ref_id, s.output_ref_kind, s.output_ref_id "
        "  FROM job_set_ancestry a "
        "  JOIN exec_workflow_step s ON s.job_set_id=a.job_set_id "
        "  JOIN exec_workflow_instance i ON i.workflow_instance_id=s.workflow_instance_id "
        "  ORDER BY a.depth ASC LIMIT 1"
        "), "
        "job_set_descendants(job_set_id, depth) AS ("
        "  SELECT job_set_id, 0 FROM step_root "
        "  UNION ALL "
        "  SELECT child.job_set_id, job_set_descendants.depth + 1 "
        "  FROM exec_job_set child "
        "  JOIN job_set_descendants ON child.parent_job_set_id=job_set_descendants.job_set_id "
        "  WHERE job_set_descendants.depth < 64"
        ") "
        "SELECT r.workflow_instance_id, r.workflow_step_id, r.job_set_id, r.workflow_kind, r.step_key, r.step_kind, "
        "r.input_ref_kind, r.input_ref_id, r.output_ref_kind, r.output_ref_id, "
        "CASE WHEN EXISTS(SELECT 1 FROM job_set_descendants WHERE depth > 0) "
        "  THEN (SELECT COALESCE(SUM(COALESCE(js.expected_total, 0)), 0) FROM exec_job_set js JOIN job_set_descendants d ON d.job_set_id=js.job_set_id WHERE d.depth > 0) "
        "  ELSE COALESCE(root_js.expected_total, 0) END, "
        "(SELECT COUNT(1) FROM exec_job j JOIN job_set_descendants d ON d.job_set_id=j.job_set_id), "
        "(SELECT COUNT(1) FROM exec_job j JOIN job_set_descendants d ON d.job_set_id=j.job_set_id "
        "  WHERE j.state IN ('COMPLETED','SUCCEEDED','SUCCEEDED_WINNER','SUPERSEDED','SUCCEEDED_DUPLICATE','FAILED','CANCELED')), "
        "(SELECT COUNT(1) FROM exec_job j JOIN job_set_descendants d ON d.job_set_id=j.job_set_id WHERE j.state='FAILED') "
        "FROM step_root r "
        "LEFT JOIN exec_job_set root_js ON root_js.job_set_id=r.job_set_id;";
    return LoadStepTerminalSnapshot(db_, kSql, job_id, snapshot_out, error_out);
}

bool WorkflowTerminalOutboxSubscriber::LoadStepTerminalSnapshotForStep(
    std::int64_t workflow_step_id,
    StepTerminalContextSnapshot* snapshot_out,
    std::string* error_out) const {
    constexpr const char* kSql =
        "WITH RECURSIVE step_root AS ("
        "  SELECT i.workflow_instance_id, s.workflow_step_id, s.job_set_id, i.workflow_kind, s.step_key, s.step_kind, "
        "s.input_ref_kind, s.input_ref_id, s.output_ref_kind, s.output_ref_id "
        "  FROM exec_workflow_step s "
        "  JOIN exec_workflow_instance i ON i.workflow_instance_id=s.workflow_instance_id "
        "  WHERE s.workflow_step_id=?1 LIMIT 1"
        "), "
        "job_set_descendants(job_set_id, depth) AS ("
        "  SELECT job_set_id, 0 FROM step_root "
        "  UNION ALL "
        "  SELECT child.job_set_id, job_set_descendants.depth + 1 "
        "  FROM exec_job_set child "
        "  JOIN job_set_descendants ON child.parent_job_set_id=job_set_descendants.job_set_id "
        "  WHERE job_set_descendants.depth < 64"
        ") "
        "SELECT r.workflow_instance_id, r.workflow_step_id, r.job_set_id, r.workflow_kind, r.step_key, r.step_kind, "
        "r.input_ref_kind, r.input_ref_id, r.output_ref_kind, r.output_ref_id, "
        "CASE WHEN EXISTS(SELECT 1 FROM job_set_descendants WHERE depth > 0) "
        "  THEN (SELECT COALESCE(SUM(COALESCE(js.expected_total, 0)), 0) FROM exec_job_set js JOIN job_set_descendants d ON d.job_set_id=js.job_set_id WHERE d.depth > 0) "
        "  ELSE COALESCE(root_js.expected_total, 0) END, "
        "(SELECT COUNT(1) FROM exec_job j JOIN job_set_descendants d ON d.job_set_id=j.job_set_id), "
        "(SELECT COUNT(1) FROM exec_job j JOIN job_set_descendants d ON d.job_set_id=j.job_set_id "
        "  WHERE j.state IN ('COMPLETED','SUCCEEDED','SUCCEEDED_WINNER','SUPERSEDED','SUCCEEDED_DUPLICATE','FAILED','CANCELED')), "
        "(SELECT COUNT(1) FROM exec_job j JOIN job_set_descendants d ON d.job_set_id=j.job_set_id WHERE j.state='FAILED') "
        "FROM step_root r "
        "LEFT JOIN exec_job_set root_js ON root_js.job_set_id=r.job_set_id;";
    return LoadStepTerminalSnapshot(db_, kSql, workflow_step_id, snapshot_out, error_out);
}

bool WorkflowTerminalOutboxSubscriber::LoadStepTerminalSnapshotForWorkflowEvent(
    std::int64_t workflow_event_id,
    StepTerminalContextSnapshot* snapshot_out,
    std::string* error_out) const {
    constexpr const char* kSql =
        "WITH RECURSIVE step_root AS ("
        "  SELECT i.workflow_instance_id, s.workflow_step_id, s.job_set_id, i.workflow_kind, s.step_key, s.step_kind, "
        "s.input_ref_kind, s.input_ref_id, s.output_ref_kind, s.output_ref_id "
        "  FROM exec_workflow_event source "
        "  JOIN exec_workflow_step s ON s.workflow_step_id=source.workflow_step_id "
        "  JOIN exec_workflow_instance i ON i.workflow_instance_id=s.workflow_instance_id "
        "  WHERE source.workflow_event_id=?1 LIMIT 1"
        "), "
        "job_set_descendants(job_set_id, depth) AS ("
        "  SELECT job_set_id, 0 FROM step_root "
        "  UNION ALL "
        "  SELECT child.job_set_id, job_set_descendants.depth + 1 "
        "  FROM exec_job_set child "
        "  JOIN job_set_descendants ON child.parent_job_set_id=job_set_descendants.job_set_id "
        "  WHERE job_set_descendants.depth < 64"
        ") "
        "SELECT r.workflow_instance_id, r.workflow_step_id, r.job_set_id, r.workflow_kind, r.step_key, r.step_kind, "
        "r.input_ref_kind, r.input_ref_id, r.output_ref_kind, r.output_ref_id, "
        "CASE WHEN EXISTS(SELECT 1 FROM job_set_descendants WHERE depth > 0) "
        "  THEN (SELECT COALESCE(SUM(COALESCE(js.expected_total, 0)), 0) FROM exec_job_set js JOIN job_set_descendants d ON d.job_set_id=js.job_set_id WHERE d.depth > 0) "
        "  ELSE COALESCE(root_js.expected_total, 0) END, "
        "(SELECT COUNT(1) FROM exec_job j JOIN job_set_descendants d ON d.job_set_id=j.job_set_id), "
        "(SELECT COUNT(1) FROM exec_job j JOIN job_set_descendants d ON d.job_set_id=j.job_set_id "
        "  WHERE j.state IN ('COMPLETED','SUCCEEDED','SUCCEEDED_WINNER','SUPERSEDED','SUCCEEDED_DUPLICATE','FAILED','CANCELED')), "
        "(SELECT COUNT(1) FROM exec_job j JOIN job_set_descendants d ON d.job_set_id=j.job_set_id WHERE j.state='FAILED') "
        "FROM step_root r "
        "LEFT JOIN exec_job_set root_js ON root_js.job_set_id=r.job_set_id;";
    return LoadStepTerminalSnapshot(db_, kSql, workflow_event_id, snapshot_out, error_out);
}

bool WorkflowTerminalOutboxSubscriber::HandleStepTerminalSnapshot(
    const StepTerminalContextSnapshot& snapshot,
    bool allow_reconcile_retry,
    std::string* error_out) const {
    const programdb::WorkflowTransitionContext context{
        .workflow_instance_id = snapshot.workflow_instance_id,
        .workflow_step_id = snapshot.workflow_step_id,
        .job_set_id = snapshot.job_set_id,
        .expected_total = snapshot.completion.expected_total,
        .discovered_total = snapshot.completion.discovered_total,
        .terminal_total = snapshot.completion.terminal_total,
        .failed_total = snapshot.completion.failed_total,
        .workflow_kind = snapshot.workflow_kind,
        .step_key = snapshot.step_key,
        .input_ref_kind = snapshot.input_ref_kind,
        .input_ref_id = snapshot.input_ref_id,
        .output_ref_kind = snapshot.output_ref_kind,
        .output_ref_id = snapshot.output_ref_id,
    };

    const auto terminal = orchestrator_->OnStepTerminal(snapshot.step_kind, context, snapshot.completion, nullptr);

    std::string command_error;
    if (!terminal.gate.can_transition) {
        if (terminal.gate.blocked_reason.has_value()
            && terminal.gate.blocked_reason->find("STEP_BLOCKED_COUNT_MISMATCH") == 0) {
            if (!command_service_->MarkStepBlocked(
                {
                    .workflow_step_id = snapshot.workflow_step_id,
                    .blocked_reason = terminal.gate.blocked_reason,
                    .requested_by = "workflow_terminal_subscriber",
                },
                &command_error)) {
                if (error_out) *error_out = command_error;
                return false;
            }

            bool reopened = false;
            if (!recovery_service_.ExecuteInvariantRemediation(
                    {
                        .workflow_instance_id = snapshot.workflow_instance_id,
                        .workflow_step_id = snapshot.workflow_step_id,
                        .violation_reason = *terminal.gate.blocked_reason,
                        .requested_by = "workflow_terminal_subscriber",
                    },
                    command_service_,
                    &reopened,
                    &command_error)) {
                if (error_out) *error_out = command_error;
                return false;
            }

            if (reopened) {
                if (allow_reconcile_retry) {
                    StepTerminalContextSnapshot reconciled{};
                    if (!LoadStepTerminalSnapshotForStep(snapshot.workflow_step_id, &reconciled, error_out)) {
                        return false;
                    }
                    return HandleStepTerminalSnapshot(reconciled, false, error_out);
                }
                return true;
            }
            return true;
        }
        return true;
    }

    if (terminal.gate.terminal_fail) {
        if (!command_service_->MarkStepBlocked(
            {
                .workflow_step_id = snapshot.workflow_step_id,
                .blocked_reason = std::nullopt,
                .requested_by = "workflow_terminal_subscriber",
            },
            &command_error)) {
            if (error_out) *error_out = command_error;
            return false;
        }

        if (!command_service_->MarkStepTerminal(
            {
                .workflow_step_id = snapshot.workflow_step_id,
                .terminal_state = "FAILED",
                .output_ref_kind = snapshot.output_ref_kind,
                .output_ref_id = snapshot.output_ref_id,
                .requested_by = "workflow_terminal_subscriber",
            },
            &command_error)) {
            if (error_out) *error_out = command_error;
            return false;
        }

        if (!command_service_->AppendLifecycleEvent(
            {
                .workflow_instance_id = snapshot.workflow_instance_id,
                .workflow_step_id = snapshot.workflow_step_id,
                .event_kind = "Execution.WorkflowTransitionEvaluated.v1",
                .message = std::optional<std::string>("transition_terminal_failed"),
                .requested_by = "workflow_terminal_subscriber",
            },
            &command_error)) {
            if (error_out) *error_out = command_error;
            return false;
        }

        if (!command_service_->TerminalFailWorkflowInstance(
            {
                .workflow_instance_id = snapshot.workflow_instance_id,
                .failure_code = "STEP_FAILED",
                .failure_message = "workflow step completed with failed jobs",
                .requested_by = "workflow_terminal_subscriber",
            },
            &command_error)) {
            if (error_out) *error_out = command_error;
            return false;
        }

        if (!command_service_->AppendLifecycleEvent(
            {
                .workflow_instance_id = snapshot.workflow_instance_id,
                .workflow_step_id = snapshot.workflow_step_id,
                .event_kind = "Execution.WorkflowTransitionBlocked.v1",
                .message = std::optional<std::string>("transition_terminal_failed"),
                .requested_by = "workflow_terminal_subscriber",
            },
            &command_error)) {
            if (error_out) *error_out = command_error;
            return false;
        }

        return true;
    }

    if (!command_service_->MarkStepBlocked(
        {
            .workflow_step_id = snapshot.workflow_step_id,
            .blocked_reason = std::nullopt,
            .requested_by = "workflow_terminal_subscriber",
        },
        &command_error)) {
        if (error_out) *error_out = command_error;
        return false;
    }

    const auto terminal_state = snapshot.failed_total > 0 ? "FAILED" : "COMPLETED";
    if (!command_service_->MarkStepTerminal(
        {
            .workflow_step_id = snapshot.workflow_step_id,
            .terminal_state = terminal_state,
            .output_ref_kind = snapshot.output_ref_kind,
            .output_ref_id = snapshot.output_ref_id,
            .requested_by = "workflow_terminal_subscriber",
        },
        &command_error)) {
        if (error_out) *error_out = command_error;
        return false;
    }

    if (!command_service_->AppendLifecycleEvent(
        {
            .workflow_instance_id = snapshot.workflow_instance_id,
            .workflow_step_id = snapshot.workflow_step_id,
            .event_kind = "Execution.WorkflowTransitionEvaluated.v1",
            .message = std::optional<std::string>("transition_evaluated"),
            .requested_by = "workflow_terminal_subscriber",
        },
        &command_error)) {
        if (error_out) *error_out = command_error;
        return false;
    }

    const bool advanced = terminal.transition.has_value() && terminal.transition->should_advance;
    const auto event_kind = advanced
        ? "Execution.WorkflowTransitionAdvanced.v1"
        : "Execution.WorkflowTransitionBlocked.v1";
    const auto event_message = advanced
        ? std::optional<std::string>("transition_advanced")
        : terminal.transition.has_value() ? terminal.transition->blocked_reason : std::optional<std::string>("transition_blocked");

    if (advanced) {
        if (!terminal.transition->spawn_steps.empty()) {
            WorkflowAppendDynamicStepsCommand append{};
            append.workflow_instance_id = snapshot.workflow_instance_id;
            append.parent_workflow_step_id = snapshot.workflow_step_id;
            append.requested_by = "workflow_terminal_subscriber";
            append.steps.reserve(terminal.transition->spawn_steps.size());
            for (const auto& step : terminal.transition->spawn_steps) {
                append.steps.push_back(WorkflowAppendDynamicStepSpec{
                    .step_key = step.step_key,
                    .step_kind = step.step_kind,
                    .input_ref_kind = step.input_ref_kind,
                    .input_ref_id = step.input_ref_id,
                    .guard_kind = step.guard_kind,
                    .guard_value = step.guard_value,
                    .priority = step.priority,
                    .max_attempts = step.max_attempts,
                });
            }
            if (!command_service_->AppendDynamicSteps(append, &command_error)) {
                if (error_out) *error_out = command_error;
                return false;
            }
        } else if (terminal.transition->next_step_key.has_value()) {
            if (!command_service_->MarkStepReady(
                {
                    .workflow_instance_id = snapshot.workflow_instance_id,
                    .step_key = *terminal.transition->next_step_key,
                    .requested_by = "workflow_terminal_subscriber",
                },
                &command_error)) {
                if (error_out) *error_out = command_error;
                return false;
            }
        } else if (!command_service_->CompleteWorkflowInstance(
            {
                .workflow_instance_id = snapshot.workflow_instance_id,
                .requested_by = "workflow_terminal_subscriber",
            },
            &command_error)) {
            if (error_out) *error_out = command_error;
            return false;
        }
    }

    if (!command_service_->AppendLifecycleEvent(
        {
            .workflow_instance_id = snapshot.workflow_instance_id,
            .workflow_step_id = snapshot.workflow_step_id,
            .event_kind = event_kind,
            .message = event_message,
            .requested_by = "workflow_terminal_subscriber",
        },
        &command_error)) {
        if (error_out) *error_out = command_error;
        return false;
    }

    return true;
}

} // namespace simcore::db::execution::workflow
