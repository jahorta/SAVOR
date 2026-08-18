#include "SqliteUiReadDb.h"

#include <algorithm>
#include <chrono>

namespace savor::db {

namespace {

std::int64_t ToEpochMillis(types::UtcTimePoint value) {
    return value.time_since_epoch().count();
}

types::UtcTimePoint FromEpochMillis(std::int64_t value) {
    return types::UtcTimePoint{ std::chrono::milliseconds(value) };
}

std::string ColumnText(sqlite3_stmt* st, int column) {
    const auto* text = sqlite3_column_text(st, column);
    return text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
}

std::optional<std::string> ColumnTextOptional(sqlite3_stmt* st, int column) {
    if (sqlite3_column_type(st, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return ColumnText(st, column);
}

std::optional<std::int64_t> ColumnInt64Optional(sqlite3_stmt* st, int column) {
    if (sqlite3_column_type(st, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st, column);
}

bool IsSubscriptionKeyValid(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table) {
    return !projector_name.empty() && !source_context.empty() && !source_outbox_table.empty();
}

bool ExecSql(sqlite3* db, const char* sql) {
    char* err_msg = nullptr;
    const auto rc = sqlite3_exec(db, sql, nullptr, nullptr, &err_msg);
    sqlite3_free(err_msg);
    return rc == SQLITE_OK;
}

std::optional<UiProjectionSubscription> ReadSubscription(
    sqlite3* db,
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table) {
    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT projector_name, source_context, source_outbox_table, "
        "COALESCE(last_outbox_id, 0), COALESCE(last_event_id, ''), updated_at_utc, "
        "COALESCE(status, 'ACTIVE'), COALESCE(last_error, '') "
        "FROM ui_projection_subscription "
        "WHERE projector_name=?1 AND source_context=?2 AND source_outbox_table=?3;";

    if (sqlite3_prepare_v2(db, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_text(st, 1, projector_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<UiProjectionSubscription> subscription;
    if (sqlite3_step(st) == SQLITE_ROW) {
        subscription = UiProjectionSubscription{};
        const auto* projector_name_text = sqlite3_column_text(st, 0);
        const auto* source_context_text = sqlite3_column_text(st, 1);
        const auto* source_outbox_table_text = sqlite3_column_text(st, 2);
        const auto* last_event_id_text = sqlite3_column_text(st, 4);
        const auto* status_text = sqlite3_column_text(st, 6);
        const auto* last_error_text = sqlite3_column_text(st, 7);

        subscription->projector_name = projector_name_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(projector_name_text);
        subscription->source_context = source_context_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(source_context_text);
        subscription->source_outbox_table = source_outbox_table_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(source_outbox_table_text);
        subscription->last_outbox_id = sqlite3_column_int64(st, 3);
        subscription->last_event_id = last_event_id_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(last_event_id_text);
        subscription->updated_at_utc = FromEpochMillis(sqlite3_column_int64(st, 5));
        subscription->status = status_text == nullptr
            ? std::string{ "ACTIVE" }
            : reinterpret_cast<const char*>(status_text);
        subscription->last_error = last_error_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(last_error_text);
    }

    sqlite3_finalize(st);
    return subscription;
}

std::optional<UiSeedProbeRunSummary> ReadSeedProbeSummaryRow(sqlite3_stmt* st) {
    UiSeedProbeRunSummary summary{};
    summary.probe_run_id = sqlite3_column_int64(st, 0);
    summary.probe_set_id = sqlite3_column_int64(st, 1);
    summary.entry_savestate_id = sqlite3_column_int64(st, 2);
    summary.seed_probe_spec_id = sqlite3_column_int64(st, 3);
    summary.codec_version = sqlite3_column_int(st, 4);
    const auto* status_text = sqlite3_column_text(st, 5);
    summary.status = status_text == nullptr ? std::string{} : reinterpret_cast<const char*>(status_text);
    if (sqlite3_column_type(st, 6) != SQLITE_NULL) {
        summary.neutral_seed_value = static_cast<std::uint32_t>(sqlite3_column_int64(st, 6));
    }
    summary.grid_count = sqlite3_column_int(st, 7);
    summary.unique_count = sqlite3_column_int(st, 8);
    summary.requested_at_utc = sqlite3_column_int64(st, 9);
    if (sqlite3_column_type(st, 10) != SQLITE_NULL) {
        summary.completed_at_utc = sqlite3_column_int64(st, 10);
    }
    return summary;
}

UiJobSummary ReadJobSummaryRow(sqlite3_stmt* st) {
    UiJobSummary row{};
    row.job_id = sqlite3_column_int64(st, 0);
    row.job_set_id = sqlite3_column_int64(st, 1);
    row.program_kind = sqlite3_column_int(st, 2);
    const auto* state_text = sqlite3_column_text(st, 3);
    row.state = state_text == nullptr ? std::string{} : reinterpret_cast<const char*>(state_text);
    row.priority = sqlite3_column_int(st, 4);
    row.queued_at_utc = sqlite3_column_int64(st, 5);
    if (sqlite3_column_type(st, 6) != SQLITE_NULL) {
        row.started_at_utc = sqlite3_column_int64(st, 6);
    }
    if (sqlite3_column_type(st, 7) != SQLITE_NULL) {
        row.ended_at_utc = sqlite3_column_int64(st, 7);
    }
    const auto* error_code_text = sqlite3_column_text(st, 8);
    row.error_code = error_code_text == nullptr ? std::string{} : reinterpret_cast<const char*>(error_code_text);
    row.attempts = sqlite3_column_int(st, 9);
    row.max_attempts = sqlite3_column_int(st, 10);
    const auto* error_text = sqlite3_column_text(st, 11);
    row.error_text = error_text == nullptr ? std::string{} : reinterpret_cast<const char*>(error_text);
    row.result_processing_state = ColumnText(st, 12);
    row.result_processing_attempts = sqlite3_column_int(st, 13);
    row.result_processing_failures = sqlite3_column_int(st, 14);
    row.result_processing_error_code = ColumnText(st, 15);
    row.result_processing_error_text = ColumnText(st, 16);
    if (sqlite3_column_type(st, 17) != SQLITE_NULL) {
        row.last_progress_attempt_id = static_cast<std::uint64_t>(
            sqlite3_column_int64(st, 17));
    }
    if (sqlite3_column_type(st, 18) != SQLITE_NULL) {
        row.last_progress_ordinal = static_cast<std::uint64_t>(
            sqlite3_column_int64(st, 18));
    }
    row.last_progress_text = ColumnText(st, 19);
    if (sqlite3_column_type(st, 20) != SQLITE_NULL) {
        row.last_progress_at_utc = sqlite3_column_int64(st, 20);
    }
    return row;
}

UiJobDetail ReadJobDetailRow(sqlite3_stmt* st) {
    UiJobDetail row{};
    row.summary = ReadJobSummaryRow(st);
    row.fingerprint = ColumnText(st, 21);
    row.claimed_by_token = ColumnTextOptional(st, 22);
    if (sqlite3_column_type(st, 23) != SQLITE_NULL) {
        row.lease_expires_at_utc = sqlite3_column_int64(st, 23);
    }
    return row;
}

void AddJobStateCount(UiJobStateCounts& counts, const std::string& state, std::int64_t count) {
    counts.total += count;
    if (state == "QUEUED") {
        counts.queued += count;
    } else if (state == "CLAIMED") {
        counts.claimed += count;
    } else if (state == "RUNNING") {
        counts.running += count;
    } else if (state == "EXECUTION_FINISHED") {
        counts.execution_finished += count;
    } else if (state == "FAILED") {
        counts.failed += count;
    } else if (state == "CANCELED") {
        counts.canceled += count;
    } else if (state == "SUPERSEDED") {
        counts.superseded += count;
    } else if (state == "SUCCEEDED"
        || state == "SUCCEEDED_WINNER"
        || state == "SUCCEEDED_DUPLICATE") {
        counts.succeeded += count;
    } else {
        counts.other += count;
    }
}

void AddWorkflowDisplayStateCount(
    UiWorkflowDisplayStateCounts& counts,
    const std::string& display_state,
    std::int64_t count) {
    counts.total += count;
    if (display_state == "RUNNING") {
        counts.running += count;
    } else if (display_state == "QUEUED") {
        counts.queued += count;
    } else if (display_state == "WAITING") {
        counts.waiting += count;
    } else if (display_state == "COMPLETED") {
        counts.completed += count;
        counts.terminal += count;
    } else if (display_state == "FAILED") {
        counts.failed += count;
        counts.terminal += count;
    } else if (display_state == "CANCELED") {
        counts.canceled += count;
        counts.terminal += count;
    } else {
        counts.other += count;
    }
}

UiJobSetSummary ReadJobSetSummaryRow(sqlite3_stmt* st) {
    UiJobSetSummary row{};
    row.job_set_id = sqlite3_column_int64(st, 0);
    row.program_kind = sqlite3_column_int(st, 1);
    row.created_at_utc = sqlite3_column_int64(st, 2);
    row.total_jobs = sqlite3_column_int64(st, 3);
    row.completed_jobs = sqlite3_column_int64(st, 4);
    row.succeeded_jobs = sqlite3_column_int64(st, 5);
    row.failed_jobs = sqlite3_column_int64(st, 6);
    row.canceled_jobs = sqlite3_column_int64(st, 7);
    return row;
}

UiWorkflowInstanceSummary ReadWorkflowInstanceRow(sqlite3_stmt* st) {
    UiWorkflowInstanceSummary row{};
    row.workflow_instance_id = sqlite3_column_int64(st, 0);
    row.workflow_kind = ColumnText(st, 1);
    row.state = ColumnText(st, 2);
    row.display_state = ColumnText(st, 3);
    row.root_scope_kind = ColumnText(st, 4);
    row.root_scope_id = ColumnInt64Optional(st, 5);
    row.created_by = ColumnText(st, 6);
    row.blocked_step_count = sqlite3_column_int64(st, 7);
    row.failed_step_count = sqlite3_column_int64(st, 8);
    row.created_at_utc = sqlite3_column_int64(st, 9);
    row.started_at_utc = ColumnInt64Optional(st, 10);
    row.completed_at_utc = ColumnInt64Optional(st, 11);
    row.failure_code = ColumnText(st, 12);
    row.failure_text = ColumnText(st, 13);
    row.battle_advancement_rank = sqlite3_column_int(st, 14);
    row.battle_desired_outcome_count = sqlite3_column_int64(st, 15);
    row.battle_final_victory_count = sqlite3_column_int64(st, 16);
    row.battle_selected_count = sqlite3_column_int64(st, 17);
    return row;
}

UiWorkflowStepSummary ReadWorkflowStepRow(sqlite3_stmt* st) {
    UiWorkflowStepSummary row{};
    row.workflow_step_id = sqlite3_column_int64(st, 0);
    row.workflow_instance_id = sqlite3_column_int64(st, 1);
    row.workflow_unit_activation_id = ColumnInt64Optional(st, 2);
    row.step_key = ColumnText(st, 3);
    row.step_kind = ColumnText(st, 4);
    row.state = ColumnText(st, 5);
    row.blocked_reason = ColumnText(st, 6);
    row.job_set_id = ColumnInt64Optional(st, 7);
    row.job_count = sqlite3_column_int64(st, 8);
    row.job_completed_count = sqlite3_column_int64(st, 9);
    row.job_failed_count = sqlite3_column_int64(st, 10);
    row.priority = sqlite3_column_int(st, 11);
    row.attempts = sqlite3_column_int(st, 12);
    row.max_attempts = sqlite3_column_int(st, 13);
    row.ready_at_utc = ColumnInt64Optional(st, 14);
    row.started_at_utc = ColumnInt64Optional(st, 15);
    row.completed_at_utc = ColumnInt64Optional(st, 16);
    row.failed_at_utc = ColumnInt64Optional(st, 17);
    row.created_at_utc = sqlite3_column_int64(st, 18);
    row.battle_advancement_rank = sqlite3_column_int(st, 19);
    row.battle_desired_outcome_count = sqlite3_column_int64(st, 20);
    row.battle_final_victory_count = sqlite3_column_int64(st, 21);
    row.battle_selected_count = sqlite3_column_int64(st, 22);
    return row;
}

UiWorkflowUnitActivationSummary ReadWorkflowUnitActivationRow(sqlite3_stmt* st) {
    UiWorkflowUnitActivationSummary row{};
    row.workflow_unit_activation_id = sqlite3_column_int64(st, 0);
    row.workflow_instance_id = sqlite3_column_int64(st, 1);
    row.parent_workflow_unit_activation_id = ColumnInt64Optional(st, 2);
    row.activation_key = ColumnText(st, 3);
    row.graph_node_key = ColumnText(st, 4);
    row.unit_kind = ColumnText(st, 5);
    row.display_name = ColumnText(st, 6);
    row.state = ColumnText(st, 7);
    row.activation_params_json = ColumnText(st, 8);
    row.authored_ref_kind = ColumnText(st, 9);
    row.authored_ref_id = ColumnInt64Optional(st, 10);
    row.failure_code = ColumnText(st, 11);
    row.failure_text = ColumnText(st, 12);
    row.created_at_utc = sqlite3_column_int64(st, 13);
    row.ready_at_utc = ColumnInt64Optional(st, 14);
    row.started_at_utc = ColumnInt64Optional(st, 15);
    row.completed_at_utc = ColumnInt64Optional(st, 16);
    row.failed_at_utc = ColumnInt64Optional(st, 17);
    return row;
}

UiWorkflowEdgeSummary ReadWorkflowEdgeRow(sqlite3_stmt* st) {
    UiWorkflowEdgeSummary row{};
    row.workflow_edge_id = sqlite3_column_int64(st, 0);
    row.workflow_instance_id = sqlite3_column_int64(st, 1);
    row.from_step_id = sqlite3_column_int64(st, 2);
    row.to_step_id = sqlite3_column_int64(st, 3);
    row.condition_kind = ColumnText(st, 4);
    row.condition_value = ColumnText(st, 5);
    row.created_at_utc = sqlite3_column_int64(st, 6);
    return row;
}

UiWorkflowUnitActivationEdgeSummary ReadWorkflowUnitActivationEdgeRow(sqlite3_stmt* st) {
    UiWorkflowUnitActivationEdgeSummary row{};
    row.workflow_unit_activation_edge_id = sqlite3_column_int64(st, 0);
    row.workflow_instance_id = sqlite3_column_int64(st, 1);
    row.from_workflow_unit_activation_id = sqlite3_column_int64(st, 2);
    row.to_workflow_unit_activation_id = sqlite3_column_int64(st, 3);
    row.output_key = ColumnText(st, 4);
    row.input_key = ColumnText(st, 5);
    row.condition_kind = ColumnText(st, 6);
    row.condition_value = ColumnText(st, 7);
    row.created_at_utc = sqlite3_column_int64(st, 8);
    return row;
}

UiWorkflowAlertSummary ReadWorkflowAlertRow(sqlite3_stmt* st) {
    UiWorkflowAlertSummary row{};
    row.workflow_alert_id = sqlite3_column_int64(st, 0);
    row.workflow_instance_id = sqlite3_column_int64(st, 1);
    row.workflow_step_id = ColumnInt64Optional(st, 2);
    row.alert_kind = ColumnText(st, 3);
    row.alert_code = ColumnText(st, 4);
    row.message = ColumnText(st, 5);
    row.is_active = sqlite3_column_int(st, 6) != 0;
    row.first_seen_at_utc = sqlite3_column_int64(st, 7);
    row.last_seen_at_utc = sqlite3_column_int64(st, 8);
    row.cleared_at_utc = ColumnInt64Optional(st, 9);
    return row;
}

UiBattleGroupSummary ReadBattleGroupRow(sqlite3_stmt* st) {
    UiBattleGroupSummary row{};
    row.battle_set_id = sqlite3_column_int64(st, 0);
    row.name = ColumnText(st, 1);
    row.status = ColumnText(st, 2);
    row.created_at_utc = sqlite3_column_int64(st, 3);
    row.completed_at_utc = ColumnInt64Optional(st, 4);
    row.wave_count = sqlite3_column_int64(st, 5);
    row.job_count = sqlite3_column_int64(st, 6);
    row.selected_count = sqlite3_column_int64(st, 7);
    row.desired_outcome_count = sqlite3_column_int64(st, 8);
    row.final_victory_count = sqlite3_column_int64(st, 9);
    row.failed_count = sqlite3_column_int64(st, 10);
    row.manual_followup_count = sqlite3_column_int64(st, 11);
    row.advancement_rank = sqlite3_column_int(st, 12);
    return row;
}

UiBattleWaveSummary ReadBattleWaveRow(sqlite3_stmt* st) {
    UiBattleWaveSummary row{};
    row.wave_id = sqlite3_column_int64(st, 0);
    row.battle_set_id = sqlite3_column_int64(st, 1);
    row.parent_wave_id = ColumnInt64Optional(st, 2);
    row.parent_turn_job_id = ColumnInt64Optional(st, 3);
    row.turn_index = sqlite3_column_int(st, 4);
    row.status = ColumnText(st, 5);
    row.created_at_utc = sqlite3_column_int64(st, 6);
    row.completed_at_utc = ColumnInt64Optional(st, 7);
    row.job_count = sqlite3_column_int64(st, 8);
    row.selected_count = sqlite3_column_int64(st, 9);
    row.desired_outcome_count = sqlite3_column_int64(st, 10);
    row.final_victory_count = sqlite3_column_int64(st, 11);
    row.failed_count = sqlite3_column_int64(st, 12);
    row.advancement_rank = sqlite3_column_int(st, 13);
    return row;
}

UiBattleAdvancementDecisionSummary ReadBattleAdvancementDecisionRow(sqlite3_stmt* st, int first_column) {
    UiBattleAdvancementDecisionSummary row{};
    row.battle_advancement_decision_id = sqlite3_column_int64(st, first_column);
    row.battle_advancement_pool_id = sqlite3_column_int64(st, first_column + 1);
    row.turn_job_id = sqlite3_column_int64(st, first_column + 2);
    row.decision_kind = ColumnText(st, first_column + 3);
    row.decision_reason = ColumnText(st, first_column + 4);
    row.created_at_utc = sqlite3_column_int64(st, first_column + 5);
    return row;
}

UiBattleManualFollowupSummary ReadBattleManualFollowupRow(sqlite3_stmt* st, int first_column) {
    UiBattleManualFollowupSummary row{};
    row.turn_job_id = sqlite3_column_int64(st, first_column);
    row.manual_followup_status = ColumnText(st, first_column + 1);
    row.recorded_dtm_artifact_id = ColumnInt64Optional(st, first_column + 2);
    row.note = ColumnText(st, first_column + 3);
    row.updated_at_utc = sqlite3_column_int64(st, first_column + 4);
    return row;
}

UiBattleTurnJobSummary ReadBattleTurnJobRow(sqlite3_stmt* st) {
    UiBattleTurnJobSummary row{};
    row.turn_job_id = sqlite3_column_int64(st, 0);
    row.exec_job_id = ColumnInt64Optional(st, 1);
    row.wave_id = sqlite3_column_int64(st, 2);
    row.battle_set_id = sqlite3_column_int64(st, 3);
    row.turn_index = sqlite3_column_int(st, 4);
    row.job_state = ColumnText(st, 5);
    row.fake_attacks_this_turn = sqlite3_column_int(st, 6);
    row.fake_attacks_used_before = sqlite3_column_int(st, 7);
    row.rng_seed = ColumnInt64Optional(st, 8);
    row.delta_vi = ColumnInt64Optional(st, 9);
    if (sqlite3_column_type(st, 10) != SQLITE_NULL) {
        row.pred_passed = sqlite3_column_int(st, 10);
    }
    if (sqlite3_column_type(st, 11) != SQLITE_NULL) {
        row.pred_total = sqlite3_column_int(st, 11);
    }
    if (sqlite3_column_type(st, 12) != SQLITE_NULL) {
        row.battle_outcome = sqlite3_column_int(st, 12);
    }
    row.has_desired_outcome = sqlite3_column_int(st, 13) != 0;
    row.has_final_victory_outcome = sqlite3_column_int(st, 14) != 0;
    row.selected_for_advancement = sqlite3_column_int(st, 15) != 0;
    row.advancement_decision_kind = ColumnText(st, 16);
    row.advancement_rank = sqlite3_column_int(st, 17);
    row.started_at_utc = ColumnInt64Optional(st, 18);
    row.ended_at_utc = ColumnInt64Optional(st, 19);
    if (sqlite3_column_type(st, 20) != SQLITE_NULL) {
        row.advancement_decision = ReadBattleAdvancementDecisionRow(st, 20);
    }
    if (sqlite3_column_type(st, 26) != SQLITE_NULL) {
        row.manual_followup = ReadBattleManualFollowupRow(st, 26);
    }
    return row;
}

UiBattleTurnJobReplicationRow ReadBattleTurnJobReplicationRow(sqlite3_stmt* st) {
    UiBattleTurnJobReplicationRow row{};
    row.turn_job_id = sqlite3_column_int64(st, 0);
    row.battle_set_id = sqlite3_column_int64(st, 1);
    row.wave_id = sqlite3_column_int64(st, 2);
    row.parent_wave_id = ColumnInt64Optional(st, 3);
    row.parent_turn_job_id = ColumnInt64Optional(st, 4);
    row.exec_job_id = ColumnInt64Optional(st, 5);
    row.source_savestate_id = ColumnInt64Optional(st, 6);
    row.seed_candidate_id = ColumnInt64Optional(st, 7);
    row.authored_plan_id = ColumnInt64Optional(st, 8);
    if (sqlite3_column_type(st, 9) != SQLITE_NULL) {
        row.authored_turn_index = sqlite3_column_int(st, 9);
    }
    row.resolved_turn_commands_blob = ColumnTextOptional(st, 10);
    row.resolved_turn_variant_key = ColumnTextOptional(st, 11);
    row.fake_attacks_used_before = sqlite3_column_int(st, 12);
    row.fake_attacks_this_turn = sqlite3_column_int(st, 13);
    row.output_savestate_id = ColumnInt64Optional(st, 14);
    row.input_trace_artifact_id = ColumnInt64Optional(st, 15);
    return row;
}

} // namespace

SqliteUiReadDb::SqliteUiReadDb(sqlite3* db)
    : db_(db) {
}

std::vector<UiProgramKind> SqliteUiReadDb::ListProgramKinds() const {
    std::vector<UiProgramKind> rows;
    if (db_ == nullptr) {
        return rows;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT DISTINCT program_kind FROM ui_job_summary ORDER BY program_kind ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return rows;
    }

    while (sqlite3_step(st) == SQLITE_ROW) {
        UiProgramKind row{};
        row.id = sqlite3_column_int(st, 0);
        row.name = "kind " + std::to_string(row.id);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(st);
    return rows;
}

UiReadPage<UiJobSummary> SqliteUiReadDb::ListJobs(
    const UiReadJobListQuery& query) const {
    UiReadPage<UiJobSummary> page{};
    if (db_ == nullptr || query.limit <= 0) {
        return page;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT s.job_id,s.job_set_id,s.program_kind,s.state,s.priority,s.queued_at_utc,"
        "s.started_at_utc,s.ended_at_utc,COALESCE(s.error_code,''),"
        "COALESCE(d.attempts,0),COALESCE(d.max_attempts,0),COALESCE(d.error_text,''),"
        "COALESCE(d.result_processing_state,''),COALESCE(d.result_processing_attempts,0),"
        "COALESCE(d.result_processing_failures,0),COALESCE(d.result_processing_error_code,''),"
        "COALESCE(d.result_processing_error_text,''),s.last_progress_attempt_id,"
        "s.last_progress_ordinal,COALESCE(s.last_progress_text,''),s.last_progress_at_utc "
        "FROM ui_job_summary s LEFT JOIN ui_job_detail d ON d.job_id=s.job_id "
        "WHERE (?1=0 OR s.program_kind=?2) "
        "AND (?3=0 OR s.job_set_id=?4) "
        "AND (?5=1 OR s.state IN (?6,?7,?8,?9,?10)) "
        "AND (?11=0 OR s.queued_at_utc < ?12 OR (s.queued_at_utc=?12 AND s.job_id < ?13)) "
        "AND (?14=0 OR s.queued_at_utc > ?15 OR (s.queued_at_utc=?15 AND s.job_id > ?16)) "
        "ORDER BY s.queued_at_utc DESC, s.job_id DESC LIMIT ?17;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return page;
    }

    sqlite3_bind_int(st, 1, query.program_kind.has_value() ? 1 : 0);
    sqlite3_bind_int(st, 2, query.program_kind.value_or(0));
    sqlite3_bind_int(st, 3, query.job_set_id.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 4, query.job_set_id.value_or(0));
    sqlite3_bind_int(st, 5, query.states.empty() ? 1 : 0);
    for (int i = 0; i < 5; ++i) {
        const std::string value = i < static_cast<int>(query.states.size()) ? query.states[static_cast<std::size_t>(i)] : std::string{};
        sqlite3_bind_text(st, 6 + i, value.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(st, 11, query.before.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 12, query.before.value_or(UiReadListCursor{}).primary);
    sqlite3_bind_int64(st, 13, query.before.value_or(UiReadListCursor{}).secondary);
    sqlite3_bind_int(st, 14, query.after.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 15, query.after.value_or(UiReadListCursor{}).primary);
    sqlite3_bind_int64(st, 16, query.after.value_or(UiReadListCursor{}).secondary);
    sqlite3_bind_int(st, 17, query.limit);

    while (sqlite3_step(st) == SQLITE_ROW) {
        page.items.push_back(ReadJobSummaryRow(st));
    }
    sqlite3_finalize(st);

    if (!page.items.empty()) {
        const auto& first = page.items.front();
        const auto& last = page.items.back();
        page.prev = UiReadListCursor{ first.queued_at_utc, first.job_id };
        page.next = UiReadListCursor{ last.queued_at_utc, last.job_id };
    }
    return page;
}

UiJobStateCounts SqliteUiReadDb::CountJobsByState(
    const UiReadJobListQuery& query) const {
    UiJobStateCounts counts{};
    if (db_ == nullptr) {
        return counts;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT s.state, COUNT(*) "
        "FROM ui_job_summary s "
        "WHERE (?1=0 OR s.program_kind=?2) "
        "AND (?3=0 OR s.job_set_id=?4) "
        "AND (?5=1 OR s.state IN (?6,?7,?8,?9,?10)) "
        "GROUP BY s.state;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return counts;
    }

    sqlite3_bind_int(st, 1, query.program_kind.has_value() ? 1 : 0);
    sqlite3_bind_int(st, 2, query.program_kind.value_or(0));
    sqlite3_bind_int(st, 3, query.job_set_id.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 4, query.job_set_id.value_or(0));
    sqlite3_bind_int(st, 5, query.states.empty() ? 1 : 0);
    for (int i = 0; i < 5; ++i) {
        const std::string value = i < static_cast<int>(query.states.size()) ? query.states[static_cast<std::size_t>(i)] : std::string{};
        sqlite3_bind_text(st, 6 + i, value.c_str(), -1, SQLITE_TRANSIENT);
    }

    while (sqlite3_step(st) == SQLITE_ROW) {
        AddJobStateCount(counts, ColumnText(st, 0), sqlite3_column_int64(st, 1));
    }
    sqlite3_finalize(st);
    return counts;
}

std::optional<UiJobSummary> SqliteUiReadDb::GetJobSummary(std::int64_t job_id) const {
    if (db_ == nullptr || job_id <= 0) {
        return std::nullopt;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT s.job_id,s.job_set_id,s.program_kind,s.state,s.priority,s.queued_at_utc,"
        "s.started_at_utc,s.ended_at_utc,COALESCE(s.error_code,''),"
        "COALESCE(d.attempts,0),COALESCE(d.max_attempts,0),COALESCE(d.error_text,''),"
        "COALESCE(d.result_processing_state,''),COALESCE(d.result_processing_attempts,0),"
        "COALESCE(d.result_processing_failures,0),COALESCE(d.result_processing_error_code,''),"
        "COALESCE(d.result_processing_error_text,''),s.last_progress_attempt_id,"
        "s.last_progress_ordinal,COALESCE(s.last_progress_text,''),s.last_progress_at_utc "
        "FROM ui_job_summary s LEFT JOIN ui_job_detail d ON d.job_id=s.job_id "
        "WHERE s.job_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st, 1, job_id);
    std::optional<UiJobSummary> row;
    if (sqlite3_step(st) == SQLITE_ROW) {
        row = ReadJobSummaryRow(st);
    }
    sqlite3_finalize(st);
    return row;
}

std::optional<UiJobDetail> SqliteUiReadDb::GetJobDetail(std::int64_t job_id) const {
    if (db_ == nullptr || job_id <= 0) {
        return std::nullopt;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT s.job_id,s.job_set_id,s.program_kind,s.state,s.priority,s.queued_at_utc,"
        "s.started_at_utc,s.ended_at_utc,COALESCE(s.error_code,''),"
        "COALESCE(d.attempts,0),COALESCE(d.max_attempts,0),COALESCE(d.error_text,''),"
        "COALESCE(d.result_processing_state,''),COALESCE(d.result_processing_attempts,0),"
        "COALESCE(d.result_processing_failures,0),COALESCE(d.result_processing_error_code,''),"
        "COALESCE(d.result_processing_error_text,''),s.last_progress_attempt_id,"
        "s.last_progress_ordinal,COALESCE(s.last_progress_text,''),s.last_progress_at_utc,"
        "COALESCE(d.fingerprint,''),d.claimed_by_token,d.lease_expires_at_utc "
        "FROM ui_job_summary s LEFT JOIN ui_job_detail d ON d.job_id=s.job_id "
        "WHERE s.job_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st, 1, job_id);
    std::optional<UiJobDetail> row;
    if (sqlite3_step(st) == SQLITE_ROW) {
        row = ReadJobDetailRow(st);
    }
    sqlite3_finalize(st);
    return row;
}

std::vector<UiJobArtifact> SqliteUiReadDb::ListJobArtifacts(std::int64_t job_id) const {
    std::vector<UiJobArtifact> rows;
    if (db_ == nullptr || job_id <= 0) {
        return rows;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT a.artifact_id,a.role_kind,COALESCE(b.filename,''),COALESCE(b.size_bytes,0),"
        "COALESCE(b.artifact_kind,''),COALESCE(a.created_at_utc,0) "
        "FROM ui_job_artifact a LEFT JOIN ui_artifact_browser b ON b.artifact_id=a.artifact_id "
        "WHERE a.job_id=?1 ORDER BY a.created_at_utc ASC, a.ui_job_artifact_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st, 1, job_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        UiJobArtifact row{};
        row.artifact_id = sqlite3_column_int64(st, 0);
        const auto* role_text = sqlite3_column_text(st, 1);
        row.role_kind = role_text == nullptr ? std::string{} : reinterpret_cast<const char*>(role_text);
        const auto* filename_text = sqlite3_column_text(st, 2);
        row.filename = filename_text == nullptr ? std::string{} : reinterpret_cast<const char*>(filename_text);
        row.size_bytes = static_cast<std::uint64_t>(sqlite3_column_int64(st, 3));
        const auto* kind_text = sqlite3_column_text(st, 4);
        row.artifact_kind = kind_text == nullptr ? std::string{} : reinterpret_cast<const char*>(kind_text);
        row.created_at_utc = sqlite3_column_int64(st, 5);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(st);
    return rows;
}

std::vector<UiCanonicalJobProgress> SqliteUiReadDb::ListJobProgress(
    std::int64_t job_id,
    int limit) const {
    std::vector<UiCanonicalJobProgress> rows;
    if (db_ == nullptr || job_id <= 0 || limit <= 0) return rows;
    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT job_id,attempt_id,ordinal,workset_id,item_id,invocation_id,"
        "library_id,library_revision,progress_point_id,routed_sequence,"
        "sample_snapshot_id,trigger_epoch,schema_id,schema_revision,"
        "schema_sha256,typed_payload,display_text,recorded_at_utc "
        "FROM ui_job_progress WHERE job_id=?1 "
        "ORDER BY attempt_id DESC,ordinal DESC LIMIT ?2;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st, 1, job_id);
    sqlite3_bind_int(st, 2, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        UiCanonicalJobProgress row{};
        row.job_id = sqlite3_column_int64(st, 0);
        row.attempt_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
        row.ordinal = static_cast<std::uint64_t>(sqlite3_column_int64(st, 2));
        row.workset_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 3));
        row.item_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 4));
        row.invocation_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 5));
        row.library_id = ColumnText(st, 6);
        row.library_revision = static_cast<std::uint32_t>(sqlite3_column_int(st, 7));
        row.progress_point_id = ColumnText(st, 8);
        if (sqlite3_column_type(st, 9) != SQLITE_NULL)
            row.routed_sequence = static_cast<std::uint64_t>(sqlite3_column_int64(st, 9));
        if (sqlite3_column_type(st, 10) != SQLITE_NULL)
            row.sample_snapshot_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 10));
        if (sqlite3_column_type(st, 11) != SQLITE_NULL)
            row.trigger_epoch = static_cast<std::uint64_t>(sqlite3_column_int64(st, 11));
        row.schema_id = ColumnText(st, 12);
        row.schema_revision = static_cast<std::uint32_t>(sqlite3_column_int(st, 13));
        row.schema_sha256 = ColumnText(st, 14);
        const auto blob_size = sqlite3_column_bytes(st, 15);
        const auto* blob = static_cast<const std::uint8_t*>(sqlite3_column_blob(st, 15));
        if (blob != nullptr && blob_size > 0)
            row.typed_payload.assign(blob, blob + blob_size);
        row.display_text = ColumnText(st, 16);
        row.recorded_at_utc = sqlite3_column_int64(st, 17);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(st);
    return rows;
}

UiReadPage<UiJobSetSummary> SqliteUiReadDb::ListJobSets(
    const UiReadJobSetListQuery& query) const {
    UiReadPage<UiJobSetSummary> page{};
    if (db_ == nullptr || query.limit <= 0) {
        return page;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT job_set_id,MIN(program_kind),MIN(queued_at_utc),COUNT(*),"
        "SUM(CASE WHEN state IN ('COMPLETED','DONE','SUCCEEDED') THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN state='SUCCEEDED' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN state='FAILED' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN state='CANCELED' THEN 1 ELSE 0 END) "
        "FROM ui_job_summary "
        "WHERE (?1=0 OR program_kind=?2) "
        "GROUP BY job_set_id "
        "HAVING (?3=0 OR MIN(queued_at_utc) < ?4 OR (MIN(queued_at_utc)=?4 AND job_set_id < ?5)) "
        "AND (?6=0 OR MIN(queued_at_utc) > ?7 OR (MIN(queued_at_utc)=?7 AND job_set_id > ?8)) "
        "ORDER BY MIN(queued_at_utc) DESC, job_set_id DESC LIMIT ?9;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return page;
    }

    sqlite3_bind_int(st, 1, query.program_kind.has_value() ? 1 : 0);
    sqlite3_bind_int(st, 2, query.program_kind.value_or(0));
    sqlite3_bind_int(st, 3, query.before.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 4, query.before.value_or(UiReadListCursor{}).primary);
    sqlite3_bind_int64(st, 5, query.before.value_or(UiReadListCursor{}).secondary);
    sqlite3_bind_int(st, 6, query.after.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 7, query.after.value_or(UiReadListCursor{}).primary);
    sqlite3_bind_int64(st, 8, query.after.value_or(UiReadListCursor{}).secondary);
    sqlite3_bind_int(st, 9, query.limit);

    while (sqlite3_step(st) == SQLITE_ROW) {
        page.items.push_back(ReadJobSetSummaryRow(st));
    }
    sqlite3_finalize(st);

    if (!page.items.empty()) {
        const auto& first = page.items.front();
        const auto& last = page.items.back();
        page.prev = UiReadListCursor{ first.created_at_utc, first.job_set_id };
        page.next = UiReadListCursor{ last.created_at_utc, last.job_set_id };
    }
    return page;
}

std::optional<UiJobSetDetail> SqliteUiReadDb::GetJobSetDetail(
    std::int64_t job_set_id,
    int jobs_limit) const {
    if (db_ == nullptr || job_set_id <= 0) {
        return std::nullopt;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT job_set_id,MIN(program_kind),MIN(queued_at_utc),COUNT(*),"
        "SUM(CASE WHEN state IN ('COMPLETED','DONE','SUCCEEDED') THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN state='SUCCEEDED' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN state='FAILED' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN state='CANCELED' THEN 1 ELSE 0 END) "
        "FROM ui_job_summary WHERE job_set_id=?1 GROUP BY job_set_id;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st, 1, job_set_id);
    std::optional<UiJobSetDetail> detail;
    if (sqlite3_step(st) == SQLITE_ROW) {
        detail = UiJobSetDetail{};
        detail->summary = ReadJobSetSummaryRow(st);
        detail->hierarchy_projection_available = false;
    }
    sqlite3_finalize(st);

    if (!detail.has_value()) {
        return std::nullopt;
    }

    if (jobs_limit > 0) {
        UiReadJobListQuery query{};
        query.job_set_id = job_set_id;
        query.limit = jobs_limit;
        detail->jobs = ListJobs(query).items;
    }
    return detail;
}

UiReadPage<UiArtifactSummary> SqliteUiReadDb::ListArtifacts(
    const UiReadArtifactListQuery& query) const {
    UiReadPage<UiArtifactSummary> page{};
    if (db_ == nullptr || query.limit <= 0) {
        return page;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT artifact_id,sha256,size_bytes,artifact_kind,filename,created_at_utc "
        "FROM ui_artifact_browser "
        "WHERE (?1=1 OR filename LIKE ?2 OR sha256 LIKE ?2 OR artifact_kind LIKE ?2) "
        "AND (?3=1 OR filename LIKE ?4) "
        "AND (?5=0 OR created_at_utc < ?6 OR (created_at_utc=?6 AND artifact_id < ?7)) "
        "AND (?8=0 OR created_at_utc > ?9 OR (created_at_utc=?9 AND artifact_id > ?10)) "
        "ORDER BY created_at_utc DESC, artifact_id DESC LIMIT ?11;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return page;
    }

    const bool search_empty = query.search.empty();
    const std::string search_like = "%" + query.search + "%";
    const bool extension_empty = query.extension.empty();
    const std::string extension_like = "%" + query.extension;
    sqlite3_bind_int(st, 1, search_empty ? 1 : 0);
    sqlite3_bind_text(st, 2, search_like.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, extension_empty ? 1 : 0);
    sqlite3_bind_text(st, 4, extension_like.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, query.before.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 6, query.before.value_or(UiReadListCursor{}).primary);
    sqlite3_bind_int64(st, 7, query.before.value_or(UiReadListCursor{}).secondary);
    sqlite3_bind_int(st, 8, query.after.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 9, query.after.value_or(UiReadListCursor{}).primary);
    sqlite3_bind_int64(st, 10, query.after.value_or(UiReadListCursor{}).secondary);
    sqlite3_bind_int(st, 11, query.limit);

    while (sqlite3_step(st) == SQLITE_ROW) {
        UiArtifactSummary row{};
        row.artifact_id = sqlite3_column_int64(st, 0);
        const auto* sha_text = sqlite3_column_text(st, 1);
        row.sha256 = sha_text == nullptr ? std::string{} : reinterpret_cast<const char*>(sha_text);
        row.size_bytes = static_cast<std::uint64_t>(sqlite3_column_int64(st, 2));
        const auto* kind_text = sqlite3_column_text(st, 3);
        row.artifact_kind = kind_text == nullptr ? std::string{} : reinterpret_cast<const char*>(kind_text);
        const auto* filename_text = sqlite3_column_text(st, 4);
        row.filename = filename_text == nullptr ? std::string{} : reinterpret_cast<const char*>(filename_text);
        row.created_at_utc = sqlite3_column_int64(st, 5);
        page.items.push_back(std::move(row));
    }
    sqlite3_finalize(st);

    if (!page.items.empty()) {
        const auto& first = page.items.front();
        const auto& last = page.items.back();
        page.prev = UiReadListCursor{ first.created_at_utc, first.artifact_id };
        page.next = UiReadListCursor{ last.created_at_utc, last.artifact_id };
    }
    return page;
}

std::vector<UiSavestateSummary> SqliteUiReadDb::ListSavestates(
    std::string_view playback_state, bool complete_only, std::string_view search, int limit) const {
    std::vector<UiSavestateSummary> rows;
    if (db_ == nullptr || limit <= 0) return rows;
    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT savestate_id,artifact_id,savestate_type,COALESCE(note,''),is_complete,playback_state,"
        "dtm_artifact_id,sha256,size_bytes,filename,created_at_utc FROM ui_state_savestate_summary "
        "WHERE (?1='' OR playback_state=?1) AND (?2=0 OR is_complete=1) "
        "AND (?3='' OR filename LIKE ?4 OR sha256 LIKE ?4 OR CAST(savestate_id AS TEXT) LIKE ?4) "
        "ORDER BY created_at_utc DESC,savestate_id DESC LIMIT ?5;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return rows;
    const std::string playback(playback_state);
    const std::string needle(search);
    const std::string like = "%" + needle + "%";
    sqlite3_bind_text(st, 1, playback.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, complete_only ? 1 : 0);
    sqlite3_bind_text(st, 3, needle.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, like.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        UiSavestateSummary row{};
        row.savestate_id = sqlite3_column_int64(st, 0);
        row.artifact_id = sqlite3_column_int64(st, 1);
        row.savestate_type = ColumnText(st, 2);
        row.note = ColumnText(st, 3);
        row.is_complete = sqlite3_column_int(st, 4) != 0;
        row.playback_state = ColumnText(st, 5);
        row.dtm_artifact_id = ColumnInt64Optional(st, 6);
        row.sha256 = ColumnText(st, 7);
        row.size_bytes = static_cast<std::uint64_t>(sqlite3_column_int64(st, 8));
        row.filename = ColumnText(st, 9);
        row.created_at_utc = sqlite3_column_int64(st, 10);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(st);
    return rows;
}

std::vector<UiTasMovieRootSummary> SqliteUiReadDb::ListTasMovieRoots(int limit) const {
    std::vector<UiTasMovieRootSummary> rows;
    if (db_ == nullptr || limit <= 0) return rows;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT tas_movie_root_id,source_dtm_artifact_id,dtm_artifact_id,rtc_value,itinerary_artifact_id,required_final_breakpoint_pc,checkpoint_savestate_id,source_context_kind,source_context_id,created_at_utc FROM ui_tas_movie_root_summary ORDER BY created_at_utc DESC,tas_movie_root_id DESC LIMIT ?1;",
            -1, &st, nullptr) != SQLITE_OK) return rows;
    sqlite3_bind_int(st, 1, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        UiTasMovieRootSummary row{};
        row.tas_movie_root_id=sqlite3_column_int64(st,0); row.source_dtm_artifact_id=sqlite3_column_int64(st,1);
        row.dtm_artifact_id=sqlite3_column_int64(st,2); row.rtc_value=static_cast<std::uint32_t>(sqlite3_column_int64(st,3));
        row.itinerary_artifact_id=sqlite3_column_int64(st,4); row.required_final_breakpoint_pc=static_cast<std::uint32_t>(sqlite3_column_int64(st,5));
        row.checkpoint_savestate_id=sqlite3_column_int64(st,6); row.source_context_kind=ColumnText(st,7);
        row.source_context_id=sqlite3_column_int64(st,8); row.created_at_utc=sqlite3_column_int64(st,9); rows.push_back(std::move(row));
    }
    sqlite3_finalize(st); return rows;
}

std::vector<UiTasMovieTreeSummary> SqliteUiReadDb::ListTasMovieTrees(int limit) const {
    std::vector<UiTasMovieTreeSummary> rows;
    if (db_ == nullptr || limit <= 0) return rows;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT tas_movie_tree_id,tas_movie_root_id,parent_tas_movie_tree_id,dtm_artifact_id,itinerary_artifact_id,required_final_breakpoint_pc,checkpoint_savestate_id,source_context_kind,source_context_id,created_at_utc FROM ui_tas_movie_tree_summary ORDER BY created_at_utc DESC,tas_movie_tree_id DESC LIMIT ?1;",
            -1, &st, nullptr) != SQLITE_OK) return rows;
    sqlite3_bind_int(st,1,limit);
    while(sqlite3_step(st)==SQLITE_ROW){ UiTasMovieTreeSummary row{}; row.tas_movie_tree_id=sqlite3_column_int64(st,0); row.tas_movie_root_id=sqlite3_column_int64(st,1); row.parent_tas_movie_tree_id=ColumnInt64Optional(st,2); row.dtm_artifact_id=sqlite3_column_int64(st,3); row.itinerary_artifact_id=sqlite3_column_int64(st,4); row.required_final_breakpoint_pc=static_cast<std::uint32_t>(sqlite3_column_int64(st,5)); row.checkpoint_savestate_id=sqlite3_column_int64(st,6); row.source_context_kind=ColumnText(st,7); row.source_context_id=sqlite3_column_int64(st,8); row.created_at_utc=sqlite3_column_int64(st,9); rows.push_back(std::move(row)); }
    sqlite3_finalize(st); return rows;
}

std::vector<UiTasMovieValidationRequestSummary> SqliteUiReadDb::ListTasMovieValidationRequests(int limit) const {
    std::vector<UiTasMovieValidationRequestSummary> rows; if(db_==nullptr||limit<=0)return rows; sqlite3_stmt* st=nullptr;
    if(sqlite3_prepare_v2(db_,"SELECT validation_request_id,workflow_instance_id,workflow_step_id,step_kind,operation,source_kind,source_ref_id,source_dtm_artifact_id,source_dtm_sha256,rtc_value,effective_dtm_sha256,itinerary_artifact_id,COALESCE(itinerary_sha256,''),required_final_breakpoint_pc,latest_validation_attempt_id,COALESCE(latest_outcome,''),COALESCE(latest_failure_reason,''),latest_actual_pc,latest_actual_input_count,produced_tas_movie_root_id,created_at_utc FROM ui_tas_movie_validation_request_summary ORDER BY created_at_utc DESC,validation_request_id DESC LIMIT ?1;",-1,&st,nullptr)!=SQLITE_OK)return rows; sqlite3_bind_int(st,1,limit);
    while(sqlite3_step(st)==SQLITE_ROW){UiTasMovieValidationRequestSummary row{}; row.validation_request_id=sqlite3_column_int64(st,0);row.workflow_instance_id=sqlite3_column_int64(st,1);row.workflow_step_id=sqlite3_column_int64(st,2);row.step_kind=ColumnText(st,3);row.operation=ColumnText(st,4);row.source_kind=ColumnText(st,5);row.source_ref_id=sqlite3_column_int64(st,6);row.source_dtm_artifact_id=sqlite3_column_int64(st,7);row.source_dtm_sha256=ColumnText(st,8);if(auto v=ColumnInt64Optional(st,9))row.rtc_value=static_cast<std::uint32_t>(*v);row.effective_dtm_sha256=ColumnText(st,10);row.itinerary_artifact_id=ColumnInt64Optional(st,11);row.itinerary_sha256=ColumnText(st,12);row.required_final_breakpoint_pc=static_cast<std::uint32_t>(sqlite3_column_int64(st,13));row.latest_validation_attempt_id=ColumnInt64Optional(st,14);row.latest_outcome=ColumnText(st,15);row.latest_failure_reason=ColumnText(st,16);if(auto v=ColumnInt64Optional(st,17))row.latest_actual_pc=static_cast<std::uint32_t>(*v);if(auto v=ColumnInt64Optional(st,18))row.latest_actual_input_count=static_cast<std::uint64_t>(*v);row.produced_tas_movie_root_id=ColumnInt64Optional(st,19);row.created_at_utc=sqlite3_column_int64(st,20);rows.push_back(std::move(row));}sqlite3_finalize(st);return rows;
}

std::vector<UiTasMovieValidationAttemptSummary> SqliteUiReadDb::ListTasMovieValidationAttempts(std::optional<std::int64_t> request_id,int limit) const {std::vector<UiTasMovieValidationAttemptSummary> rows;if(db_==nullptr||limit<=0)return rows;sqlite3_stmt* st=nullptr;if(sqlite3_prepare_v2(db_,"SELECT validation_attempt_id,validation_request_id,source_job_id,outcome,COALESCE(failure_reason,''),expected_pc,expected_input_count,actual_pc,actual_input_count,last_known_good_savestate_id,produced_tas_movie_root_id,worker_id,recorded_at_utc FROM ui_tas_movie_validation_attempt_summary WHERE (?1=0 OR validation_request_id=?1) ORDER BY validation_attempt_id DESC LIMIT ?2;",-1,&st,nullptr)!=SQLITE_OK)return rows;sqlite3_bind_int64(st,1,request_id.value_or(0));sqlite3_bind_int(st,2,limit);while(sqlite3_step(st)==SQLITE_ROW){UiTasMovieValidationAttemptSummary row{};row.validation_attempt_id=sqlite3_column_int64(st,0);row.validation_request_id=sqlite3_column_int64(st,1);row.source_job_id=sqlite3_column_int64(st,2);row.outcome=ColumnText(st,3);row.failure_reason=ColumnText(st,4);if(auto v=ColumnInt64Optional(st,5))row.expected_pc=static_cast<std::uint32_t>(*v);if(auto v=ColumnInt64Optional(st,6))row.expected_input_count=static_cast<std::uint64_t>(*v);row.actual_pc=static_cast<std::uint32_t>(sqlite3_column_int64(st,7));row.actual_input_count=static_cast<std::uint64_t>(sqlite3_column_int64(st,8));row.last_known_good_savestate_id=ColumnInt64Optional(st,9);row.produced_tas_movie_root_id=ColumnInt64Optional(st,10);row.worker_id=ColumnText(st,11);row.recorded_at_utc=sqlite3_column_int64(st,12);rows.push_back(std::move(row));}sqlite3_finalize(st);return rows;}

std::vector<UiTasMovieSterilizationRequestSummary> SqliteUiReadDb::ListTasMovieSterilizationRequests(int limit) const {std::vector<UiTasMovieSterilizationRequestSummary> rows;if(db_==nullptr||limit<=0)return rows;sqlite3_stmt* st=nullptr;if(sqlite3_prepare_v2(db_,"SELECT sterilization_request_id,workflow_instance_id,workflow_step_id,source_savestate_id,source_savestate_artifact_id,source_savestate_sha256,source_dtm_artifact_id,source_dtm_sha256,reused_savestate_id,latest_sterilization_attempt_id,latest_produced_savestate_id,COALESCE(latest_candidate_savestate_sha256,''),created_at_utc FROM ui_tas_movie_sterilization_request_summary ORDER BY created_at_utc DESC,sterilization_request_id DESC LIMIT ?1;",-1,&st,nullptr)!=SQLITE_OK)return rows;sqlite3_bind_int(st,1,limit);while(sqlite3_step(st)==SQLITE_ROW){UiTasMovieSterilizationRequestSummary row{};row.sterilization_request_id=sqlite3_column_int64(st,0);row.workflow_instance_id=sqlite3_column_int64(st,1);row.workflow_step_id=sqlite3_column_int64(st,2);row.source_savestate_id=sqlite3_column_int64(st,3);row.source_savestate_artifact_id=sqlite3_column_int64(st,4);row.source_savestate_sha256=ColumnText(st,5);row.source_dtm_artifact_id=sqlite3_column_int64(st,6);row.source_dtm_sha256=ColumnText(st,7);row.reused_savestate_id=ColumnInt64Optional(st,8);row.latest_sterilization_attempt_id=ColumnInt64Optional(st,9);row.latest_produced_savestate_id=ColumnInt64Optional(st,10);row.latest_candidate_savestate_sha256=ColumnText(st,11);row.created_at_utc=sqlite3_column_int64(st,12);rows.push_back(std::move(row));}sqlite3_finalize(st);return rows;}

std::vector<UiTasMovieSterilizationAttemptSummary> SqliteUiReadDb::ListTasMovieSterilizationAttempts(std::optional<std::int64_t> request_id,int limit) const {std::vector<UiTasMovieSterilizationAttemptSummary> rows;if(db_==nullptr||limit<=0)return rows;sqlite3_stmt* st=nullptr;if(sqlite3_prepare_v2(db_,"SELECT sterilization_attempt_id,sterilization_request_id,source_job_id,candidate_savestate_sha256,produced_savestate_id,worker_id,recorded_at_utc FROM ui_tas_movie_sterilization_attempt_summary WHERE (?1=0 OR sterilization_request_id=?1) ORDER BY sterilization_attempt_id DESC LIMIT ?2;",-1,&st,nullptr)!=SQLITE_OK)return rows;sqlite3_bind_int64(st,1,request_id.value_or(0));sqlite3_bind_int(st,2,limit);while(sqlite3_step(st)==SQLITE_ROW){UiTasMovieSterilizationAttemptSummary row{};row.sterilization_attempt_id=sqlite3_column_int64(st,0);row.sterilization_request_id=sqlite3_column_int64(st,1);row.source_job_id=sqlite3_column_int64(st,2);row.candidate_savestate_sha256=ColumnText(st,3);row.produced_savestate_id=sqlite3_column_int64(st,4);row.worker_id=ColumnText(st,5);row.recorded_at_utc=sqlite3_column_int64(st,6);rows.push_back(std::move(row));}sqlite3_finalize(st);return rows;}

std::vector<UiBattleContextSummary> SqliteUiReadDb::ListBattleContexts(bool complete_only,int limit) const {std::vector<UiBattleContextSummary> rows;if(db_==nullptr||limit<=0)return rows;sqlite3_stmt* st=nullptr;if(sqlite3_prepare_v2(db_,"SELECT context_probe_id,wave_id,source_savestate_id,exec_job_id,probe_status,context_version,context_artifact_id,entry_pc,recorded_at_utc,created_at_utc FROM ui_battle_context_summary WHERE (?1=0 OR probe_status='COMPLETED') ORDER BY created_at_utc DESC,context_probe_id DESC LIMIT ?2;",-1,&st,nullptr)!=SQLITE_OK)return rows;sqlite3_bind_int(st,1,complete_only?1:0);sqlite3_bind_int(st,2,limit);while(sqlite3_step(st)==SQLITE_ROW){UiBattleContextSummary row{};row.context_probe_id=sqlite3_column_int64(st,0);row.wave_id=ColumnInt64Optional(st,1);row.source_savestate_id=sqlite3_column_int64(st,2);row.exec_job_id=ColumnInt64Optional(st,3);row.probe_status=ColumnText(st,4);if(auto v=ColumnInt64Optional(st,5))row.context_version=static_cast<int>(*v);row.context_artifact_id=ColumnInt64Optional(st,6);if(auto v=ColumnInt64Optional(st,7))row.entry_pc=static_cast<std::uint32_t>(*v);row.recorded_at_utc=ColumnInt64Optional(st,8);row.created_at_utc=sqlite3_column_int64(st,9);rows.push_back(std::move(row));}sqlite3_finalize(st);return rows;}

std::vector<UiArchiveCatalogRow> SqliteUiReadDb::ListArchiveCatalog(
    const UiArchiveCatalogListQuery& query) const {
    std::vector<UiArchiveCatalogRow> rows;
    if (db_ == nullptr) {
        return rows;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT archive_package_id,source_context,source_root_job_set_id,"
        "COALESCE(source_scope_kind,'root_job_set'),COALESCE(source_workflow_count,0),"
        "COALESCE(selection_summary,''),COALESCE(archive_name,''),COALESCE(archive_notes,''),"
        "created_at_utc,schema_version,event_catalog_version,time_range_start_utc,time_range_end_utc,checksum_status "
        "FROM ui_archive_catalog "
        "WHERE (?1=1 OR archive_name LIKE ?2 OR archive_notes LIKE ?2 OR CAST(archive_package_id AS TEXT) LIKE ?2 OR selection_summary LIKE ?2) "
        "AND (?3=1 OR source_scope_kind=?4) "
        "AND (?5=1 OR checksum_status=?6) "
        "AND (?7=0 OR source_scope_kind='workflow_selection') "
        "AND (?8=0 OR created_at_utc>=?9) "
        "AND (?10=0 OR created_at_utc<=?11) "
        "ORDER BY created_at_utc DESC, archive_package_id DESC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return rows;
    }

    const bool search_empty = query.search.empty();
    const std::string search_like = "%" + query.search + "%";
    sqlite3_bind_int(st, 1, search_empty ? 1 : 0);
    sqlite3_bind_text(st, 2, search_like.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, query.source_scope_kind.empty() ? 1 : 0);
    sqlite3_bind_text(st, 4, query.source_scope_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, query.checksum_status.empty() ? 1 : 0);
    sqlite3_bind_text(st, 6, query.checksum_status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 7, query.workflow_packages_only ? 1 : 0);
    sqlite3_bind_int(st, 8, query.created_from_utc.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 9, query.created_from_utc.value_or(0));
    sqlite3_bind_int(st, 10, query.created_to_utc.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 11, query.created_to_utc.value_or(0));

    while (sqlite3_step(st) == SQLITE_ROW) {
        UiArchiveCatalogRow row{};
        row.archive_package_id = sqlite3_column_int64(st, 0);
        row.source_context = ColumnText(st, 1);
        row.source_root_job_set_id = sqlite3_column_int64(st, 2);
        row.source_scope_kind = ColumnText(st, 3);
        row.source_workflow_count = sqlite3_column_int64(st, 4);
        row.selection_summary = ColumnText(st, 5);
        row.archive_name = ColumnText(st, 6);
        row.archive_notes = ColumnText(st, 7);
        row.created_at_utc = sqlite3_column_int64(st, 8);
        row.schema_version = sqlite3_column_int(st, 9);
        row.event_catalog_version = sqlite3_column_int(st, 10);
        row.time_range_start_utc = sqlite3_column_int64(st, 11);
        row.time_range_end_utc = sqlite3_column_int64(st, 12);
        row.checksum_status = ColumnText(st, 13);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(st);
    return rows;
}

std::vector<UiArchiveRehydrateRequestRow> SqliteUiReadDb::ListArchiveRehydrateRequests(
    std::int64_t archive_package_id) const {
    std::vector<UiArchiveRehydrateRequestRow> rows;
    if (db_ == nullptr || archive_package_id <= 0) {
        return rows;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT rehydrate_request_id,archive_package_id,status,target_namespace,requested_at_utc,completed_at_utc,COALESCE(error_text,'') "
        "FROM ui_archive_rehydrate_request "
        "WHERE archive_package_id=?1 "
        "ORDER BY requested_at_utc DESC, rehydrate_request_id DESC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st, 1, archive_package_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        UiArchiveRehydrateRequestRow row{};
        row.rehydrate_request_id = sqlite3_column_int64(st, 0);
        row.archive_package_id = sqlite3_column_int64(st, 1);
        row.status = ColumnText(st, 2);
        row.target_namespace = ColumnText(st, 3);
        row.requested_at_utc = sqlite3_column_int64(st, 4);
        row.completed_at_utc = ColumnInt64Optional(st, 5);
        row.error_text = ColumnText(st, 6);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(st);
    return rows;
}

bool SqliteUiReadDb::UpsertArtifactSummary(
    const UiArtifactSummary& summary,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out != nullptr) *error_out = "database handle is null";
        return false;
    }
    if (summary.artifact_id <= 0 || summary.sha256.empty() || summary.filename.empty()) {
        if (error_out != nullptr) *error_out = "artifact_id, sha256, and filename are required";
        return false;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "INSERT INTO ui_artifact_browser(artifact_id,sha256,size_bytes,artifact_kind,filename,created_at_utc) "
        "VALUES(?1,?2,?3,?4,?5,?6) "
        "ON CONFLICT(artifact_id) DO UPDATE SET "
        "sha256=excluded.sha256,size_bytes=excluded.size_bytes,artifact_kind=excluded.artifact_kind,"
        "filename=excluded.filename,created_at_utc=excluded.created_at_utc;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(st, 1, summary.artifact_id);
    sqlite3_bind_text(st, 2, summary.sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, static_cast<sqlite3_int64>(summary.size_bytes));
    sqlite3_bind_text(st, 4, summary.artifact_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, summary.filename.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, summary.created_at_utc);

    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok && error_out != nullptr) {
        *error_out = sqlite3_errmsg(db_);
    }
    sqlite3_finalize(st);
    return ok;
}

UiReadPage<UiWorkflowInstanceSummary> SqliteUiReadDb::ListWorkflowInstances(
    const UiWorkflowInstanceListQuery& query) const {
    UiReadPage<UiWorkflowInstanceSummary> page{};
    if (db_ == nullptr || query.limit <= 0) {
        return page;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT i.workflow_instance_id,i.workflow_kind,i.state,i.display_state,i.root_scope_kind,i.root_scope_id,COALESCE(i.created_by,''),"
        "i.blocked_step_count,i.failed_step_count,i.created_at_utc,i.started_at_utc,i.completed_at_utc,"
        "COALESCE(i.failure_code,''),COALESCE(i.failure_text,''),"
        "i.battle_advancement_rank,i.battle_desired_outcome_count,i.battle_final_victory_count,i.battle_selected_count "
        "FROM ui_workflow_instance i "
        "WHERE (?1=1 OR state=?2) "
        "AND (?3=1 OR display_state=?4) "
        "AND (?5=1 OR workflow_kind=?6) "
        "AND (?7=0 OR created_at_utc < ?8 OR (created_at_utc=?8 AND workflow_instance_id < ?9)) "
        "AND (?10=0 OR created_at_utc > ?11 OR (created_at_utc=?11 AND workflow_instance_id > ?12)) "
        "AND (?13=0 OR battle_final_victory_count > 0) "
        "AND (?14=0 OR battle_final_victory_count = 0) "
        "ORDER BY created_at_utc DESC, workflow_instance_id DESC LIMIT ?15;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return page;
    }

    sqlite3_bind_int(st, 1, query.state.empty() ? 1 : 0);
    sqlite3_bind_text(st, 2, query.state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, query.display_state.empty() ? 1 : 0);
    sqlite3_bind_text(st, 4, query.display_state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, query.workflow_kind.empty() ? 1 : 0);
    sqlite3_bind_text(st, 6, query.workflow_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 7, query.before.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 8, query.before.value_or(UiReadListCursor{}).primary);
    sqlite3_bind_int64(st, 9, query.before.value_or(UiReadListCursor{}).secondary);
    sqlite3_bind_int(st, 10, query.after.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 11, query.after.value_or(UiReadListCursor{}).primary);
    sqlite3_bind_int64(st, 12, query.after.value_or(UiReadListCursor{}).secondary);
    sqlite3_bind_int(st, 13, query.battle_final_victory_only ? 1 : 0);
    sqlite3_bind_int(st, 14, query.battle_final_victory_absent_only ? 1 : 0);
    sqlite3_bind_int(st, 15, query.limit);

    while (sqlite3_step(st) == SQLITE_ROW) {
        page.items.push_back(ReadWorkflowInstanceRow(st));
    }
    sqlite3_finalize(st);

    if (!page.items.empty()) {
        const auto& first = page.items.front();
        const auto& last = page.items.back();
        page.prev = UiReadListCursor{ first.created_at_utc, first.workflow_instance_id };
        page.next = UiReadListCursor{ last.created_at_utc, last.workflow_instance_id };
    }
    return page;
}

UiWorkflowDisplayStateCounts SqliteUiReadDb::CountWorkflowDisplayStates() const {
    UiWorkflowDisplayStateCounts counts{};
    if (db_ == nullptr) {
        return counts;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT display_state, COUNT(*) "
        "FROM ui_workflow_instance "
        "GROUP BY display_state;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return counts;
    }

    while (sqlite3_step(st) == SQLITE_ROW) {
        AddWorkflowDisplayStateCount(counts, ColumnText(st, 0), sqlite3_column_int64(st, 1));
    }
    sqlite3_finalize(st);
    return counts;
}

std::optional<UiWorkflowDetail> SqliteUiReadDb::GetWorkflowDetail(
    std::int64_t workflow_instance_id) const {
    if (db_ == nullptr || workflow_instance_id <= 0) {
        return std::nullopt;
    }

    sqlite3_stmt* inst = nullptr;
    constexpr const char* kInstanceSql =
        "SELECT i.workflow_instance_id,i.workflow_kind,i.state,i.display_state,i.root_scope_kind,i.root_scope_id,COALESCE(i.created_by,''),"
        "i.blocked_step_count,i.failed_step_count,i.created_at_utc,i.started_at_utc,i.completed_at_utc,"
        "COALESCE(i.failure_code,''),COALESCE(i.failure_text,''),"
        "i.battle_advancement_rank,i.battle_desired_outcome_count,i.battle_final_victory_count,i.battle_selected_count "
        "FROM ui_workflow_instance i WHERE i.workflow_instance_id=?1;";
    if (sqlite3_prepare_v2(db_, kInstanceSql, -1, &inst, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(inst, 1, workflow_instance_id);
    std::optional<UiWorkflowDetail> detail;
    if (sqlite3_step(inst) == SQLITE_ROW) {
        detail = UiWorkflowDetail{};
        detail->instance = ReadWorkflowInstanceRow(inst);
    }
    sqlite3_finalize(inst);
    if (!detail.has_value()) {
        return std::nullopt;
    }

    sqlite3_stmt* activations = nullptr;
    constexpr const char* kActivationsSql =
        "SELECT workflow_unit_activation_id,workflow_instance_id,parent_workflow_unit_activation_id,activation_key,graph_node_key,"
        "unit_kind,display_name,state,COALESCE(activation_params_json,''),COALESCE(authored_ref_kind,''),authored_ref_id,"
        "COALESCE(failure_code,''),COALESCE(failure_text,''),created_at_utc,ready_at_utc,started_at_utc,completed_at_utc,failed_at_utc "
        "FROM ui_workflow_unit_activation WHERE workflow_instance_id=?1 ORDER BY created_at_utc ASC, workflow_unit_activation_id ASC;";
    if (sqlite3_prepare_v2(db_, kActivationsSql, -1, &activations, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(activations, 1, workflow_instance_id);
        while (sqlite3_step(activations) == SQLITE_ROW) {
            detail->unit_activations.push_back(ReadWorkflowUnitActivationRow(activations));
        }
    }
    sqlite3_finalize(activations);

    sqlite3_stmt* activation_edges = nullptr;
    constexpr const char* kActivationEdgesSql =
        "SELECT workflow_unit_activation_edge_id,workflow_instance_id,from_workflow_unit_activation_id,to_workflow_unit_activation_id,"
        "COALESCE(output_key,''),COALESCE(input_key,''),COALESCE(condition_kind,''),COALESCE(condition_value,''),created_at_utc "
        "FROM ui_workflow_unit_activation_edge WHERE workflow_instance_id=?1 ORDER BY created_at_utc ASC, workflow_unit_activation_edge_id ASC;";
    if (sqlite3_prepare_v2(db_, kActivationEdgesSql, -1, &activation_edges, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(activation_edges, 1, workflow_instance_id);
        while (sqlite3_step(activation_edges) == SQLITE_ROW) {
            detail->unit_activation_edges.push_back(ReadWorkflowUnitActivationEdgeRow(activation_edges));
        }
    }
    sqlite3_finalize(activation_edges);

    sqlite3_stmt* steps = nullptr;
    constexpr const char* kStepsSql =
        "SELECT s.workflow_step_id,s.workflow_instance_id,s.workflow_unit_activation_id,s.step_key,s.step_kind,s.state,COALESCE(s.blocked_reason,''),s.job_set_id,"
        "s.job_count,s.job_completed_count,s.job_failed_count,s.priority,s.attempts,s.max_attempts,"
        "s.ready_at_utc,s.started_at_utc,s.completed_at_utc,s.failed_at_utc,s.created_at_utc,s.battle_advancement_rank,"
        "s.battle_desired_outcome_count,s.battle_final_victory_count,s.battle_selected_count "
        "FROM ui_workflow_step s WHERE s.workflow_instance_id=?1 ORDER BY s.created_at_utc ASC, s.workflow_step_id ASC;";
    if (sqlite3_prepare_v2(db_, kStepsSql, -1, &steps, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(steps, 1, workflow_instance_id);
        while (sqlite3_step(steps) == SQLITE_ROW) {
            detail->steps.push_back(ReadWorkflowStepRow(steps));
        }
    }
    sqlite3_finalize(steps);

    sqlite3_stmt* edges = nullptr;
    constexpr const char* kEdgesSql =
        "SELECT workflow_edge_id,workflow_instance_id,from_step_id,to_step_id,"
        "COALESCE(condition_kind,''),COALESCE(condition_value,''),created_at_utc "
        "FROM ui_workflow_edge WHERE workflow_instance_id=?1 ORDER BY created_at_utc ASC, workflow_edge_id ASC;";
    if (sqlite3_prepare_v2(db_, kEdgesSql, -1, &edges, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(edges, 1, workflow_instance_id);
        while (sqlite3_step(edges) == SQLITE_ROW) {
            detail->edges.push_back(ReadWorkflowEdgeRow(edges));
        }
    }
    sqlite3_finalize(edges);

    sqlite3_stmt* alerts = nullptr;
    constexpr const char* kAlertsSql =
        "SELECT workflow_alert_id,workflow_instance_id,workflow_step_id,alert_kind,COALESCE(alert_code,''),"
        "message,is_active,first_seen_at_utc,last_seen_at_utc,cleared_at_utc "
        "FROM ui_workflow_alert WHERE workflow_instance_id=?1 ORDER BY is_active DESC, last_seen_at_utc DESC, workflow_alert_id DESC;";
    if (sqlite3_prepare_v2(db_, kAlertsSql, -1, &alerts, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(alerts, 1, workflow_instance_id);
        while (sqlite3_step(alerts) == SQLITE_ROW) {
            detail->alerts.push_back(ReadWorkflowAlertRow(alerts));
        }
    }
    sqlite3_finalize(alerts);

    return detail;
}

UiReadPage<UiBattleGroupSummary> SqliteUiReadDb::ListBattleGroups(
    const UiBattleGroupListQuery& query) const {
    UiReadPage<UiBattleGroupSummary> page{};
    if (db_ == nullptr || query.limit <= 0) {
        return page;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT g.battle_set_id,g.name,g.status,g.created_at_utc,g.completed_at_utc,"
        "g.wave_count,g.turn_job_count,g.selected_count,g.desired_outcome_count,g.final_victory_count,"
        "g.failed_count,g.manual_followup_count,g.advancement_rank "
        "FROM ui_battle_group g "
        "WHERE (?1=0 OR g.created_at_utc < ?2 OR (g.created_at_utc=?2 AND g.battle_set_id < ?3)) "
        "AND (?4=0 OR g.created_at_utc > ?5 OR (g.created_at_utc=?5 AND g.battle_set_id > ?6)) "
        "AND (?7=0 OR g.selected_count > 0) "
        "AND (?8=0 OR g.final_victory_count > 0) "
        "ORDER BY g.created_at_utc DESC,g.battle_set_id DESC LIMIT ?9;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return page;
    }

    sqlite3_bind_int(st, 1, query.before.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 2, query.before.value_or(UiReadListCursor{}).primary);
    sqlite3_bind_int64(st, 3, query.before.value_or(UiReadListCursor{}).secondary);
    sqlite3_bind_int(st, 4, query.after.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 5, query.after.value_or(UiReadListCursor{}).primary);
    sqlite3_bind_int64(st, 6, query.after.value_or(UiReadListCursor{}).secondary);
    sqlite3_bind_int(st, 7, query.child_selected_only ? 1 : 0);
    sqlite3_bind_int(st, 8, query.final_victory_only ? 1 : 0);
    sqlite3_bind_int(st, 9, query.limit);

    while (sqlite3_step(st) == SQLITE_ROW) {
        page.items.push_back(ReadBattleGroupRow(st));
    }
    sqlite3_finalize(st);

    if (!page.items.empty()) {
        const auto& first = page.items.front();
        const auto& last = page.items.back();
        page.prev = UiReadListCursor{ first.created_at_utc, first.battle_set_id };
        page.next = UiReadListCursor{ last.created_at_utc, last.battle_set_id };
    }
    return page;
}

std::vector<UiBattleWaveSummary> SqliteUiReadDb::ListBattleWaves(
    std::int64_t battle_set_id) const {
    std::vector<UiBattleWaveSummary> rows;
    if (db_ == nullptr || battle_set_id <= 0) {
        return rows;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT w.wave_id,w.battle_set_id,w.parent_wave_id,w.parent_turn_job_id,w.turn_index,w.status,w.created_at_utc,w.completed_at_utc,"
        "w.job_count,w.selected_count,w.desired_outcome_count,w.final_victory_count,w.failed_count,w.advancement_rank "
        "FROM ui_battle_wave w "
        "WHERE w.battle_set_id=?1 "
        "ORDER BY w.turn_index ASC,w.created_at_utc ASC,w.wave_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return rows;
    }

    sqlite3_bind_int64(st, 1, battle_set_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        rows.push_back(ReadBattleWaveRow(st));
    }
    sqlite3_finalize(st);
    return rows;
}

std::vector<UiBattleTurnJobSummary> SqliteUiReadDb::ListBattleTurnJobsForWaves(
    const std::vector<std::int64_t>& wave_ids,
    bool final_victory_only) const {
    std::vector<UiBattleTurnJobSummary> rows;
    if (db_ == nullptr || wave_ids.empty()) {
        return rows;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT j.turn_job_id,j.exec_job_id,j.wave_id,w.battle_set_id,w.turn_index,j.job_state,"
        "j.fake_attacks_this_turn,j.fake_attacks_used_before,j.rng_seed,j.delta_vi,"
        "j.pred_passed,j.pred_total,j.battle_outcome,j.has_desired_outcome,j.has_final_victory_outcome,j.selected_for_advancement,j.advancement_decision_kind,j.advancement_rank,"
        "j.started_at_utc,j.ended_at_utc,"
        "d.battle_advancement_decision_id,d.battle_advancement_pool_id,d.turn_job_id,d.decision_kind,d.decision_reason,d.created_at_utc,"
        "f.turn_job_id,f.manual_followup_status,f.recorded_dtm_artifact_id,f.note,f.updated_at_utc "
        "FROM ui_battle_turn_job j "
        "JOIN ui_battle_wave w ON w.wave_id=j.wave_id "
        "LEFT JOIN ui_battle_advancement_decision d ON d.turn_job_id=j.turn_job_id AND d.decision_kind=j.advancement_decision_kind "
        "LEFT JOIN ui_battle_manual_followup f ON f.turn_job_id=j.turn_job_id "
        "WHERE j.wave_id=?1 AND (?2=0 OR j.has_final_victory_outcome=1) "
        "ORDER BY j.turn_job_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return rows;
    }

    for (std::int64_t wave_id : wave_ids) {
        if (wave_id <= 0) {
            continue;
        }
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_int64(st, 1, wave_id);
        sqlite3_bind_int(st, 2, final_victory_only ? 1 : 0);
        while (sqlite3_step(st) == SQLITE_ROW) {
            rows.push_back(ReadBattleTurnJobRow(st));
        }
    }
    sqlite3_finalize(st);
    return rows;
}

std::optional<UiBattleTurnJobDetail> SqliteUiReadDb::GetBattleTurnJobDetail(
    std::int64_t turn_job_id) const {
    if (db_ == nullptr || turn_job_id <= 0) {
        return std::nullopt;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT j.turn_job_id,j.exec_job_id,j.wave_id,w.battle_set_id,w.turn_index,j.job_state,"
        "j.fake_attacks_this_turn,j.fake_attacks_used_before,j.rng_seed,j.delta_vi,"
        "j.pred_passed,j.pred_total,j.battle_outcome,j.has_desired_outcome,j.has_final_victory_outcome,j.selected_for_advancement,j.advancement_decision_kind,j.advancement_rank,"
        "j.started_at_utc,j.ended_at_utc,"
        "d.battle_advancement_decision_id,d.battle_advancement_pool_id,d.turn_job_id,d.decision_kind,d.decision_reason,d.created_at_utc,"
        "f.turn_job_id,f.manual_followup_status,f.recorded_dtm_artifact_id,f.note,f.updated_at_utc "
        "FROM ui_battle_turn_job j "
        "JOIN ui_battle_wave w ON w.wave_id=j.wave_id "
        "LEFT JOIN ui_battle_advancement_decision d ON d.turn_job_id=j.turn_job_id AND d.decision_kind=j.advancement_decision_kind "
        "LEFT JOIN ui_battle_manual_followup f ON f.turn_job_id=j.turn_job_id "
        "WHERE j.turn_job_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st, 1, turn_job_id);
    std::optional<UiBattleTurnJobDetail> detail;
    if (sqlite3_step(st) == SQLITE_ROW) {
        detail = UiBattleTurnJobDetail{};
        detail->summary = ReadBattleTurnJobRow(st);
    }
    sqlite3_finalize(st);

    if (!detail.has_value()) {
        return std::nullopt;
    }

    UiBattleGroupListQuery group_query{};
    group_query.limit = 500;
    if (auto groups = ListBattleGroups(group_query); !groups.items.empty()) {
        for (const auto& group : groups.items) {
            if (group.battle_set_id == detail->summary.battle_set_id) {
                detail->group = group;
                break;
            }
        }
    }

    for (const auto& wave : ListBattleWaves(detail->summary.battle_set_id)) {
        if (wave.wave_id == detail->summary.wave_id) {
            detail->wave = wave;
            break;
        }
    }
    detail->artifacts = detail->summary.exec_job_id.has_value()
        ? ListJobArtifacts(*detail->summary.exec_job_id)
        : std::vector<UiJobArtifact>{};
    return detail;
}

std::optional<UiBattleTurnJobReplicationRow> SqliteUiReadDb::GetBattleTurnJobReplication(
    std::int64_t turn_job_id) const {
    if (db_ == nullptr || turn_job_id <= 0) {
        return std::nullopt;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT turn_job_id,battle_set_id,wave_id,parent_wave_id,parent_turn_job_id,exec_job_id,"
        "source_savestate_id,seed_candidate_id,authored_plan_id,authored_turn_index,resolved_turn_commands_blob,resolved_turn_variant_key,"
        "fake_attacks_used_before,fake_attacks_this_turn,output_savestate_id,input_trace_artifact_id "
        "FROM ui_battle_turn_job_replication WHERE turn_job_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st, 1, turn_job_id);
    std::optional<UiBattleTurnJobReplicationRow> row;
    if (sqlite3_step(st) == SQLITE_ROW) {
        row = ReadBattleTurnJobReplicationRow(st);
    }
    sqlite3_finalize(st);
    return row;
}

std::vector<UiBattleTurnJobReplicationRow> SqliteUiReadDb::ListBattleTurnJobReplicationChain(
    std::int64_t turn_job_id) const {
    std::vector<UiBattleTurnJobReplicationRow> chain;
    std::int64_t current = turn_job_id;
    for (int guard = 0; guard < 64 && current > 0; ++guard) {
        auto row = GetBattleTurnJobReplication(current);
        if (!row.has_value()) {
            break;
        }
        current = row->parent_turn_job_id.value_or(0);
        chain.push_back(std::move(*row));
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}

UiSeedProbeRunPage SqliteUiReadDb::ListSeedProbeRuns(
    const UiReadSeedProbeRunListQuery& query) const {
    UiSeedProbeRunPage page{};
    if (db_ == nullptr || query.limit <= 0) {
        return page;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT probe_run_id,probe_set_id,entry_savestate_id,seed_probe_spec_id,codec_version,"
        "status,neutral_seed_value,grid_count,unique_count,requested_at_utc,completed_at_utc "
        "FROM ui_seed_probe_summary "
        "WHERE (?1=1 OR status LIKE ?2 OR CAST(probe_run_id AS TEXT) LIKE ?2) "
        "AND (?3=0 OR status IN ('COMPLETED','COMPLETED_PARTIAL')) "
        "AND (?4=0 OR requested_at_utc < ?5 OR (requested_at_utc=?5 AND probe_run_id < ?6)) "
        "AND (?7=0 OR requested_at_utc > ?8 OR (requested_at_utc=?8 AND probe_run_id > ?9)) "
        "ORDER BY requested_at_utc DESC, probe_run_id DESC LIMIT ?10;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return page;
    }

    const bool search_empty = query.search.empty();
    const std::string search_like = "%" + query.search + "%";
    sqlite3_bind_int(st, 1, search_empty ? 1 : 0);
    sqlite3_bind_text(st, 2, search_like.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, query.only_completed ? 1 : 0);
    sqlite3_bind_int(st, 4, query.before.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 5, query.before.value_or(UiReadSeedProbeRunListCursor{}).requested_at_utc);
    sqlite3_bind_int64(st, 6, query.before.value_or(UiReadSeedProbeRunListCursor{}).probe_run_id);
    sqlite3_bind_int(st, 7, query.after.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 8, query.after.value_or(UiReadSeedProbeRunListCursor{}).requested_at_utc);
    sqlite3_bind_int64(st, 9, query.after.value_or(UiReadSeedProbeRunListCursor{}).probe_run_id);
    sqlite3_bind_int(st, 10, query.limit);

    while (sqlite3_step(st) == SQLITE_ROW) {
        if (auto row = ReadSeedProbeSummaryRow(st); row.has_value()) {
            page.items.push_back(std::move(*row));
        }
    }
    sqlite3_finalize(st);

    if (!page.items.empty()) {
        const auto& first = page.items.front();
        const auto& last = page.items.back();
        page.prev = UiReadSeedProbeRunListCursor{ first.requested_at_utc, first.probe_run_id };
        page.next = UiReadSeedProbeRunListCursor{ last.requested_at_utc, last.probe_run_id };
    }
    return page;
}

std::optional<UiSeedProbeRunSummary> SqliteUiReadDb::GetSeedProbeRunSummary(
    std::int64_t probe_run_id) const {
    if (db_ == nullptr || probe_run_id <= 0) {
        return std::nullopt;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT probe_run_id,probe_set_id,entry_savestate_id,seed_probe_spec_id,codec_version,"
        "status,neutral_seed_value,grid_count,unique_count,requested_at_utc,completed_at_utc "
        "FROM ui_seed_probe_summary WHERE probe_run_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st, 1, probe_run_id);
    std::optional<UiSeedProbeRunSummary> summary;
    if (sqlite3_step(st) == SQLITE_ROW) {
        summary = ReadSeedProbeSummaryRow(st);
    }
    sqlite3_finalize(st);
    return summary;
}

std::vector<UiSeedProbeDeltaPoint> SqliteUiReadDb::ListSeedProbeDeltaPoints(
    std::int64_t probe_run_id) const {
    std::vector<UiSeedProbeDeltaPoint> points;
    if (db_ == nullptr || probe_run_id <= 0) {
        return points;
    }
    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT delta_point_id,probe_run_id,source_family,axis_x,axis_y,seed_value,seed_delta "
        "FROM ui_seed_probe_delta_point WHERE probe_run_id=?1 "
        "ORDER BY source_family ASC, axis_x ASC, axis_y ASC, seed_value ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return points;
    }
    sqlite3_bind_int64(st, 1, probe_run_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        UiSeedProbeDeltaPoint point{};
        point.delta_point_id = sqlite3_column_int64(st, 0);
        point.probe_run_id = sqlite3_column_int64(st, 1);
        const auto* family = sqlite3_column_text(st, 2);
        point.source_family = family == nullptr ? std::string{} : reinterpret_cast<const char*>(family);
        point.axis_x = sqlite3_column_int(st, 3);
        point.axis_y = sqlite3_column_int(st, 4);
        point.seed_value = static_cast<std::uint32_t>(sqlite3_column_int64(st, 5));
        point.seed_delta = static_cast<std::int32_t>(sqlite3_column_int(st, 6));
        points.push_back(std::move(point));
    }
    sqlite3_finalize(st);
    return points;
}

std::vector<UiSeedProbeUniqueValue> SqliteUiReadDb::ListSeedProbeUniqueValues(
    std::int64_t probe_run_id) const {
    std::vector<UiSeedProbeUniqueValue> values;
    if (db_ == nullptr || probe_run_id <= 0) {
        return values;
    }
    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT unique_value_id,probe_run_id,seed_value,seed_delta,main_x,main_y,cstick_x,cstick_y,trigger_x,trigger_y "
        "FROM ui_seed_probe_unique_value WHERE probe_run_id=?1 ORDER BY seed_value ASC, unique_value_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return values;
    }
    sqlite3_bind_int64(st, 1, probe_run_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        UiSeedProbeUniqueValue value{};
        value.unique_value_id = sqlite3_column_int64(st, 0);
        value.probe_run_id = sqlite3_column_int64(st, 1);
        value.seed_value = static_cast<std::uint32_t>(sqlite3_column_int64(st, 2));
        value.seed_delta = static_cast<std::int32_t>(sqlite3_column_int(st, 3));
        value.main_x = sqlite3_column_int(st, 4);
        value.main_y = sqlite3_column_int(st, 5);
        value.cstick_x = sqlite3_column_int(st, 6);
        value.cstick_y = sqlite3_column_int(st, 7);
        value.trigger_x = sqlite3_column_int(st, 8);
        value.trigger_y = sqlite3_column_int(st, 9);
        values.push_back(std::move(value));
    }
    sqlite3_finalize(st);
    return values;
}

bool SqliteUiReadDb::UpsertSeedProbeRunSummary(
    const UiSeedProbeRunSummary& summary,
    std::string* error_out) {
    if (db_ == nullptr || summary.probe_run_id <= 0) {
        if (error_out) *error_out = "invalid seed probe summary";
        return false;
    }
    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "INSERT INTO ui_seed_probe_summary("
        "probe_run_id,probe_set_id,entry_savestate_id,seed_probe_spec_id,codec_version,status,"
        "neutral_seed_value,grid_count,unique_count,requested_at_utc,completed_at_utc) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11) "
        "ON CONFLICT(probe_run_id) DO UPDATE SET "
        "probe_set_id=excluded.probe_set_id,entry_savestate_id=excluded.entry_savestate_id,"
        "seed_probe_spec_id=excluded.seed_probe_spec_id,codec_version=excluded.codec_version,"
        "status=excluded.status,neutral_seed_value=excluded.neutral_seed_value,"
        "grid_count=excluded.grid_count,unique_count=excluded.unique_count,"
        "requested_at_utc=excluded.requested_at_utc,completed_at_utc=excluded.completed_at_utc;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st, 1, summary.probe_run_id);
    sqlite3_bind_int64(st, 2, summary.probe_set_id);
    sqlite3_bind_int64(st, 3, summary.entry_savestate_id);
    sqlite3_bind_int64(st, 4, summary.seed_probe_spec_id);
    sqlite3_bind_int(st, 5, summary.codec_version);
    sqlite3_bind_text(st, 6, summary.status.c_str(), -1, SQLITE_TRANSIENT);
    if (summary.neutral_seed_value.has_value()) sqlite3_bind_int64(st, 7, *summary.neutral_seed_value);
    else sqlite3_bind_null(st, 7);
    sqlite3_bind_int(st, 8, summary.grid_count);
    sqlite3_bind_int(st, 9, summary.unique_count);
    sqlite3_bind_int64(st, 10, summary.requested_at_utc);
    if (summary.completed_at_utc.has_value()) sqlite3_bind_int64(st, 11, *summary.completed_at_utc);
    else sqlite3_bind_null(st, 11);
    const auto rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE && error_out) *error_out = sqlite3_errmsg(db_);
    return rc == SQLITE_DONE;
}

bool SqliteUiReadDb::ReplaceSeedProbeDeltaPoints(
    std::int64_t probe_run_id,
    const std::vector<UiSeedProbeDeltaPoint>& points,
    std::string* error_out) {
    if (db_ == nullptr || probe_run_id <= 0) {
        if (error_out) *error_out = "invalid seed probe delta projection";
        return false;
    }
    if (!ExecSql(db_, "BEGIN IMMEDIATE;")) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    bool success = true;
    sqlite3_stmt* del = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM ui_seed_probe_delta_point WHERE probe_run_id=?1;", -1, &del, nullptr) != SQLITE_OK) {
        success = false;
    } else {
        sqlite3_bind_int64(del, 1, probe_run_id);
        success = sqlite3_step(del) == SQLITE_DONE;
    }
    sqlite3_finalize(del);

    sqlite3_stmt* ins = nullptr;
    constexpr const char* kInsert =
        "INSERT INTO ui_seed_probe_delta_point(delta_point_id,probe_run_id,source_family,axis_x,axis_y,seed_value,seed_delta) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7);";
    if (success && sqlite3_prepare_v2(db_, kInsert, -1, &ins, nullptr) != SQLITE_OK) {
        success = false;
    }
    for (const auto& point : points) {
        if (!success) break;
        sqlite3_reset(ins);
        sqlite3_clear_bindings(ins);
        sqlite3_bind_int64(ins, 1, point.delta_point_id);
        sqlite3_bind_int64(ins, 2, probe_run_id);
        sqlite3_bind_text(ins, 3, point.source_family.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(ins, 4, point.axis_x);
        sqlite3_bind_int(ins, 5, point.axis_y);
        sqlite3_bind_int64(ins, 6, point.seed_value);
        sqlite3_bind_int(ins, 7, point.seed_delta);
        success = sqlite3_step(ins) == SQLITE_DONE;
    }
    sqlite3_finalize(ins);

    if (success) success = ExecSql(db_, "COMMIT;");
    else ExecSql(db_, "ROLLBACK;");
    if (!success && error_out) *error_out = sqlite3_errmsg(db_);
    return success;
}

bool SqliteUiReadDb::ReplaceSeedProbeUniqueValues(
    std::int64_t probe_run_id,
    const std::vector<UiSeedProbeUniqueValue>& values,
    std::string* error_out) {
    if (db_ == nullptr || probe_run_id <= 0) {
        if (error_out) *error_out = "invalid seed probe unique projection";
        return false;
    }
    if (!ExecSql(db_, "BEGIN IMMEDIATE;")) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    bool success = true;
    sqlite3_stmt* del = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM ui_seed_probe_unique_value WHERE probe_run_id=?1;", -1, &del, nullptr) != SQLITE_OK) {
        success = false;
    } else {
        sqlite3_bind_int64(del, 1, probe_run_id);
        success = sqlite3_step(del) == SQLITE_DONE;
    }
    sqlite3_finalize(del);

    sqlite3_stmt* ins = nullptr;
    constexpr const char* kInsert =
        "INSERT INTO ui_seed_probe_unique_value(unique_value_id,probe_run_id,seed_value,seed_delta,main_x,main_y,cstick_x,cstick_y,trigger_x,trigger_y) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);";
    if (success && sqlite3_prepare_v2(db_, kInsert, -1, &ins, nullptr) != SQLITE_OK) {
        success = false;
    }
    for (const auto& value : values) {
        if (!success) break;
        sqlite3_reset(ins);
        sqlite3_clear_bindings(ins);
        sqlite3_bind_int64(ins, 1, value.unique_value_id);
        sqlite3_bind_int64(ins, 2, probe_run_id);
        sqlite3_bind_int64(ins, 3, value.seed_value);
        sqlite3_bind_int(ins, 4, value.seed_delta);
        sqlite3_bind_int(ins, 5, value.main_x);
        sqlite3_bind_int(ins, 6, value.main_y);
        sqlite3_bind_int(ins, 7, value.cstick_x);
        sqlite3_bind_int(ins, 8, value.cstick_y);
        sqlite3_bind_int(ins, 9, value.trigger_x);
        sqlite3_bind_int(ins, 10, value.trigger_y);
        success = sqlite3_step(ins) == SQLITE_DONE;
    }
    sqlite3_finalize(ins);

    if (success) success = ExecSql(db_, "COMMIT;");
    else ExecSql(db_, "ROLLBACK;");
    if (!success && error_out) *error_out = sqlite3_errmsg(db_);
    return success;
}

std::optional<UiProjectionSubscription> SqliteUiReadDb::GetProjectionSubscription(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table) const {
    if (!IsSubscriptionKeyValid(projector_name, source_context, source_outbox_table)) {
        return std::nullopt;
    }

    return ReadSubscription(db_, projector_name, source_context, source_outbox_table);
}

std::vector<UiProjectionSubscription> SqliteUiReadDb::ListProjectionSubscriptions(
    const std::string& source_context,
    const std::string& source_outbox_table) const {
    std::vector<UiProjectionSubscription> subscriptions;
    if (source_context.empty() || source_outbox_table.empty()) {
        return subscriptions;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT projector_name, source_context, source_outbox_table, "
        "COALESCE(last_outbox_id, 0), COALESCE(last_event_id, ''), updated_at_utc, "
        "COALESCE(status, 'ACTIVE'), COALESCE(last_error, '') "
        "FROM ui_projection_subscription "
        "WHERE source_context=?1 AND source_outbox_table=?2 "
        "ORDER BY projector_name ASC;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return subscriptions;
    }
    sqlite3_bind_text(st, 1, source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(st) == SQLITE_ROW) {
        UiProjectionSubscription subscription{};
        const auto* projector_name_text = sqlite3_column_text(st, 0);
        const auto* source_context_text = sqlite3_column_text(st, 1);
        const auto* source_outbox_table_text = sqlite3_column_text(st, 2);
        const auto* last_event_id_text = sqlite3_column_text(st, 4);
        const auto* status_text = sqlite3_column_text(st, 6);
        const auto* last_error_text = sqlite3_column_text(st, 7);
        subscription.projector_name = projector_name_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(projector_name_text);
        subscription.source_context = source_context_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(source_context_text);
        subscription.source_outbox_table = source_outbox_table_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(source_outbox_table_text);
        subscription.last_outbox_id = sqlite3_column_int64(st, 3);
        subscription.last_event_id = last_event_id_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(last_event_id_text);
        subscription.updated_at_utc = FromEpochMillis(sqlite3_column_int64(st, 5));
        subscription.status = status_text == nullptr
            ? std::string{ "ACTIVE" }
            : reinterpret_cast<const char*>(status_text);
        subscription.last_error = last_error_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(last_error_text);
        subscriptions.push_back(std::move(subscription));
    }

    sqlite3_finalize(st);
    return subscriptions;
}

std::optional<std::int64_t> SqliteUiReadDb::ComputeSafeFloorOutboxId(
    const std::string& source_context,
    const std::string& source_outbox_table) const {
    if (source_context.empty() || source_outbox_table.empty()) {
        return std::nullopt;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT MIN(last_outbox_id) "
        "FROM ui_projection_subscription "
        "WHERE source_context=?1 AND source_outbox_table=?2 AND status='ACTIVE';";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_text(st, 1, source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<std::int64_t> safe_floor;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
        safe_floor = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return safe_floor;
}

std::optional<UiProjectionSubscription> SqliteUiReadDb::GetOrCreateProjectionSubscription(
    const UiProjectionSubscription& subscription) {
    if (!IsSubscriptionKeyValid(
            subscription.projector_name,
            subscription.source_context,
            subscription.source_outbox_table)) {
        return std::nullopt;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "INSERT INTO ui_projection_subscription("
        "projector_name, source_context, source_outbox_table, last_outbox_id, last_event_id, updated_at_utc, status, last_error) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
        "ON CONFLICT(projector_name, source_context, source_outbox_table) DO NOTHING;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_text(st, 1, subscription.projector_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, subscription.source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, subscription.source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, subscription.last_outbox_id);
    if (subscription.last_event_id.empty()) {
        sqlite3_bind_null(st, 5);
    }
    else {
        sqlite3_bind_text(st, 5, subscription.last_event_id.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(st, 6, ToEpochMillis(subscription.updated_at_utc));
    const auto status = subscription.status.empty() ? std::string{ "ACTIVE" } : subscription.status;
    sqlite3_bind_text(st, 7, status.c_str(), -1, SQLITE_TRANSIENT);
    if (subscription.last_error.empty()) {
        sqlite3_bind_null(st, 8);
    }
    else {
        sqlite3_bind_text(st, 8, subscription.last_error.c_str(), -1, SQLITE_TRANSIENT);
    }

    const auto rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        return std::nullopt;
    }

    return GetProjectionSubscription(
        subscription.projector_name,
        subscription.source_context,
        subscription.source_outbox_table);
}

bool SqliteUiReadDb::AdvanceProjectionSubscriptionCursor(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table,
    std::int64_t last_outbox_id,
    const std::string& last_event_id,
    types::UtcTimePoint updated_at_utc,
    const std::optional<UiProjectionSubscriptionBatchAudit>& batch_audit) {
    if (!IsSubscriptionKeyValid(projector_name, source_context, source_outbox_table)) {
        return false;
    }

    if (!ExecSql(db_, "BEGIN IMMEDIATE;")) {
        return false;
    }

    bool success = true;
    sqlite3_stmt* st = nullptr;
    constexpr const char* kUpdateSql =
        "UPDATE ui_projection_subscription "
        "SET last_outbox_id=?4, last_event_id=?5, updated_at_utc=?6, status='ACTIVE', last_error=NULL "
        "WHERE projector_name=?1 AND source_context=?2 AND source_outbox_table=?3;";

    if (sqlite3_prepare_v2(db_, kUpdateSql, -1, &st, nullptr) != SQLITE_OK) {
        success = false;
    }
    else {
        sqlite3_bind_text(st, 1, projector_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, source_context.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 4, last_outbox_id);
        if (last_event_id.empty()) {
            sqlite3_bind_null(st, 5);
        }
        else {
            sqlite3_bind_text(st, 5, last_event_id.c_str(), -1, SQLITE_TRANSIENT);
        }
        sqlite3_bind_int64(st, 6, ToEpochMillis(updated_at_utc));

        if (sqlite3_step(st) != SQLITE_DONE || sqlite3_changes(db_) == 0) {
            success = false;
        }
    }
    sqlite3_finalize(st);

    if (success && batch_audit.has_value()) {
        sqlite3_stmt* audit_st = nullptr;
        constexpr const char* kAuditSql =
            "INSERT INTO ui_projection_subscription_audit("
            "projector_name, source_context, source_outbox_table, from_outbox_id, to_outbox_id, processed_count, failed_count, recorded_at_utc) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);";

        if (sqlite3_prepare_v2(db_, kAuditSql, -1, &audit_st, nullptr) != SQLITE_OK) {
            success = false;
        }
        else {
            sqlite3_bind_text(audit_st, 1, batch_audit->projector_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(audit_st, 2, batch_audit->source_context.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(audit_st, 3, batch_audit->source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(audit_st, 4, batch_audit->from_outbox_id);
            sqlite3_bind_int64(audit_st, 5, batch_audit->to_outbox_id);
            sqlite3_bind_int64(audit_st, 6, batch_audit->processed_count);
            sqlite3_bind_int64(audit_st, 7, batch_audit->failed_count);
            sqlite3_bind_int64(audit_st, 8, ToEpochMillis(batch_audit->recorded_at_utc));

            if (sqlite3_step(audit_st) != SQLITE_DONE) {
                success = false;
            }
        }
        sqlite3_finalize(audit_st);
    }

    if (success) {
        success = ExecSql(db_, "COMMIT;");
    }
    else {
        ExecSql(db_, "ROLLBACK;");
    }

    return success;
}

bool SqliteUiReadDb::SetProjectionSubscriptionError(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table,
    const std::string& last_error,
    types::UtcTimePoint updated_at_utc) {
    if (!IsSubscriptionKeyValid(projector_name, source_context, source_outbox_table)) {
        return false;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "UPDATE ui_projection_subscription "
        "SET status='ERROR', last_error=?4, updated_at_utc=?5 "
        "WHERE projector_name=?1 AND source_context=?2 AND source_outbox_table=?3;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(st, 1, projector_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, last_error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, ToEpochMillis(updated_at_utc));

    const auto rc = sqlite3_step(st);
    const auto rows = sqlite3_changes(db_);
    sqlite3_finalize(st);

    return rc == SQLITE_DONE && rows > 0;
}

bool SqliteUiReadDb::PauseProjectionSubscription(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table,
    types::UtcTimePoint updated_at_utc,
    const std::string& reason) {
    if (!IsSubscriptionKeyValid(projector_name, source_context, source_outbox_table)) {
        return false;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "UPDATE ui_projection_subscription "
        "SET status='PAUSED', last_error=?4, updated_at_utc=?5 "
        "WHERE projector_name=?1 AND source_context=?2 AND source_outbox_table=?3;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(st, 1, projector_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    if (reason.empty()) {
        sqlite3_bind_null(st, 4);
    }
    else {
        sqlite3_bind_text(st, 4, reason.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(st, 5, ToEpochMillis(updated_at_utc));

    const auto rc = sqlite3_step(st);
    const auto rows = sqlite3_changes(db_);
    sqlite3_finalize(st);

    return rc == SQLITE_DONE && rows > 0;
}

bool SqliteUiReadDb::ResumeProjectionSubscription(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table,
    types::UtcTimePoint updated_at_utc) {
    if (!IsSubscriptionKeyValid(projector_name, source_context, source_outbox_table)) {
        return false;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "UPDATE ui_projection_subscription "
        "SET status='ACTIVE', last_error=NULL, updated_at_utc=?4 "
        "WHERE projector_name=?1 AND source_context=?2 AND source_outbox_table=?3;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(st, 1, projector_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, ToEpochMillis(updated_at_utc));

    const auto rc = sqlite3_step(st);
    const auto rows = sqlite3_changes(db_);
    sqlite3_finalize(st);

    return rc == SQLITE_DONE && rows > 0;
}

} // namespace savor::db
