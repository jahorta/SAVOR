#include "ValidationPhase3.h"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "Common/Events/OutboxRelay.h"
#include "Common/Migrations/MigrationRunner.h"
#include "Execution/Workflow/SqliteExecutionDb.h"
#include "Runner/Parallel/SimCoreDB/WorkflowCoordinatorBridge.h"
#include "Common/Types/UtcTimestamp.h"
#include "DbPreparer.h"
#include "ExecSql.h"

namespace {

using simcore::db::events::EventEnvelope;
using simcore::db::events::OutboxRelay;
using simcore::db::events::OutboxRelayDispatchBinding;
using simcore::db::events::OutboxRelayResult;

constexpr const char* kDefaultPhase3SeedRowsJson = R"JSON({
  "au_seed_probe_spec": [
    {
      "seed_probe_spec_id": 1,
      "name": "default",
      "priority": 0,
      "run_ms": 10000,
      "vi_stall_ms": 2000,
      "clear_result_winners": 1,
      "samples_per_axis": 20,
      "min_value": 47,
      "max_value": 207,
      "cap_trigger_top": 1,
      "ignore_trigger_minmax": 1,
      "combo_attempts_per_target": 200,
      "combo_sampler_tries": 8,
      "auto_schedule_battle_run": 0,
      "created_at_utc": 1743465600000
    }
  ],
  "state_artifact": [
    {
      "artifact_id": 101,
      "sha256": "phase3-placeholder-sha256",
      "size_bytes": 1024,
      "compression_kind": 0,
      "artifact_kind": "SAV",
      "filename": "placeholder_phase3.sav",
      "file_ext": ".sav",
      "created_at_utc": 1743465600000
    }
  ],
  "state_savestate": [
    {
      "savestate_id": 201,
      "artifact_id": 101,
      "savestate_type": "TRANSITION",
      "is_complete": 1,
      "created_at_utc": 1743465600000
    }
  ],
  "exec_outbox_message": [
    {
      "outbox_id": 3901,
      "event_id": "evt-p3-r1",
      "event_type": "Execution.WorkflowStepReady.v1",
      "event_version": 1,
      "context_name": "Execution",
      "aggregate_kind": "workflow_step",
      "aggregate_id": "901",
      "correlation_id": "corr-p3",
      "causation_id": "cause-p3",
      "occurred_at_utc": 1743465600000,
      "payload_ref_kind": "workflow_event",
      "payload_ref_id": 1
    }
  ]
})JSON";

} // namespace

ValidationResult ValidatePhase3ReplayRobustness(const std::filesystem::path& migration_root, const Phase3DbSeedOptions& seed_options) {
    using namespace simcore::db;
    using namespace simcore::db::events;

    ValidationResult result{ .name = "phase3.replay_robustness" };
    sqlite3* db = nullptr;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        result.message = "failed to open sqlite memory db";
        if (db != nullptr) sqlite3_close(db);
        return result;
    }
    auto close_db = [&]() { if (db != nullptr) sqlite3_close(db); db = nullptr; };

    std::string err;
    DbPreparer preparer(db, migration_root);
    if (!preparer.InitializeRequiredMigrations(&err)) {
        result.message = "failed applying execution/authoring/state migrations: " + err;
        close_db();
        return result;
    }
    if (seed_options.savestate_file.has_value() && !seed_options.savestate_file->empty()) {
        if (!preparer.SeedSavestateArtifactAndOverride(seed_options.savestate_file.value(), &err)) {
            result.message = "failed seeding --savestate-file artifact rows: " + err;
            close_db();
            return result;
        }
    }

    const auto& default_rows_json = seed_options.default_rows_json.value_or(kDefaultPhase3SeedRowsJson);
    if (!preparer.SeedRowsFromJsonObject(default_rows_json, &err)) {
        result.message = "failed seeding default fixture rows from JSON: " + err;
        close_db();
        return result;
    }
    if (seed_options.jsonl_folder.has_value() && !seed_options.jsonl_folder->empty()) {
        if (!preparer.SeedRowsFromJsonlDirectory(seed_options.jsonl_folder.value(), &err)) {
            result.message = "failed applying JSONL fixture rows: " + err;
            close_db();
            return result;
        }
    }

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT filename FROM state_artifact WHERE artifact_id=101;", -1, &st, nullptr) != SQLITE_OK
        || sqlite3_step(st) != SQLITE_ROW) {
        if (st != nullptr) sqlite3_finalize(st);
        result.message = "failed reading placeholder state_artifact row";
        close_db();
        return result;
    }
    const auto* filename_text = sqlite3_column_text(st, 0);
    const std::filesystem::path placeholder_sav = filename_text != nullptr
        ? std::filesystem::path(reinterpret_cast<const char*>(filename_text))
        : std::filesystem::path();
    sqlite3_finalize(st);
    if (placeholder_sav.extension() != ".sav") {
        result.message = "placeholder savestate fixture must use .sav extension";
        close_db();
        return result;
    }

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
    if (!relay.RelayBatchFromCursor(0, 8, bindings, &first, &err)) {
        result.message = "first replay pass failed: " + err;
        close_db();
        return result;
    }
    OutboxRelayResult second{};
    if (!relay.RelayBatchFromCursor(first.last_scanned_outbox_id, 8, bindings, &second, &err)) {
        result.message = "second replay pass failed: " + err;
        close_db();
        return result;
    }
    if (first.published_count != 1 || second.published_count != 0 || handled != 1) {
        result.message = "replay cursor robustness failed (first=" + std::to_string(first.published_count)
            + ", second=" + std::to_string(second.published_count)
            + ", handled=" + std::to_string(handled) + ")";
        close_db();
        return result;
    }

    result.passed = true;
    result.message = "replay cursor is robust across restarts and phase-3 placeholder authoring/state rows are present";
    close_db();
    return result;
}

ValidationResult ValidatePhase3PerServiceDedupeIsolation() {
    using namespace simcore::runner::parallel::simcoredb;

    ValidationResult result{ .name = "phase3.per_service_dedupe_isolation" };
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
    if (!a_first || a_dup || !b_first || b_dup) {
        result.message = "dedupe expectations failed across bridge instances";
        return result;
    }
    if (service_a_seen != 1 || service_b_seen != 1) {
        result.message = "terminal callbacks must fire once per service-local dedupe table";
        return result;
    }

    result.passed = true;
    result.message = "dedupe isolation verified: each service instance dedupes independently";
    return result;
}

ValidationResult ValidatePhase3ProgressTerminalStreamSeparation(const std::filesystem::path& migration_root) {
    using namespace simcore::db;
    using namespace simcore::db::execution::workflow;
    using namespace simcore::db::migrations;

    ValidationResult result{ .name = "phase3.progress_terminal_stream_separation" };
    sqlite3* db = nullptr;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        result.message = "failed to open sqlite memory db";
        if (db != nullptr) sqlite3_close(db);
        return result;
    }
    auto close_db = [&]() { if (db != nullptr) sqlite3_close(db); db = nullptr; };

    std::string err;
    const MigrationSourceOptions options{ .source_kind = MigrationSourceKind::Filesystem, .filesystem_root = migration_root };
    if (!ApplyContextMigrations(db, MigrationContext::Execution, options, &err)) {
        result.message = "failed applying execution migrations: " + err;
        close_db();
        return result;
    }
    if (!ExecSql(db, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(3801, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'validation', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, created_at_utc, ready_at_utc)
VALUES(3802, 3801, 'Neutral', 'seedprobe.neutral', 'READY', 0, 2, unixepoch()*1000, unixepoch()*1000);
)SQL", &err)) {
        result.message = "seed setup failed: " + err;
        close_db();
        return result;
    }

    SqliteExecutionDb execution_db(db);
    auto* commands = execution_db.WorkflowCommandService();
    if (commands == nullptr) {
        result.message = "workflow command service unavailable";
        close_db();
        return result;
    }

    for (int i = 0; i < 100; ++i) {
        if (!commands->AppendStepInputEvent({
                .workflow_instance_id = 3801,
                .workflow_step_id = 3802,
                .event_kind = "Execution.WorkflowStepInputFragmentReady.v1",
                .source_key = std::optional<std::string>("progress"),
                .request_id = std::optional<std::string>("frag-" + std::to_string(i)),
                .message = std::optional<std::string>("progress"),
                .requested_by = "SimCoreDBValidation",
            }, &err)) {
            result.message = "failed appending progress fragments: " + err;
            close_db();
            return result;
        }
    }
    if (!commands->MarkStepTerminal({ .workflow_step_id = 3802, .terminal_state = "COMPLETED", .requested_by = "SimCoreDBValidation" }, &err)) {
        result.message = "failed writing terminal event: " + err;
        close_db();
        return result;
    }

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM exec_workflow_input_event WHERE workflow_step_id=3802;", -1, &st, nullptr) != SQLITE_OK
        || sqlite3_step(st) != SQLITE_ROW) {
        if (st != nullptr) sqlite3_finalize(st);
        result.message = "failed counting progress stream records";
        close_db();
        return result;
    }
    const int input_count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_step_id=3802;", -1, &st, nullptr) != SQLITE_OK
        || sqlite3_step(st) != SQLITE_ROW) {
        if (st != nullptr) sqlite3_finalize(st);
        result.message = "failed counting terminal stream records";
        close_db();
        return result;
    }
    const int terminal_count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);

    if (input_count < 100 || terminal_count != 1) {
        result.message = "expected high-volume input stream and one terminal stream record";
        close_db();
        return result;
    }

    result.passed = true;
    result.message = "progress input events and terminal events remain separated under high-frequency traffic";
    close_db();
    return result;
}

ValidationResult ValidatePhase3LagDeadLetterReadiness(const std::filesystem::path& migration_root) {
    using namespace simcore::db;
    using namespace simcore::db::events;
    using namespace simcore::db::migrations;
    using namespace simcore::db::retention;

    ValidationResult result{ .name = "phase3.lag_dead_letter_readiness" };
    sqlite3* db = nullptr;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        result.message = "failed to open sqlite memory db";
        if (db != nullptr) sqlite3_close(db);
        return result;
    }
    auto close_db = [&]() { if (db != nullptr) sqlite3_close(db); db = nullptr; };

    std::string err;
    const MigrationSourceOptions options{ .source_kind = MigrationSourceKind::Filesystem, .filesystem_root = migration_root };
    if (!ApplyContextMigrations(db, MigrationContext::Execution, options, &err)) {
        result.message = "failed applying execution migrations: " + err;
        close_db();
        return result;
    }
    if (!ExecSql(db, R"SQL(
INSERT INTO exec_outbox_message(
    outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id
) VALUES
    (3951,'evt-p3-lag','Execution.WorkflowStepReady.v1',1,'Execution','workflow_step','1','corr','cause',unixepoch()*1000,'workflow_event',1);
)SQL", &err)) {
        result.message = "failed seeding outbox relay row: " + err;
        close_db();
        return result;
    }

    OutboxRelay relay({
        .db = db,
        .outbox_table = "exec_outbox_message",
        .context_name = "Execution",
        .max_attempts = 1,
    });
    std::vector<OutboxRelayDispatchBinding> bindings{ {
        .key = { .event_type = "Execution.WorkflowStepReady.v1", .event_version = 1 },
        .handler = [&](const EventEnvelope&, std::string* handler_error) {
            if (handler_error) *handler_error = "intentional-failure";
            return false;
        },
    } };
    OutboxRelayResult relay_result{};
    if (!relay.RelayBatchFromCursor(0, 10, bindings, &relay_result, &err)) {
        result.message = "relay batch failed: " + err;
        close_db();
        return result;
    }
    if (relay_result.dead_lettered_count != 1) {
        result.message = "expected one dead-lettered row";
        close_db();
        return result;
    }

    simcore::db::execution::workflow::SqliteExecutionDb execution_db(db);
    const auto preview = execution_db.PreviewOutboxRetention(
        std::vector<OutboxSubscriptionSnapshot>{
            OutboxSubscriptionSnapshot{
                .projector_name = "phase3-validation-subscriber",
                .last_outbox_id = 0,
                .updated_at_utc = simcore::db::types::UtcNow(),
                .status = "ACTIVE",
            },
        },
        simcore::db::types::UtcNow(),
        OutboxRetentionPolicy{});
    if (preview.lag_per_subscription.empty() || preview.lag_per_subscription.front().lag_outbox_rows < 1) {
        result.message = "lag readiness expected at least one lagging outbox row";
        close_db();
        return result;
    }

    result.passed = true;
    result.message = "dead-letter and lag readiness checks passed";
    close_db();
    return result;
}
