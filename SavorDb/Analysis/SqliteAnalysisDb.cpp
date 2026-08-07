#include "SqliteAnalysisDb.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>

#include "../Common/Events/EventPayloadDispatch.h"
#include "../Common/Events/EventPayloadValidation.h"
#include "../Common/Events/OutboxEventIds.h"

namespace savor::db::analysis {

namespace {

struct Statement {
    Statement() {}
    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* st = nullptr;
};

std::optional<std::int64_t> ColumnInt64Optional(sqlite3_stmt* st, int index) {
    if (sqlite3_column_type(st, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st, index);
}

std::optional<int> ColumnIntOptional(sqlite3_stmt* st, int index) {
    if (sqlite3_column_type(st, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int(st, index);
}

std::optional<types::UtcTimePoint> ColumnTimeOptional(sqlite3_stmt* st, int index) {
    if (sqlite3_column_type(st, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    return types::UtcTimePoint(std::chrono::milliseconds(sqlite3_column_int64(st, index)));
}

types::UtcTimePoint ColumnTime(sqlite3_stmt* st, int index) {
    return types::UtcTimePoint(std::chrono::milliseconds(sqlite3_column_int64(st, index)));
}

std::string ColumnText(sqlite3_stmt* st, int index) {
    const auto* text = sqlite3_column_text(st, index);
    if (text == nullptr) {
        return "";
    }
    return std::string(
        reinterpret_cast<const char*>(text),
        static_cast<std::size_t>(sqlite3_column_bytes(st, index)));
}

std::optional<std::string> ColumnTextOptional(sqlite3_stmt* st, int index) {
    if (sqlite3_column_type(st, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    return ColumnText(st, index);
}

std::string_view ToDbString(TasMovieValidationOperation value) {
    switch (value) {
    case TasMovieValidationOperation::EstablishRootCursor: return "ESTABLISH_ROOT_CURSOR";
    case TasMovieValidationOperation::Validate: return "VALIDATE";
    default: return "";
    }
}

TasMovieValidationOperation ParseTasMovieValidationOperation(std::string_view value) {
    if (value == "ESTABLISH_ROOT_CURSOR") return TasMovieValidationOperation::EstablishRootCursor;
    if (value == "VALIDATE") return TasMovieValidationOperation::Validate;
    return TasMovieValidationOperation::Unknown;
}

std::string_view ToDbString(TasMovieValidationSourceKind value) {
    switch (value) {
    case TasMovieValidationSourceKind::DtmArtifact: return "DTM_ARTIFACT";
    case TasMovieValidationSourceKind::RootEstablishment: return "ROOT_ESTABLISHMENT";
    case TasMovieValidationSourceKind::Tree: return "TREE";
    default: return "";
    }
}

TasMovieValidationSourceKind ParseTasMovieValidationSourceKind(std::string_view value) {
    if (value == "DTM_ARTIFACT") return TasMovieValidationSourceKind::DtmArtifact;
    if (value == "ROOT_ESTABLISHMENT") return TasMovieValidationSourceKind::RootEstablishment;
    if (value == "TREE") return TasMovieValidationSourceKind::Tree;
    return TasMovieValidationSourceKind::Unknown;
}

std::string_view ToDbString(TasMovieValidationOutcome value) {
    switch (value) {
    case TasMovieValidationOutcome::RootCursorEstablished: return "ROOT_CURSOR_ESTABLISHED";
    case TasMovieValidationOutcome::Valid: return "VALID";
    case TasMovieValidationOutcome::Invalid: return "INVALID";
    default: return "";
    }
}

TasMovieValidationOutcome ParseTasMovieValidationOutcome(std::string_view value) {
    if (value == "ROOT_CURSOR_ESTABLISHED") return TasMovieValidationOutcome::RootCursorEstablished;
    if (value == "VALID") return TasMovieValidationOutcome::Valid;
    if (value == "INVALID") return TasMovieValidationOutcome::Invalid;
    return TasMovieValidationOutcome::Unknown;
}

std::string_view ToDbString(TasMovieValidationFailureReason value) {
    switch (value) {
    case TasMovieValidationFailureReason::MovieDesynchronized: return "MOVIE_DESYNCHRONIZED";
    case TasMovieValidationFailureReason::ExpectedTerminalNotReached: return "EXPECTED_TERMINAL_NOT_REACHED";
    case TasMovieValidationFailureReason::Unknown: return "UNKNOWN";
    default: return "";
    }
}

TasMovieValidationFailureReason ParseTasMovieValidationFailureReason(std::string_view value) {
    if (value == "MOVIE_DESYNCHRONIZED") return TasMovieValidationFailureReason::MovieDesynchronized;
    if (value == "EXPECTED_TERMINAL_NOT_REACHED") return TasMovieValidationFailureReason::ExpectedTerminalNotReached;
    if (value == "UNKNOWN") return TasMovieValidationFailureReason::Unknown;
    return TasMovieValidationFailureReason::None;
}

TasMovieValidationStatus ParseTasMovieValidationStatus(std::string_view value) {
    if (value == "VALID") return TasMovieValidationStatus::Valid;
    if (value == "QUARANTINED") return TasMovieValidationStatus::Quarantined;
    return TasMovieValidationStatus::Untested;
}

TasMovieValidationRequestRecord ReadTasMovieValidationRequest(sqlite3_stmt* st) {
    TasMovieValidationRequestRecord row{};
    row.validation_request_id = sqlite3_column_int64(st, 0);
    row.materialization_key = ColumnText(st, 1);
    row.workflow_instance_id = sqlite3_column_int64(st, 2);
    row.workflow_step_id = sqlite3_column_int64(st, 3);
    row.step_kind = ColumnText(st, 4);
    row.operation = ParseTasMovieValidationOperation(ColumnText(st, 5));
    row.source_kind = ParseTasMovieValidationSourceKind(ColumnText(st, 6));
    row.source_ref_id = sqlite3_column_int64(st, 7);
    row.source_dtm_artifact_id = sqlite3_column_int64(st, 8);
    row.source_dtm_sha256 = ColumnText(st, 9);
    row.rtc_value = ColumnInt64Optional(st, 10);
    row.effective_dtm_sha256 = ColumnText(st, 11);
    row.itinerary_artifact_id = ColumnInt64Optional(st, 12);
    row.itinerary_sha256 = ColumnTextOptional(st, 13);
    row.required_final_breakpoint_pc = static_cast<std::uint32_t>(sqlite3_column_int64(st, 14));
    row.capture_root_checkpoint = sqlite3_column_int(st, 15) != 0;
    row.full_phase_program_kind = sqlite3_column_int64(st, 16);
    row.full_phase_program_version = sqlite3_column_int64(st, 17);
    row.full_phase_canonical_id = ColumnText(st, 18);
    row.full_phase_contract_revision = sqlite3_column_int64(st, 19);
    row.full_phase_sha256 = ColumnText(st, 20);
    row.module_canonical_id = ColumnText(st, 21);
    row.module_revision = sqlite3_column_int64(st, 22);
    row.module_sha256 = ColumnText(st, 23);
    row.created_at_utc = ColumnTime(st, 24);
    return row;
}

TasMovieValidationAttemptRecord ReadTasMovieValidationAttempt(sqlite3_stmt* st) {
    TasMovieValidationAttemptRecord row{};
    row.validation_attempt_id = sqlite3_column_int64(st, 0);
    row.validation_request_id = sqlite3_column_int64(st, 1);
    row.source_job_id = sqlite3_column_int64(st, 2);
    row.worker_terminal_sha256 = ColumnText(st, 3);
    row.outcome = ParseTasMovieValidationOutcome(ColumnText(st, 4));
    row.failure_reason = ParseTasMovieValidationFailureReason(ColumnText(st, 5));
    if (auto v = ColumnInt64Optional(st, 6)) row.expected_pc = static_cast<std::uint32_t>(*v);
    if (auto v = ColumnInt64Optional(st, 7)) row.expected_input_count = static_cast<std::uint64_t>(*v);
    row.actual_pc = static_cast<std::uint32_t>(sqlite3_column_int64(st, 8));
    row.actual_input_count = static_cast<std::uint64_t>(sqlite3_column_int64(st, 9));
    if (auto v = ColumnInt64Optional(st, 10)) row.last_verified_itinerary_index = static_cast<std::uint64_t>(*v);
    row.last_known_good_savestate_id = ColumnInt64Optional(st, 11);
    row.candidate_itinerary_artifact_id = ColumnInt64Optional(st, 12);
    row.candidate_itinerary_sha256 = ColumnTextOptional(st, 13);
    row.produced_tas_movie_root_id = ColumnInt64Optional(st, 14);
    row.worker_id = ColumnText(st, 15);
    row.worker_process_generation = static_cast<std::uint64_t>(sqlite3_column_int64(st, 16));
    row.workset_epoch = static_cast<std::uint64_t>(sqlite3_column_int64(st, 17));
    row.recorded_at_utc = ColumnTime(st, 18);
    return row;
}

bool TasMovieValidationRequestIdentityMatches(
    const TasMovieValidationRequestRecord& row,
    const CreateTasMovieValidationRequestCommand& command) {
    return row.materialization_key == command.materialization_key
        && row.workflow_instance_id == command.workflow_instance_id
        && row.workflow_step_id == command.workflow_step_id
        && row.step_kind == command.step_kind
        && row.operation == command.operation
        && row.source_kind == command.source_kind
        && row.source_ref_id == command.source_ref_id
        && row.source_dtm_artifact_id == command.source_dtm_artifact_id
        && row.source_dtm_sha256 == command.source_dtm_sha256
        && row.rtc_value == command.rtc_value
        && row.effective_dtm_sha256 == command.effective_dtm_sha256
        && row.itinerary_artifact_id == command.itinerary_artifact_id
        && row.itinerary_sha256 == command.itinerary_sha256
        && row.required_final_breakpoint_pc == command.required_final_breakpoint_pc
        && row.capture_root_checkpoint == command.capture_root_checkpoint
        && row.full_phase_program_kind == command.full_phase_program_kind
        && row.full_phase_program_version == command.full_phase_program_version
        && row.full_phase_canonical_id == command.full_phase_canonical_id
        && row.full_phase_contract_revision == command.full_phase_contract_revision
        && row.full_phase_sha256 == command.full_phase_sha256
        && row.module_canonical_id == command.module_canonical_id
        && row.module_revision == command.module_revision
        && row.module_sha256 == command.module_sha256;
}

bool TasMovieValidationAttemptIdentityMatches(
    const TasMovieValidationAttemptRecord& row,
    const RecordTasMovieValidationAttemptCommand& command) {
    return row.validation_request_id == command.validation_request_id
        && row.source_job_id == command.source_job_id
        && row.worker_terminal_sha256 == command.worker_terminal_sha256
        && row.outcome == command.outcome
        && row.failure_reason == command.failure_reason
        && row.expected_pc == command.expected_pc
        && row.expected_input_count == command.expected_input_count
        && row.actual_pc == command.actual_pc
        && row.actual_input_count == command.actual_input_count
        && row.last_verified_itinerary_index == command.last_verified_itinerary_index
        && row.last_known_good_savestate_id == command.last_known_good_savestate_id
        && row.candidate_itinerary_artifact_id == command.candidate_itinerary_artifact_id
        && row.candidate_itinerary_sha256 == command.candidate_itinerary_sha256
        && row.produced_tas_movie_root_id == command.produced_tas_movie_root_id
        && row.worker_id == command.worker_id
        && row.worker_process_generation == command.worker_process_generation
        && row.workset_epoch == command.workset_epoch;
}

TasMovieCheckpointSterilizationRequestRecord
ReadTasMovieCheckpointSterilizationRequest(sqlite3_stmt* st) {
    TasMovieCheckpointSterilizationRequestRecord row{};
    row.sterilization_request_id = sqlite3_column_int64(st, 0);
    row.materialization_key = ColumnText(st, 1);
    row.workflow_instance_id = sqlite3_column_int64(st, 2);
    row.workflow_step_id = sqlite3_column_int64(st, 3);
    row.source_savestate_id = sqlite3_column_int64(st, 4);
    row.source_savestate_artifact_id = sqlite3_column_int64(st, 5);
    row.source_savestate_sha256 = ColumnText(st, 6);
    row.source_dtm_artifact_id = sqlite3_column_int64(st, 7);
    row.source_dtm_sha256 = ColumnText(st, 8);
    row.reused_savestate_id = ColumnInt64Optional(st, 9);
    row.full_phase_program_kind = sqlite3_column_int64(st, 10);
    row.full_phase_program_version = sqlite3_column_int64(st, 11);
    row.full_phase_canonical_id = ColumnText(st, 12);
    row.full_phase_contract_revision = sqlite3_column_int64(st, 13);
    row.full_phase_sha256 = ColumnText(st, 14);
    row.module_canonical_id = ColumnText(st, 15);
    row.module_revision = sqlite3_column_int64(st, 16);
    row.module_sha256 = ColumnText(st, 17);
    row.created_at_utc = ColumnTime(st, 18);
    return row;
}

TasMovieCheckpointSterilizationAttemptRecord
ReadTasMovieCheckpointSterilizationAttempt(sqlite3_stmt* st) {
    TasMovieCheckpointSterilizationAttemptRecord row{};
    row.sterilization_attempt_id = sqlite3_column_int64(st, 0);
    row.sterilization_request_id = sqlite3_column_int64(st, 1);
    row.source_job_id = sqlite3_column_int64(st, 2);
    row.worker_terminal_sha256 = ColumnText(st, 3);
    row.candidate_savestate_sha256 = ColumnText(st, 4);
    row.produced_savestate_id = sqlite3_column_int64(st, 5);
    row.worker_id = ColumnText(st, 6);
    row.worker_process_generation =
        static_cast<std::uint64_t>(sqlite3_column_int64(st, 7));
    row.workset_epoch = static_cast<std::uint64_t>(sqlite3_column_int64(st, 8));
    row.recorded_at_utc = ColumnTime(st, 9);
    return row;
}

bool TasMovieCheckpointSterilizationRequestIdentityMatches(
    const TasMovieCheckpointSterilizationRequestRecord& row,
    const CreateTasMovieCheckpointSterilizationRequestCommand& command) {
    return row.materialization_key == command.materialization_key
        && row.workflow_instance_id == command.workflow_instance_id
        && row.workflow_step_id == command.workflow_step_id
        && row.source_savestate_id == command.source_savestate_id
        && row.source_savestate_artifact_id == command.source_savestate_artifact_id
        && row.source_savestate_sha256 == command.source_savestate_sha256
        && row.source_dtm_artifact_id == command.source_dtm_artifact_id
        && row.source_dtm_sha256 == command.source_dtm_sha256
        && row.reused_savestate_id == command.reused_savestate_id
        && row.full_phase_program_kind == command.full_phase_program_kind
        && row.full_phase_program_version == command.full_phase_program_version
        && row.full_phase_canonical_id == command.full_phase_canonical_id
        && row.full_phase_contract_revision == command.full_phase_contract_revision
        && row.full_phase_sha256 == command.full_phase_sha256
        && row.module_canonical_id == command.module_canonical_id
        && row.module_revision == command.module_revision
        && row.module_sha256 == command.module_sha256;
}

bool TasMovieCheckpointSterilizationAttemptIdentityMatches(
    const TasMovieCheckpointSterilizationAttemptRecord& row,
    const RecordTasMovieCheckpointSterilizationAttemptCommand& command) {
    return row.sterilization_request_id == command.sterilization_request_id
        && row.source_job_id == command.source_job_id
        && row.worker_terminal_sha256 == command.worker_terminal_sha256
        && row.candidate_savestate_sha256 == command.candidate_savestate_sha256
        && row.produced_savestate_id == command.produced_savestate_id
        && row.worker_id == command.worker_id
        && row.worker_process_generation == command.worker_process_generation
        && row.workset_epoch == command.workset_epoch;
}

SeedProbeResultRow ReadSeedProbeResultRow(sqlite3_stmt* st) {
    SeedProbeResultRow row{};
    row.probe_result_id = sqlite3_column_int64(st, 0);
    row.probe_run_id = sqlite3_column_int64(st, 1);
    row.input_frame_id = sqlite3_column_int64(st, 2);
    row.source_job_id = sqlite3_column_int64(st, 3);
    row.seed_value = static_cast<std::uint32_t>(sqlite3_column_int64(st, 4));
    row.origin_worker_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 5));
    row.origin_process_generation = static_cast<std::uint64_t>(sqlite3_column_int64(st, 6));
    row.origin_workset_epoch = static_cast<std::uint64_t>(sqlite3_column_int64(st, 7));
    row.terminal_sha256 = ColumnText(st, 8);
    row.confirmation_of_probe_result_id = ColumnInt64Optional(st, 9);
    row.evidence_state = ParseSeedProbeEvidenceState(ColumnText(st, 10));
    row.recorded_at_utc = ColumnTime(st, 11);
    return row;
}

bool IsLowerHexSha256(std::string_view value) {
    return value.size() == 64
        && std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isdigit(c) != 0 || (c >= 'a' && c <= 'f');
        });
}

std::string ColumnBlob(sqlite3_stmt* st, int index) {
    const auto* blob = sqlite3_column_blob(st, index);
    if (blob == nullptr) {
        return "";
    }
    return std::string(
        static_cast<const char*>(blob),
        static_cast<std::size_t>(sqlite3_column_bytes(st, index)));
}



enum class OutboxMode {
    None,
    AnalysisSpine,
    SplitSeedProbeBattle,
};

bool TableExists(sqlite3* db, std::string_view table_name) {
    if (db == nullptr) {
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(st.st, 1, table_name.data(), static_cast<int>(table_name.size()), SQLITE_TRANSIENT);
    return sqlite3_step(st.st) == SQLITE_ROW;
}

OutboxMode ResolveOutboxMode(sqlite3* db) {
    if (TableExists(db, "asp_outbox_message")) {
        return OutboxMode::AnalysisSpine;
    }

    const bool has_sp = TableExists(db, "sp_outbox_message");
    const bool has_ab = TableExists(db, "ab_outbox_message");
    if (has_sp || has_ab) {
        return OutboxMode::SplitSeedProbeBattle;
    }

    return OutboxMode::None;
}

bool InsertSeedProbeOutboxEvent(
    sqlite3* db,
    std::string_view event_type,
    std::string_view aggregate_kind,
    std::string_view aggregate_id,
    std::string_view correlation_id,
    std::string_view causation_id,
    std::int64_t occurred_at_utc,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id,
    std::string* error_out) {
    constexpr int kMaxEventIdAttempts = 5;
    for (int attempt = 0; attempt < kMaxEventIdAttempts; ++attempt) {
        std::string event_id;
        if (!outbox::MakeDbOwnedEventId(
                db,
                "AnalysisSeedProbe",
                event_type,
                payload_ref_kind,
                payload_ref_id,
                &event_id,
                error_out)) {
            return false;
        }

        Statement st;
        if (sqlite3_prepare_v2(
                db,
                "INSERT INTO sp_outbox_message("
                "event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id) "
                "VALUES(?1,?2,1,'AnalysisSeedProbe',?3,?4,?5,?6,?7,?8,?9);",
                -1,
                &st.st,
                nullptr)
            != SQLITE_OK) {
            if (error_out) {
                *error_out = sqlite3_errmsg(db);
            }
            return false;
        }

        sqlite3_bind_text(st.st, 1, event_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 2, event_type.data(), static_cast<int>(event_type.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 3, aggregate_kind.data(), static_cast<int>(aggregate_kind.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 4, aggregate_id.data(), static_cast<int>(aggregate_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 5, correlation_id.data(), static_cast<int>(correlation_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 6, causation_id.data(), static_cast<int>(causation_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(st.st, 7, occurred_at_utc);
        sqlite3_bind_text(st.st, 8, payload_ref_kind.data(), static_cast<int>(payload_ref_kind.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(st.st, 9, payload_ref_id);
        const auto rc = sqlite3_step(st.st);
        if (rc == SQLITE_DONE) {
            return true;
        }
        if (!outbox::IsUniqueConstraint(db)) {
            if (error_out != nullptr) {
                *error_out = sqlite3_errmsg(db);
            }
            return false;
        }
    }
    if (error_out != nullptr) {
        *error_out = "failed to generate a unique seed-probe outbox event id";
    }
    return false;
}

bool InsertBattleOutboxEvent(
    sqlite3* db,
    std::string_view event_type,
    std::string_view aggregate_kind,
    std::string_view aggregate_id,
    std::string_view correlation_id,
    std::string_view causation_id,
    std::int64_t occurred_at_utc,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id,
    std::string* error_out) {
    constexpr int kMaxEventIdAttempts = 5;
    for (int attempt = 0; attempt < kMaxEventIdAttempts; ++attempt) {
        std::string event_id;
        if (!outbox::MakeDbOwnedEventId(
                db,
                "AnalysisBattle",
                event_type,
                payload_ref_kind,
                payload_ref_id,
                &event_id,
                error_out)) {
            return false;
        }

        Statement st;
        if (sqlite3_prepare_v2(
                db,
                "INSERT INTO ab_outbox_message("
                "event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id) "
                "VALUES(?1,?2,1,'AnalysisBattle',?3,?4,?5,?6,?7,?8,?9);",
                -1,
                &st.st,
                nullptr)
            != SQLITE_OK) {
            if (error_out) {
                *error_out = sqlite3_errmsg(db);
            }
            return false;
        }

        sqlite3_bind_text(st.st, 1, event_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 2, event_type.data(), static_cast<int>(event_type.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 3, aggregate_kind.data(), static_cast<int>(aggregate_kind.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 4, aggregate_id.data(), static_cast<int>(aggregate_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 5, correlation_id.data(), static_cast<int>(correlation_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 6, causation_id.data(), static_cast<int>(causation_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(st.st, 7, occurred_at_utc);
        sqlite3_bind_text(st.st, 8, payload_ref_kind.data(), static_cast<int>(payload_ref_kind.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(st.st, 9, payload_ref_id);
        const auto rc = sqlite3_step(st.st);
        if (rc == SQLITE_DONE) {
            return true;
        }
        if (!outbox::IsUniqueConstraint(db)) {
            if (error_out != nullptr) {
                *error_out = sqlite3_errmsg(db);
            }
            return false;
        }
    }
    if (error_out != nullptr) {
        *error_out = "failed to generate a unique battle outbox event id";
    }
    return false;
}

std::optional<std::int64_t> CreateAnalysisInputSetForPendingProbeRun(
    sqlite3* db,
    std::int64_t created_at_utc,
    std::string* error_out) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "INSERT INTO an_input_set(content_hash,source_ref_kind,source_ref_id,created_at_utc) "
            "VALUES(NULL,'sp_probe_run',NULL,?1);",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, created_at_utc);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return std::nullopt;
    }
    return sqlite3_last_insert_rowid(db);
}

bool AttachAnalysisInputSetToProbeRun(
    sqlite3* db,
    std::int64_t input_set_id,
    std::int64_t probe_run_id,
    std::string* error_out) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "UPDATE an_input_set SET source_ref_id=?2 WHERE input_set_id=?1 AND source_ref_kind='sp_probe_run';",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, input_set_id);
    sqlite3_bind_int64(st.st, 2, probe_run_id);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    return sqlite3_changes(db) > 0;
}

std::optional<std::int64_t> AcceptedInputSetIdForProbeRun(sqlite3* db, std::int64_t probe_run_id) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT accepted_input_set_id FROM sp_probe_run WHERE probe_run_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, probe_run_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st.st, 0);
}

std::optional<std::int64_t> BattleSetIdForWave(sqlite3* db, std::int64_t wave_id) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT battle_set_id FROM ab_turn_wave WHERE wave_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, wave_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st.st, 0);
}

std::optional<std::int64_t> BattleSetIdForBattleAdvancementPool(sqlite3* db, std::int64_t battle_advancement_pool_id) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT battle_set_id FROM ab_battle_advancement_pool WHERE battle_advancement_pool_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, battle_advancement_pool_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st.st, 0);
}

std::optional<std::int64_t> BattleSetIdForTurnJob(sqlite3* db, std::int64_t turn_job_id) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT w.battle_set_id "
            "FROM ab_turn_job j "
            "JOIN ab_turn_wave w ON w.wave_id=j.wave_id "
            "WHERE j.turn_job_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, turn_job_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st.st, 0);
}

void BindOptionalInt64(sqlite3_stmt* st, int index, std::optional<std::int64_t> value) {
    if (value.has_value()) {
        sqlite3_bind_int64(st, index, *value);
    }
    else {
        sqlite3_bind_null(st, index);
    }
}

struct BattleTurnJobProjectionRef {
    std::int64_t battle_set_id = 0;
    std::int64_t turn_job_id = 0;
};

std::optional<BattleTurnJobProjectionRef> BattleTurnJobProjectionRefForExecJob(sqlite3* db, std::int64_t exec_job_id) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT w.battle_set_id,j.turn_job_id "
            "FROM ab_turn_job j "
            "JOIN ab_turn_wave w ON w.wave_id=j.wave_id "
            "WHERE j.exec_job_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, exec_job_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    return BattleTurnJobProjectionRef{
        .battle_set_id = sqlite3_column_int64(st.st, 0),
        .turn_job_id = sqlite3_column_int64(st.st, 1),
    };
}

std::optional<std::int64_t> ManualFollowupIdForTurnJob(sqlite3* db, std::int64_t turn_job_id) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT manual_followup_id FROM ab_manual_followup WHERE turn_job_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, turn_job_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st.st, 0);
}

events::EventEnvelope ReadEnvelope(sqlite3_stmt* st, int column_offset = 0) {
    events::EventEnvelope envelope{};

    const auto* event_id = sqlite3_column_text(st, column_offset + 0);
    const auto* event_type = sqlite3_column_text(st, column_offset + 1);
    const auto event_version = sqlite3_column_int(st, column_offset + 2);
    const auto* context_name = sqlite3_column_text(st, column_offset + 3);
    const auto* aggregate_kind = sqlite3_column_text(st, column_offset + 4);
    const auto* aggregate_id = sqlite3_column_text(st, column_offset + 5);
    const auto* correlation_id = sqlite3_column_text(st, column_offset + 6);
    const auto* causation_id = sqlite3_column_text(st, column_offset + 7);
    const auto occurred_at_utc = sqlite3_column_int64(st, column_offset + 8);
    const auto* payload_ref_kind = sqlite3_column_text(st, column_offset + 9);
    const auto payload_ref_id = sqlite3_column_int64(st, column_offset + 10);

    envelope.event_id = event_id == nullptr ? "" : reinterpret_cast<const char*>(event_id);
    envelope.event_type = event_type == nullptr ? "" : reinterpret_cast<const char*>(event_type);
    envelope.event_version = event_version;
    envelope.context_name = context_name == nullptr ? "" : reinterpret_cast<const char*>(context_name);
    envelope.aggregate_kind = aggregate_kind == nullptr ? "" : reinterpret_cast<const char*>(aggregate_kind);
    envelope.aggregate_id = aggregate_id == nullptr ? "" : reinterpret_cast<const char*>(aggregate_id);
    envelope.correlation_id = correlation_id == nullptr ? "" : reinterpret_cast<const char*>(correlation_id);
    envelope.causation_id = causation_id == nullptr ? "" : reinterpret_cast<const char*>(causation_id);
    envelope.occurred_at_utc = types::UtcTimePoint(std::chrono::milliseconds(occurred_at_utc));
    envelope.payload_ref_kind = payload_ref_kind == nullptr ? "" : reinterpret_cast<const char*>(payload_ref_kind);
    envelope.payload_ref_id = payload_ref_id;

    return envelope;
}

std::optional<events::AnalysisSeedProbePayloadView> ResolveSeedProbeByKind(
    const SqliteSeedProbePayloadRowResolver& resolver,
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) {
    if (event_version != 1 || payload_ref_id <= 0) {
        return std::nullopt;
    }

    events::AnalysisSeedProbePayloadView record{};

    if (payload_ref_kind == "probe_set") {
        const auto view = resolver.ResolveSeedProbeSetCreated(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.probe_set_id = view->probe_set_id;
        return record;
    }
    if (payload_ref_kind == "probe_run") {
        const auto view = resolver.ResolveSeedProbeRunRequested(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.probe_set_id = view->probe_set_id;
        record.probe_run_id = view->probe_run_id;
        return record;
    }
    if (payload_ref_kind == "probe_result") {
        const auto view = resolver.ResolveSeedProbeResult(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.probe_run_id = view->probe_run_id;
        record.probe_result_id = view->probe_result_id;
        return record;
    }
    if (payload_ref_kind == "encounter_projection") {
        const auto view = resolver.ResolveSeedProbeEncounterProjectionRecorded(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.probe_run_id = view->probe_run_id;
        return record;
    }
    return std::nullopt;
}

std::optional<events::AnalysisBattlePayloadView> ResolveBattleByKind(
    const SqliteBattlePayloadRowResolver& resolver,
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) {
    if (event_version != 1 || payload_ref_id <= 0) {
        return std::nullopt;
    }

    events::AnalysisBattlePayloadView record{};

    if (payload_ref_kind == "battle_set") {
        const auto view = resolver.ResolveBattleSetCreated(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.battle_set_id = view->battle_set_id;
        return record;
    }
    if (payload_ref_kind == "seed_candidate") {
        const auto view = resolver.ResolveBattleSeedCandidateAdded(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.battle_set_id = view->battle_set_id;
        return record;
    }
    if (payload_ref_kind == "turn_wave") {
        const auto view = resolver.ResolveBattleTurnWaveCreated(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.battle_set_id = view->battle_set_id;
        record.wave_id = view->wave_id;
        return record;
    }
    if (payload_ref_kind == "turn_job") {
        const auto view = resolver.ResolveBattleTurnJobRecorded(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.battle_set_id = view->battle_set_id;
        record.wave_id = view->wave_id;
        record.turn_job_id = view->turn_job_id;
        return record;
    }
    if (payload_ref_kind == "battle_advancement_pool") {
        const auto view = resolver.ResolveBattleBattleAdvancementPoolCreated(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.battle_set_id = view->battle_set_id;
        return record;
    }
    if (payload_ref_kind == "battle_advancement_decision") {
        const auto view = resolver.ResolveBattleBattleAdvancementDecisionRecorded(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.battle_set_id = view->battle_set_id;
        record.turn_job_id = view->turn_job_id;
        return record;
    }
    if (payload_ref_kind == "manual_followup") {
        const auto view = resolver.ResolveBattleManualFollowupUpdated(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.battle_set_id = view->battle_set_id;
        record.turn_job_id = view->turn_job_id;
        return record;
    }
    if (payload_ref_kind == "battle_completion") {
        const auto view = resolver.ResolveBattleCompletion(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) return std::nullopt;
        record.battle_completion_id = view->battle_completion_id;
        return record;
    }
    if (payload_ref_kind == "battle_results") {
        const auto view = resolver.ResolveBattleResults(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) return std::nullopt;
        record.battle_completion_id = view->battle_completion_id;
        record.battle_results_id = view->battle_results_id;
        return record;
    }

    return std::nullopt;
}

std::optional<events::AnalysisSpinePayloadView> ResolveSpineByKind(
    const SqliteAnalysisSpinePayloadRowResolver& resolver,
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) {
    if (event_version != 1 || payload_ref_id <= 0) {
        return std::nullopt;
    }

    events::AnalysisSpinePayloadView record{};

    if (payload_ref_kind == "run") {
        const auto view = resolver.ResolveSpineRunCreated(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.run_id = view->run_id;
        return record;
    }
    if (payload_ref_kind == "state_ref") {
        const auto view = resolver.ResolveSpineStateRefRegistered(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.run_id = view->run_id;
        record.state_ref_id = view->state_ref_id;
        return record;
    }
    if (payload_ref_kind == "lineage_edge") {
        const auto view = resolver.ResolveSpineLineageEdgeAdded(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.run_id = view->child_run_id;
        record.lineage_edge_id = view->lineage_edge_id;
        return record;
    }
    if (payload_ref_kind == "artifact_ref") {
        const auto view = resolver.ResolveSpineArtifactLinked(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.run_id = view->run_id;
        record.artifact_ref_id = view->artifact_ref_id;
        return record;
    }

    return std::nullopt;
}

} // namespace

SqliteAnalysisDb::SqliteAnalysisDb(sqlite3* db)
    : db_(db)
    , seed_probe_row_resolver_(db_)
    , battle_row_resolver_(db_)
    , spine_row_resolver_(db_) {
}

std::optional<TasMovieValidationRequestRecord> SqliteAnalysisDb::GetTasMovieValidationRequest(
    std::int64_t validation_request_id) const {
    if (db_ == nullptr || validation_request_id <= 0) return std::nullopt;
    Statement st;
    constexpr const char* kSql =
        "SELECT validation_request_id,materialization_key,workflow_instance_id,workflow_step_id,step_kind,"
        "operation,source_kind,source_ref_id,source_dtm_artifact_id,source_dtm_sha256,rtc_value,"
        "effective_dtm_sha256,itinerary_artifact_id,itinerary_sha256,required_final_breakpoint_pc,"
        "capture_root_checkpoint,full_phase_program_kind,full_phase_program_version,full_phase_canonical_id,"
        "full_phase_contract_revision,full_phase_sha256,module_canonical_id,module_revision,module_sha256,created_at_utc "
        "FROM tmv_validation_request WHERE validation_request_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(st.st, 1, validation_request_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    return ReadTasMovieValidationRequest(st.st);
}

std::optional<TasMovieValidationRequestRecord> SqliteAnalysisDb::GetTasMovieValidationRequestForWorkflowStep(
    std::int64_t workflow_step_id) const {
    if (db_ == nullptr || workflow_step_id <= 0) return std::nullopt;
    Statement st;
    if (sqlite3_prepare_v2(db_, "SELECT validation_request_id FROM tmv_validation_request WHERE workflow_step_id=?1;", -1, &st.st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(st.st, 1, workflow_step_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    return GetTasMovieValidationRequest(sqlite3_column_int64(st.st, 0));
}

bool SqliteAnalysisDb::CreateTasMovieValidationRequest(
    const CreateTasMovieValidationRequestCommand& command,
    std::int64_t* validation_request_id_out,
    std::string* error_out) {
    const bool establishment = command.operation == TasMovieValidationOperation::EstablishRootCursor
        && command.source_kind == TasMovieValidationSourceKind::DtmArtifact
        && command.step_kind == "tasmovie.establish_root_cursor"
        && !command.rtc_value && !command.itinerary_artifact_id && !command.itinerary_sha256
        && !command.capture_root_checkpoint;
    const bool root_validation = command.operation == TasMovieValidationOperation::Validate
        && command.source_kind == TasMovieValidationSourceKind::RootEstablishment
        && command.step_kind == "tasmovie.validate_root"
        && command.rtc_value && command.itinerary_artifact_id && command.itinerary_sha256;
    const bool tree_validation = command.operation == TasMovieValidationOperation::Validate
        && command.source_kind == TasMovieValidationSourceKind::Tree
        && command.step_kind == "tasmovie.validate_tree"
        && !command.rtc_value && command.itinerary_artifact_id && command.itinerary_sha256
        && !command.capture_root_checkpoint;
    if (db_ == nullptr || command.materialization_key.empty() || command.workflow_instance_id <= 0
        || command.workflow_step_id <= 0 || command.source_ref_id <= 0
        || command.source_dtm_artifact_id <= 0 || !IsLowerHexSha256(command.source_dtm_sha256)
        || !IsLowerHexSha256(command.effective_dtm_sha256)
        || !IsLowerHexSha256(command.full_phase_sha256) || !IsLowerHexSha256(command.module_sha256)
        || command.full_phase_canonical_id.empty() || command.module_canonical_id.empty()
        || !(establishment || root_validation || tree_validation)) {
        if (error_out) *error_out = "invalid immutable TAS movie validation request";
        return false;
    }
    if (const auto existing = GetTasMovieValidationRequestForWorkflowStep(command.workflow_step_id); existing.has_value()) {
        if (!TasMovieValidationRequestIdentityMatches(*existing, command)) {
            if (error_out) *error_out = "workflow step already has a different TAS movie validation request";
            return false;
        }
        if (validation_request_id_out) *validation_request_id_out = existing->validation_request_id;
        return true;
    }
    Statement st;
    constexpr const char* kSql =
        "INSERT INTO tmv_validation_request(materialization_key,workflow_instance_id,workflow_step_id,step_kind,"
        "operation,source_kind,source_ref_id,source_dtm_artifact_id,source_dtm_sha256,rtc_value,effective_dtm_sha256,"
        "itinerary_artifact_id,itinerary_sha256,required_final_breakpoint_pc,capture_root_checkpoint,"
        "full_phase_program_kind,full_phase_program_version,full_phase_canonical_id,full_phase_contract_revision,"
        "full_phase_sha256,module_canonical_id,module_revision,module_sha256,created_at_utc) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23,?24);";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(st.st, 1, command.materialization_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 2, command.workflow_instance_id);
    sqlite3_bind_int64(st.st, 3, command.workflow_step_id);
    sqlite3_bind_text(st.st, 4, command.step_kind.c_str(), -1, SQLITE_TRANSIENT);
    const auto operation = ToDbString(command.operation);
    const auto source_kind = ToDbString(command.source_kind);
    sqlite3_bind_text(st.st, 5, operation.data(), static_cast<int>(operation.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 6, source_kind.data(), static_cast<int>(source_kind.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 7, command.source_ref_id);
    sqlite3_bind_int64(st.st, 8, command.source_dtm_artifact_id);
    sqlite3_bind_text(st.st, 9, command.source_dtm_sha256.c_str(), -1, SQLITE_TRANSIENT);
    if (command.rtc_value) sqlite3_bind_int64(st.st, 10, *command.rtc_value); else sqlite3_bind_null(st.st, 10);
    sqlite3_bind_text(st.st, 11, command.effective_dtm_sha256.c_str(), -1, SQLITE_TRANSIENT);
    if (command.itinerary_artifact_id) sqlite3_bind_int64(st.st, 12, *command.itinerary_artifact_id); else sqlite3_bind_null(st.st, 12);
    if (command.itinerary_sha256) sqlite3_bind_text(st.st, 13, command.itinerary_sha256->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st.st, 13);
    sqlite3_bind_int64(st.st, 14, command.required_final_breakpoint_pc);
    sqlite3_bind_int(st.st, 15, command.capture_root_checkpoint ? 1 : 0);
    sqlite3_bind_int64(st.st, 16, command.full_phase_program_kind);
    sqlite3_bind_int64(st.st, 17, command.full_phase_program_version);
    sqlite3_bind_text(st.st, 18, command.full_phase_canonical_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 19, command.full_phase_contract_revision);
    sqlite3_bind_text(st.st, 20, command.full_phase_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 21, command.module_canonical_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 22, command.module_revision);
    sqlite3_bind_text(st.st, 23, command.module_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 24, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (validation_request_id_out) *validation_request_id_out = sqlite3_last_insert_rowid(db_);
    return true;
}

std::optional<TasMovieValidationAttemptRecord> SqliteAnalysisDb::GetTasMovieValidationAttempt(
    std::int64_t validation_attempt_id) const {
    if (db_ == nullptr || validation_attempt_id <= 0) return std::nullopt;
    Statement st;
    constexpr const char* kSql =
        "SELECT validation_attempt_id,validation_request_id,source_job_id,worker_terminal_sha256,outcome,"
        "failure_reason,expected_pc,expected_input_count,actual_pc,actual_input_count,last_verified_itinerary_index,"
        "last_known_good_savestate_id,candidate_itinerary_artifact_id,candidate_itinerary_sha256,"
        "produced_tas_movie_root_id,worker_id,worker_process_generation,workset_epoch,recorded_at_utc "
        "FROM tmv_validation_attempt WHERE validation_attempt_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(st.st, 1, validation_attempt_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    return ReadTasMovieValidationAttempt(st.st);
}

std::optional<TasMovieValidationAttemptRecord> SqliteAnalysisDb::FindTasMovieValidationAttempt(
    std::int64_t source_job_id,
    std::string_view worker_terminal_sha256) const {
    if (db_ == nullptr || source_job_id <= 0 || worker_terminal_sha256.empty()) return std::nullopt;
    Statement st;
    if (sqlite3_prepare_v2(db_, "SELECT validation_attempt_id FROM tmv_validation_attempt WHERE source_job_id=?1 AND worker_terminal_sha256=?2;", -1, &st.st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(st.st, 1, source_job_id);
    sqlite3_bind_text(st.st, 2, worker_terminal_sha256.data(), static_cast<int>(worker_terminal_sha256.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    return GetTasMovieValidationAttempt(sqlite3_column_int64(st.st, 0));
}

bool SqliteAnalysisDb::RecordTasMovieValidationAttempt(
    const RecordTasMovieValidationAttemptCommand& command,
    std::int64_t* validation_attempt_id_out,
    std::string* error_out) {
    const bool root_cursor = command.outcome == TasMovieValidationOutcome::RootCursorEstablished
        && command.failure_reason == TasMovieValidationFailureReason::None
        && command.candidate_itinerary_artifact_id && command.candidate_itinerary_sha256
        && IsLowerHexSha256(*command.candidate_itinerary_sha256)
        && !command.produced_tas_movie_root_id
        && !command.expected_pc && !command.expected_input_count
        && !command.last_verified_itinerary_index;
    const bool valid = command.outcome == TasMovieValidationOutcome::Valid
        && command.failure_reason == TasMovieValidationFailureReason::None
        && !command.candidate_itinerary_artifact_id && !command.candidate_itinerary_sha256
        && !command.expected_pc && !command.expected_input_count;
    const bool invalid = command.outcome == TasMovieValidationOutcome::Invalid
        && command.failure_reason != TasMovieValidationFailureReason::None
        && !command.candidate_itinerary_artifact_id && !command.candidate_itinerary_sha256
        && !command.produced_tas_movie_root_id;
    constexpr auto kI64Max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (db_ == nullptr || command.validation_request_id <= 0 || command.source_job_id <= 0
        || !IsLowerHexSha256(command.worker_terminal_sha256) || command.worker_id.empty()
        || command.actual_input_count > kI64Max
        || (command.expected_input_count && *command.expected_input_count > kI64Max)
        || (command.last_verified_itinerary_index && *command.last_verified_itinerary_index > kI64Max)
        || command.worker_process_generation > kI64Max
        || command.workset_epoch > kI64Max
        || !(root_cursor || valid || invalid)) {
        if (error_out) *error_out = "invalid immutable TAS movie validation attempt";
        return false;
    }
    const auto request = GetTasMovieValidationRequest(command.validation_request_id);
    if (!request.has_value()) {
        if (error_out) *error_out = "TAS movie validation request does not exist";
        return false;
    }
    const bool request_outcome_matches =
        (root_cursor
            && request->operation
                == TasMovieValidationOperation::EstablishRootCursor)
        || ((valid || invalid)
            && request->operation == TasMovieValidationOperation::Validate);
    const bool root_publication_matches =
        !valid
        || (request->capture_root_checkpoint
                ? command.produced_tas_movie_root_id.has_value()
                : !command.produced_tas_movie_root_id.has_value());
    if (!request_outcome_matches || !root_publication_matches) {
        if (error_out)
            *error_out =
                "TAS movie validation attempt does not match its immutable request";
        return false;
    }
    if (const auto existing = FindTasMovieValidationAttempt(command.source_job_id, command.worker_terminal_sha256); existing.has_value()) {
        if (!TasMovieValidationAttemptIdentityMatches(*existing, command)) {
            if (error_out) *error_out = "worker terminal already identifies a different TAS movie validation attempt";
            return false;
        }
        if (validation_attempt_id_out) *validation_attempt_id_out = existing->validation_attempt_id;
        return true;
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    Statement st;
    constexpr const char* kSql =
        "INSERT INTO tmv_validation_attempt(validation_request_id,source_job_id,worker_terminal_sha256,outcome,"
        "failure_reason,expected_pc,expected_input_count,actual_pc,actual_input_count,last_verified_itinerary_index,"
        "last_known_good_savestate_id,candidate_itinerary_artifact_id,candidate_itinerary_sha256,produced_tas_movie_root_id,"
        "worker_id,worker_process_generation,workset_epoch,recorded_at_utc) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18);";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, command.validation_request_id);
    sqlite3_bind_int64(st.st, 2, command.source_job_id);
    sqlite3_bind_text(st.st, 3, command.worker_terminal_sha256.c_str(), -1, SQLITE_TRANSIENT);
    const auto outcome = ToDbString(command.outcome);
    sqlite3_bind_text(st.st, 4, outcome.data(), static_cast<int>(outcome.size()), SQLITE_TRANSIENT);
    if (invalid) {
        const auto reason = ToDbString(command.failure_reason);
        sqlite3_bind_text(st.st, 5, reason.data(), static_cast<int>(reason.size()), SQLITE_TRANSIENT);
    } else sqlite3_bind_null(st.st, 5);
    if (command.expected_pc) sqlite3_bind_int64(st.st, 6, *command.expected_pc); else sqlite3_bind_null(st.st, 6);
    if (command.expected_input_count) sqlite3_bind_int64(st.st, 7, static_cast<std::int64_t>(*command.expected_input_count)); else sqlite3_bind_null(st.st, 7);
    sqlite3_bind_int64(st.st, 8, command.actual_pc);
    sqlite3_bind_int64(st.st, 9, static_cast<std::int64_t>(command.actual_input_count));
    if (command.last_verified_itinerary_index) sqlite3_bind_int64(st.st, 10, static_cast<std::int64_t>(*command.last_verified_itinerary_index)); else sqlite3_bind_null(st.st, 10);
    if (command.last_known_good_savestate_id) sqlite3_bind_int64(st.st, 11, *command.last_known_good_savestate_id); else sqlite3_bind_null(st.st, 11);
    if (command.candidate_itinerary_artifact_id) sqlite3_bind_int64(st.st, 12, *command.candidate_itinerary_artifact_id); else sqlite3_bind_null(st.st, 12);
    if (command.candidate_itinerary_sha256) sqlite3_bind_text(st.st, 13, command.candidate_itinerary_sha256->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st.st, 13);
    if (command.produced_tas_movie_root_id) sqlite3_bind_int64(st.st, 14, *command.produced_tas_movie_root_id); else sqlite3_bind_null(st.st, 14);
    sqlite3_bind_text(st.st, 15, command.worker_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 16, static_cast<std::int64_t>(command.worker_process_generation));
    sqlite3_bind_int64(st.st, 17, static_cast<std::int64_t>(command.workset_epoch));
    sqlite3_bind_int64(st.st, 18, command.recorded_at_utc.time_since_epoch().count());
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    const auto id = sqlite3_last_insert_rowid(db_);
    if (request->operation == TasMovieValidationOperation::Validate && (valid || invalid)) {
        Statement status;
        constexpr const char* kStatusSql =
            "INSERT INTO tmv_dtm_validation_status(effective_dtm_sha256,status,validation_attempt_id,updated_at_utc) "
            "VALUES(?1,?2,?3,?4) ON CONFLICT(effective_dtm_sha256) DO UPDATE SET "
            "status=excluded.status,validation_attempt_id=excluded.validation_attempt_id,updated_at_utc=excluded.updated_at_utc;";
        if (sqlite3_prepare_v2(db_, kStatusSql, -1, &status.st, nullptr) != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        sqlite3_bind_text(status.st, 1, request->effective_dtm_sha256.c_str(), -1, SQLITE_TRANSIENT);
        const auto status_value = valid ? "VALID" : "QUARANTINED";
        sqlite3_bind_text(status.st, 2, status_value, -1, SQLITE_STATIC);
        sqlite3_bind_int64(status.st, 3, id);
        sqlite3_bind_int64(status.st, 4, command.recorded_at_utc.time_since_epoch().count());
        if (sqlite3_step(status.st) != SQLITE_DONE) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
    }
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    if (validation_attempt_id_out) *validation_attempt_id_out = id;
    return true;
}

std::optional<TasMovieValidationStatusRecord> SqliteAnalysisDb::GetTasMovieValidationStatus(
    std::string_view effective_dtm_sha256) const {
    if (db_ == nullptr || !IsLowerHexSha256(effective_dtm_sha256)) return std::nullopt;
    Statement st;
    if (sqlite3_prepare_v2(db_, "SELECT effective_dtm_sha256,status,validation_attempt_id,updated_at_utc FROM tmv_dtm_validation_status WHERE effective_dtm_sha256=?1;", -1, &st.st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(st.st, 1, effective_dtm_sha256.data(), static_cast<int>(effective_dtm_sha256.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    TasMovieValidationStatusRecord row{};
    row.effective_dtm_sha256 = ColumnText(st.st, 0);
    row.status = ParseTasMovieValidationStatus(ColumnText(st.st, 1));
    row.validation_attempt_id = sqlite3_column_int64(st.st, 2);
    row.updated_at_utc = ColumnTime(st.st, 3);
    return row;
}

std::optional<TasMovieCheckpointSterilizationRequestRecord>
SqliteAnalysisDb::GetTasMovieCheckpointSterilizationRequest(
    std::int64_t request_id) const {
    if (db_ == nullptr || request_id <= 0) return std::nullopt;
    Statement st;
    constexpr const char* kSql =
        "SELECT sterilization_request_id,materialization_key,workflow_instance_id,workflow_step_id,"
        "source_savestate_id,source_savestate_artifact_id,source_savestate_sha256,"
        "source_dtm_artifact_id,source_dtm_sha256,reused_savestate_id,"
        "full_phase_program_kind,full_phase_program_version,full_phase_canonical_id,"
        "full_phase_contract_revision,full_phase_sha256,module_canonical_id,module_revision,"
        "module_sha256,created_at_utc FROM tmv_checkpoint_sterilization_request "
        "WHERE sterilization_request_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK)
        return std::nullopt;
    sqlite3_bind_int64(st.st, 1, request_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    return ReadTasMovieCheckpointSterilizationRequest(st.st);
}

std::optional<TasMovieCheckpointSterilizationRequestRecord>
SqliteAnalysisDb::GetTasMovieCheckpointSterilizationRequestForWorkflowStep(
    std::int64_t workflow_step_id) const {
    if (db_ == nullptr || workflow_step_id <= 0) return std::nullopt;
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT sterilization_request_id FROM tmv_checkpoint_sterilization_request "
            "WHERE workflow_step_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, workflow_step_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    return GetTasMovieCheckpointSterilizationRequest(sqlite3_column_int64(st.st, 0));
}

bool SqliteAnalysisDb::CreateTasMovieCheckpointSterilizationRequest(
    const CreateTasMovieCheckpointSterilizationRequestCommand& command,
    std::int64_t* request_id_out,
    std::string* error_out) {
    if (db_ == nullptr || command.materialization_key.empty()
        || command.workflow_instance_id <= 0 || command.workflow_step_id <= 0
        || command.source_savestate_id <= 0
        || command.source_savestate_artifact_id <= 0
        || !IsLowerHexSha256(command.source_savestate_sha256)
        || command.source_dtm_artifact_id <= 0
        || !IsLowerHexSha256(command.source_dtm_sha256)
        || (command.reused_savestate_id && *command.reused_savestate_id <= 0)
        || command.full_phase_program_kind != 11
        || command.full_phase_program_version <= 0
        || command.full_phase_canonical_id.empty()
        || command.full_phase_contract_revision <= 0
        || !IsLowerHexSha256(command.full_phase_sha256)
        || command.module_canonical_id.empty() || command.module_revision <= 0
        || !IsLowerHexSha256(command.module_sha256)) {
        if (error_out)
            *error_out = "invalid immutable TAS movie checkpoint sterilization request";
        return false;
    }
    if (const auto existing =
            GetTasMovieCheckpointSterilizationRequestForWorkflowStep(
                command.workflow_step_id);
        existing.has_value()) {
        if (!TasMovieCheckpointSterilizationRequestIdentityMatches(*existing, command)) {
            if (error_out)
                *error_out = "workflow step already has a different TAS movie checkpoint sterilization request";
            return false;
        }
        if (request_id_out)
            *request_id_out = existing->sterilization_request_id;
        return true;
    }

    Statement st;
    constexpr const char* kSql =
        "INSERT INTO tmv_checkpoint_sterilization_request("
        "materialization_key,workflow_instance_id,workflow_step_id,source_savestate_id,"
        "source_savestate_artifact_id,source_savestate_sha256,source_dtm_artifact_id,"
        "source_dtm_sha256,reused_savestate_id,full_phase_program_kind,"
        "full_phase_program_version,full_phase_canonical_id,full_phase_contract_revision,"
        "full_phase_sha256,module_canonical_id,module_revision,module_sha256,created_at_utc) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18);";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(st.st, 1, command.materialization_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 2, command.workflow_instance_id);
    sqlite3_bind_int64(st.st, 3, command.workflow_step_id);
    sqlite3_bind_int64(st.st, 4, command.source_savestate_id);
    sqlite3_bind_int64(st.st, 5, command.source_savestate_artifact_id);
    sqlite3_bind_text(st.st, 6, command.source_savestate_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 7, command.source_dtm_artifact_id);
    sqlite3_bind_text(st.st, 8, command.source_dtm_sha256.c_str(), -1, SQLITE_TRANSIENT);
    if (command.reused_savestate_id)
        sqlite3_bind_int64(st.st, 9, *command.reused_savestate_id);
    else
        sqlite3_bind_null(st.st, 9);
    sqlite3_bind_int64(st.st, 10, command.full_phase_program_kind);
    sqlite3_bind_int64(st.st, 11, command.full_phase_program_version);
    sqlite3_bind_text(st.st, 12, command.full_phase_canonical_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 13, command.full_phase_contract_revision);
    sqlite3_bind_text(st.st, 14, command.full_phase_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 15, command.module_canonical_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 16, command.module_revision);
    sqlite3_bind_text(st.st, 17, command.module_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 18, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (request_id_out) *request_id_out = sqlite3_last_insert_rowid(db_);
    return true;
}

std::optional<TasMovieCheckpointSterilizationAttemptRecord>
SqliteAnalysisDb::GetTasMovieCheckpointSterilizationAttempt(
    std::int64_t attempt_id) const {
    if (db_ == nullptr || attempt_id <= 0) return std::nullopt;
    Statement st;
    constexpr const char* kSql =
        "SELECT sterilization_attempt_id,sterilization_request_id,source_job_id,"
        "worker_terminal_sha256,candidate_savestate_sha256,produced_savestate_id,worker_id,"
        "worker_process_generation,workset_epoch,recorded_at_utc "
        "FROM tmv_checkpoint_sterilization_attempt WHERE sterilization_attempt_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK)
        return std::nullopt;
    sqlite3_bind_int64(st.st, 1, attempt_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    return ReadTasMovieCheckpointSterilizationAttempt(st.st);
}

std::optional<TasMovieCheckpointSterilizationAttemptRecord>
SqliteAnalysisDb::FindTasMovieCheckpointSterilizationAttempt(
    std::int64_t source_job_id,
    std::string_view worker_terminal_sha256) const {
    if (db_ == nullptr || source_job_id <= 0
        || !IsLowerHexSha256(worker_terminal_sha256)) {
        return std::nullopt;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT sterilization_attempt_id FROM tmv_checkpoint_sterilization_attempt "
            "WHERE source_job_id=?1 AND worker_terminal_sha256=?2;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, source_job_id);
    sqlite3_bind_text(st.st, 2, worker_terminal_sha256.data(),
        static_cast<int>(worker_terminal_sha256.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    return GetTasMovieCheckpointSterilizationAttempt(sqlite3_column_int64(st.st, 0));
}

bool SqliteAnalysisDb::RecordTasMovieCheckpointSterilizationAttempt(
    const RecordTasMovieCheckpointSterilizationAttemptCommand& command,
    std::int64_t* attempt_id_out,
    std::string* error_out) {
    constexpr auto kI64Max =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (db_ == nullptr || command.sterilization_request_id <= 0
        || command.source_job_id <= 0
        || !IsLowerHexSha256(command.worker_terminal_sha256)
        || !IsLowerHexSha256(command.candidate_savestate_sha256)
        || command.produced_savestate_id <= 0 || command.worker_id.empty()
        || command.worker_process_generation > kI64Max
        || command.workset_epoch == 0 || command.workset_epoch > kI64Max
        || !GetTasMovieCheckpointSterilizationRequest(
                command.sterilization_request_id)
                .has_value()) {
        if (error_out)
            *error_out = "invalid immutable TAS movie checkpoint sterilization attempt";
        return false;
    }
    if (const auto existing = FindTasMovieCheckpointSterilizationAttempt(
            command.source_job_id, command.worker_terminal_sha256);
        existing.has_value()) {
        if (!TasMovieCheckpointSterilizationAttemptIdentityMatches(*existing, command)) {
            if (error_out)
                *error_out = "worker terminal already identifies a different TAS movie checkpoint sterilization attempt";
            return false;
        }
        if (attempt_id_out) *attempt_id_out = existing->sterilization_attempt_id;
        return true;
    }

    Statement st;
    constexpr const char* kSql =
        "INSERT INTO tmv_checkpoint_sterilization_attempt(sterilization_request_id,"
        "source_job_id,worker_terminal_sha256,candidate_savestate_sha256,"
        "produced_savestate_id,worker_id,worker_process_generation,workset_epoch,recorded_at_utc) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, command.sterilization_request_id);
    sqlite3_bind_int64(st.st, 2, command.source_job_id);
    sqlite3_bind_text(st.st, 3, command.worker_terminal_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 4, command.candidate_savestate_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 5, command.produced_savestate_id);
    sqlite3_bind_text(st.st, 6, command.worker_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 7, static_cast<std::int64_t>(command.worker_process_generation));
    sqlite3_bind_int64(st.st, 8, static_cast<std::int64_t>(command.workset_epoch));
    sqlite3_bind_int64(st.st, 9, command.recorded_at_utc.time_since_epoch().count());
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (attempt_id_out) *attempt_id_out = sqlite3_last_insert_rowid(db_);
    return true;
}

std::optional<std::int64_t> SqliteAnalysisDb::LookupSeedProbeRunSavestateId(std::int64_t probe_run_id) const {
    if (db_ == nullptr || probe_run_id <= 0) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT entry_savestate_id FROM sp_probe_run WHERE probe_run_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, probe_run_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    return sqlite3_column_int64(st.st, 0);
}

std::optional<SeedProbeResultRow> SqliteAnalysisDb::GetSeedProbeResult(
    std::int64_t probe_result_id) const {
    if (db_ == nullptr || probe_result_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT probe_result_id,probe_run_id,input_frame_id,source_job_id,seed_value,"
            "origin_worker_id,origin_process_generation,origin_workset_epoch,terminal_sha256,"
            "confirmation_of_probe_result_id,evidence_state,recorded_at_utc "
            "FROM sp_probe_result WHERE probe_result_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, probe_result_id);
    return sqlite3_step(st.st) == SQLITE_ROW
        ? std::optional<SeedProbeResultRow>{ReadSeedProbeResultRow(st.st)}
        : std::nullopt;
}

std::optional<SeedProbeResultRow> SqliteAnalysisDb::GetSeedProbeResultForSourceJob(
    std::int64_t source_job_id) const {
    if (db_ == nullptr || source_job_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT probe_result_id,probe_run_id,input_frame_id,source_job_id,seed_value,"
            "origin_worker_id,origin_process_generation,origin_workset_epoch,terminal_sha256,"
            "confirmation_of_probe_result_id,evidence_state,recorded_at_utc "
            "FROM sp_probe_result WHERE source_job_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, source_job_id);
    return sqlite3_step(st.st) == SQLITE_ROW
        ? std::optional<SeedProbeResultRow>{ReadSeedProbeResultRow(st.st)}
        : std::nullopt;
}

std::vector<SeedProbeResultRow> SqliteAnalysisDb::ListSeedProbeResults(
    std::int64_t probe_run_id) const {
    std::vector<SeedProbeResultRow> rows;
    if (db_ == nullptr || probe_run_id <= 0) {
        return rows;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT probe_result_id,probe_run_id,input_frame_id,source_job_id,seed_value,"
            "origin_worker_id,origin_process_generation,origin_workset_epoch,terminal_sha256,"
            "confirmation_of_probe_result_id,evidence_state,recorded_at_utc "
            "FROM sp_probe_result WHERE probe_run_id=?1 ORDER BY probe_result_id ASC;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, probe_run_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        rows.push_back(ReadSeedProbeResultRow(st.st));
    }
    return rows;
}

std::optional<SeedProbeResultRow> SqliteAnalysisDb::FindConfirmedSeedProbeResultForAcceptedInputSetFrame(
    std::int64_t accepted_input_set_id,
    std::int64_t input_frame_id) const {
    if (db_ == nullptr || accepted_input_set_id <= 0 || input_frame_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT r.probe_result_id,r.probe_run_id,r.input_frame_id,r.source_job_id,r.seed_value,"
            "r.origin_worker_id,r.origin_process_generation,r.origin_workset_epoch,r.terminal_sha256,"
            "r.confirmation_of_probe_result_id,r.evidence_state,r.recorded_at_utc "
            "FROM sp_probe_run pr "
            "JOIN sp_probe_result r ON r.probe_run_id=pr.probe_run_id "
            "JOIN an_input_set_frame accepted "
            "ON accepted.input_set_id=pr.accepted_input_set_id "
            "AND accepted.input_frame_id=r.input_frame_id "
            "WHERE pr.accepted_input_set_id=?1 AND r.input_frame_id=?2 "
            "AND r.evidence_state='CONFIRMED' AND r.confirmation_of_probe_result_id IS NULL "
            "ORDER BY r.probe_result_id ASC "
            "LIMIT 1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, accepted_input_set_id);
    sqlite3_bind_int64(st.st, 2, input_frame_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return ReadSeedProbeResultRow(st.st);
}

std::optional<AnalysisInputSetFrameRow> SqliteAnalysisDb::GetAnalysisInputFrame(std::int64_t input_frame_id) const {
    if (db_ == nullptr || input_frame_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT f.input_frame_id,m.x,m.y,c.x,c.y,t.x,t.y "
            "FROM sp_input_frame f "
            "JOIN sp_axis_xy m ON m.axis_xy_id=f.main_axis_xy_id "
            "JOIN sp_axis_xy c ON c.axis_xy_id=f.cstick_axis_xy_id "
            "JOIN sp_axis_xy t ON t.axis_xy_id=f.trigger_axis_xy_id "
            "WHERE f.input_frame_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, input_frame_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return AnalysisInputSetFrameRow{
        .input_frame_id = sqlite3_column_int64(st.st, 0),
        .ordinal = 0,
        .main_x = sqlite3_column_int(st.st, 1),
        .main_y = sqlite3_column_int(st.st, 2),
        .cstick_x = sqlite3_column_int(st.st, 3),
        .cstick_y = sqlite3_column_int(st.st, 4),
        .trigger_x = sqlite3_column_int(st.st, 5),
        .trigger_y = sqlite3_column_int(st.st, 6),
    };
}

std::vector<AnalysisInputSetFrameRow> SqliteAnalysisDb::ListAnalysisInputSetFrames(std::int64_t input_set_id) const {
    std::vector<AnalysisInputSetFrameRow> rows;
    if (db_ == nullptr || input_set_id <= 0) {
        return rows;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT sf.input_frame_id,sf.ordinal,m.x,m.y,c.x,c.y,t.x,t.y "
            "FROM an_input_set_frame sf "
            "JOIN sp_input_frame f ON f.input_frame_id=sf.input_frame_id "
            "JOIN sp_axis_xy m ON m.axis_xy_id=f.main_axis_xy_id "
            "JOIN sp_axis_xy c ON c.axis_xy_id=f.cstick_axis_xy_id "
            "JOIN sp_axis_xy t ON t.axis_xy_id=f.trigger_axis_xy_id "
            "WHERE sf.input_set_id=?1 "
            "ORDER BY sf.ordinal ASC;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, input_set_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        rows.push_back(AnalysisInputSetFrameRow{
            .input_frame_id = sqlite3_column_int64(st.st, 0),
            .ordinal = sqlite3_column_int(st.st, 1),
            .main_x = sqlite3_column_int(st.st, 2),
            .main_y = sqlite3_column_int(st.st, 3),
            .cstick_x = sqlite3_column_int(st.st, 4),
            .cstick_y = sqlite3_column_int(st.st, 5),
            .trigger_x = sqlite3_column_int(st.st, 6),
            .trigger_y = sqlite3_column_int(st.st, 7),
        });
    }
    return rows;
}

bool SqliteAnalysisDb::EnsureSeedProbeInputFrame(
    std::int64_t main_axis_xy_id,
    std::int64_t cstick_axis_xy_id,
    std::int64_t trigger_axis_xy_id,
    std::int64_t* input_frame_id_out,
    std::string* error_out) {
    if (input_frame_id_out != nullptr) {
        *input_frame_id_out = 0;
    }
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (main_axis_xy_id < 0 || main_axis_xy_id > 0xffff
        || cstick_axis_xy_id < 0 || cstick_axis_xy_id > 0xffff
        || trigger_axis_xy_id < 0 || trigger_axis_xy_id > 0xffff) {
        if (error_out) *error_out = "axis ids must be packed uint16 x/y values";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    const auto rollback = [&]() {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    };

    const auto ensure_axis = [&](std::int64_t axis_xy_id, std::string* axis_error) -> bool {
        const auto axis_x = static_cast<int>((static_cast<std::uint64_t>(axis_xy_id) >> 8) & 0xff);
        const auto axis_y = static_cast<int>(static_cast<std::uint64_t>(axis_xy_id) & 0xff);

        Statement st;
        if (sqlite3_prepare_v2(
                db_,
                "INSERT OR IGNORE INTO sp_axis_xy(axis_xy_id,x,y) VALUES(?1,?2,?3);",
                -1,
                &st.st,
                nullptr)
            != SQLITE_OK) {
            if (axis_error) *axis_error = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(st.st, 1, axis_xy_id);
        sqlite3_bind_int(st.st, 2, axis_x);
        sqlite3_bind_int(st.st, 3, axis_y);
        if (sqlite3_step(st.st) != SQLITE_DONE) {
            if (axis_error) *axis_error = sqlite3_errmsg(db_);
            return false;
        }
        return true;
    };

    if (!ensure_axis(main_axis_xy_id, error_out)
        || !ensure_axis(cstick_axis_xy_id, error_out)
        || !ensure_axis(trigger_axis_xy_id, error_out)) {
        rollback();
        return false;
    }

    Statement insert_frame;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT OR IGNORE INTO sp_input_frame(main_axis_xy_id,cstick_axis_xy_id,trigger_axis_xy_id) "
            "VALUES(?1,?2,?3);",
            -1,
            &insert_frame.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }
    sqlite3_bind_int64(insert_frame.st, 1, main_axis_xy_id);
    sqlite3_bind_int64(insert_frame.st, 2, cstick_axis_xy_id);
    sqlite3_bind_int64(insert_frame.st, 3, trigger_axis_xy_id);
    if (sqlite3_step(insert_frame.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }

    Statement select_frame;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT input_frame_id FROM sp_input_frame "
            "WHERE main_axis_xy_id=?1 AND cstick_axis_xy_id=?2 AND trigger_axis_xy_id=?3 "
            "LIMIT 1;",
            -1,
            &select_frame.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }
    sqlite3_bind_int64(select_frame.st, 1, main_axis_xy_id);
    sqlite3_bind_int64(select_frame.st, 2, cstick_axis_xy_id);
    sqlite3_bind_int64(select_frame.st, 3, trigger_axis_xy_id);
    if (sqlite3_step(select_frame.st) != SQLITE_ROW) {
        if (error_out) *error_out = "input frame could not be resolved";
        rollback();
        return false;
    }

    const auto input_frame_id = sqlite3_column_int64(select_frame.st, 0);
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }

    if (input_frame_id_out != nullptr) {
        *input_frame_id_out = input_frame_id;
    }
    return true;
}

bool SqliteAnalysisDb::RecordSeedProbeObservation(
    const RecordSeedProbeObservationCommand& command,
    RecordSeedProbeObservationReceipt* receipt_out,
    std::string* error_out) {
    if (receipt_out) *receipt_out = {};
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.probe_run_id <= 0
        || command.input_frame_id <= 0
        || command.source_job_id <= 0
        || command.endpoint == SeedProbeEndpoint::Unknown
        || command.recorded_at_utc.time_since_epoch().count() <= 0
        || command.origin_worker_id > static_cast<std::uint64_t>(
            (std::numeric_limits<std::int64_t>::max)())
        || command.origin_process_generation == 0
        || command.origin_process_generation > static_cast<std::uint64_t>(
            (std::numeric_limits<std::int64_t>::max)())
        || command.origin_workset_epoch == 0
        || command.origin_workset_epoch > static_cast<std::uint64_t>(
            (std::numeric_limits<std::int64_t>::max)())
        || !IsLowerHexSha256(command.terminal_sha256)
        || (command.confirmation_of_probe_result_id.has_value()
            && *command.confirmation_of_probe_result_id <= 0)) {
        if (error_out) *error_out =
            "invalid factual SeedProbe observation command";
        return false;
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const auto rollback = [&]() {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    };
    const auto fail = [&](std::string message) {
        if (error_out) *error_out = std::move(message);
        rollback();
        return false;
    };

    RecordSeedProbeObservationReceipt receipt{};
    std::optional<SeedProbeResultRow> observation;
    bool emit_run_failed = false;
    {
        Statement existing;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT probe_result_id,probe_run_id,input_frame_id,source_job_id,seed_value,"
                "origin_worker_id,origin_process_generation,origin_workset_epoch,terminal_sha256,"
                "confirmation_of_probe_result_id,evidence_state,recorded_at_utc "
                "FROM sp_probe_result WHERE source_job_id=?1;",
                -1, &existing.st, nullptr) != SQLITE_OK) {
            return fail(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(existing.st, 1, command.source_job_id);
        const auto rc = sqlite3_step(existing.st);
        if (rc == SQLITE_ROW) {
            observation = ReadSeedProbeResultRow(existing.st);
            const auto& row = *observation;
            const bool same = row.probe_run_id == command.probe_run_id
                && row.input_frame_id == command.input_frame_id
                && row.source_job_id == command.source_job_id
                && row.seed_value == command.seed_value
                && row.origin_worker_id == command.origin_worker_id
                && row.origin_process_generation
                    == command.origin_process_generation
                && row.origin_workset_epoch == command.origin_workset_epoch
                && row.terminal_sha256 == command.terminal_sha256
                && row.confirmation_of_probe_result_id
                    == command.confirmation_of_probe_result_id;
            if (!same) {
                return fail(
                    "source job already has a different immutable SeedProbe observation");
            }
        } else if (rc != SQLITE_DONE) {
            return fail(sqlite3_errmsg(db_));
        }
    }

    Statement current_run;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT status,established_endpoint,conflicting_endpoint "
            "FROM sp_probe_run WHERE probe_run_id=?1;",
            -1, &current_run.st, nullptr) != SQLITE_OK) {
        return fail(sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(current_run.st, 1, command.probe_run_id);
    if (sqlite3_step(current_run.st) != SQLITE_ROW) {
        return fail("SeedProbe run does not exist");
    }
    const auto run_status = ParseSeedProbeRunStatus(
        ColumnText(current_run.st, 0));
    auto established = ParseSeedProbeEndpoint(
        ColumnText(current_run.st, 1));
    const auto prior_conflict = ParseSeedProbeEndpoint(
        ColumnText(current_run.st, 2));

    if (established == SeedProbeEndpoint::Unknown) {
        Statement establish;
        if (sqlite3_prepare_v2(
                db_,
                "UPDATE sp_probe_run SET established_endpoint=?2,"
                "established_endpoint_source_job_id=?3 "
                "WHERE probe_run_id=?1 AND established_endpoint IS NULL "
                "AND status<>'INVALIDATED';",
                -1, &establish.st, nullptr) != SQLITE_OK) {
            return fail(sqlite3_errmsg(db_));
        }
        const auto endpoint = ToDbString(command.endpoint);
        sqlite3_bind_int64(establish.st, 1, command.probe_run_id);
        sqlite3_bind_text(establish.st, 2, endpoint.data(),
            static_cast<int>(endpoint.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(establish.st, 3, command.source_job_id);
        if (sqlite3_step(establish.st) != SQLITE_DONE
            || sqlite3_changes(db_) != 1) {
            return fail(
                "SeedProbe endpoint establishment changed concurrently");
        }
        established = command.endpoint;
        receipt.endpoint_disposition =
            SeedProbeEndpointObservationDisposition::Established;
    } else if (established == command.endpoint) {
        receipt.endpoint_disposition =
            run_status == SeedProbeRunStatus::Invalidated
            ? SeedProbeEndpointObservationDisposition::
                AlreadyInvalidatedMatching
            : SeedProbeEndpointObservationDisposition::Matched;
    } else if (run_status == SeedProbeRunStatus::Invalidated) {
        receipt.endpoint_disposition =
            SeedProbeEndpointObservationDisposition::
                AlreadyInvalidatedConflicting;
        receipt.conflicting_endpoint =
            prior_conflict == SeedProbeEndpoint::Unknown
            ? std::optional<SeedProbeEndpoint>(command.endpoint)
            : std::optional<SeedProbeEndpoint>(prior_conflict);
    } else {
        Statement invalidate;
        if (sqlite3_prepare_v2(
                db_,
                "UPDATE sp_probe_run SET status='INVALIDATED',"
                "conflicting_endpoint=?2,"
                "conflicting_endpoint_source_job_id=?3,"
                "invalidation_diagnostic=?4,invalidated_at_utc=?5,"
                "completed_at_utc=?5 "
                "WHERE probe_run_id=?1 AND status<>'INVALIDATED' "
                "AND established_endpoint=?6;",
                -1, &invalidate.st, nullptr) != SQLITE_OK) {
            return fail(sqlite3_errmsg(db_));
        }
        const auto conflict = ToDbString(command.endpoint);
        const auto expected = ToDbString(established);
        const auto recorded_at =
            command.recorded_at_utc.time_since_epoch().count();
        sqlite3_bind_int64(invalidate.st, 1, command.probe_run_id);
        sqlite3_bind_text(invalidate.st, 2, conflict.data(),
            static_cast<int>(conflict.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(invalidate.st, 3, command.source_job_id);
        sqlite3_bind_text(invalidate.st, 4,
            command.endpoint_mismatch_diagnostic.c_str(), -1,
            SQLITE_TRANSIENT);
        sqlite3_bind_int64(invalidate.st, 5, recorded_at);
        sqlite3_bind_text(invalidate.st, 6, expected.data(),
            static_cast<int>(expected.size()), SQLITE_TRANSIENT);
        if (sqlite3_step(invalidate.st) != SQLITE_DONE
            || sqlite3_changes(db_) != 1) {
            return fail(
                "SeedProbe endpoint invalidation changed concurrently");
        }
        receipt.endpoint_disposition =
            SeedProbeEndpointObservationDisposition::Invalidated;
        receipt.conflicting_endpoint = command.endpoint;
        receipt.run_invalidated = true;
        emit_run_failed = true;
    }
    receipt.established_endpoint = established;
    if (!receipt.conflicting_endpoint.has_value()
        && prior_conflict != SeedProbeEndpoint::Unknown) {
        receipt.conflicting_endpoint = prior_conflict;
    }

    if (!observation.has_value()) {
        if (command.confirmation_of_probe_result_id.has_value()) {
            Statement provisional;
            if (sqlite3_prepare_v2(
                    db_,
                    "SELECT probe_run_id,input_frame_id,source_job_id,origin_worker_id,"
                    "origin_process_generation,origin_workset_epoch,confirmation_of_probe_result_id "
                    "FROM sp_probe_result WHERE probe_result_id=?1;",
                    -1, &provisional.st, nullptr) != SQLITE_OK) {
                return fail(sqlite3_errmsg(db_));
            }
            sqlite3_bind_int64(
                provisional.st, 1,
                *command.confirmation_of_probe_result_id);
            if (sqlite3_step(provisional.st) != SQLITE_ROW
                || sqlite3_column_int64(provisional.st, 0)
                    != command.probe_run_id
                || sqlite3_column_int64(provisional.st, 1)
                    != command.input_frame_id
                || sqlite3_column_int64(provisional.st, 2)
                    == command.source_job_id
                || (static_cast<std::uint64_t>(
                        sqlite3_column_int64(provisional.st, 3))
                        == command.origin_worker_id
                    && static_cast<std::uint64_t>(
                        sqlite3_column_int64(provisional.st, 4))
                        == command.origin_process_generation
                    && static_cast<std::uint64_t>(
                        sqlite3_column_int64(provisional.st, 5))
                        == command.origin_workset_epoch)
                || sqlite3_column_type(provisional.st, 6)
                    != SQLITE_NULL) {
                return fail(
                    "confirmation must reference a representative from the same run and frame with a distinct job and scoped WorksetEpoch");
            }
        }

        Statement insert_result;
        if (sqlite3_prepare_v2(
                db_,
                "INSERT INTO sp_probe_result("
                "probe_run_id,input_frame_id,source_job_id,seed_value,origin_worker_id,"
                "origin_process_generation,origin_workset_epoch,terminal_sha256,"
                "confirmation_of_probe_result_id,evidence_state,recorded_at_utc) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,'OBSERVED',?10);",
                -1, &insert_result.st, nullptr) != SQLITE_OK) {
            return fail(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(insert_result.st, 1, command.probe_run_id);
        sqlite3_bind_int64(insert_result.st, 2, command.input_frame_id);
        sqlite3_bind_int64(insert_result.st, 3, command.source_job_id);
        sqlite3_bind_int64(insert_result.st, 4,
            static_cast<std::int64_t>(command.seed_value));
        sqlite3_bind_int64(insert_result.st, 5,
            static_cast<std::int64_t>(command.origin_worker_id));
        sqlite3_bind_int64(insert_result.st, 6,
            static_cast<std::int64_t>(command.origin_process_generation));
        sqlite3_bind_int64(insert_result.st, 7,
            static_cast<std::int64_t>(command.origin_workset_epoch));
        sqlite3_bind_text(insert_result.st, 8,
            command.terminal_sha256.c_str(), -1, SQLITE_TRANSIENT);
        if (command.confirmation_of_probe_result_id.has_value()) {
            sqlite3_bind_int64(insert_result.st, 9,
                *command.confirmation_of_probe_result_id);
        } else {
            sqlite3_bind_null(insert_result.st, 9);
        }
        sqlite3_bind_int64(insert_result.st, 10,
            command.recorded_at_utc.time_since_epoch().count());
        if (sqlite3_step(insert_result.st) != SQLITE_DONE) {
            return fail(sqlite3_errmsg(db_));
        }
        const auto result_id = sqlite3_last_insert_rowid(db_);
        receipt.inserted = true;
        if (!InsertSeedProbeOutboxEvent(
                db_, "AnalysisSeedProbe.ObservationRecorded.v1",
                "probe_run", std::to_string(command.probe_run_id),
                command.correlation_id, command.causation_id,
                command.recorded_at_utc.time_since_epoch().count(),
                "probe_result", result_id, error_out)) {
            rollback();
            return false;
        }

        Statement inserted;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT probe_result_id,probe_run_id,input_frame_id,source_job_id,seed_value,"
                "origin_worker_id,origin_process_generation,origin_workset_epoch,terminal_sha256,"
                "confirmation_of_probe_result_id,evidence_state,recorded_at_utc "
                "FROM sp_probe_result WHERE probe_result_id=?1;",
                -1, &inserted.st, nullptr) != SQLITE_OK) {
            return fail(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(inserted.st, 1, result_id);
        if (sqlite3_step(inserted.st) != SQLITE_ROW) {
            return fail("inserted SeedProbe observation could not be read");
        }
        observation = ReadSeedProbeResultRow(inserted.st);
    }

    receipt.observation = *observation;
    if (emit_run_failed
        && !InsertSeedProbeOutboxEvent(
            db_, "AnalysisSeedProbe.RunFailed.v1", "probe_run",
            std::to_string(command.probe_run_id),
            command.correlation_id, command.causation_id,
            command.recorded_at_utc.time_since_epoch().count(),
            "probe_run", command.probe_run_id, error_out)) {
        rollback();
        return false;
    }
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr)
        != SQLITE_OK) {
        return fail(sqlite3_errmsg(db_));
    }
    if (receipt_out) *receipt_out = std::move(receipt);
    if (error_out) error_out->clear();
    return true;
}

bool SqliteAnalysisDb::TransitionSeedProbeEvidence(
    const TransitionSeedProbeEvidenceCommand& command,
    bool* changed_out,
    std::string* error_out) {
    if (changed_out) *changed_out = false;
    if (db_ == nullptr
        || command.probe_result_id <= 0
        || command.expected_state == SeedProbeEvidenceState::Unknown
        || command.new_state == SeedProbeEvidenceState::Unknown) {
        if (error_out) *error_out = "invalid SeedProbe evidence transition";
        return false;
    }
    const bool allowed =
        (command.expected_state == SeedProbeEvidenceState::Observed
            && command.new_state == SeedProbeEvidenceState::Provisional)
        || (command.expected_state == SeedProbeEvidenceState::Provisional
            && (command.new_state == SeedProbeEvidenceState::Confirmed
                || command.new_state == SeedProbeEvidenceState::Rejected));
    if (!allowed) {
        if (error_out) *error_out = "unsupported SeedProbe evidence transition";
        return false;
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const auto rollback = [&]() {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    };

    Statement current;
        if (sqlite3_prepare_v2(
            db_,
            "SELECT probe_run_id,evidence_state,seed_value,source_job_id,origin_worker_id,"
            "origin_process_generation,origin_workset_epoch,confirmation_of_probe_result_id "
            "FROM sp_probe_result WHERE probe_result_id=?1;",
            -1,
            &current.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }
    sqlite3_bind_int64(current.st, 1, command.probe_result_id);
    if (sqlite3_step(current.st) != SQLITE_ROW) {
        if (error_out) *error_out = "SeedProbe result does not exist";
        rollback();
        return false;
    }
    const auto probe_run_id = sqlite3_column_int64(current.st, 0);
    const auto current_state = ParseSeedProbeEvidenceState(ColumnText(current.st, 1));
    const auto seed_value = sqlite3_column_int64(current.st, 2);
    const auto source_job_id = sqlite3_column_int64(current.st, 3);
    const auto origin_worker_id = sqlite3_column_int64(current.st, 4);
    const auto origin_process_generation = sqlite3_column_int64(current.st, 5);
    const auto origin_workset_epoch = sqlite3_column_int64(current.st, 6);
    if (sqlite3_column_type(current.st, 7) != SQLITE_NULL) {
        if (error_out) *error_out = "confirmation observations cannot become evidence representatives";
        rollback();
        return false;
    }
    if (current_state == command.new_state) {
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            rollback();
            return false;
        }
        return true;
    }
    if (current_state != command.expected_state) {
        if (error_out) *error_out = "SeedProbe evidence state changed concurrently";
        rollback();
        return false;
    }

    if (command.new_state == SeedProbeEvidenceState::Provisional) {
        Statement winner;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT probe_result_id FROM sp_probe_result "
                "WHERE probe_run_id=?1 AND seed_value=?2 "
                "AND evidence_state IN ('PROVISIONAL','CONFIRMED') LIMIT 1;",
                -1,
                &winner.st,
                nullptr)
            != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            rollback();
            return false;
        }
        sqlite3_bind_int64(winner.st, 1, probe_run_id);
        sqlite3_bind_int64(winner.st, 2, seed_value);
        if (sqlite3_step(winner.st) == SQLITE_ROW
            && sqlite3_column_int64(winner.st, 0) != command.probe_result_id) {
            if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
                if (error_out) *error_out = sqlite3_errmsg(db_);
                rollback();
                return false;
            }
            return true;
        }
    }

    if (command.new_state == SeedProbeEvidenceState::Confirmed) {
        Statement confirmation;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT 1 FROM sp_probe_result "
                "WHERE confirmation_of_probe_result_id=?1 AND seed_value=?2 "
                "AND source_job_id<>?3 "
                "AND (origin_worker_id<>?4 OR origin_process_generation<>?5 OR origin_workset_epoch<>?6) "
                "LIMIT 1;",
                -1,
                &confirmation.st,
                nullptr)
            != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            rollback();
            return false;
        }
        sqlite3_bind_int64(confirmation.st, 1, command.probe_result_id);
        sqlite3_bind_int64(confirmation.st, 2, seed_value);
        sqlite3_bind_int64(confirmation.st, 3, source_job_id);
        sqlite3_bind_int64(confirmation.st, 4, origin_worker_id);
        sqlite3_bind_int64(confirmation.st, 5, origin_process_generation);
        sqlite3_bind_int64(confirmation.st, 6, origin_workset_epoch);
        if (sqlite3_step(confirmation.st) != SQLITE_ROW) {
            if (error_out) *error_out = "confirmation requires an equal observation from a distinct job and scoped WorksetEpoch";
            rollback();
            return false;
        }
    }

    Statement update;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE sp_probe_result SET evidence_state=?2 "
            "WHERE probe_result_id=?1 AND evidence_state=?3;",
            -1,
            &update.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }
    const auto expected = ToDbString(command.expected_state);
    const auto desired = ToDbString(command.new_state);
    sqlite3_bind_int64(update.st, 1, command.probe_result_id);
    sqlite3_bind_text(update.st, 2, desired.data(), static_cast<int>(desired.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(update.st, 3, expected.data(), static_cast<int>(expected.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(update.st) != SQLITE_DONE || sqlite3_changes(db_) != 1) {
        if (error_out) *error_out = "SeedProbe evidence state changed concurrently";
        rollback();
        return false;
    }

    if (!InsertSeedProbeOutboxEvent(
            db_,
            "AnalysisSeedProbe.EvidenceStateChanged.v1",
            "probe_run",
            std::to_string(probe_run_id),
            command.correlation_id,
            command.causation_id,
            command.changed_at_utc.time_since_epoch().count(),
            "probe_result",
            command.probe_result_id,
            error_out)) {
        rollback();
        return false;
    }
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }
    if (changed_out) *changed_out = true;
    return true;
}

bool SqliteAnalysisDb::CreateSeedProbeSet(
    const CreateSeedProbeSetCommand& command,
    std::int64_t* probe_set_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.name.empty()
        || command.probe_flavor.empty()
        || command.breakpoint_policy_name.empty()
        || command.segment_source_kind.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement find_existing;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT probe_set_id,probe_flavor,breakpoint_policy_name,dungeon_segment_file_num,"
            "dungeon_segment_file_letter,dungeon_segment_code,segment_source_kind "
            "FROM sp_probe_set WHERE name=?1 LIMIT 1;",
            -1,
            &find_existing.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(find_existing.st, 1, command.name.c_str(), -1, SQLITE_TRANSIENT);
    const auto existing_step = sqlite3_step(find_existing.st);
    if (existing_step == SQLITE_ROW) {
        const auto read_text = [&](int column) {
            const auto* text = sqlite3_column_text(find_existing.st, column);
            return text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
        };
        const auto read_optional_text = [&](int column) -> std::optional<std::string> {
            if (sqlite3_column_type(find_existing.st, column) == SQLITE_NULL) return std::nullopt;
            return read_text(column);
        };
        const auto read_optional_int64 = [&](int column) -> std::optional<std::int64_t> {
            if (sqlite3_column_type(find_existing.st, column) == SQLITE_NULL) return std::nullopt;
            return sqlite3_column_int64(find_existing.st, column);
        };
        const auto existing_id = sqlite3_column_int64(find_existing.st, 0);
        const bool exact_definition_match =
            read_text(1) == command.probe_flavor
            && read_text(2) == command.breakpoint_policy_name
            && read_optional_int64(3) == command.dungeon_segment_file_num
            && read_optional_text(4) == command.dungeon_segment_file_letter
            && read_optional_text(5) == command.dungeon_segment_code
            && read_text(6) == command.segment_source_kind;
        if (!exact_definition_match) {
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            if (error_out) {
                *error_out = "seed probe set name already exists with a different definition";
            }
            return false;
        }
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        if (probe_set_id_out) *probe_set_id_out = existing_id;
        return true;
    }
    if (existing_step != SQLITE_DONE) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    Statement insert_set;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO sp_probe_set(name,probe_flavor,breakpoint_policy_name,dungeon_segment_file_num,dungeon_segment_file_letter,dungeon_segment_code,segment_source_kind,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8);",
            -1,
            &insert_set.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_text(insert_set.st, 1, command.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_set.st, 2, command.probe_flavor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_set.st, 3, command.breakpoint_policy_name.c_str(), -1, SQLITE_TRANSIENT);
    if (command.dungeon_segment_file_num.has_value()) sqlite3_bind_int64(insert_set.st, 4, command.dungeon_segment_file_num.value());
    else sqlite3_bind_null(insert_set.st, 4);
    if (command.dungeon_segment_file_letter.has_value()) sqlite3_bind_text(insert_set.st, 5, command.dungeon_segment_file_letter->c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(insert_set.st, 5);
    if (command.dungeon_segment_code.has_value()) sqlite3_bind_text(insert_set.st, 6, command.dungeon_segment_code->c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(insert_set.st, 6);
    sqlite3_bind_text(insert_set.st, 7, command.segment_source_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_set.st, 8, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert_set.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto probe_set_id = sqlite3_last_insert_rowid(db_);
    if (!InsertSeedProbeOutboxEvent(
            db_,
            "AnalysisSeedProbe.SetCreated.v1",
            "probe_set",
            std::to_string(probe_set_id),
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "probe_set",
            probe_set_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (probe_set_id_out) {
        *probe_set_id_out = probe_set_id;
    }
    return true;
}

bool SqliteAnalysisDb::RequestSeedProbeRun(
    const RequestSeedProbeRunCommand& command,
    std::int64_t* probe_run_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.materialization_key.empty()
        || command.probe_set_id <= 0
        || command.entry_savestate_id <= 0
        || command.seed_probe_spec_id <= 0
        || command.launch_samples_per_axis <= 0
        || command.codec_version <= 0
        || command.status != SeedProbeRunStatus::Survey) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement existing;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT probe_run_id,probe_set_id,entry_savestate_id,"
            "seed_probe_spec_id,launch_samples_per_axis,codec_version "
            "FROM sp_probe_run WHERE materialization_key=?1;",
            -1,
            &existing.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(
            db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_bind_text(
        existing.st,
        1,
        command.materialization_key.c_str(),
        -1,
        SQLITE_TRANSIENT);
    const auto existing_rc = sqlite3_step(existing.st);
    if (existing_rc == SQLITE_ROW) {
        const auto existing_id =
            sqlite3_column_int64(existing.st, 0);
        const bool matches =
            sqlite3_column_int64(existing.st, 1)
                    == command.probe_set_id
            && sqlite3_column_int64(existing.st, 2)
                    == command.entry_savestate_id
            && sqlite3_column_int64(existing.st, 3)
                    == command.seed_probe_spec_id
            && sqlite3_column_int(existing.st, 4)
                    == command.launch_samples_per_axis
            && sqlite3_column_int(existing.st, 5)
                    == command.codec_version;
        if (!matches) {
            if (error_out) {
                *error_out =
                    "SeedProbe materialization key conflicts with "
                    "different run facts";
            }
            (void)sqlite3_exec(
                db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        if (sqlite3_exec(
                db_, "COMMIT;", nullptr, nullptr, nullptr)
            != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            (void)sqlite3_exec(
                db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        if (probe_run_id_out) {
            *probe_run_id_out = existing_id;
        }
        if (error_out) {
            error_out->clear();
        }
        return true;
    }
    if (existing_rc != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(
            db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto accepted_input_set_id = CreateAnalysisInputSetForPendingProbeRun(
        db_,
        command.requested_at_utc.time_since_epoch().count(),
        error_out);
    if (!accepted_input_set_id.has_value()) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    Statement insert_run;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO sp_probe_run(materialization_key,probe_set_id,"
            "entry_savestate_id,seed_probe_spec_id,"
            "launch_samples_per_axis,codec_version,status,"
            "accepted_input_set_id,requested_at_utc,completed_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,NULL);",
            -1,
            &insert_run.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_text(
        insert_run.st,
        1,
        command.materialization_key.c_str(),
        -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_run.st, 2, command.probe_set_id);
    sqlite3_bind_int64(insert_run.st, 3, command.entry_savestate_id);
    sqlite3_bind_int64(insert_run.st, 4, command.seed_probe_spec_id);
    sqlite3_bind_int(insert_run.st, 5, command.launch_samples_per_axis);
    sqlite3_bind_int(insert_run.st, 6, command.codec_version);
    const auto status = ToDbString(command.status);
    sqlite3_bind_text(insert_run.st, 7, status.data(), static_cast<int>(status.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_run.st, 8, *accepted_input_set_id);
    sqlite3_bind_int64(insert_run.st, 9, command.requested_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert_run.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto probe_run_id = sqlite3_last_insert_rowid(db_);
    if (!AttachAnalysisInputSetToProbeRun(db_, *accepted_input_set_id, probe_run_id, error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto aggregate_id = std::to_string(probe_run_id);
    if (!InsertSeedProbeOutboxEvent(
            db_,
            "AnalysisSeedProbe.RunRequested.v1",
            "probe_run",
            aggregate_id,
            command.correlation_id,
            command.causation_id,
            command.requested_at_utc.time_since_epoch().count(),
            "probe_run",
            probe_run_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (probe_run_id_out) {
        *probe_run_id_out = probe_run_id;
    }
    return true;
}

std::optional<SeedProbeRunSnapshot> SqliteAnalysisDb::GetSeedProbeRun(std::int64_t probe_run_id) const {
    if (db_ == nullptr || probe_run_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT r.probe_run_id,r.materialization_key,r.probe_set_id,"
            "s.probe_flavor,r.seed_probe_spec_id,r.entry_savestate_id,"
            "r.launch_samples_per_axis,r.codec_version,r.status,r.accepted_input_set_id,r.requested_at_utc,r.completed_at_utc,"
            "r.established_endpoint,r.established_endpoint_source_job_id,"
            "r.conflicting_endpoint,r.conflicting_endpoint_source_job_id,"
            "r.invalidation_diagnostic,r.invalidated_at_utc "
            "FROM sp_probe_run r JOIN sp_probe_set s ON s.probe_set_id=r.probe_set_id "
            "WHERE r.probe_run_id=?1 LIMIT 1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, probe_run_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    SeedProbeRunSnapshot snapshot{};
    snapshot.probe_run_id = sqlite3_column_int64(st.st, 0);
    snapshot.materialization_key = ColumnText(st.st, 1);
    snapshot.probe_set_id = sqlite3_column_int64(st.st, 2);
    snapshot.probe_flavor = ColumnText(st.st, 3);
    snapshot.seed_probe_spec_id = sqlite3_column_int64(st.st, 4);
    snapshot.entry_savestate_id = sqlite3_column_int64(st.st, 5);
    snapshot.launch_samples_per_axis = sqlite3_column_type(st.st, 6) == SQLITE_NULL ? 0 : sqlite3_column_int(st.st, 6);
    snapshot.codec_version = sqlite3_column_int(st.st, 7);
    snapshot.status = ParseSeedProbeRunStatus(ColumnText(st.st, 8));
    snapshot.accepted_input_set_id = sqlite3_column_int64(st.st, 9);
    snapshot.requested_at_utc = types::UtcTimePoint(std::chrono::milliseconds(sqlite3_column_int64(st.st, 10)));
    if (sqlite3_column_type(st.st, 11) != SQLITE_NULL) {
        snapshot.completed_at_utc = types::UtcTimePoint(std::chrono::milliseconds(sqlite3_column_int64(st.st, 11)));
    }
    snapshot.established_endpoint = ParseSeedProbeEndpoint(
        ColumnText(st.st, 12));
    if (sqlite3_column_type(st.st, 13) != SQLITE_NULL) {
        snapshot.established_endpoint_source_job_id =
            sqlite3_column_int64(st.st, 13);
    }
    if (sqlite3_column_type(st.st, 14) != SQLITE_NULL) {
        snapshot.conflicting_endpoint = ParseSeedProbeEndpoint(
            ColumnText(st.st, 14));
    }
    if (sqlite3_column_type(st.st, 15) != SQLITE_NULL) {
        snapshot.conflicting_endpoint_source_job_id =
            sqlite3_column_int64(st.st, 15);
    }
    if (sqlite3_column_type(st.st, 16) != SQLITE_NULL) {
        snapshot.invalidation_diagnostic = ColumnText(st.st, 16);
    }
    if (sqlite3_column_type(st.st, 17) != SQLITE_NULL) {
        snapshot.invalidated_at_utc = types::UtcTimePoint(
            std::chrono::milliseconds(
                sqlite3_column_int64(st.st, 17)));
    }
    return snapshot;
}

bool SqliteAnalysisDb::UpdateSeedProbeRunStatus(
    const UpdateSeedProbeRunStatusCommand& command,
    bool* changed_out,
    std::string* error_out) {
    if (changed_out) *changed_out = false;
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.probe_run_id <= 0
        || command.expected_status == SeedProbeRunStatus::Unknown
        || command.new_status == SeedProbeRunStatus::Unknown) {
        if (error_out) *error_out = "invalid SeedProbe run status transition";
        return false;
    }
    const bool terminal =
        command.new_status == SeedProbeRunStatus::Completed
        || command.new_status == SeedProbeRunStatus::CompletedPartial
        || command.new_status == SeedProbeRunStatus::Failed;
    const bool allowed =
        (command.expected_status == SeedProbeRunStatus::Survey
            && (command.new_status == SeedProbeRunStatus::Search
                || command.new_status == SeedProbeRunStatus::Confirm
                || command.new_status == SeedProbeRunStatus::Failed))
        || (command.expected_status == SeedProbeRunStatus::Search
            && (command.new_status == SeedProbeRunStatus::Confirm
                || command.new_status == SeedProbeRunStatus::Failed))
        || (command.expected_status == SeedProbeRunStatus::Confirm
            && (command.new_status == SeedProbeRunStatus::Search
                || command.new_status == SeedProbeRunStatus::Completed
                || command.new_status == SeedProbeRunStatus::CompletedPartial
                || command.new_status == SeedProbeRunStatus::Failed));
    if (!allowed) {
        if (error_out) *error_out = "unsupported SeedProbe run status transition";
        return false;
    }
    if (terminal != command.completed_at_utc.has_value()) {
        if (error_out) *error_out = "terminal SeedProbe status requires completed_at_utc and nonterminal status forbids it";
        return false;
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const auto rollback = [&]() {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    };
    Statement current;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT status FROM sp_probe_run WHERE probe_run_id=?1;",
            -1,
            &current.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }
    sqlite3_bind_int64(current.st, 1, command.probe_run_id);
    if (sqlite3_step(current.st) != SQLITE_ROW) {
        if (error_out) *error_out = "SeedProbe run does not exist";
        rollback();
        return false;
    }
    const auto current_status = ParseSeedProbeRunStatus(ColumnText(current.st, 0));
    if (current_status == command.new_status) {
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            rollback();
            return false;
        }
        return true;
    }
    if (current_status != command.expected_status) {
        if (error_out) *error_out = "SeedProbe run status changed concurrently";
        rollback();
        return false;
    }

    if (command.new_status == SeedProbeRunStatus::Completed
        || command.new_status == SeedProbeRunStatus::CompletedPartial) {
        Statement accepted;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT "
                "(SELECT COUNT(1) FROM sp_probe_result r "
                " WHERE r.probe_run_id=?1 AND r.evidence_state='CONFIRMED' "
                " AND r.confirmation_of_probe_result_id IS NULL),"
                "(SELECT COUNT(1) FROM an_input_set_frame f JOIN sp_probe_run pr "
                " ON pr.accepted_input_set_id=f.input_set_id WHERE pr.probe_run_id=?1),"
                "(SELECT COUNT(1) FROM an_input_set_frame f JOIN sp_probe_run pr "
                " ON pr.accepted_input_set_id=f.input_set_id "
                " WHERE pr.probe_run_id=?1 AND NOT EXISTS ("
                "  SELECT 1 FROM sp_probe_result r WHERE r.probe_run_id=pr.probe_run_id "
                "  AND r.input_frame_id=f.input_frame_id AND r.evidence_state='CONFIRMED' "
                "  AND r.confirmation_of_probe_result_id IS NULL));",
                -1,
                &accepted.st,
                nullptr)
            != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            rollback();
            return false;
        }
        sqlite3_bind_int64(accepted.st, 1, command.probe_run_id);
        if (sqlite3_step(accepted.st) != SQLITE_ROW
            || sqlite3_column_int64(accepted.st, 0) <= 0
            || sqlite3_column_int64(accepted.st, 0) != sqlite3_column_int64(accepted.st, 1)
            || sqlite3_column_int64(accepted.st, 2) != 0) {
            if (error_out) {
                *error_out = "completed SeedProbe run must publish every confirmed representative frame";
            }
            rollback();
            return false;
        }
    }

    Statement update;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE sp_probe_run SET status=?2,completed_at_utc=?3 "
            "WHERE probe_run_id=?1 AND status=?4;",
            -1,
            &update.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }
    const auto expected = ToDbString(command.expected_status);
    const auto desired = ToDbString(command.new_status);
    sqlite3_bind_int64(update.st, 1, command.probe_run_id);
    sqlite3_bind_text(update.st, 2, desired.data(), static_cast<int>(desired.size()), SQLITE_TRANSIENT);
    if (command.completed_at_utc.has_value()) {
        sqlite3_bind_int64(update.st, 3, command.completed_at_utc->time_since_epoch().count());
    } else {
        sqlite3_bind_null(update.st, 3);
    }
    sqlite3_bind_text(update.st, 4, expected.data(), static_cast<int>(expected.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(update.st) != SQLITE_DONE || sqlite3_changes(db_) != 1) {
        if (error_out) *error_out = "SeedProbe run status changed concurrently";
        rollback();
        return false;
    }

    const std::string_view event_type =
        command.new_status == SeedProbeRunStatus::Failed
        ? "AnalysisSeedProbe.RunFailed.v1"
        : terminal
            ? "AnalysisSeedProbe.RunCompleted.v1"
            : "AnalysisSeedProbe.RunStatusChanged.v1";
    if (!InsertSeedProbeOutboxEvent(
            db_,
            event_type,
            "probe_run",
            std::to_string(command.probe_run_id),
            command.correlation_id,
            command.causation_id,
            command.changed_at_utc.time_since_epoch().count(),
            "probe_run",
            command.probe_run_id,
            error_out)) {
        rollback();
        return false;
    }
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }
    if (changed_out) *changed_out = true;
    return true;
}

bool SqliteAnalysisDb::ReplaceSeedProbeAcceptedInputFrames(
    const ReplaceSeedProbeAcceptedInputFramesCommand& command,
    std::string* error_out) {
    if (db_ == nullptr || command.probe_run_id <= 0 || command.input_frame_ids.empty()) {
        if (error_out) *error_out = "SeedProbe run and at least one accepted input frame are required";
        return false;
    }
    std::unordered_set<std::int64_t> distinct;
    for (const auto input_frame_id : command.input_frame_ids) {
        if (input_frame_id <= 0 || !distinct.insert(input_frame_id).second) {
            if (error_out) *error_out = "accepted input frames must be positive and unique";
            return false;
        }
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const auto rollback = [&]() {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    };
    const auto input_set_id = AcceptedInputSetIdForProbeRun(db_, command.probe_run_id);
    if (!input_set_id.has_value()) {
        if (error_out) *error_out = "SeedProbe run does not have an accepted input set";
        rollback();
        return false;
    }

    Statement confirmed;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT 1 FROM sp_probe_result "
            "WHERE probe_run_id=?1 AND input_frame_id=?2 "
            "AND evidence_state='CONFIRMED' AND confirmation_of_probe_result_id IS NULL LIMIT 1;",
            -1,
            &confirmed.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }
    for (const auto input_frame_id : command.input_frame_ids) {
        sqlite3_reset(confirmed.st);
        sqlite3_clear_bindings(confirmed.st);
        sqlite3_bind_int64(confirmed.st, 1, command.probe_run_id);
        sqlite3_bind_int64(confirmed.st, 2, input_frame_id);
        if (sqlite3_step(confirmed.st) != SQLITE_ROW) {
            if (error_out) {
                *error_out = "accepted input frames must be confirmed representatives from the same SeedProbe run";
            }
            rollback();
            return false;
        }
    }

    bool already_matches = true;
    std::size_t current_ordinal = 0;
    Statement current;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT ordinal,input_frame_id FROM an_input_set_frame "
            "WHERE input_set_id=?1 ORDER BY ordinal ASC;",
            -1,
            &current.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }
    sqlite3_bind_int64(current.st, 1, *input_set_id);
    for (;;) {
        const auto step = sqlite3_step(current.st);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            rollback();
            return false;
        }
        if (current_ordinal >= command.input_frame_ids.size()
            || sqlite3_column_int64(current.st, 0) != static_cast<std::int64_t>(current_ordinal)
            || sqlite3_column_int64(current.st, 1) != command.input_frame_ids[current_ordinal]) {
            already_matches = false;
        }
        ++current_ordinal;
    }
    already_matches = already_matches && current_ordinal == command.input_frame_ids.size();
    if (already_matches) {
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            rollback();
            return false;
        }
        return true;
    }

    Statement remove;
    if (sqlite3_prepare_v2(
            db_,
            "DELETE FROM an_input_set_frame WHERE input_set_id=?1;",
            -1,
            &remove.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }
    sqlite3_bind_int64(remove.st, 1, *input_set_id);
    if (sqlite3_step(remove.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }

    Statement insert;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO an_input_set_frame(input_set_id,ordinal,input_frame_id,added_at_utc) "
            "VALUES(?1,?2,?3,?4);",
            -1,
            &insert.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }
    for (std::size_t ordinal = 0; ordinal < command.input_frame_ids.size(); ++ordinal) {
        sqlite3_reset(insert.st);
        sqlite3_clear_bindings(insert.st);
        sqlite3_bind_int64(insert.st, 1, *input_set_id);
        sqlite3_bind_int64(insert.st, 2, static_cast<std::int64_t>(ordinal));
        sqlite3_bind_int64(insert.st, 3, command.input_frame_ids[ordinal]);
        sqlite3_bind_int64(insert.st, 4, command.replaced_at_utc.time_since_epoch().count());
        if (sqlite3_step(insert.st) != SQLITE_DONE) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            rollback();
            return false;
        }
    }

    if (!InsertSeedProbeOutboxEvent(
            db_,
            "AnalysisSeedProbe.AcceptedInputFramesReplaced.v1",
            "probe_run",
            std::to_string(command.probe_run_id),
            command.correlation_id,
            command.causation_id,
            command.replaced_at_utc.time_since_epoch().count(),
            "probe_run",
            command.probe_run_id,
            error_out)) {
        rollback();
        return false;
    }
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }
    return true;
}

bool SqliteAnalysisDb::SetSeedProbeRunEntrySavestate(
    const SetSeedProbeRunEntrySavestateCommand& command,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.probe_run_id <= 0 || command.entry_savestate_id <= 0) {
        if (error_out) *error_out = "probe_run_id and entry_savestate_id must be > 0";
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE sp_probe_run SET entry_savestate_id=?2 WHERE probe_run_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, command.probe_run_id);
    sqlite3_bind_int64(st.st, 2, command.entry_savestate_id);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_changes(db_) <= 0) {
        if (error_out) *error_out = "seed probe run not found";
        return false;
    }
    return true;
}


bool SqliteAnalysisDb::RecordSeedProbeEncounterProjection(
    const RecordSeedProbeEncounterProjectionCommand& command,
    std::int64_t* encounter_projection_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.probe_run_id <= 0 || command.encounter_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_projection;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO sp_encounter_projection(probe_run_id,seed_value,option_ordinal,encounter_id,encounter_frame,stutter_step_at,movement_required,recorded_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8);",
            -1,
            &insert_projection.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_projection.st, 1, command.probe_run_id);
    sqlite3_bind_int64(insert_projection.st, 2, command.seed_value);
    sqlite3_bind_int(insert_projection.st, 3, command.option_ordinal);
    sqlite3_bind_text(insert_projection.st, 4, command.encounter_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_projection.st, 5, command.encounter_frame);
    if (command.stutter_step_at.has_value()) {
        sqlite3_bind_int64(insert_projection.st, 6, command.stutter_step_at.value());
    } else {
        sqlite3_bind_null(insert_projection.st, 6);
    }
    sqlite3_bind_int(insert_projection.st, 7, command.movement_required ? 1 : 0);
    sqlite3_bind_int64(insert_projection.st, 8, command.recorded_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert_projection.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto encounter_projection_id = sqlite3_last_insert_rowid(db_);
    if (!InsertSeedProbeOutboxEvent(
            db_,
            "AnalysisSeedProbe.EncounterProjectionRecorded.v1",
            "probe_run",
            std::to_string(command.probe_run_id),
            command.correlation_id,
            command.causation_id,
            command.recorded_at_utc.time_since_epoch().count(),
            "encounter_projection",
            encounter_projection_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (encounter_projection_id_out) {
        *encounter_projection_id_out = encounter_projection_id;
    }
    return true;
}


bool SqliteAnalysisDb::CreateBattleSet(
    const CreateBattleSetCommand& command,
    std::int64_t* battle_set_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.name.empty()
        || command.entry_savestate_id <= 0
        || command.battle_run_spec_id <= 0
        || command.explorer_settings_id <= 0
        || command.status == BattleSetStatus::Unknown) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_set;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ab_battle_set(name,entry_savestate_id,battle_run_spec_id,explorer_settings_id,launch_fake_attack_min,launch_fake_attack_max,status,created_at_utc,completed_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,NULL);",
            -1,
            &insert_set.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_text(insert_set.st, 1, command.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_set.st, 2, command.entry_savestate_id);
    sqlite3_bind_int64(insert_set.st, 3, command.battle_run_spec_id);
    sqlite3_bind_int64(insert_set.st, 4, command.explorer_settings_id);
    sqlite3_bind_int(insert_set.st, 5, command.launch_fake_attack_min);
    sqlite3_bind_int(insert_set.st, 6, command.launch_fake_attack_max);
    const auto status = ToDbString(command.status);
    sqlite3_bind_text(insert_set.st, 7, status.data(), static_cast<int>(status.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_set.st, 8, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert_set.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto battle_set_id = sqlite3_last_insert_rowid(db_);
    if (!InsertBattleOutboxEvent(
            db_,
            "AnalysisBattle.BattleSetCreated.v1",
            "battle_set",
            std::to_string(battle_set_id),
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "battle_set",
            battle_set_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (battle_set_id_out) {
        *battle_set_id_out = battle_set_id;
    }
    return true;
}

bool SqliteAnalysisDb::AddBattleSeedCandidate(
    const AddBattleSeedCandidateCommand& command,
    std::int64_t* seed_candidate_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.battle_set_id <= 0
        || command.seed_value < 0
        || command.seed_value > static_cast<std::int64_t>((std::numeric_limits<std::uint32_t>::max)())
        || command.source_kind == BattleSeedCandidateSourceKind::Unknown
        || command.candidate_status == BattleSeedCandidateStatus::Unknown) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    if (command.source_kind == BattleSeedCandidateSourceKind::SeedProbeConfirmedResult) {
        if (!command.source_probe_result_id.has_value() || *command.source_probe_result_id <= 0) {
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            if (error_out) *error_out = "confirmed SeedProbe candidate requires source_probe_result_id";
            return false;
        }
        Statement source;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT input_frame_id,seed_value,evidence_state,confirmation_of_probe_result_id "
                "FROM sp_probe_result WHERE probe_result_id=?1;",
                -1,
                &source.st,
                nullptr)
            != SQLITE_OK) {
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(source.st, 1, *command.source_probe_result_id);
        if (sqlite3_step(source.st) != SQLITE_ROW
            || ColumnText(source.st, 2) != "CONFIRMED"
            || sqlite3_column_type(source.st, 3) != SQLITE_NULL
            || static_cast<std::uint32_t>(sqlite3_column_int64(source.st, 1))
                != static_cast<std::uint32_t>(command.seed_value)
            || (command.source_input_frame_id.has_value()
                && sqlite3_column_int64(source.st, 0) != *command.source_input_frame_id)) {
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            if (error_out) {
                *error_out = "SeedProbe candidate source must be the matching confirmed representative result";
            }
            return false;
        }
    } else if (command.source_probe_result_id.has_value()) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = "only SP_CONFIRMED_RESULT candidates may reference a SeedProbe result";
        return false;
    }

    Statement insert_candidate;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ab_seed_candidate(battle_set_id,source_probe_result_id,source_input_frame_id,seed_value,source_kind,candidate_status,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7);",
            -1,
            &insert_candidate.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_candidate.st, 1, command.battle_set_id);
    if (command.source_probe_result_id.has_value()) sqlite3_bind_int64(insert_candidate.st, 2, command.source_probe_result_id.value());
    else sqlite3_bind_null(insert_candidate.st, 2);
    if (command.source_input_frame_id.has_value()) sqlite3_bind_int64(insert_candidate.st, 3, command.source_input_frame_id.value());
    else sqlite3_bind_null(insert_candidate.st, 3);
    sqlite3_bind_int64(insert_candidate.st, 4, command.seed_value);
    const auto source_kind = ToDbString(command.source_kind);
    const auto candidate_status = ToDbString(command.candidate_status);
    sqlite3_bind_text(insert_candidate.st, 5, source_kind.data(), static_cast<int>(source_kind.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_candidate.st, 6, candidate_status.data(), static_cast<int>(candidate_status.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_candidate.st, 7, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert_candidate.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto seed_candidate_id = sqlite3_last_insert_rowid(db_);
    if (!InsertBattleOutboxEvent(
            db_,
            "AnalysisBattle.SeedCandidateAdded.v1",
            "battle_set",
            std::to_string(command.battle_set_id),
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "seed_candidate",
            seed_candidate_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (seed_candidate_id_out) {
        *seed_candidate_id_out = seed_candidate_id;
    }
    return true;
}

bool SqliteAnalysisDb::CreateBattleTurnWave(
    const CreateBattleTurnWaveCommand& command,
    std::int64_t* wave_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.battle_set_id <= 0
        || command.seed_candidate_id <= 0
        || command.status == BattleTurnWaveStatus::Unknown) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_wave;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ab_turn_wave(battle_set_id,turn_index,context_probe_id,parent_wave_id,parent_turn_job_id,seed_candidate_id,battle_advancement_pool_id,status,created_at_utc,completed_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);",
            -1,
            &insert_wave.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_wave.st, 1, command.battle_set_id);
    sqlite3_bind_int(insert_wave.st, 2, command.turn_index);
    if (command.context_probe_id.has_value()) sqlite3_bind_int64(insert_wave.st, 3, command.context_probe_id.value());
    else sqlite3_bind_null(insert_wave.st, 3);
    if (command.parent_wave_id.has_value()) sqlite3_bind_int64(insert_wave.st, 4, command.parent_wave_id.value());
    else sqlite3_bind_null(insert_wave.st, 4);
    if (command.parent_turn_job_id.has_value()) sqlite3_bind_int64(insert_wave.st, 5, command.parent_turn_job_id.value());
    else sqlite3_bind_null(insert_wave.st, 5);
    sqlite3_bind_int64(insert_wave.st, 6, command.seed_candidate_id);
    if (command.battle_advancement_pool_id.has_value()) sqlite3_bind_int64(insert_wave.st, 7, command.battle_advancement_pool_id.value());
    else sqlite3_bind_null(insert_wave.st, 7);
    const auto status = ToDbString(command.status);
    sqlite3_bind_text(insert_wave.st, 8, status.data(), static_cast<int>(status.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_wave.st, 9, command.created_at_utc.time_since_epoch().count());
    if (command.completed_at_utc.has_value()) sqlite3_bind_int64(insert_wave.st, 10, command.completed_at_utc->time_since_epoch().count());
    else sqlite3_bind_null(insert_wave.st, 10);
    if (sqlite3_step(insert_wave.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto wave_id = sqlite3_last_insert_rowid(db_);
    if (!InsertBattleOutboxEvent(
            db_,
            "AnalysisBattle.TurnWaveCreated.v1",
            "battle_set",
            std::to_string(command.battle_set_id),
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "turn_wave",
            wave_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (wave_id_out) {
        *wave_id_out = wave_id;
    }
    return true;
}

bool SqliteAnalysisDb::CreateBattleContextProbe(
    const CreateBattleContextProbeCommand& command,
    std::int64_t* context_probe_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.source_savestate_id <= 0 || command.probe_status == BattleContextProbeStatus::Unknown) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    std::optional<std::int64_t> battle_set_id;
    if (command.wave_id > 0) {
        battle_set_id = BattleSetIdForWave(db_, command.wave_id);
        if (!battle_set_id.has_value()) {
            if (error_out) *error_out = "wave_id does not resolve to battle_set";
            return false;
        }
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    Statement insert_probe;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ab_battle_context_probe(wave_id,source_savestate_id,exec_job_id,probe_status,context_blob,context_version,recorded_at_utc,created_at_utc) "
            "VALUES(?1,?2,NULL,?3,NULL,NULL,NULL,?4);",
            -1,
            &insert_probe.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (command.wave_id > 0) sqlite3_bind_int64(insert_probe.st, 1, command.wave_id);
    else sqlite3_bind_null(insert_probe.st, 1);
    sqlite3_bind_int64(insert_probe.st, 2, command.source_savestate_id);
    const auto probe_status = ToDbString(command.probe_status);
    sqlite3_bind_text(insert_probe.st, 3, probe_status.data(), static_cast<int>(probe_status.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_probe.st, 4, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert_probe.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto context_probe_id = sqlite3_last_insert_rowid(db_);
    if (!InsertBattleOutboxEvent(
            db_,
            "AnalysisBattle.ContextProbeCreated.v1",
            battle_set_id.has_value() ? "battle_set" : "battle_context_probe",
            battle_set_id.has_value() ? std::to_string(*battle_set_id) : std::to_string(context_probe_id),
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "battle_context_probe",
            context_probe_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (context_probe_id_out) *context_probe_id_out = context_probe_id;
    return true;
}

bool SqliteAnalysisDb::SetBattleContextProbeExecJobId(
    std::int64_t context_probe_id,
    std::int64_t exec_job_id,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (context_probe_id <= 0 || exec_job_id <= 0) {
        if (error_out) *error_out = "context_probe_id and exec_job_id are required";
        return false;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE ab_battle_context_probe SET exec_job_id=?2, probe_status='RUNNING' WHERE context_probe_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, context_probe_id);
    sqlite3_bind_int64(st.st, 2, exec_job_id);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    return sqlite3_changes(db_) > 0;
}

bool SqliteAnalysisDb::CompleteBattleContextProbe(
    const CompleteBattleContextProbeCommand& command,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.exec_job_id <= 0 || command.probe_status == BattleContextProbeStatus::Unknown) {
        if (error_out) *error_out = "exec_job_id and probe_status are required";
        return false;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE ab_battle_context_probe SET probe_status=?2,context_blob=?3,context_version=?4,recorded_at_utc=?5 WHERE exec_job_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, command.exec_job_id);
    const auto probe_status = ToDbString(command.probe_status);
    sqlite3_bind_text(st.st, 2, probe_status.data(), static_cast<int>(probe_status.size()), SQLITE_TRANSIENT);
    if (command.context_blob.has_value()) {
        sqlite3_bind_blob(
            st.st,
            3,
            command.context_blob->data(),
            static_cast<int>(command.context_blob->size()),
            SQLITE_TRANSIENT);
    }
    else sqlite3_bind_null(st.st, 3);
    if (command.context_version.has_value()) sqlite3_bind_int(st.st, 4, *command.context_version);
    else sqlite3_bind_null(st.st, 4);
    sqlite3_bind_int64(st.st, 5, command.recorded_at_utc.time_since_epoch().count());
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    return sqlite3_changes(db_) > 0;
}

bool SqliteAnalysisDb::RecordBattleTurnJob(
    const RecordBattleTurnJobCommand& command,
    std::int64_t* turn_job_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.wave_id <= 0 || command.plan_id <= 0 || command.job_state == BattleTurnJobState::Unknown) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    const auto battle_set_id = BattleSetIdForWave(db_, command.wave_id);
    if (!battle_set_id.has_value()) {
        if (error_out) *error_out = "wave_id does not resolve to battle_set";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_job;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ab_turn_job(wave_id,exec_job_id,plan_id,source_savestate_id,seed_candidate_id,authored_plan_id,authored_turn_index,resolved_turn_commands_blob,resolved_turn_variant_key,fake_attacks_this_turn,fake_attacks_used_before,job_state,started_at_utc,ended_at_utc,has_results,vi_start,vi_end,delta_vi,rng_seed,battle_outcome,plan_materialize_err,pred_passed,pred_total,pred_abort_run,output_savestate_id,applied_input_artifact_id,input_trace_artifact_id,result_context_blob_base64,result_context_version,recorded_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23,?24,?25,?26,?27,?28,?29,?30);",
            -1,
            &insert_job.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_job.st, 1, command.wave_id);
    if (command.exec_job_id.has_value()) sqlite3_bind_int64(insert_job.st, 2, command.exec_job_id.value());
    else sqlite3_bind_null(insert_job.st, 2);
    sqlite3_bind_int64(insert_job.st, 3, command.plan_id);
    if (command.source_savestate_id.has_value()) sqlite3_bind_int64(insert_job.st, 4, *command.source_savestate_id);
    else sqlite3_bind_null(insert_job.st, 4);
    if (command.seed_candidate_id.has_value()) sqlite3_bind_int64(insert_job.st, 5, *command.seed_candidate_id);
    else sqlite3_bind_null(insert_job.st, 5);
    if (command.authored_plan_id.has_value()) sqlite3_bind_int64(insert_job.st, 6, *command.authored_plan_id);
    else sqlite3_bind_int64(insert_job.st, 6, command.plan_id);
    if (command.authored_turn_index.has_value()) sqlite3_bind_int(insert_job.st, 7, *command.authored_turn_index);
    else sqlite3_bind_null(insert_job.st, 7);
    if (command.resolved_turn_commands_blob.has_value()) sqlite3_bind_text(insert_job.st, 8, command.resolved_turn_commands_blob->c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(insert_job.st, 8);
    if (command.resolved_turn_variant_key.has_value()) sqlite3_bind_text(insert_job.st, 9, command.resolved_turn_variant_key->c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(insert_job.st, 9);
    sqlite3_bind_int(insert_job.st, 10, command.fake_attacks_this_turn);
    sqlite3_bind_int(insert_job.st, 11, command.fake_attacks_used_before);
    const auto job_state = ToDbString(command.job_state);
    sqlite3_bind_text(insert_job.st, 12, job_state.data(), static_cast<int>(job_state.size()), SQLITE_TRANSIENT);
    if (command.started_at_utc.has_value()) sqlite3_bind_int64(insert_job.st, 13, command.started_at_utc->time_since_epoch().count());
    else sqlite3_bind_null(insert_job.st, 13);
    if (command.ended_at_utc.has_value()) sqlite3_bind_int64(insert_job.st, 14, command.ended_at_utc->time_since_epoch().count());
    else sqlite3_bind_null(insert_job.st, 14);
    sqlite3_bind_int(insert_job.st, 15, command.has_results ? 1 : 0);
    if (command.vi_start.has_value()) sqlite3_bind_int(insert_job.st, 16, command.vi_start.value());
    else sqlite3_bind_null(insert_job.st, 16);
    if (command.vi_end.has_value()) sqlite3_bind_int(insert_job.st, 17, command.vi_end.value());
    else sqlite3_bind_null(insert_job.st, 17);
    if (command.delta_vi.has_value()) sqlite3_bind_int(insert_job.st, 18, command.delta_vi.value());
    else sqlite3_bind_null(insert_job.st, 18);
    if (command.rng_seed.has_value()) sqlite3_bind_int64(insert_job.st, 19, command.rng_seed.value());
    else sqlite3_bind_null(insert_job.st, 19);
    if (command.battle_outcome.has_value()) sqlite3_bind_int(insert_job.st, 20, static_cast<int>(*command.battle_outcome));
    else sqlite3_bind_null(insert_job.st, 20);
    if (command.plan_materialize_err.has_value()) sqlite3_bind_int(insert_job.st, 21, command.plan_materialize_err.value());
    else sqlite3_bind_null(insert_job.st, 21);
    if (command.pred_passed.has_value()) sqlite3_bind_int(insert_job.st, 22, command.pred_passed.value());
    else sqlite3_bind_null(insert_job.st, 22);
    if (command.pred_total.has_value()) sqlite3_bind_int(insert_job.st, 23, command.pred_total.value());
    else sqlite3_bind_null(insert_job.st, 23);
    if (command.pred_abort_run.has_value()) sqlite3_bind_int(insert_job.st, 24, command.pred_abort_run.value());
    else sqlite3_bind_null(insert_job.st, 24);
    if (command.output_savestate_id.has_value()) sqlite3_bind_int64(insert_job.st, 25, command.output_savestate_id.value());
    else sqlite3_bind_null(insert_job.st, 25);
    if (command.applied_input_artifact_id.has_value()) sqlite3_bind_int64(insert_job.st, 26, command.applied_input_artifact_id.value());
    else sqlite3_bind_null(insert_job.st, 26);
    const auto trace_artifact_id = command.input_trace_artifact_id.has_value()
        ? command.input_trace_artifact_id
        : command.applied_input_artifact_id;
    if (trace_artifact_id.has_value()) sqlite3_bind_int64(insert_job.st, 27, *trace_artifact_id);
    else sqlite3_bind_null(insert_job.st, 27);
    if (command.result_context_blob_base64.has_value()) sqlite3_bind_text(insert_job.st, 28, command.result_context_blob_base64->c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(insert_job.st, 28);
    if (command.result_context_version.has_value()) sqlite3_bind_int(insert_job.st, 29, *command.result_context_version);
    else sqlite3_bind_null(insert_job.st, 29);
    if (command.recorded_at_utc.has_value()) sqlite3_bind_int64(insert_job.st, 30, command.recorded_at_utc->time_since_epoch().count());
    else sqlite3_bind_null(insert_job.st, 30);

    if (sqlite3_step(insert_job.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto turn_job_id = sqlite3_last_insert_rowid(db_);
    const auto occurred_at_utc = command.recorded_at_utc.has_value()
        ? command.recorded_at_utc->time_since_epoch().count()
        : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (!InsertBattleOutboxEvent(
            db_,
            "AnalysisBattle.TurnJobRecorded.v1",
            "battle_set",
            std::to_string(battle_set_id.value()),
            command.correlation_id,
            command.causation_id,
            occurred_at_utc,
            "turn_job",
            turn_job_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (turn_job_id_out) {
        *turn_job_id_out = turn_job_id;
    }
    return true;
}

bool SqliteAnalysisDb::SetBattleTurnJobExecJobId(
    std::int64_t turn_job_id,
    std::int64_t exec_job_id,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (turn_job_id <= 0 || exec_job_id <= 0) {
        if (error_out) *error_out = "turn_job_id and exec_job_id are required";
        return false;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE ab_turn_job SET exec_job_id=?2 WHERE turn_job_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, turn_job_id);
    sqlite3_bind_int64(st.st, 2, exec_job_id);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    return sqlite3_changes(db_) > 0;
}

bool SqliteAnalysisDb::UpdateBattleTurnJobResult(
    const RecordBattleTurnJobCommand& command,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (!command.exec_job_id.has_value() || *command.exec_job_id <= 0 || command.job_state == BattleTurnJobState::Unknown) {
        if (error_out) *error_out = "exec_job_id and job_state are required";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE ab_turn_job SET "
            "job_state=?2,ended_at_utc=?3,has_results=?4,vi_start=?5,vi_end=?6,delta_vi=?7,rng_seed=?8,"
            "battle_outcome=?9,plan_materialize_err=?10,pred_passed=?11,pred_total=?12,pred_abort_run=?13,"
            "output_savestate_id=?14,applied_input_artifact_id=?15,input_trace_artifact_id=?16,result_context_blob_base64=?17,result_context_version=?18,recorded_at_utc=?19 "
            "WHERE exec_job_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, *command.exec_job_id);
    const auto job_state = ToDbString(command.job_state);
    sqlite3_bind_text(st.st, 2, job_state.data(), static_cast<int>(job_state.size()), SQLITE_TRANSIENT);
    if (command.ended_at_utc.has_value()) sqlite3_bind_int64(st.st, 3, command.ended_at_utc->time_since_epoch().count());
    else sqlite3_bind_null(st.st, 3);
    sqlite3_bind_int(st.st, 4, command.has_results ? 1 : 0);
    if (command.vi_start.has_value()) sqlite3_bind_int(st.st, 5, *command.vi_start); else sqlite3_bind_null(st.st, 5);
    if (command.vi_end.has_value()) sqlite3_bind_int(st.st, 6, *command.vi_end); else sqlite3_bind_null(st.st, 6);
    if (command.delta_vi.has_value()) sqlite3_bind_int(st.st, 7, *command.delta_vi); else sqlite3_bind_null(st.st, 7);
    if (command.rng_seed.has_value()) sqlite3_bind_int64(st.st, 8, *command.rng_seed); else sqlite3_bind_null(st.st, 8);
    if (command.battle_outcome.has_value()) sqlite3_bind_int(st.st, 9, static_cast<int>(*command.battle_outcome)); else sqlite3_bind_null(st.st, 9);
    if (command.plan_materialize_err.has_value()) sqlite3_bind_int(st.st, 10, *command.plan_materialize_err); else sqlite3_bind_null(st.st, 10);
    if (command.pred_passed.has_value()) sqlite3_bind_int(st.st, 11, *command.pred_passed); else sqlite3_bind_null(st.st, 11);
    if (command.pred_total.has_value()) sqlite3_bind_int(st.st, 12, *command.pred_total); else sqlite3_bind_null(st.st, 12);
    if (command.pred_abort_run.has_value()) sqlite3_bind_int(st.st, 13, *command.pred_abort_run); else sqlite3_bind_null(st.st, 13);
    if (command.output_savestate_id.has_value()) sqlite3_bind_int64(st.st, 14, *command.output_savestate_id); else sqlite3_bind_null(st.st, 14);
    if (command.applied_input_artifact_id.has_value()) sqlite3_bind_int64(st.st, 15, *command.applied_input_artifact_id); else sqlite3_bind_null(st.st, 15);
    const auto trace_artifact_id = command.input_trace_artifact_id.has_value()
        ? command.input_trace_artifact_id
        : command.applied_input_artifact_id;
    if (trace_artifact_id.has_value()) sqlite3_bind_int64(st.st, 16, *trace_artifact_id); else sqlite3_bind_null(st.st, 16);
    if (command.result_context_blob_base64.has_value()) sqlite3_bind_text(st.st, 17, command.result_context_blob_base64->c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st.st, 17);
    if (command.result_context_version.has_value()) sqlite3_bind_int(st.st, 18, *command.result_context_version);
    else sqlite3_bind_null(st.st, 18);
    if (command.recorded_at_utc.has_value()) sqlite3_bind_int64(st.st, 19, command.recorded_at_utc->time_since_epoch().count());
    else sqlite3_bind_null(st.st, 19);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_changes(db_) <= 0) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto projection_ref = BattleTurnJobProjectionRefForExecJob(db_, *command.exec_job_id);
    if (!projection_ref.has_value()) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = "exec_job_id does not resolve to battle turn job";
        return false;
    }

    const auto occurred_at_utc = command.recorded_at_utc.has_value()
        ? command.recorded_at_utc->time_since_epoch().count()
        : (command.ended_at_utc.has_value()
            ? command.ended_at_utc->time_since_epoch().count()
            : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    if (!InsertBattleOutboxEvent(
            db_,
            "AnalysisBattle.TurnJobResultUpdated.v1",
            "battle_set",
            std::to_string(projection_ref->battle_set_id),
            "",
            "",
            occurred_at_utc,
            "turn_job",
            projection_ref->turn_job_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

bool SqliteAnalysisDb::CreateBattleAdvancementPool(
    const CreateBattleAdvancementPoolCommand& command,
    std::int64_t* battle_advancement_pool_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.battle_set_id <= 0
        || command.pool_name.empty()
        || command.criterion_kind == BattleAdvancementCriterionKind::Unknown) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_pool;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ab_battle_advancement_pool(battle_set_id,turn_index,pool_name,criterion_kind,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5);",
            -1,
            &insert_pool.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_pool.st, 1, command.battle_set_id);
    sqlite3_bind_int(insert_pool.st, 2, command.turn_index);
    sqlite3_bind_text(insert_pool.st, 3, command.pool_name.c_str(), -1, SQLITE_TRANSIENT);
    const auto criterion_kind = ToDbString(command.criterion_kind);
    sqlite3_bind_text(insert_pool.st, 4, criterion_kind.data(), static_cast<int>(criterion_kind.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_pool.st, 5, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert_pool.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto battle_advancement_pool_id = sqlite3_last_insert_rowid(db_);
    if (!InsertBattleOutboxEvent(
            db_,
            "AnalysisBattle.BattleAdvancementPoolCreated.v1",
            "battle_set",
            std::to_string(command.battle_set_id),
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "battle_advancement_pool",
            battle_advancement_pool_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (battle_advancement_pool_id_out) {
        *battle_advancement_pool_id_out = battle_advancement_pool_id;
    }
    return true;
}

bool SqliteAnalysisDb::EnsureBattleAdvancementPool(
    const CreateBattleAdvancementPoolCommand& command,
    std::int64_t* battle_advancement_pool_id_out,
    std::string* error_out) {
    if (battle_advancement_pool_id_out != nullptr) {
        *battle_advancement_pool_id_out = 0;
    }
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.battle_set_id <= 0 || command.pool_name.empty()) {
        if (error_out) *error_out = "battle_set_id and pool_name are required";
        return false;
    }

    Statement existing;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT battle_advancement_pool_id FROM ab_battle_advancement_pool WHERE battle_set_id=?1 AND turn_index=?2 AND pool_name=?3;",
            -1,
            &existing.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(existing.st, 1, command.battle_set_id);
    sqlite3_bind_int(existing.st, 2, command.turn_index);
    sqlite3_bind_text(existing.st, 3, command.pool_name.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(existing.st) == SQLITE_ROW) {
        if (battle_advancement_pool_id_out) *battle_advancement_pool_id_out = sqlite3_column_int64(existing.st, 0);
        return true;
    }

    return CreateBattleAdvancementPool(command, battle_advancement_pool_id_out, error_out);
}

bool SqliteAnalysisDb::RecordBattleAdvancementDecision(
    const RecordBattleAdvancementDecisionCommand& command,
    std::int64_t* battle_advancement_decision_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.battle_advancement_pool_id <= 0 || command.turn_job_id <= 0 || command.decision_kind == BattleAdvancementDecisionKind::Unknown) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    const auto battle_set_id = BattleSetIdForBattleAdvancementPool(db_, command.battle_advancement_pool_id);
    if (!battle_set_id.has_value()) {
        if (error_out) *error_out = "battle_advancement_pool_id does not resolve to battle_set";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_decision;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT OR IGNORE INTO ab_battle_advancement_decision(battle_advancement_pool_id,turn_job_id,decision_kind,decision_reason,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5);",
            -1,
            &insert_decision.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_decision.st, 1, command.battle_advancement_pool_id);
    sqlite3_bind_int64(insert_decision.st, 2, command.turn_job_id);
    const auto decision_kind = ToDbString(command.decision_kind);
    sqlite3_bind_text(insert_decision.st, 3, decision_kind.data(), static_cast<int>(decision_kind.size()), SQLITE_TRANSIENT);
    if (command.decision_reason.has_value()) sqlite3_bind_text(insert_decision.st, 4, command.decision_reason->c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(insert_decision.st, 4);
    sqlite3_bind_int64(insert_decision.st, 5, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert_decision.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    std::int64_t battle_advancement_decision_id = sqlite3_last_insert_rowid(db_);
    const bool inserted_decision = sqlite3_changes(db_) > 0;
    if (!inserted_decision) {
        Statement existing;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT battle_advancement_decision_id FROM ab_battle_advancement_decision WHERE battle_advancement_pool_id=?1 AND turn_job_id=?2;",
                -1,
                &existing.st,
                nullptr)
            == SQLITE_OK) {
            sqlite3_bind_int64(existing.st, 1, command.battle_advancement_pool_id);
            sqlite3_bind_int64(existing.st, 2, command.turn_job_id);
            if (sqlite3_step(existing.st) == SQLITE_ROW) {
                battle_advancement_decision_id = sqlite3_column_int64(existing.st, 0);
            }
        }
    }
    if (inserted_decision && !InsertBattleOutboxEvent(
            db_,
            "AnalysisBattle.BattleAdvancementDecisionRecorded.v1",
            "battle_set",
            std::to_string(battle_set_id.value()),
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "battle_advancement_decision",
            battle_advancement_decision_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (battle_advancement_decision_id_out) {
        *battle_advancement_decision_id_out = battle_advancement_decision_id;
    }
    return true;
}

bool SqliteAnalysisDb::UpsertBattleManualFollowup(
    const UpsertBattleManualFollowupCommand& command,
    std::int64_t* manual_followup_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.turn_job_id <= 0 || command.manual_followup_status == BattleManualFollowupStatus::Unknown) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    const auto battle_set_id = BattleSetIdForTurnJob(db_, command.turn_job_id);
    if (!battle_set_id.has_value()) {
        if (error_out) *error_out = "turn_job_id does not resolve to battle_set";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement upsert_followup;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ab_manual_followup(turn_job_id,manual_followup_status,recorded_dtm_artifact_id,recorded_dtmini_artifact_id,recorded_sav_artifact_id,note,updated_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7) "
            "ON CONFLICT(turn_job_id) DO UPDATE SET "
            "manual_followup_status=excluded.manual_followup_status,"
            "recorded_dtm_artifact_id=excluded.recorded_dtm_artifact_id,"
            "recorded_dtmini_artifact_id=excluded.recorded_dtmini_artifact_id,"
            "recorded_sav_artifact_id=excluded.recorded_sav_artifact_id,"
            "note=excluded.note,"
            "updated_at_utc=excluded.updated_at_utc;",
            -1,
            &upsert_followup.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(upsert_followup.st, 1, command.turn_job_id);
    const auto manual_followup_status = ToDbString(command.manual_followup_status);
    sqlite3_bind_text(upsert_followup.st, 2, manual_followup_status.data(), static_cast<int>(manual_followup_status.size()), SQLITE_TRANSIENT);
    if (command.recorded_dtm_artifact_id.has_value()) sqlite3_bind_int64(upsert_followup.st, 3, command.recorded_dtm_artifact_id.value());
    else sqlite3_bind_null(upsert_followup.st, 3);
    if (command.recorded_dtmini_artifact_id.has_value()) sqlite3_bind_int64(upsert_followup.st, 4, command.recorded_dtmini_artifact_id.value());
    else sqlite3_bind_null(upsert_followup.st, 4);
    if (command.recorded_sav_artifact_id.has_value()) sqlite3_bind_int64(upsert_followup.st, 5, command.recorded_sav_artifact_id.value());
    else sqlite3_bind_null(upsert_followup.st, 5);
    if (command.note.has_value()) sqlite3_bind_text(upsert_followup.st, 6, command.note->c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(upsert_followup.st, 6);
    sqlite3_bind_int64(upsert_followup.st, 7, command.updated_at_utc.time_since_epoch().count());
    if (sqlite3_step(upsert_followup.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto manual_followup_id = ManualFollowupIdForTurnJob(db_, command.turn_job_id);
    if (!manual_followup_id.has_value()) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = "turn_job manual_followup_id could not be resolved";
        return false;
    }

    if (!InsertBattleOutboxEvent(
            db_,
            "AnalysisBattle.ManualFollowupUpdated.v1",
            "battle_set",
            std::to_string(battle_set_id.value()),
            command.correlation_id,
            command.causation_id,
            command.updated_at_utc.time_since_epoch().count(),
            "manual_followup",
            manual_followup_id.value(),
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (manual_followup_id_out) {
        *manual_followup_id_out = manual_followup_id.value();
    }
    return true;
}

bool SqliteAnalysisDb::UpdateBattleSetStatus(
    std::int64_t battle_set_id,
    BattleSetStatus status,
    std::optional<types::UtcTimePoint> completed_at_utc,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (battle_set_id <= 0 || status == BattleSetStatus::Unknown) {
        if (error_out) *error_out = "battle_set_id and status are required";
        return false;
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE ab_battle_set SET status=?2, completed_at_utc=?3 WHERE battle_set_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, battle_set_id);
    const auto status_text = ToDbString(status);
    sqlite3_bind_text(st.st, 2, status_text.data(), static_cast<int>(status_text.size()), SQLITE_TRANSIENT);
    if (completed_at_utc.has_value()) sqlite3_bind_int64(st.st, 3, completed_at_utc->time_since_epoch().count());
    else sqlite3_bind_null(st.st, 3);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_changes(db_) <= 0) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto occurred_at_utc = completed_at_utc.has_value()
        ? completed_at_utc->time_since_epoch().count()
        : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (!InsertBattleOutboxEvent(
            db_,
            "AnalysisBattle.BattleSetStatusUpdated.v1",
            "battle_set",
            std::to_string(battle_set_id),
            "",
            "",
            occurred_at_utc,
            "battle_set",
            battle_set_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

bool SqliteAnalysisDb::UpdateBattleTurnWaveStatus(
    std::int64_t wave_id,
    BattleTurnWaveStatus status,
    std::optional<types::UtcTimePoint> completed_at_utc,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (wave_id <= 0 || status == BattleTurnWaveStatus::Unknown) {
        if (error_out) *error_out = "wave_id and status are required";
        return false;
    }
    const auto battle_set_id = BattleSetIdForWave(db_, wave_id);
    if (!battle_set_id.has_value()) {
        if (error_out) *error_out = "wave_id does not resolve to battle_set";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE ab_turn_wave SET status=?2, completed_at_utc=?3 WHERE wave_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, wave_id);
    const auto status_text = ToDbString(status);
    sqlite3_bind_text(st.st, 2, status_text.data(), static_cast<int>(status_text.size()), SQLITE_TRANSIENT);
    if (completed_at_utc.has_value()) sqlite3_bind_int64(st.st, 3, completed_at_utc->time_since_epoch().count());
    else sqlite3_bind_null(st.st, 3);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_changes(db_) <= 0) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto occurred_at_utc = completed_at_utc.has_value()
        ? completed_at_utc->time_since_epoch().count()
        : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (!InsertBattleOutboxEvent(
            db_,
            "AnalysisBattle.TurnWaveStatusUpdated.v1",
            "battle_set",
            std::to_string(battle_set_id.value()),
            "",
            "",
            occurred_at_utc,
            "turn_wave",
            wave_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

std::optional<BattleSetSnapshot> SqliteAnalysisDb::GetBattleSet(std::int64_t battle_set_id) const {
    if (db_ == nullptr || battle_set_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT battle_set_id,name,entry_savestate_id,battle_run_spec_id,explorer_settings_id,launch_fake_attack_min,launch_fake_attack_max,status,created_at_utc,completed_at_utc "
        "FROM ab_battle_set WHERE battle_set_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, battle_set_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    BattleSetSnapshot out{};
    out.battle_set_id = sqlite3_column_int64(st.st, 0);
    out.name = ColumnText(st.st, 1);
    out.entry_savestate_id = sqlite3_column_int64(st.st, 2);
    out.battle_run_spec_id = sqlite3_column_int64(st.st, 3);
    out.explorer_settings_id = sqlite3_column_int64(st.st, 4);
    out.launch_fake_attack_min = sqlite3_column_int(st.st, 5);
    out.launch_fake_attack_max = sqlite3_column_int(st.st, 6);
    out.status = ParseBattleSetStatus(ColumnText(st.st, 7));
    out.created_at_utc = ColumnTime(st.st, 8);
    out.completed_at_utc = ColumnTimeOptional(st.st, 9);
    return out;
}

std::vector<BattleSeedCandidateRow> SqliteAnalysisDb::ListBattleSeedCandidates(std::int64_t battle_set_id) const {
    std::vector<BattleSeedCandidateRow> rows;
    if (db_ == nullptr || battle_set_id <= 0) {
        return rows;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT seed_candidate_id,battle_set_id,source_probe_result_id,source_input_frame_id,seed_value,source_kind,candidate_status,created_at_utc "
        "FROM ab_seed_candidate WHERE battle_set_id=?1 ORDER BY seed_candidate_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, battle_set_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        BattleSeedCandidateRow row{};
        row.seed_candidate_id = sqlite3_column_int64(st.st, 0);
        row.battle_set_id = sqlite3_column_int64(st.st, 1);
        row.source_probe_result_id = ColumnInt64Optional(st.st, 2);
        row.source_input_frame_id = ColumnInt64Optional(st.st, 3);
        row.seed_value = sqlite3_column_int64(st.st, 4);
        row.source_kind = ParseBattleSeedCandidateSourceKind(ColumnText(st.st, 5));
        row.candidate_status = ParseBattleSeedCandidateStatus(ColumnText(st.st, 6));
        row.created_at_utc = ColumnTime(st.st, 7);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::optional<BattleSeedCandidateRow> SqliteAnalysisDb::GetBattleSeedCandidate(std::int64_t seed_candidate_id) const {
    if (db_ == nullptr || seed_candidate_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT seed_candidate_id,battle_set_id,source_probe_result_id,source_input_frame_id,seed_value,source_kind,candidate_status,created_at_utc "
        "FROM ab_seed_candidate WHERE seed_candidate_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, seed_candidate_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    BattleSeedCandidateRow row{};
    row.seed_candidate_id = sqlite3_column_int64(st.st, 0);
    row.battle_set_id = sqlite3_column_int64(st.st, 1);
    row.source_probe_result_id = ColumnInt64Optional(st.st, 2);
    row.source_input_frame_id = ColumnInt64Optional(st.st, 3);
    row.seed_value = sqlite3_column_int64(st.st, 4);
    row.source_kind = ParseBattleSeedCandidateSourceKind(ColumnText(st.st, 5));
    row.candidate_status = ParseBattleSeedCandidateStatus(ColumnText(st.st, 6));
    row.created_at_utc = ColumnTime(st.st, 7);
    return row;
}

std::optional<BattleTurnWaveSnapshot> SqliteAnalysisDb::GetBattleTurnWave(std::int64_t wave_id) const {
    if (db_ == nullptr || wave_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT wave_id,battle_set_id,turn_index,context_probe_id,parent_wave_id,parent_turn_job_id,seed_candidate_id,battle_advancement_pool_id,status,created_at_utc,completed_at_utc "
        "FROM ab_turn_wave WHERE wave_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, wave_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    BattleTurnWaveSnapshot out{};
    out.wave_id = sqlite3_column_int64(st.st, 0);
    out.battle_set_id = sqlite3_column_int64(st.st, 1);
    out.turn_index = sqlite3_column_int(st.st, 2);
    out.context_probe_id = ColumnInt64Optional(st.st, 3);
    out.parent_wave_id = ColumnInt64Optional(st.st, 4);
    out.parent_turn_job_id = ColumnInt64Optional(st.st, 5);
    out.seed_candidate_id = sqlite3_column_int64(st.st, 6);
    out.battle_advancement_pool_id = ColumnInt64Optional(st.st, 7);
    out.status = ParseBattleTurnWaveStatus(ColumnText(st.st, 8));
    out.created_at_utc = ColumnTime(st.st, 9);
    out.completed_at_utc = ColumnTimeOptional(st.st, 10);
    return out;
}

std::vector<BattleTurnWaveSnapshot> SqliteAnalysisDb::ListBattleTurnWaves(std::int64_t battle_set_id) const {
    std::vector<BattleTurnWaveSnapshot> rows;
    if (db_ == nullptr || battle_set_id <= 0) {
        return rows;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT wave_id,battle_set_id,turn_index,context_probe_id,parent_wave_id,parent_turn_job_id,seed_candidate_id,battle_advancement_pool_id,status,created_at_utc,completed_at_utc "
        "FROM ab_turn_wave WHERE battle_set_id=?1 ORDER BY turn_index ASC, wave_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, battle_set_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        BattleTurnWaveSnapshot row{};
        row.wave_id = sqlite3_column_int64(st.st, 0);
        row.battle_set_id = sqlite3_column_int64(st.st, 1);
        row.turn_index = sqlite3_column_int(st.st, 2);
        row.context_probe_id = ColumnInt64Optional(st.st, 3);
        row.parent_wave_id = ColumnInt64Optional(st.st, 4);
        row.parent_turn_job_id = ColumnInt64Optional(st.st, 5);
        row.seed_candidate_id = sqlite3_column_int64(st.st, 6);
        row.battle_advancement_pool_id = ColumnInt64Optional(st.st, 7);
        row.status = ParseBattleTurnWaveStatus(ColumnText(st.st, 8));
        row.created_at_utc = ColumnTime(st.st, 9);
        row.completed_at_utc = ColumnTimeOptional(st.st, 10);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<BattleTurnWaveSnapshot> SqliteAnalysisDb::ListBattleTurnWavesForContextProbe(std::int64_t context_probe_id) const {
    std::vector<BattleTurnWaveSnapshot> rows;
    if (db_ == nullptr || context_probe_id <= 0) {
        return rows;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT wave_id,battle_set_id,turn_index,context_probe_id,parent_wave_id,parent_turn_job_id,seed_candidate_id,battle_advancement_pool_id,status,created_at_utc,completed_at_utc "
        "FROM ab_turn_wave WHERE context_probe_id=?1 ORDER BY turn_index ASC, wave_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, context_probe_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        BattleTurnWaveSnapshot row{};
        row.wave_id = sqlite3_column_int64(st.st, 0);
        row.battle_set_id = sqlite3_column_int64(st.st, 1);
        row.turn_index = sqlite3_column_int(st.st, 2);
        row.context_probe_id = ColumnInt64Optional(st.st, 3);
        row.parent_wave_id = ColumnInt64Optional(st.st, 4);
        row.parent_turn_job_id = ColumnInt64Optional(st.st, 5);
        row.seed_candidate_id = sqlite3_column_int64(st.st, 6);
        row.battle_advancement_pool_id = ColumnInt64Optional(st.st, 7);
        row.status = ParseBattleTurnWaveStatus(ColumnText(st.st, 8));
        row.created_at_utc = ColumnTime(st.st, 9);
        row.completed_at_utc = ColumnTimeOptional(st.st, 10);
        rows.push_back(std::move(row));
    }
    return rows;
}

namespace {
BattleContextProbeSnapshot ReadBattleContextProbe(sqlite3_stmt* st) {
    BattleContextProbeSnapshot row{};
    row.context_probe_id = sqlite3_column_int64(st, 0);
    row.wave_id = sqlite3_column_int64(st, 1);
    row.source_savestate_id = sqlite3_column_int64(st, 2);
    row.exec_job_id = ColumnInt64Optional(st, 3);
    row.probe_status = ParseBattleContextProbeStatus(ColumnText(st, 4));
    if (sqlite3_column_type(st, 5) != SQLITE_NULL) {
        row.context_blob = ColumnBlob(st, 5);
    }
    row.context_version = ColumnIntOptional(st, 6);
    row.recorded_at_utc = ColumnTimeOptional(st, 7);
    row.created_at_utc = ColumnTime(st, 8);
    return row;
}

BattleTurnJobSnapshot ReadBattleTurnJob(sqlite3_stmt* st) {
    BattleTurnJobSnapshot row{};
    row.turn_job_id = sqlite3_column_int64(st, 0);
    row.wave_id = sqlite3_column_int64(st, 1);
    row.exec_job_id = ColumnInt64Optional(st, 2);
    row.plan_id = sqlite3_column_int64(st, 3);
    row.source_savestate_id = ColumnInt64Optional(st, 4);
    row.seed_candidate_id = ColumnInt64Optional(st, 5);
    row.authored_plan_id = ColumnInt64Optional(st, 6);
    row.authored_turn_index = ColumnIntOptional(st, 7);
    row.resolved_turn_commands_blob = ColumnTextOptional(st, 8);
    row.resolved_turn_variant_key = ColumnTextOptional(st, 9);
    row.fake_attacks_this_turn = sqlite3_column_int(st, 10);
    row.fake_attacks_used_before = sqlite3_column_int(st, 11);
    row.job_state = ParseBattleTurnJobState(ColumnText(st, 12));
    row.started_at_utc = ColumnTimeOptional(st, 13);
    row.ended_at_utc = ColumnTimeOptional(st, 14);
    row.has_results = sqlite3_column_int(st, 15) != 0;
    row.vi_start = ColumnIntOptional(st, 16);
    row.vi_end = ColumnIntOptional(st, 17);
    row.delta_vi = ColumnIntOptional(st, 18);
    row.rng_seed = ColumnInt64Optional(st, 19);
    if (const auto value = ColumnIntOptional(st, 20); value.has_value()) {
        row.battle_outcome = static_cast<BattleTurnOutcome>(*value);
    }
    row.plan_materialize_err = ColumnIntOptional(st, 21);
    row.pred_passed = ColumnIntOptional(st, 22);
    row.pred_total = ColumnIntOptional(st, 23);
    row.pred_abort_run = ColumnIntOptional(st, 24);
    row.output_savestate_id = ColumnInt64Optional(st, 25);
    row.applied_input_artifact_id = ColumnInt64Optional(st, 26);
    row.input_trace_artifact_id = ColumnInt64Optional(st, 27);
    row.result_context_blob_base64 = ColumnTextOptional(st, 28);
    row.result_context_version = ColumnIntOptional(st, 29);
    row.recorded_at_utc = ColumnTimeOptional(st, 30);
    return row;
}
} // namespace

std::optional<BattleContextProbeSnapshot> SqliteAnalysisDb::GetBattleContextProbe(std::int64_t context_probe_id) const {
    if (db_ == nullptr || context_probe_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT context_probe_id,wave_id,source_savestate_id,exec_job_id,probe_status,context_blob,context_version,recorded_at_utc,created_at_utc "
        "FROM ab_battle_context_probe WHERE context_probe_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, context_probe_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return ReadBattleContextProbe(st.st);
}

std::optional<BattleContextProbeSnapshot> SqliteAnalysisDb::GetBattleContextProbeForExecJob(std::int64_t exec_job_id) const {
    if (db_ == nullptr || exec_job_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT context_probe_id,wave_id,source_savestate_id,exec_job_id,probe_status,context_blob,context_version,recorded_at_utc,created_at_utc "
        "FROM ab_battle_context_probe WHERE exec_job_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, exec_job_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return ReadBattleContextProbe(st.st);
}

std::optional<BattleContextProbeSnapshot> SqliteAnalysisDb::GetLatestBattleContextForWave(std::int64_t wave_id) const {
    if (db_ == nullptr || wave_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT context_probe_id,wave_id,source_savestate_id,exec_job_id,probe_status,context_blob,context_version,recorded_at_utc,created_at_utc "
        "FROM ab_battle_context_probe WHERE wave_id=?1 AND probe_status='SUCCEEDED' AND context_blob IS NOT NULL "
        "ORDER BY COALESCE(recorded_at_utc, created_at_utc) DESC, context_probe_id DESC LIMIT 1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, wave_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return ReadBattleContextProbe(st.st);
}

std::optional<BattleTurnJobSnapshot> SqliteAnalysisDb::GetBattleTurnJob(std::int64_t turn_job_id) const {
    if (db_ == nullptr || turn_job_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT turn_job_id,wave_id,exec_job_id,plan_id,source_savestate_id,seed_candidate_id,authored_plan_id,authored_turn_index,resolved_turn_commands_blob,resolved_turn_variant_key,fake_attacks_this_turn,fake_attacks_used_before,job_state,"
        "started_at_utc,ended_at_utc,has_results,vi_start,vi_end,delta_vi,rng_seed,battle_outcome,plan_materialize_err,"
        "pred_passed,pred_total,pred_abort_run,output_savestate_id,applied_input_artifact_id,input_trace_artifact_id,result_context_blob_base64,result_context_version,recorded_at_utc "
        "FROM ab_turn_job WHERE turn_job_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, turn_job_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return ReadBattleTurnJob(st.st);
}

std::optional<BattleTurnJobSnapshot> SqliteAnalysisDb::GetBattleTurnJobForExecJob(std::int64_t exec_job_id) const {
    if (db_ == nullptr || exec_job_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT turn_job_id,wave_id,exec_job_id,plan_id,source_savestate_id,seed_candidate_id,authored_plan_id,authored_turn_index,resolved_turn_commands_blob,resolved_turn_variant_key,fake_attacks_this_turn,fake_attacks_used_before,job_state,"
        "started_at_utc,ended_at_utc,has_results,vi_start,vi_end,delta_vi,rng_seed,battle_outcome,plan_materialize_err,"
        "pred_passed,pred_total,pred_abort_run,output_savestate_id,applied_input_artifact_id,input_trace_artifact_id,result_context_blob_base64,result_context_version,recorded_at_utc "
        "FROM ab_turn_job WHERE exec_job_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, exec_job_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return ReadBattleTurnJob(st.st);
}

std::vector<BattleTurnJobSnapshot> SqliteAnalysisDb::ListBattleTurnJobsByOutputSavestateId(
    std::int64_t output_savestate_id) const {
    std::vector<BattleTurnJobSnapshot> rows;
    if (db_ == nullptr || output_savestate_id <= 0) {
        return rows;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT turn_job_id,wave_id,exec_job_id,plan_id,source_savestate_id,seed_candidate_id,authored_plan_id,authored_turn_index,resolved_turn_commands_blob,resolved_turn_variant_key,fake_attacks_this_turn,fake_attacks_used_before,job_state,"
        "started_at_utc,ended_at_utc,has_results,vi_start,vi_end,delta_vi,rng_seed,battle_outcome,plan_materialize_err,"
        "pred_passed,pred_total,pred_abort_run,output_savestate_id,applied_input_artifact_id,input_trace_artifact_id,result_context_blob_base64,result_context_version,recorded_at_utc "
        "FROM ab_turn_job WHERE output_savestate_id=?1 ORDER BY turn_job_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, output_savestate_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        rows.push_back(ReadBattleTurnJob(st.st));
    }
    return rows;
}

std::vector<BattleTurnJobSnapshot> SqliteAnalysisDb::ListBattleTurnJobsForWave(std::int64_t wave_id) const {
    std::vector<BattleTurnJobSnapshot> rows;
    if (db_ == nullptr || wave_id <= 0) {
        return rows;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT turn_job_id,wave_id,exec_job_id,plan_id,source_savestate_id,seed_candidate_id,authored_plan_id,authored_turn_index,resolved_turn_commands_blob,resolved_turn_variant_key,fake_attacks_this_turn,fake_attacks_used_before,job_state,"
        "started_at_utc,ended_at_utc,has_results,vi_start,vi_end,delta_vi,rng_seed,battle_outcome,plan_materialize_err,"
        "pred_passed,pred_total,pred_abort_run,output_savestate_id,applied_input_artifact_id,input_trace_artifact_id,result_context_blob_base64,result_context_version,recorded_at_utc "
        "FROM ab_turn_job WHERE wave_id=?1 ORDER BY turn_job_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, wave_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        rows.push_back(ReadBattleTurnJob(st.st));
    }
    return rows;
}

std::vector<BattleTurnJobSnapshot> SqliteAnalysisDb::ListBattleTurnJobsForBattleTurn(
    std::int64_t battle_set_id,
    int turn_index) const {
    std::vector<BattleTurnJobSnapshot> rows;
    if (db_ == nullptr || battle_set_id <= 0 || turn_index <= 0) {
        return rows;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT j.turn_job_id,j.wave_id,j.exec_job_id,j.plan_id,j.source_savestate_id,j.seed_candidate_id,j.authored_plan_id,j.authored_turn_index,j.resolved_turn_commands_blob,j.resolved_turn_variant_key,j.fake_attacks_this_turn,j.fake_attacks_used_before,j.job_state,"
        "j.started_at_utc,j.ended_at_utc,j.has_results,j.vi_start,j.vi_end,j.delta_vi,j.rng_seed,j.battle_outcome,j.plan_materialize_err,"
        "j.pred_passed,j.pred_total,j.pred_abort_run,j.output_savestate_id,j.applied_input_artifact_id,j.input_trace_artifact_id,j.result_context_blob_base64,j.result_context_version,j.recorded_at_utc "
        "FROM ab_turn_job j "
        "JOIN ab_turn_wave w ON w.wave_id=j.wave_id "
        "WHERE w.battle_set_id=?1 AND w.turn_index=?2 "
        "ORDER BY j.turn_job_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, battle_set_id);
    sqlite3_bind_int(st.st, 2, turn_index);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        rows.push_back(ReadBattleTurnJob(st.st));
    }
    return rows;
}

std::vector<BattleAdvancementDecisionRow> SqliteAnalysisDb::ListBattleAdvancementDecisionsForPool(
    std::int64_t battle_advancement_pool_id) const {
    std::vector<BattleAdvancementDecisionRow> rows;
    if (db_ == nullptr || battle_advancement_pool_id <= 0) {
        return rows;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT battle_advancement_decision_id,battle_advancement_pool_id,turn_job_id,decision_kind,decision_reason,created_at_utc "
        "FROM ab_battle_advancement_decision WHERE battle_advancement_pool_id=?1 ORDER BY battle_advancement_decision_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, battle_advancement_pool_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        BattleAdvancementDecisionRow row{};
        row.battle_advancement_decision_id = sqlite3_column_int64(st.st, 0);
        row.battle_advancement_pool_id = sqlite3_column_int64(st.st, 1);
        row.turn_job_id = sqlite3_column_int64(st.st, 2);
        row.decision_kind = ParseBattleAdvancementDecisionKind(ColumnText(st.st, 3));
        const auto* reason = sqlite3_column_text(st.st, 4);
        row.decision_reason = reason ? std::optional<std::string>(reinterpret_cast<const char*>(reason)) : std::nullopt;
        row.created_at_utc = ColumnTime(st.st, 5);
        rows.push_back(std::move(row));
    }
    return rows;
}

bool SqliteAnalysisDb::CreateBattleCompletion(
    const CreateBattleCompletionCommand& command,
    std::int64_t* battle_completion_id_out,
    std::string* error_out) {
    if (battle_completion_id_out) *battle_completion_id_out = 0;
    if (db_ == nullptr || command.workflow_instance_id <= 0 || command.workflow_step_id <= 0
        || command.entry_savestate_id <= 0 || command.status != "QUEUED"
        || (command.exec_job_id.has_value() && *command.exec_job_id <= 0)) {
        if (error_out) *error_out = "invalid battle completion create command";
        return false;
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    auto fail = [&](std::string message) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = std::move(message);
        return false;
    };
    Statement existing;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT battle_completion_id,workflow_instance_id,exec_job_id,entry_savestate_id "
            "FROM ab_battle_completion WHERE workflow_step_id=?1;",
            -1,
            &existing.st,
            nullptr)
        != SQLITE_OK) {
        return fail(sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(existing.st, 1, command.workflow_step_id);
    if (sqlite3_step(existing.st) == SQLITE_ROW) {
        const auto id = sqlite3_column_int64(existing.st, 0);
        if (sqlite3_column_int64(existing.st, 1) != command.workflow_instance_id
            || (command.exec_job_id.has_value()
                && ColumnInt64Optional(existing.st, 2) != command.exec_job_id)
            || sqlite3_column_int64(existing.st, 3) != command.entry_savestate_id) {
            return fail("battle completion workflow step already has different defining fields");
        }
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            return fail(sqlite3_errmsg(db_));
        }
        if (battle_completion_id_out) *battle_completion_id_out = id;
        return true;
    }

    Statement insert;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ab_battle_completion(workflow_instance_id,workflow_step_id,exec_job_id,entry_savestate_id,status,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6);",
            -1,
            &insert.st,
            nullptr)
        != SQLITE_OK) {
        return fail(sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(insert.st, 1, command.workflow_instance_id);
    sqlite3_bind_int64(insert.st, 2, command.workflow_step_id);
    BindOptionalInt64(insert.st, 3, command.exec_job_id);
    sqlite3_bind_int64(insert.st, 4, command.entry_savestate_id);
    sqlite3_bind_text(insert.st, 5, command.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.st, 6, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert.st) != SQLITE_DONE) {
        return fail(sqlite3_errmsg(db_));
    }
    const auto id = sqlite3_last_insert_rowid(db_);
    if (!InsertBattleOutboxEvent(
            db_, "AnalysisBattle.BattleCompletionCreated.v1", "battle_completion", std::to_string(id),
            command.correlation_id, command.causation_id, command.created_at_utc.time_since_epoch().count(),
            "battle_completion", id, error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return fail(sqlite3_errmsg(db_));
    }
    if (battle_completion_id_out) *battle_completion_id_out = id;
    return true;
}

bool SqliteAnalysisDb::BindBattleCompletionExecutionJob(
    const BindBattleCompletionExecutionJobCommand& command,
    std::string* error_out) {
    if (db_ == nullptr || command.battle_completion_id <= 0 || command.workflow_instance_id <= 0
        || command.workflow_step_id <= 0 || command.exec_job_id <= 0) {
        if (error_out) *error_out = "invalid battle completion execution-job binding command";
        return false;
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    auto fail = [&](std::string message) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = std::move(message);
        return false;
    };
    Statement current;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT workflow_instance_id,workflow_step_id,exec_job_id,status,completed_at_utc "
            "FROM ab_battle_completion WHERE battle_completion_id=?1;",
            -1,
            &current.st,
            nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    sqlite3_bind_int64(current.st, 1, command.battle_completion_id);
    if (sqlite3_step(current.st) != SQLITE_ROW) return fail("battle completion row not found");
    if (sqlite3_column_int64(current.st, 0) != command.workflow_instance_id
        || sqlite3_column_int64(current.st, 1) != command.workflow_step_id) {
        return fail("battle completion execution-job binding identity mismatch");
    }
    if (ColumnText(current.st, 3) != "QUEUED" || sqlite3_column_type(current.st, 4) != SQLITE_NULL) {
        return fail("battle completion execution job can only be bound while queued");
    }
    const auto existing_job_id = ColumnInt64Optional(current.st, 2);
    if (existing_job_id.has_value()) {
        if (*existing_job_id != command.exec_job_id) {
            return fail("battle completion is already bound to a different execution job");
        }
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
        return true;
    }
    Statement update;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE ab_battle_completion SET exec_job_id=?2 "
            "WHERE battle_completion_id=?1 AND workflow_instance_id=?3 AND workflow_step_id=?4 "
            "AND exec_job_id IS NULL AND status='QUEUED' AND completed_at_utc IS NULL;",
            -1,
            &update.st,
            nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    sqlite3_bind_int64(update.st, 1, command.battle_completion_id);
    sqlite3_bind_int64(update.st, 2, command.exec_job_id);
    sqlite3_bind_int64(update.st, 3, command.workflow_instance_id);
    sqlite3_bind_int64(update.st, 4, command.workflow_step_id);
    if (sqlite3_step(update.st) != SQLITE_DONE || sqlite3_changes(db_) != 1) {
        return fail("battle completion execution-job binding lost its queued-state precondition");
    }
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    return true;
}

bool SqliteAnalysisDb::CompleteBattleCompletion(
    const CompleteBattleCompletionCommand& command,
    std::string* error_out) {
    if (db_ == nullptr || command.battle_completion_id <= 0 || command.completion_savestate_id <= 0
        || command.manifest_version <= 0 || command.manifest_blob.empty() || command.status != "COMPLETED"
        || command.mismatch_count < 0 || command.invariant_failure_count < 0) {
        if (error_out) *error_out = "invalid battle completion result command";
        return false;
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    auto fail = [&](std::string message) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = std::move(message);
        return false;
    };
    Statement current;
    if (sqlite3_prepare_v2(
        db_,
        "SELECT completion_savestate_id,entry_rng_seed,completion_rng_seed,manifest_version,manifest_blob,"
            "manifest_artifact_id,input_trace_artifact_id,mismatch_count,invariant_failure_count,status,completed_at_utc,exec_job_id "
            "FROM ab_battle_completion WHERE battle_completion_id=?1;",
            -1,
            &current.st,
            nullptr) != SQLITE_OK) {
        return fail(sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(current.st, 1, command.battle_completion_id);
    if (sqlite3_step(current.st) != SQLITE_ROW) {
        return fail("battle completion row not found");
    }
    const auto bound_exec_job_id = ColumnInt64Optional(current.st, 11);
    if (!bound_exec_job_id.has_value() || *bound_exec_job_id <= 0) {
        return fail("battle completion must be bound to an execution job before completion");
    }
    const bool is_terminal = sqlite3_column_type(current.st, 0) != SQLITE_NULL
        || sqlite3_column_type(current.st, 10) != SQLITE_NULL
        || ColumnText(current.st, 9) != "QUEUED";
    if (is_terminal) {
        const bool matches = sqlite3_column_type(current.st, 0) != SQLITE_NULL
            && sqlite3_column_int64(current.st, 0) == command.completion_savestate_id
            && ColumnInt64Optional(current.st, 1) == command.entry_rng_seed
            && ColumnInt64Optional(current.st, 2) == command.completion_rng_seed
            && ColumnIntOptional(current.st, 3) == std::optional<int>(command.manifest_version)
            && sqlite3_column_type(current.st, 4) != SQLITE_NULL
            && ColumnBlob(current.st, 4) == command.manifest_blob
            && ColumnInt64Optional(current.st, 5) == command.manifest_artifact_id
            && ColumnInt64Optional(current.st, 6) == command.input_trace_artifact_id
            && sqlite3_column_int(current.st, 7) == command.mismatch_count
            && sqlite3_column_int(current.st, 8) == command.invariant_failure_count
            && ColumnText(current.st, 9) == command.status
            && ColumnTimeOptional(current.st, 10) == std::optional<types::UtcTimePoint>(command.completed_at_utc);
        if (!matches) {
            return fail("battle completion row is already completed with different result fields");
        }
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
        return true;
    }

    Statement update;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE ab_battle_completion SET completion_savestate_id=?2,entry_rng_seed=?3,completion_rng_seed=?4,"
            "manifest_version=?5,manifest_blob=?6,manifest_artifact_id=?7,input_trace_artifact_id=?8,"
            "mismatch_count=?9,invariant_failure_count=?10,status=?11,completed_at_utc=?12 "
            "WHERE battle_completion_id=?1 AND completion_savestate_id IS NULL "
            "AND exec_job_id IS NOT NULL AND exec_job_id>0 "
            "AND status='QUEUED' AND completed_at_utc IS NULL;",
            -1,
            &update.st,
            nullptr)
        != SQLITE_OK) {
        return fail(sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(update.st, 1, command.battle_completion_id);
    sqlite3_bind_int64(update.st, 2, command.completion_savestate_id);
    BindOptionalInt64(update.st, 3, command.entry_rng_seed);
    BindOptionalInt64(update.st, 4, command.completion_rng_seed);
    sqlite3_bind_int(update.st, 5, command.manifest_version);
    sqlite3_bind_blob(update.st, 6, command.manifest_blob.data(), static_cast<int>(command.manifest_blob.size()), SQLITE_TRANSIENT);
    BindOptionalInt64(update.st, 7, command.manifest_artifact_id);
    BindOptionalInt64(update.st, 8, command.input_trace_artifact_id);
    sqlite3_bind_int(update.st, 9, command.mismatch_count);
    sqlite3_bind_int(update.st, 10, command.invariant_failure_count);
    sqlite3_bind_text(update.st, 11, command.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(update.st, 12, command.completed_at_utc.time_since_epoch().count());
    if (sqlite3_step(update.st) != SQLITE_DONE || sqlite3_changes(db_) != 1) {
        return fail(sqlite3_errmsg(db_));
    }
    if (!InsertBattleOutboxEvent(
            db_, "AnalysisBattle.BattleCompletionCompleted.v1", "battle_completion",
            std::to_string(command.battle_completion_id), command.correlation_id, command.causation_id,
            command.completed_at_utc.time_since_epoch().count(), "battle_completion", command.battle_completion_id, error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    return true;
}

bool SqliteAnalysisDb::FailBattleCompletion(
    const FailBattleCompletionCommand& command,
    std::string* error_out) {
    const bool has_manifest = command.manifest_blob.has_value();
    if (db_ == nullptr || command.battle_completion_id <= 0
        || command.mismatch_count < 0 || command.invariant_failure_count < 0
        || command.manifest_version.has_value() != has_manifest
        || (command.manifest_version.has_value() && *command.manifest_version <= 0)
        || (has_manifest && command.manifest_blob->empty())) {
        if (error_out) *error_out = "invalid battle completion failure command";
        return false;
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    auto fail = [&](std::string message) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = std::move(message);
        return false;
    };
    Statement current;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT completion_savestate_id,manifest_version,manifest_blob,mismatch_count,"
            "invariant_failure_count,status,completed_at_utc "
            "FROM ab_battle_completion WHERE battle_completion_id=?1;",
            -1,
            &current.st,
            nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    sqlite3_bind_int64(current.st, 1, command.battle_completion_id);
    if (sqlite3_step(current.st) != SQLITE_ROW) return fail("battle completion row not found");
    const auto current_status = ColumnText(current.st, 5);
    const bool current_terminal = sqlite3_column_type(current.st, 0) != SQLITE_NULL
        || sqlite3_column_type(current.st, 6) != SQLITE_NULL
        || current_status != "QUEUED";
    if (current_terminal) {
        const auto current_blob = sqlite3_column_type(current.st, 2) == SQLITE_NULL
            ? std::optional<std::string>{}
            : std::optional<std::string>{ColumnBlob(current.st, 2)};
        const bool matches = sqlite3_column_type(current.st, 0) == SQLITE_NULL
            && current_status == "FAILED"
            && ColumnIntOptional(current.st, 1) == command.manifest_version
            && current_blob == command.manifest_blob
            && sqlite3_column_int(current.st, 3) == command.mismatch_count
            && sqlite3_column_int(current.st, 4) == command.invariant_failure_count
            && sqlite3_column_type(current.st, 6) != SQLITE_NULL;
        if (!matches) return fail("battle completion already has a conflicting terminal result");
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
        return true;
    }

    Statement update;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE ab_battle_completion SET manifest_version=?2,manifest_blob=?3,mismatch_count=?4,"
            "invariant_failure_count=?5,status='FAILED',completed_at_utc=?6 "
            "WHERE battle_completion_id=?1 AND completion_savestate_id IS NULL "
            "AND status='QUEUED' AND completed_at_utc IS NULL;",
            -1,
            &update.st,
            nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    sqlite3_bind_int64(update.st, 1, command.battle_completion_id);
    if (command.manifest_version.has_value()) sqlite3_bind_int(update.st, 2, *command.manifest_version);
    else sqlite3_bind_null(update.st, 2);
    if (command.manifest_blob.has_value()) {
        sqlite3_bind_blob(update.st, 3, command.manifest_blob->data(),
            static_cast<int>(command.manifest_blob->size()), SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(update.st, 3);
    }
    sqlite3_bind_int(update.st, 4, command.mismatch_count);
    sqlite3_bind_int(update.st, 5, command.invariant_failure_count);
    sqlite3_bind_int64(update.st, 6, command.completed_at_utc.time_since_epoch().count());
    if (sqlite3_step(update.st) != SQLITE_DONE || sqlite3_changes(db_) != 1) {
        return fail("battle completion failure transition lost its queued-state precondition");
    }
    if (!InsertBattleOutboxEvent(
            db_, "AnalysisBattle.BattleCompletionFailed.v1", "battle_completion",
            std::to_string(command.battle_completion_id), command.correlation_id, command.causation_id,
            command.completed_at_utc.time_since_epoch().count(), "battle_completion",
            command.battle_completion_id, error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    return true;
}

std::optional<BattleCompletionRecord> SqliteAnalysisDb::GetBattleCompletion(
    std::int64_t battle_completion_id) const {
    if (db_ == nullptr || battle_completion_id <= 0) return std::nullopt;
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT battle_completion_id,workflow_instance_id,workflow_step_id,exec_job_id,entry_savestate_id,"
            "completion_savestate_id,entry_rng_seed,completion_rng_seed,manifest_version,manifest_blob,manifest_artifact_id,"
            "input_trace_artifact_id,mismatch_count,invariant_failure_count,status,created_at_utc,completed_at_utc "
            "FROM ab_battle_completion WHERE battle_completion_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(st.st, 1, battle_completion_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    BattleCompletionRecord row{};
    row.battle_completion_id = sqlite3_column_int64(st.st, 0);
    row.workflow_instance_id = sqlite3_column_int64(st.st, 1);
    row.workflow_step_id = sqlite3_column_int64(st.st, 2);
    row.exec_job_id = ColumnInt64Optional(st.st, 3);
    row.entry_savestate_id = sqlite3_column_int64(st.st, 4);
    row.completion_savestate_id = ColumnInt64Optional(st.st, 5);
    row.entry_rng_seed = ColumnInt64Optional(st.st, 6);
    row.completion_rng_seed = ColumnInt64Optional(st.st, 7);
    row.manifest_version = ColumnIntOptional(st.st, 8);
    if (sqlite3_column_type(st.st, 9) != SQLITE_NULL) row.manifest_blob = ColumnBlob(st.st, 9);
    row.manifest_artifact_id = ColumnInt64Optional(st.st, 10);
    row.input_trace_artifact_id = ColumnInt64Optional(st.st, 11);
    row.mismatch_count = sqlite3_column_int(st.st, 12);
    row.invariant_failure_count = sqlite3_column_int(st.st, 13);
    row.status = ColumnText(st.st, 14);
    row.created_at_utc = ColumnTime(st.st, 15);
    row.completed_at_utc = ColumnTimeOptional(st.st, 16);
    return row;
}

bool SqliteAnalysisDb::CreateBattleResults(
    const CreateBattleResultsCommand& command,
    std::int64_t* battle_results_id_out,
    std::string* error_out) {
    if (battle_results_id_out) *battle_results_id_out = 0;
    const auto effect = ToDbString(command.rng_effect_kind);
    const bool valid_effect_count =
        (command.rng_effect_kind == RngEffectKind::Preserve && command.fixed_draw_count == std::optional<std::int64_t>(0))
        || (command.rng_effect_kind == RngEffectKind::AdvanceFixed && command.fixed_draw_count.has_value() && *command.fixed_draw_count >= 0)
        || (command.rng_effect_kind == RngEffectKind::Variable && !command.fixed_draw_count.has_value());
    const bool valid_seed_ref =
        command.selected_seed_ref_kind == "analysisseedprobe.confirmed_result";
    if (db_ == nullptr || command.battle_completion_id <= 0 || command.workflow_instance_id <= 0
        || command.workflow_step_id <= 0 || command.entry_savestate_id <= 0 || command.selected_seed_ref_id <= 0
        || command.selected_seed_value < 0
        || command.selected_seed_value > static_cast<std::int64_t>((std::numeric_limits<std::uint32_t>::max)())
        || command.status != "QUEUED" || effect.empty() || !valid_effect_count || !valid_seed_ref
        || (command.exec_job_id.has_value() && *command.exec_job_id <= 0)) {
        if (error_out) *error_out = "invalid battle results create command";
        return false;
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    auto fail = [&](std::string message) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = std::move(message);
        return false;
    };
    Statement completion_check;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT workflow_instance_id,completion_savestate_id,status "
            "FROM ab_battle_completion WHERE battle_completion_id=?1;",
            -1,
            &completion_check.st,
            nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    sqlite3_bind_int64(completion_check.st, 1, command.battle_completion_id);
    if (sqlite3_step(completion_check.st) != SQLITE_ROW) {
        return fail("battle_completion_id does not resolve to a completion aggregate");
    }
    if (sqlite3_column_int64(completion_check.st, 0) != command.workflow_instance_id
        || sqlite3_column_type(completion_check.st, 1) == SQLITE_NULL
        || ColumnText(completion_check.st, 2) != "COMPLETED") {
        return fail("battle results require a completed aggregate in the same workflow");
    }
    const auto completion_savestate_id = sqlite3_column_int64(completion_check.st, 1);

    Statement seed_check;
    constexpr const char* seed_sql =
        "SELECT r.seed_value,pr.entry_savestate_id,pr.codec_version,ps.probe_flavor "
        "FROM sp_probe_result r "
        "JOIN sp_probe_run pr ON pr.probe_run_id=r.probe_run_id "
        "JOIN sp_probe_set ps ON ps.probe_set_id=pr.probe_set_id "
        "WHERE r.probe_result_id=?1 AND r.evidence_state='CONFIRMED' "
        "AND r.confirmation_of_probe_result_id IS NULL;";
    if (sqlite3_prepare_v2(db_, seed_sql, -1, &seed_check.st, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    sqlite3_bind_int64(seed_check.st, 1, command.selected_seed_ref_id);
    if (sqlite3_step(seed_check.st) != SQLITE_ROW) {
        return fail("selected seed reference does not resolve to a seed row");
    }
    if (sqlite3_column_int64(seed_check.st, 0) != command.selected_seed_value) {
        return fail("selected seed value does not match the referenced seed row");
    }
    if (sqlite3_column_int64(seed_check.st, 1) != completion_savestate_id) {
        return fail("selected seed was not probed from the completion savestate");
    }
    if (sqlite3_column_int(seed_check.st, 2) != 2 || ColumnText(seed_check.st, 3) != "FIELD_RETURN") {
        return fail("selected seed must come from a codec-v2 FIELD_RETURN probe run");
    }
    Statement existing;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT battle_results_id,battle_completion_id,workflow_instance_id,exec_job_id,selected_seed_ref_kind,"
            "selected_seed_ref_id,entry_savestate_id,selected_seed_value,rng_effect_kind,fixed_draw_count "
            "FROM ab_battle_results WHERE workflow_step_id=?1;",
            -1,
            &existing.st,
            nullptr)
        != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    sqlite3_bind_int64(existing.st, 1, command.workflow_step_id);
    if (sqlite3_step(existing.st) == SQLITE_ROW) {
        const auto id = sqlite3_column_int64(existing.st, 0);
        if (sqlite3_column_int64(existing.st, 1) != command.battle_completion_id
            || sqlite3_column_int64(existing.st, 2) != command.workflow_instance_id
            || (command.exec_job_id.has_value()
                && ColumnInt64Optional(existing.st, 3) != command.exec_job_id)
            || ColumnText(existing.st, 4) != command.selected_seed_ref_kind
            || sqlite3_column_int64(existing.st, 5) != command.selected_seed_ref_id
            || sqlite3_column_int64(existing.st, 6) != command.entry_savestate_id
            || sqlite3_column_int64(existing.st, 7) != command.selected_seed_value
            || ColumnText(existing.st, 8) != effect
            || ColumnInt64Optional(existing.st, 9) != command.fixed_draw_count) {
            return fail("battle results workflow step already has different defining fields");
        }
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
        if (battle_results_id_out) *battle_results_id_out = id;
        return true;
    }

    Statement insert;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ab_battle_results(battle_completion_id,workflow_instance_id,workflow_step_id,exec_job_id,"
            "selected_seed_ref_kind,selected_seed_ref_id,entry_savestate_id,selected_seed_value,rng_effect_kind,fixed_draw_count,status,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12);",
            -1,
            &insert.st,
            nullptr)
        != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    sqlite3_bind_int64(insert.st, 1, command.battle_completion_id);
    sqlite3_bind_int64(insert.st, 2, command.workflow_instance_id);
    sqlite3_bind_int64(insert.st, 3, command.workflow_step_id);
    BindOptionalInt64(insert.st, 4, command.exec_job_id);
    sqlite3_bind_text(insert.st, 5, command.selected_seed_ref_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.st, 6, command.selected_seed_ref_id);
    sqlite3_bind_int64(insert.st, 7, command.entry_savestate_id);
    sqlite3_bind_int64(insert.st, 8, command.selected_seed_value);
    sqlite3_bind_text(insert.st, 9, effect.data(), static_cast<int>(effect.size()), SQLITE_TRANSIENT);
    BindOptionalInt64(insert.st, 10, command.fixed_draw_count);
    sqlite3_bind_text(insert.st, 11, command.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.st, 12, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert.st) != SQLITE_DONE) return fail(sqlite3_errmsg(db_));
    const auto id = sqlite3_last_insert_rowid(db_);
    if (!InsertBattleOutboxEvent(
            db_, "AnalysisBattle.BattleResultsCreated.v1", "battle_results", std::to_string(id),
            command.correlation_id, command.causation_id, command.created_at_utc.time_since_epoch().count(),
            "battle_results", id, error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    if (battle_results_id_out) *battle_results_id_out = id;
    return true;
}

bool SqliteAnalysisDb::BindBattleResultsExecutionJob(
    const BindBattleResultsExecutionJobCommand& command,
    std::string* error_out) {
    if (db_ == nullptr || command.battle_results_id <= 0 || command.workflow_instance_id <= 0
        || command.workflow_step_id <= 0 || command.exec_job_id <= 0) {
        if (error_out) *error_out = "invalid battle results execution-job binding command";
        return false;
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    auto fail = [&](std::string message) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = std::move(message);
        return false;
    };
    Statement current;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT workflow_instance_id,workflow_step_id,exec_job_id,status,completed_at_utc "
            "FROM ab_battle_results WHERE battle_results_id=?1;",
            -1,
            &current.st,
            nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    sqlite3_bind_int64(current.st, 1, command.battle_results_id);
    if (sqlite3_step(current.st) != SQLITE_ROW) return fail("battle results row not found");
    if (sqlite3_column_int64(current.st, 0) != command.workflow_instance_id
        || sqlite3_column_int64(current.st, 1) != command.workflow_step_id) {
        return fail("battle results execution-job binding identity mismatch");
    }
    if (ColumnText(current.st, 3) != "QUEUED" || sqlite3_column_type(current.st, 4) != SQLITE_NULL) {
        return fail("battle results execution job can only be bound while queued");
    }
    const auto existing_job_id = ColumnInt64Optional(current.st, 2);
    if (existing_job_id.has_value()) {
        if (*existing_job_id != command.exec_job_id) {
            return fail("battle results are already bound to a different execution job");
        }
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
        return true;
    }
    Statement update;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE ab_battle_results SET exec_job_id=?2 "
            "WHERE battle_results_id=?1 AND workflow_instance_id=?3 AND workflow_step_id=?4 "
            "AND exec_job_id IS NULL AND status='QUEUED' AND completed_at_utc IS NULL;",
            -1,
            &update.st,
            nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    sqlite3_bind_int64(update.st, 1, command.battle_results_id);
    sqlite3_bind_int64(update.st, 2, command.exec_job_id);
    sqlite3_bind_int64(update.st, 3, command.workflow_instance_id);
    sqlite3_bind_int64(update.st, 4, command.workflow_step_id);
    if (sqlite3_step(update.st) != SQLITE_DONE || sqlite3_changes(db_) != 1) {
        return fail("battle results execution-job binding lost its queued-state precondition");
    }
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    return true;
}

bool SqliteAnalysisDb::CompleteBattleResults(
    const CompleteBattleResultsCommand& command,
    std::string* error_out) {
    if (db_ == nullptr || command.battle_results_id <= 0 || command.final_savestate_id <= 0
        || command.status != "COMPLETED" || command.mismatch_count < 0 || command.invariant_failure_count < 0) {
        if (error_out) *error_out = "invalid battle results completion command";
        return false;
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    auto fail = [&](std::string message) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = std::move(message);
        return false;
    };
    Statement current;
    if (sqlite3_prepare_v2(
        db_,
        "SELECT final_savestate_id,entry_rng_seed,final_rng_seed,result_artifact_id,input_trace_artifact_id,"
            "mismatch_count,invariant_failure_count,status,completed_at_utc,exec_job_id "
            "FROM ab_battle_results WHERE battle_results_id=?1;",
            -1,
            &current.st,
            nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    sqlite3_bind_int64(current.st, 1, command.battle_results_id);
    if (sqlite3_step(current.st) != SQLITE_ROW) return fail("battle results row not found");
    const auto bound_exec_job_id = ColumnInt64Optional(current.st, 9);
    if (!bound_exec_job_id.has_value() || *bound_exec_job_id <= 0) {
        return fail("battle results must be bound to an execution job before completion");
    }
    const bool is_terminal = sqlite3_column_type(current.st, 0) != SQLITE_NULL
        || sqlite3_column_type(current.st, 8) != SQLITE_NULL
        || ColumnText(current.st, 7) != "QUEUED";
    if (is_terminal) {
        const bool matches = sqlite3_column_type(current.st, 0) != SQLITE_NULL
            && sqlite3_column_int64(current.st, 0) == command.final_savestate_id
            && ColumnInt64Optional(current.st, 1) == command.entry_rng_seed
            && ColumnInt64Optional(current.st, 2) == command.final_rng_seed
            && ColumnInt64Optional(current.st, 3) == command.result_artifact_id
            && ColumnInt64Optional(current.st, 4) == command.input_trace_artifact_id
            && sqlite3_column_int(current.st, 5) == command.mismatch_count
            && sqlite3_column_int(current.st, 6) == command.invariant_failure_count
            && ColumnText(current.st, 7) == command.status
            && ColumnTimeOptional(current.st, 8) == std::optional<types::UtcTimePoint>(command.completed_at_utc);
        if (!matches) return fail("battle results row is already completed with different result fields");
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
        return true;
    }
    Statement update;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE ab_battle_results SET final_savestate_id=?2,entry_rng_seed=?3,final_rng_seed=?4,result_artifact_id=?5,"
            "input_trace_artifact_id=?6,mismatch_count=?7,invariant_failure_count=?8,status=?9,completed_at_utc=?10 "
            "WHERE battle_results_id=?1 AND final_savestate_id IS NULL "
            "AND exec_job_id IS NOT NULL AND exec_job_id>0 "
            "AND status='QUEUED' AND completed_at_utc IS NULL;",
            -1,
            &update.st,
            nullptr)
        != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    sqlite3_bind_int64(update.st, 1, command.battle_results_id);
    sqlite3_bind_int64(update.st, 2, command.final_savestate_id);
    BindOptionalInt64(update.st, 3, command.entry_rng_seed);
    BindOptionalInt64(update.st, 4, command.final_rng_seed);
    BindOptionalInt64(update.st, 5, command.result_artifact_id);
    BindOptionalInt64(update.st, 6, command.input_trace_artifact_id);
    sqlite3_bind_int(update.st, 7, command.mismatch_count);
    sqlite3_bind_int(update.st, 8, command.invariant_failure_count);
    sqlite3_bind_text(update.st, 9, command.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(update.st, 10, command.completed_at_utc.time_since_epoch().count());
    if (sqlite3_step(update.st) != SQLITE_DONE || sqlite3_changes(db_) != 1) return fail(sqlite3_errmsg(db_));
    if (!InsertBattleOutboxEvent(
            db_, "AnalysisBattle.BattleResultsCompleted.v1", "battle_results", std::to_string(command.battle_results_id),
            command.correlation_id, command.causation_id, command.completed_at_utc.time_since_epoch().count(),
            "battle_results", command.battle_results_id, error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    return true;
}

bool SqliteAnalysisDb::FailBattleResults(
    const FailBattleResultsCommand& command,
    std::string* error_out) {
    if (db_ == nullptr || command.battle_results_id <= 0
        || command.mismatch_count < 0 || command.invariant_failure_count < 0
        || (command.result_artifact_id.has_value() && *command.result_artifact_id <= 0)
        || (command.input_trace_artifact_id.has_value() && *command.input_trace_artifact_id <= 0)) {
        if (error_out) *error_out = "invalid battle results failure command";
        return false;
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    auto fail = [&](std::string message) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = std::move(message);
        return false;
    };
    Statement current;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT final_savestate_id,entry_rng_seed,final_rng_seed,result_artifact_id,input_trace_artifact_id,"
            "mismatch_count,invariant_failure_count,status,completed_at_utc "
            "FROM ab_battle_results WHERE battle_results_id=?1;",
            -1,
            &current.st,
            nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    sqlite3_bind_int64(current.st, 1, command.battle_results_id);
    if (sqlite3_step(current.st) != SQLITE_ROW) return fail("battle results row not found");
    const auto current_status = ColumnText(current.st, 7);
    const bool current_terminal = sqlite3_column_type(current.st, 0) != SQLITE_NULL
        || sqlite3_column_type(current.st, 8) != SQLITE_NULL
        || current_status != "QUEUED";
    if (current_terminal) {
        const bool matches = sqlite3_column_type(current.st, 0) == SQLITE_NULL
            && current_status == "FAILED"
            && ColumnInt64Optional(current.st, 1) == command.entry_rng_seed
            && ColumnInt64Optional(current.st, 2) == command.final_rng_seed
            && ColumnInt64Optional(current.st, 3) == command.result_artifact_id
            && ColumnInt64Optional(current.st, 4) == command.input_trace_artifact_id
            && sqlite3_column_int(current.st, 5) == command.mismatch_count
            && sqlite3_column_int(current.st, 6) == command.invariant_failure_count
            && sqlite3_column_type(current.st, 8) != SQLITE_NULL;
        if (!matches) return fail("battle results already have a conflicting terminal result");
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
        return true;
    }

    Statement update;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE ab_battle_results SET entry_rng_seed=?2,final_rng_seed=?3,result_artifact_id=?4,"
            "input_trace_artifact_id=?5,mismatch_count=?6,invariant_failure_count=?7,"
            "status='FAILED',completed_at_utc=?8 "
            "WHERE battle_results_id=?1 AND final_savestate_id IS NULL "
            "AND status='QUEUED' AND completed_at_utc IS NULL;",
            -1,
            &update.st,
            nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    sqlite3_bind_int64(update.st, 1, command.battle_results_id);
    BindOptionalInt64(update.st, 2, command.entry_rng_seed);
    BindOptionalInt64(update.st, 3, command.final_rng_seed);
    BindOptionalInt64(update.st, 4, command.result_artifact_id);
    BindOptionalInt64(update.st, 5, command.input_trace_artifact_id);
    sqlite3_bind_int(update.st, 6, command.mismatch_count);
    sqlite3_bind_int(update.st, 7, command.invariant_failure_count);
    sqlite3_bind_int64(update.st, 8, command.completed_at_utc.time_since_epoch().count());
    if (sqlite3_step(update.st) != SQLITE_DONE || sqlite3_changes(db_) != 1) {
        return fail("battle results failure transition lost its queued-state precondition");
    }
    if (!InsertBattleOutboxEvent(
            db_, "AnalysisBattle.BattleResultsFailed.v1", "battle_results",
            std::to_string(command.battle_results_id), command.correlation_id, command.causation_id,
            command.completed_at_utc.time_since_epoch().count(), "battle_results",
            command.battle_results_id, error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    return true;
}

std::optional<BattleResultsRecord> SqliteAnalysisDb::GetBattleResults(std::int64_t battle_results_id) const {
    if (db_ == nullptr || battle_results_id <= 0) return std::nullopt;
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT battle_results_id,battle_completion_id,workflow_instance_id,workflow_step_id,exec_job_id,"
            "selected_seed_ref_kind,selected_seed_ref_id,entry_savestate_id,final_savestate_id,selected_seed_value,"
            "entry_rng_seed,final_rng_seed,rng_effect_kind,fixed_draw_count,result_artifact_id,input_trace_artifact_id,"
            "mismatch_count,invariant_failure_count,status,created_at_utc,completed_at_utc "
            "FROM ab_battle_results WHERE battle_results_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(st.st, 1, battle_results_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    BattleResultsRecord row{};
    row.battle_results_id = sqlite3_column_int64(st.st, 0);
    row.battle_completion_id = sqlite3_column_int64(st.st, 1);
    row.workflow_instance_id = sqlite3_column_int64(st.st, 2);
    row.workflow_step_id = sqlite3_column_int64(st.st, 3);
    row.exec_job_id = ColumnInt64Optional(st.st, 4);
    row.selected_seed_ref_kind = ColumnText(st.st, 5);
    row.selected_seed_ref_id = sqlite3_column_int64(st.st, 6);
    row.entry_savestate_id = sqlite3_column_int64(st.st, 7);
    row.final_savestate_id = ColumnInt64Optional(st.st, 8);
    row.selected_seed_value = sqlite3_column_int64(st.st, 9);
    row.entry_rng_seed = ColumnInt64Optional(st.st, 10);
    row.final_rng_seed = ColumnInt64Optional(st.st, 11);
    row.rng_effect_kind = ParseRngEffectKind(ColumnText(st.st, 12));
    row.fixed_draw_count = ColumnInt64Optional(st.st, 13);
    row.result_artifact_id = ColumnInt64Optional(st.st, 14);
    row.input_trace_artifact_id = ColumnInt64Optional(st.st, 15);
    row.mismatch_count = sqlite3_column_int(st.st, 16);
    row.invariant_failure_count = sqlite3_column_int(st.st, 17);
    row.status = ColumnText(st.st, 18);
    row.created_at_utc = ColumnTime(st.st, 19);
    row.completed_at_utc = ColumnTimeOptional(st.st, 20);
    return row;
}

std::vector<events::EventEnvelope> SqliteAnalysisDb::ReadUnpublishedOutboxBatch(
    std::int64_t after_outbox_id,
    int max_batch_size) {
    std::vector<events::EventEnvelope> batch;
    if (db_ == nullptr || max_batch_size <= 0) {
        return batch;
    }

    const auto mode = ResolveOutboxMode(db_);
    if (mode == OutboxMode::None) {
        return batch;
    }

    Statement st;
    if (mode == OutboxMode::AnalysisSpine) {
        constexpr auto* kSql =
            "SELECT event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,"
            "correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id "
            "FROM asp_outbox_message "
            "WHERE outbox_id > ?1 "
            "AND published_at_utc IS NULL "
            "ORDER BY outbox_id ASC LIMIT ?2;";

        if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
            return batch;
        }

        sqlite3_bind_int64(st.st, 1, after_outbox_id);
        sqlite3_bind_int(st.st, 2, max_batch_size);

        while (sqlite3_step(st.st) == SQLITE_ROW) {
            batch.push_back(ReadEnvelope(st.st));
        }

        return batch;
    }

    const bool has_sp = TableExists(db_, "sp_outbox_message");
    const bool has_ab = TableExists(db_, "ab_outbox_message");
    if (!has_sp && !has_ab) {
        return batch;
    }

    std::string sql =
        "SELECT event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,"
        "correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id "
        "FROM (";
    if (has_sp) {
        sql +=
            "SELECT (outbox_id * 2) AS synthetic_outbox_id, "
            "event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,"
            "correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id "
            "FROM sp_outbox_message WHERE published_at_utc IS NULL ";
    }
    if (has_sp && has_ab) {
        sql += "UNION ALL ";
    }
    if (has_ab) {
        sql +=
            "SELECT (outbox_id * 2) + 1 AS synthetic_outbox_id, "
            "event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,"
            "correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id "
            "FROM ab_outbox_message WHERE published_at_utc IS NULL";
    }
    sql += ") q WHERE q.synthetic_outbox_id > ?1 ORDER BY q.synthetic_outbox_id ASC LIMIT ?2;";

    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st.st, nullptr) != SQLITE_OK) {
        return batch;
    }

    sqlite3_bind_int64(st.st, 1, after_outbox_id);
    sqlite3_bind_int(st.st, 2, max_batch_size);

    while (sqlite3_step(st.st) == SQLITE_ROW) {
        batch.push_back(ReadEnvelope(st.st));
    }

    return batch;
}

bool SqliteAnalysisDb::MarkOutboxPublished(
    std::int64_t outbox_id,
    types::UtcTimePoint published_at_utc) {
    if (db_ == nullptr || outbox_id <= 0) {
        return false;
    }

    const auto mode = ResolveOutboxMode(db_);
    if (mode == OutboxMode::None) {
        return false;
    }

    Statement st;
    if (mode == OutboxMode::AnalysisSpine) {
        constexpr auto* kSql =
            "UPDATE asp_outbox_message "
            "SET published_at_utc=?2 "
            "WHERE outbox_id=?1;";
        if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
            return false;
        }

        sqlite3_bind_int64(st.st, 1, outbox_id);
        sqlite3_bind_int64(st.st, 2, published_at_utc.time_since_epoch().count());
        return sqlite3_step(st.st) == SQLITE_DONE;
    }

    const bool is_ab = (outbox_id % 2) == 1;
    const auto table_outbox_id = outbox_id / 2;
    if (table_outbox_id <= 0) {
        return false;
    }

    if ((is_ab && !TableExists(db_, "ab_outbox_message")) || (!is_ab && !TableExists(db_, "sp_outbox_message"))) {
        return false;
    }

    const auto* sql = is_ab
        ? "UPDATE ab_outbox_message SET published_at_utc=?2 WHERE outbox_id=?1;"
        : "UPDATE sp_outbox_message SET published_at_utc=?2 WHERE outbox_id=?1;";

    if (sqlite3_prepare_v2(db_, sql, -1, &st.st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st.st, 1, table_outbox_id);
    sqlite3_bind_int64(st.st, 2, published_at_utc.time_since_epoch().count());
    return sqlite3_step(st.st) == SQLITE_DONE;
}

bool SqliteAnalysisDb::MarkOutboxPublishFailure(
    std::int64_t outbox_id,
    std::string_view last_error) {
    if (db_ == nullptr || outbox_id <= 0) {
        return false;
    }

    const auto mode = ResolveOutboxMode(db_);
    if (mode == OutboxMode::None) {
        return false;
    }

    Statement st;
    if (mode == OutboxMode::AnalysisSpine) {
        constexpr auto* kSql =
            "UPDATE asp_outbox_message "
            "SET attempt_count=attempt_count+1, last_error=?2 "
            "WHERE outbox_id=?1;";
        if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
            return false;
        }

        sqlite3_bind_int64(st.st, 1, outbox_id);
        sqlite3_bind_text(st.st, 2, last_error.data(), static_cast<int>(last_error.size()), SQLITE_TRANSIENT);
        return sqlite3_step(st.st) == SQLITE_DONE;
    }

    const bool is_ab = (outbox_id % 2) == 1;
    const auto table_outbox_id = outbox_id / 2;
    if (table_outbox_id <= 0) {
        return false;
    }

    if ((is_ab && !TableExists(db_, "ab_outbox_message")) || (!is_ab && !TableExists(db_, "sp_outbox_message"))) {
        return false;
    }

    const auto* sql = is_ab
        ? "UPDATE ab_outbox_message SET attempt_count=attempt_count+1, last_error=?2 WHERE outbox_id=?1;"
        : "UPDATE sp_outbox_message SET attempt_count=attempt_count+1, last_error=?2 WHERE outbox_id=?1;";

    if (sqlite3_prepare_v2(db_, sql, -1, &st.st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st.st, 1, table_outbox_id);
    sqlite3_bind_text(st.st, 2, last_error.data(), static_cast<int>(last_error.size()), SQLITE_TRANSIENT);
    return sqlite3_step(st.st) == SQLITE_DONE;
}

retention::OutboxRetentionPreview SqliteAnalysisDb::PreviewOutboxRetention(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy) const {
    std::int64_t max_outbox_id = 0;
    Statement st;
    const auto mode = ResolveOutboxMode(db_);
    if (mode == OutboxMode::AnalysisSpine) {
        if (sqlite3_prepare_v2(db_, "SELECT COALESCE(MAX(outbox_id), 0) FROM asp_outbox_message;", -1, &st.st, nullptr)
                == SQLITE_OK
            && sqlite3_step(st.st) == SQLITE_ROW) {
            max_outbox_id = sqlite3_column_int64(st.st, 0);
        }
    }
    else if (mode == OutboxMode::SplitSeedProbeBattle) {
        if (sqlite3_prepare_v2(
                db_,
                "SELECT MAX(v) FROM ("
                "SELECT COALESCE(MAX(outbox_id),0) AS v FROM sp_outbox_message "
                "UNION ALL "
                "SELECT COALESCE(MAX(outbox_id),0) AS v FROM ab_outbox_message"
                ");",
                -1,
                &st.st,
                nullptr)
                == SQLITE_OK
            && sqlite3_step(st.st) == SQLITE_ROW) {
            max_outbox_id = sqlite3_column_int64(st.st, 0);
        }
    }
    return retention::BuildOutboxRetentionPreview(max_outbox_id, subscriptions, now_utc, policy);
}

bool SqliteAnalysisDb::PurgeOutboxThroughRetentionFloor(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy,
    int max_rows,
    int* rows_deleted_out,
    std::string* error_out) {
    if (rows_deleted_out) *rows_deleted_out = 0;
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (max_rows <= 0) {
        if (error_out) *error_out = "max_rows must be > 0";
        return false;
    }

    const auto preview = PreviewOutboxRetention(subscriptions, now_utc, policy);
    if (preview.IsPurgeBlocked()) {
        if (error_out) *error_out = "purge blocked by required paused/error subscriptions";
        return false;
    }
    if (!preview.safe_purge_floor_outbox_id.has_value()) {
        if (error_out) *error_out = "safe purge floor unavailable";
        return false;
    }

    int deleted = 0;
    const auto mode = ResolveOutboxMode(db_);
    auto delete_from = [&](const char* table_name, int limit) -> bool {
        Statement st;
        std::string sql =
            "DELETE FROM " + std::string(table_name)
            + " WHERE outbox_id IN (SELECT outbox_id FROM " + std::string(table_name)
            + " WHERE published_at_utc IS NOT NULL AND outbox_id < ?1 ORDER BY outbox_id ASC LIMIT ?2);";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st.st, nullptr) != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(st.st, 1, preview.safe_purge_floor_outbox_id.value());
        sqlite3_bind_int(st.st, 2, limit);
        if (sqlite3_step(st.st) != SQLITE_DONE) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        deleted += sqlite3_changes(db_);
        return true;
    };

    if (mode == OutboxMode::AnalysisSpine) {
        if (!delete_from("asp_outbox_message", max_rows)) return false;
    }
    else if (mode == OutboxMode::SplitSeedProbeBattle) {
        const int sp_limit = std::max(1, max_rows / 2);
        const int ab_limit = std::max(1, max_rows - sp_limit);
        if (TableExists(db_, "sp_outbox_message") && !delete_from("sp_outbox_message", sp_limit)) return false;
        if (TableExists(db_, "ab_outbox_message") && !delete_from("ab_outbox_message", ab_limit)) return false;
    }

    if (rows_deleted_out) *rows_deleted_out = deleted;
    return true;
}

std::optional<SeedProbePayloadRecord> SqliteAnalysisDb::ResolveSeedProbePayload(
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    return ResolveSeedProbeByKind(seed_probe_row_resolver_, event_version, payload_ref_kind, payload_ref_id);
}

std::optional<SeedProbePayloadRecord> SqliteAnalysisDb::ResolveSeedProbePayload(
    const events::EventEnvelope& envelope) const {
    if (!events::ValidateAnalysisSeedProbePayloadV1(envelope)) {
        return std::nullopt;
    }

    const auto contract = events::ResolvePayloadResolverContract(envelope.event_type, envelope.event_version);
    if (!contract.has_value() || contract.value() != events::PayloadResolverContract::AnalysisSeedProbeV1) {
        return std::nullopt;
    }

    if (envelope.event_type == "AnalysisSeedProbe.SetCreated.v1") {
        const auto view = seed_probe_row_resolver_.ResolveSeedProbeSetCreated(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        SeedProbePayloadRecord record{};
        record.probe_set_id = view->probe_set_id;
        return record;
    }
    if (envelope.event_type == "AnalysisSeedProbe.RunRequested.v1"
        || envelope.event_type == "AnalysisSeedProbe.AcceptedInputFramesReplaced.v1"
        || envelope.event_type == "AnalysisSeedProbe.RunStatusChanged.v1"
        || envelope.event_type == "AnalysisSeedProbe.RunCompleted.v1"
        || envelope.event_type == "AnalysisSeedProbe.RunFailed.v1") {
        const auto view = seed_probe_row_resolver_.ResolveSeedProbeRunRequested(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        SeedProbePayloadRecord record{};
        record.probe_set_id = view->probe_set_id;
        record.probe_run_id = view->probe_run_id;
        return record;
    }
    if (envelope.event_type == "AnalysisSeedProbe.ObservationRecorded.v1"
        || envelope.event_type == "AnalysisSeedProbe.EvidenceStateChanged.v1") {
        const auto view = seed_probe_row_resolver_.ResolveSeedProbeResult(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        SeedProbePayloadRecord record{};
        record.probe_run_id = view->probe_run_id;
        record.probe_result_id = view->probe_result_id;
        return record;
    }
    if (envelope.event_type == "AnalysisSeedProbe.EncounterProjectionRecorded.v1") {
        const auto view = seed_probe_row_resolver_.ResolveSeedProbeEncounterProjectionRecorded(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        SeedProbePayloadRecord record{};
        record.probe_run_id = view->probe_run_id;
        return record;
    }
    return std::nullopt;
}

std::optional<BattlePayloadRecord> SqliteAnalysisDb::ResolveBattlePayload(
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    return ResolveBattleByKind(battle_row_resolver_, event_version, payload_ref_kind, payload_ref_id);
}

std::optional<BattlePayloadRecord> SqliteAnalysisDb::ResolveBattlePayload(
    const events::EventEnvelope& envelope) const {
    if (!events::ValidateAnalysisBattlePayloadV1(envelope)) {
        return std::nullopt;
    }

    const auto contract = events::ResolvePayloadResolverContract(envelope.event_type, envelope.event_version);
    if (!contract.has_value() || contract.value() != events::PayloadResolverContract::AnalysisBattleV1) {
        return std::nullopt;
    }

    if (envelope.event_type == "AnalysisBattle.BattleSetCreated.v1") {
        const auto view = battle_row_resolver_.ResolveBattleSetCreated(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        BattlePayloadRecord record{};
        record.battle_set_id = view->battle_set_id;
        return record;
    }
    if (envelope.event_type == "AnalysisBattle.SeedCandidateAdded.v1") {
        const auto view = battle_row_resolver_.ResolveBattleSeedCandidateAdded(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        BattlePayloadRecord record{};
        record.battle_set_id = view->battle_set_id;
        return record;
    }
    if (envelope.event_type == "AnalysisBattle.TurnWaveCreated.v1") {
        const auto view = battle_row_resolver_.ResolveBattleTurnWaveCreated(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        BattlePayloadRecord record{};
        record.battle_set_id = view->battle_set_id;
        record.wave_id = view->wave_id;
        return record;
    }
    if (envelope.event_type == "AnalysisBattle.TurnJobRecorded.v1") {
        const auto view = battle_row_resolver_.ResolveBattleTurnJobRecorded(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        BattlePayloadRecord record{};
        record.battle_set_id = view->battle_set_id;
        record.wave_id = view->wave_id;
        record.turn_job_id = view->turn_job_id;
        return record;
    }
    if (envelope.event_type == "AnalysisBattle.TurnJobResultUpdated.v1") {
        const auto view = battle_row_resolver_.ResolveBattleTurnJobResultUpdated(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        BattlePayloadRecord record{};
        record.battle_set_id = view->battle_set_id;
        record.wave_id = view->wave_id;
        record.turn_job_id = view->turn_job_id;
        return record;
    }
    if (envelope.event_type == "AnalysisBattle.TurnWaveStatusUpdated.v1") {
        const auto view = battle_row_resolver_.ResolveBattleTurnWaveStatusUpdated(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        BattlePayloadRecord record{};
        record.battle_set_id = view->battle_set_id;
        record.wave_id = view->wave_id;
        return record;
    }
    if (envelope.event_type == "AnalysisBattle.BattleSetStatusUpdated.v1") {
        const auto view = battle_row_resolver_.ResolveBattleSetStatusUpdated(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        BattlePayloadRecord record{};
        record.battle_set_id = view->battle_set_id;
        return record;
    }
    if (envelope.event_type == "AnalysisBattle.BattleAdvancementPoolCreated.v1") {
        const auto view = battle_row_resolver_.ResolveBattleBattleAdvancementPoolCreated(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        BattlePayloadRecord record{};
        record.battle_set_id = view->battle_set_id;
        return record;
    }
    if (envelope.event_type == "AnalysisBattle.BattleAdvancementDecisionRecorded.v1") {
        const auto view = battle_row_resolver_.ResolveBattleBattleAdvancementDecisionRecorded(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        BattlePayloadRecord record{};
        record.battle_set_id = view->battle_set_id;
        record.turn_job_id = view->turn_job_id;
        return record;
    }
    if (envelope.event_type == "AnalysisBattle.ManualFollowupUpdated.v1") {
        const auto view = battle_row_resolver_.ResolveBattleManualFollowupUpdated(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        BattlePayloadRecord record{};
        record.battle_set_id = view->battle_set_id;
        record.turn_job_id = view->turn_job_id;
        return record;
    }
    if (envelope.event_type == "AnalysisBattle.BattleCompletionCreated.v1"
        || envelope.event_type == "AnalysisBattle.BattleCompletionCompleted.v1"
        || envelope.event_type == "AnalysisBattle.BattleCompletionFailed.v1") {
        const auto view = battle_row_resolver_.ResolveBattleCompletion(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) return std::nullopt;
        BattlePayloadRecord record{};
        record.battle_completion_id = view->battle_completion_id;
        return record;
    }
    if (envelope.event_type == "AnalysisBattle.BattleResultsCreated.v1"
        || envelope.event_type == "AnalysisBattle.BattleResultsCompleted.v1"
        || envelope.event_type == "AnalysisBattle.BattleResultsFailed.v1") {
        const auto view = battle_row_resolver_.ResolveBattleResults(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) return std::nullopt;
        BattlePayloadRecord record{};
        record.battle_completion_id = view->battle_completion_id;
        record.battle_results_id = view->battle_results_id;
        return record;
    }

    return std::nullopt;
}

std::optional<SpinePayloadRecord> SqliteAnalysisDb::ResolveSpinePayload(
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    return ResolveSpineByKind(spine_row_resolver_, event_version, payload_ref_kind, payload_ref_id);
}

std::optional<SpinePayloadRecord> SqliteAnalysisDb::ResolveSpinePayload(
    const events::EventEnvelope& envelope) const {
    if (!events::ValidateAnalysisSpinePayloadV1(envelope)) {
        return std::nullopt;
    }

    const auto contract = events::ResolvePayloadResolverContract(envelope.event_type, envelope.event_version);
    if (!contract.has_value() || contract.value() != events::PayloadResolverContract::AnalysisSpineV1) {
        return std::nullopt;
    }

    if (envelope.event_type == "AnalysisSpine.RunCreated.v1") {
        const auto view = spine_row_resolver_.ResolveSpineRunCreated(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        SpinePayloadRecord record{};
        record.run_id = view->run_id;
        return record;
    }
    if (envelope.event_type == "AnalysisSpine.StateRefRegistered.v1") {
        const auto view = spine_row_resolver_.ResolveSpineStateRefRegistered(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        SpinePayloadRecord record{};
        record.run_id = view->run_id;
        record.state_ref_id = view->state_ref_id;
        return record;
    }
    if (envelope.event_type == "AnalysisSpine.LineageEdgeAdded.v1") {
        const auto view = spine_row_resolver_.ResolveSpineLineageEdgeAdded(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        SpinePayloadRecord record{};
        record.run_id = view->child_run_id;
        record.lineage_edge_id = view->lineage_edge_id;
        return record;
    }
    if (envelope.event_type == "AnalysisSpine.ArtifactLinked.v1") {
        const auto view = spine_row_resolver_.ResolveSpineArtifactLinked(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        SpinePayloadRecord record{};
        record.run_id = view->run_id;
        record.artifact_ref_id = view->artifact_ref_id;
        return record;
    }

    return std::nullopt;
}

} // namespace savor::db::analysis
