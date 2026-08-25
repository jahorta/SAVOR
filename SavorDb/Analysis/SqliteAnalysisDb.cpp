#include "SqliteAnalysisDb.h"

#include <map>
#include <set>

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

bool InsertTasMovieOutboxEvent(
    sqlite3* db,
    std::string_view event_type,
    std::string_view aggregate_kind,
    std::int64_t aggregate_id,
    std::int64_t occurred_at_utc,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id,
    std::string* error_out) {
    constexpr int kMaxEventIdAttempts = 5;
    for (int attempt = 0; attempt < kMaxEventIdAttempts; ++attempt) {
        std::string event_id;
        if (!outbox::MakeDbOwnedEventId(
                db,
                "AnalysisTasMovie",
                event_type,
                payload_ref_kind,
                payload_ref_id,
                &event_id,
                error_out)) {
            return false;
        }
        Statement st;
        constexpr const char* kSql =
            "INSERT INTO tmv_outbox_message("
            "event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id) "
            "VALUES(?1,?2,1,'AnalysisTasMovie',?3,?4,'','',?5,?6,?7);";
        if (sqlite3_prepare_v2(db, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db);
            return false;
        }
        sqlite3_bind_text(st.st, 1, event_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 2, event_type.data(), static_cast<int>(event_type.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 3, aggregate_kind.data(), static_cast<int>(aggregate_kind.size()), SQLITE_TRANSIENT);
        const auto aggregate_text = std::to_string(aggregate_id);
        sqlite3_bind_text(st.st, 4, aggregate_text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st.st, 5, occurred_at_utc);
        sqlite3_bind_text(st.st, 6, payload_ref_kind.data(), static_cast<int>(payload_ref_kind.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(st.st, 7, payload_ref_id);
        const auto rc = sqlite3_step(st.st);
        if (rc == SQLITE_DONE) return true;
        if (!outbox::IsUniqueConstraint(db)) {
            if (error_out) *error_out = sqlite3_errmsg(db);
            return false;
        }
    }
    if (error_out) *error_out = "failed to generate a unique TAS movie outbox event id";
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
    if (payload_ref_kind == "battle_recording") {
        const auto view = resolver.ResolveBattleRecording(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) return std::nullopt;
        record.battle_completion_id = view->battle_completion_id;
        record.battle_recording_id = view->battle_recording_id;
        return record;
    }
    if (payload_ref_kind == "battle_replay") {
        const auto view = resolver.ResolveBattleReplay(
            payload_ref_kind, payload_ref_id);
        if (!view.has_value()) return std::nullopt;
        record.battle_completion_id = view->battle_completion_id;
        record.battle_replay_id = view->battle_replay_id;
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
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
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
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
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
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    const auto id = sqlite3_last_insert_rowid(db_);
    if (!InsertTasMovieOutboxEvent(
            db_, "AnalysisTasMovie.ValidationRequestCreated.v1", "validation_request", id,
            command.created_at_utc.time_since_epoch().count(), "validation_request", id, error_out)
        || sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out && error_out->empty()) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    if (validation_request_id_out) *validation_request_id_out = id;
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
    if (!InsertTasMovieOutboxEvent(
            db_, "AnalysisTasMovie.ValidationAttemptRecorded.v1", "validation_request",
            command.validation_request_id, command.recorded_at_utc.time_since_epoch().count(),
            "validation_attempt", id, error_out)
        || sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
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

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
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
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
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
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    const auto id = sqlite3_last_insert_rowid(db_);
    if (!InsertTasMovieOutboxEvent(
            db_, "AnalysisTasMovie.SterilizationRequestCreated.v1", "sterilization_request", id,
            command.created_at_utc.time_since_epoch().count(), "sterilization_request", id, error_out)
        || sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out && error_out->empty()) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    if (request_id_out) *request_id_out = id;
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

std::vector<TasMovieCheckpointSterilizationAttemptRecord>
SqliteAnalysisDb::ListTasMovieCheckpointSterilizationAttemptsForRequest(
    const std::int64_t request_id) const {
    std::vector<TasMovieCheckpointSterilizationAttemptRecord> rows;
    if (db_ == nullptr || request_id <= 0) return rows;
    Statement st;
    constexpr const char* kSql =
        "SELECT sterilization_attempt_id,sterilization_request_id,source_job_id,"
        "worker_terminal_sha256,candidate_savestate_sha256,produced_savestate_id,worker_id,"
        "worker_process_generation,workset_epoch,recorded_at_utc "
        "FROM tmv_checkpoint_sterilization_attempt "
        "WHERE sterilization_request_id=?1 ORDER BY sterilization_attempt_id;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, request_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        rows.push_back(ReadTasMovieCheckpointSterilizationAttempt(st.st));
    }
    return rows;
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

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    Statement st;
    constexpr const char* kSql =
        "INSERT INTO tmv_checkpoint_sterilization_attempt(sterilization_request_id,"
        "source_job_id,worker_terminal_sha256,candidate_savestate_sha256,"
        "produced_savestate_id,worker_id,worker_process_generation,workset_epoch,recorded_at_utc) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
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
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    const auto id = sqlite3_last_insert_rowid(db_);
    if (!InsertTasMovieOutboxEvent(
            db_, "AnalysisTasMovie.SterilizationAttemptRecorded.v1", "sterilization_request",
            command.sterilization_request_id, command.recorded_at_utc.time_since_epoch().count(),
            "sterilization_attempt", id, error_out)
        || sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out && error_out->empty()) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    if (attempt_id_out) *attempt_id_out = id;
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
        || command.battle_plan_id <= 0
        || command.battle_plan_fingerprint.empty()
        || command.continuation_mode == BattleContinuationMode::Unknown
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
            "INSERT INTO ab_battle_set(name,entry_savestate_id,battle_plan_id,battle_plan_fingerprint,"
            "continuation_mode,continue_automatic_exploration_after_victory,"
            "launch_fake_attack_min,launch_fake_attack_max,status,created_at_utc,completed_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,NULL);",
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
    sqlite3_bind_int64(insert_set.st, 3, command.battle_plan_id);
    sqlite3_bind_text(insert_set.st, 4, command.battle_plan_fingerprint.c_str(), -1, SQLITE_TRANSIENT);
    const auto continuation = ToDbString(command.continuation_mode);
    sqlite3_bind_text(insert_set.st, 5, continuation.data(), static_cast<int>(continuation.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(insert_set.st, 6,
        command.continue_automatic_exploration_after_victory ? 1 : 0);
    sqlite3_bind_int(insert_set.st, 7, command.launch_fake_attack_min);
    sqlite3_bind_int(insert_set.st, 8, command.launch_fake_attack_max);
    const auto status = ToDbString(command.status);
    sqlite3_bind_text(insert_set.st, 9, status.data(), static_cast<int>(status.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_set.st, 10, command.created_at_utc.time_since_epoch().count());
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

bool SqliteAnalysisDb::EnsureBattleStart(
    const EnsureBattleStartCommand& command,
    EnsureBattleStartReceipt* receipt_out,
    std::string* error_out) {
    EnsureBattleStartReceipt receipt{};
    if (receipt_out) *receipt_out = receipt;
    if (db_ == nullptr || command.workflow_instance_id <= 0
        || command.workflow_step_id <= 0 || command.probe_run_id <= 0
        || command.context_probe_id <= 0 || command.battle_set_name.empty()
        || command.entry_savestate_id <= 0 || command.battle_plan_id <= 0
        || command.battle_plan_fingerprint.empty()
        || command.continuation_mode == BattleContinuationMode::Unknown
        || command.launch_fake_attack_min < 0
        || command.launch_fake_attack_max < command.launch_fake_attack_min) {
        if (error_out) *error_out = "battle start join is incomplete";
        return false;
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const auto rollback = [&](std::string message) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = std::move(message);
        return false;
    };
    const auto commit = [&]() {
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr)
            != SQLITE_OK) {
            const std::string message = sqlite3_errmsg(db_);
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            if (error_out) *error_out = message;
            return false;
        }
        if (error_out) error_out->clear();
        if (receipt_out) *receipt_out = receipt;
        return true;
    };

    Statement existing;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT battle_start_id,workflow_instance_id,probe_run_id,"
            "context_probe_id,battle_set_id,entry_savestate_id,"
            "battle_plan_id,battle_plan_fingerprint,continuation_mode,"
            "continue_automatic_exploration_after_victory,"
            "launch_fake_attack_min,launch_fake_attack_max FROM ab_battle_start "
            "WHERE workflow_step_id=?1;",
            -1, &existing.st, nullptr) != SQLITE_OK) {
        return rollback(sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(existing.st, 1, command.workflow_step_id);
    const auto existing_rc = sqlite3_step(existing.st);
    if (existing_rc == SQLITE_ROW) {
        receipt.battle_start_id = sqlite3_column_int64(existing.st, 0);
        receipt.battle_set_id = sqlite3_column_int64(existing.st, 4);
        const bool exact =
            sqlite3_column_int64(existing.st, 1) == command.workflow_instance_id
            && sqlite3_column_int64(existing.st, 2) == command.probe_run_id
            && sqlite3_column_int64(existing.st, 3) == command.context_probe_id
            && sqlite3_column_int64(existing.st, 5) == command.entry_savestate_id
            && sqlite3_column_int64(existing.st, 6) == command.battle_plan_id
            && ColumnText(existing.st, 7) == command.battle_plan_fingerprint
            && ParseBattleContinuationMode(ColumnText(existing.st, 8)) == command.continuation_mode
            && sqlite3_column_int(existing.st, 9)
                == (command.continue_automatic_exploration_after_victory ? 1 : 0)
            && sqlite3_column_int(existing.st, 10) == command.launch_fake_attack_min
            && sqlite3_column_int(existing.st, 11) == command.launch_fake_attack_max;
        if (!exact) return rollback("workflow step already identifies a different battle start join");
        Statement waves;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT wave_id FROM ab_turn_wave WHERE battle_set_id=?1 "
                "AND turn_index=1 AND parent_wave_id IS NULL "
                "AND parent_turn_job_id IS NULL ORDER BY wave_id ASC;",
                -1, &waves.st, nullptr) != SQLITE_OK) {
            return rollback(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(waves.st, 1, receipt.battle_set_id);
        while (sqlite3_step(waves.st) == SQLITE_ROW)
            receipt.first_wave_ids.push_back(sqlite3_column_int64(waves.st, 0));
        if (receipt.first_wave_ids.empty())
            return rollback("persisted battle start join has no first-turn waves");
        return commit();
    }
    if (existing_rc != SQLITE_DONE) return rollback(sqlite3_errmsg(db_));

    Statement run;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT entry_savestate_id,status,accepted_input_set_id "
            "FROM sp_probe_run WHERE probe_run_id=?1;",
            -1, &run.st, nullptr) != SQLITE_OK) {
        return rollback(sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(run.st, 1, command.probe_run_id);
    if (sqlite3_step(run.st) != SQLITE_ROW
        || sqlite3_column_int64(run.st, 0) != command.entry_savestate_id
        || (ColumnText(run.st, 1) != "COMPLETED"
            && ColumnText(run.st, 1) != "COMPLETED_PARTIAL")
        || sqlite3_column_int64(run.st, 2) <= 0) {
        return rollback("battle start requires a completed SeedProbe run from the entry savestate");
    }

    Statement context;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT source_savestate_id,probe_status,workflow_instance_id "
            "FROM ab_battle_context_probe WHERE context_probe_id=?1;",
            -1, &context.st, nullptr) != SQLITE_OK) {
        return rollback(sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(context.st, 1, command.context_probe_id);
    if (sqlite3_step(context.st) != SQLITE_ROW
        || sqlite3_column_int64(context.st, 0) != command.entry_savestate_id
        || ColumnText(context.st, 1) != "SUCCEEDED"
        || sqlite3_column_int64(context.st, 2) != command.workflow_instance_id) {
        return rollback("battle start requires a successful Battle Context capture from the same entry state and workflow");
    }

    struct Candidate { std::int64_t result_id; std::int64_t frame_id; std::int64_t seed; };
    std::vector<Candidate> candidates;
    Statement accepted;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT r.probe_result_id,r.input_frame_id,r.seed_value "
            "FROM sp_probe_run pr "
            "JOIN an_input_set_frame f ON f.input_set_id=pr.accepted_input_set_id "
            "JOIN sp_probe_result r ON r.probe_run_id=pr.probe_run_id "
            "AND r.input_frame_id=f.input_frame_id "
            "WHERE pr.probe_run_id=?1 AND r.evidence_state='CONFIRMED' "
            "AND r.confirmation_of_probe_result_id IS NULL "
            "ORDER BY f.ordinal ASC,r.probe_result_id ASC;",
            -1, &accepted.st, nullptr) != SQLITE_OK) {
        return rollback(sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(accepted.st, 1, command.probe_run_id);
    std::unordered_set<std::int64_t> accepted_frames;
    for (;;) {
        const auto rc = sqlite3_step(accepted.st);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) return rollback(sqlite3_errmsg(db_));
        Candidate candidate{
            sqlite3_column_int64(accepted.st, 0),
            sqlite3_column_int64(accepted.st, 1),
            sqlite3_column_int64(accepted.st, 2)};
        if (!accepted_frames.insert(candidate.frame_id).second)
            return rollback("accepted SeedProbe frame resolves to multiple confirmed representatives");
        candidates.push_back(candidate);
    }
    if (candidates.empty())
        return rollback("completed SeedProbe run has no accepted confirmed representatives");

    const auto now = command.created_at_utc.time_since_epoch().count();
    Statement insert_set;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ab_battle_set(name,entry_savestate_id,battle_plan_id,"
            "battle_plan_fingerprint,continuation_mode,"
            "continue_automatic_exploration_after_victory,"
            "launch_fake_attack_min,launch_fake_attack_max,"
            "status,created_at_utc,completed_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,'ACTIVE',?9,NULL);",
            -1, &insert_set.st, nullptr) != SQLITE_OK) {
        return rollback(sqlite3_errmsg(db_));
    }
    sqlite3_bind_text(insert_set.st, 1, command.battle_set_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_set.st, 2, command.entry_savestate_id);
    sqlite3_bind_int64(insert_set.st, 3, command.battle_plan_id);
    sqlite3_bind_text(insert_set.st, 4, command.battle_plan_fingerprint.c_str(), -1, SQLITE_TRANSIENT);
    const auto continuation = ToDbString(command.continuation_mode);
    sqlite3_bind_text(insert_set.st, 5, continuation.data(), static_cast<int>(continuation.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(insert_set.st, 6,
        command.continue_automatic_exploration_after_victory ? 1 : 0);
    sqlite3_bind_int(insert_set.st, 7, command.launch_fake_attack_min);
    sqlite3_bind_int(insert_set.st, 8, command.launch_fake_attack_max);
    sqlite3_bind_int64(insert_set.st, 9, now);
    if (sqlite3_step(insert_set.st) != SQLITE_DONE)
        return rollback(sqlite3_errmsg(db_));
    receipt.battle_set_id = sqlite3_last_insert_rowid(db_);
    if (!InsertBattleOutboxEvent(
            db_, "AnalysisBattle.BattleSetCreated.v1", "battle_set",
            std::to_string(receipt.battle_set_id), command.correlation_id,
            command.causation_id, now, "battle_set", receipt.battle_set_id,
            error_out)) {
        const auto message = error_out ? *error_out : std::string("battle set outbox failed");
        return rollback(message);
    }

    Statement insert_candidate;
    Statement insert_wave;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ab_seed_candidate(battle_set_id,source_probe_result_id,"
            "source_input_frame_id,seed_value,source_kind,candidate_status,created_at_utc) "
            "VALUES(?1,?2,?3,?4,'SP_CONFIRMED_RESULT','READY',?5);",
            -1, &insert_candidate.st, nullptr) != SQLITE_OK
        || sqlite3_prepare_v2(
            db_,
            "INSERT INTO ab_turn_wave(battle_set_id,turn_index,context_probe_id,"
            "parent_wave_id,parent_turn_job_id,seed_candidate_id,battle_advancement_pool_id,"
            "status,created_at_utc,completed_at_utc) "
            "VALUES(?1,1,?2,NULL,NULL,?3,NULL,'READY',?4,NULL);",
            -1, &insert_wave.st, nullptr) != SQLITE_OK) {
        return rollback(sqlite3_errmsg(db_));
    }
    for (const auto& candidate : candidates) {
        sqlite3_reset(insert_candidate.st);
        sqlite3_clear_bindings(insert_candidate.st);
        sqlite3_bind_int64(insert_candidate.st, 1, receipt.battle_set_id);
        sqlite3_bind_int64(insert_candidate.st, 2, candidate.result_id);
        sqlite3_bind_int64(insert_candidate.st, 3, candidate.frame_id);
        sqlite3_bind_int64(insert_candidate.st, 4, candidate.seed);
        sqlite3_bind_int64(insert_candidate.st, 5, now);
        if (sqlite3_step(insert_candidate.st) != SQLITE_DONE)
            return rollback(sqlite3_errmsg(db_));
        const auto seed_candidate_id = sqlite3_last_insert_rowid(db_);
        if (!InsertBattleOutboxEvent(
                db_, "AnalysisBattle.SeedCandidateAdded.v1", "battle_set",
                std::to_string(receipt.battle_set_id), command.correlation_id,
                command.causation_id, now, "seed_candidate", seed_candidate_id,
                error_out)) {
            const auto message = error_out ? *error_out : std::string("seed candidate outbox failed");
            return rollback(message);
        }
        sqlite3_reset(insert_wave.st);
        sqlite3_clear_bindings(insert_wave.st);
        sqlite3_bind_int64(insert_wave.st, 1, receipt.battle_set_id);
        sqlite3_bind_int64(insert_wave.st, 2, command.context_probe_id);
        sqlite3_bind_int64(insert_wave.st, 3, seed_candidate_id);
        sqlite3_bind_int64(insert_wave.st, 4, now);
        if (sqlite3_step(insert_wave.st) != SQLITE_DONE)
            return rollback(sqlite3_errmsg(db_));
        const auto wave_id = sqlite3_last_insert_rowid(db_);
        receipt.first_wave_ids.push_back(wave_id);
        if (!InsertBattleOutboxEvent(
                db_, "AnalysisBattle.TurnWaveCreated.v1", "battle_set",
                std::to_string(receipt.battle_set_id), command.correlation_id,
                command.causation_id, now, "turn_wave", wave_id, error_out)) {
            const auto message = error_out ? *error_out : std::string("turn wave outbox failed");
            return rollback(message);
        }
    }

    Statement insert_start;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ab_battle_start(workflow_instance_id,workflow_step_id,"
            "probe_run_id,context_probe_id,battle_set_id,entry_savestate_id,"
            "battle_plan_id,battle_plan_fingerprint,continuation_mode,"
            "continue_automatic_exploration_after_victory,launch_fake_attack_min,"
            "launch_fake_attack_max,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13);",
            -1, &insert_start.st, nullptr) != SQLITE_OK) {
        return rollback(sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(insert_start.st, 1, command.workflow_instance_id);
    sqlite3_bind_int64(insert_start.st, 2, command.workflow_step_id);
    sqlite3_bind_int64(insert_start.st, 3, command.probe_run_id);
    sqlite3_bind_int64(insert_start.st, 4, command.context_probe_id);
    sqlite3_bind_int64(insert_start.st, 5, receipt.battle_set_id);
    sqlite3_bind_int64(insert_start.st, 6, command.entry_savestate_id);
    sqlite3_bind_int64(insert_start.st, 7, command.battle_plan_id);
    sqlite3_bind_text(insert_start.st, 8, command.battle_plan_fingerprint.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_start.st, 9, continuation.data(), static_cast<int>(continuation.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(insert_start.st, 10,
        command.continue_automatic_exploration_after_victory ? 1 : 0);
    sqlite3_bind_int(insert_start.st, 11, command.launch_fake_attack_min);
    sqlite3_bind_int(insert_start.st, 12, command.launch_fake_attack_max);
    sqlite3_bind_int64(insert_start.st, 13, now);
    if (sqlite3_step(insert_start.st) != SQLITE_DONE)
        return rollback(sqlite3_errmsg(db_));
    receipt.battle_start_id = sqlite3_last_insert_rowid(db_);
    receipt.created = true;
    return commit();
}

bool SqliteAnalysisDb::BindBattlePredicateExecutionPackage(
    const BindBattlePredicateExecutionPackageCommand& command,
    std::int64_t* binding_id_out,
    std::string* error_out) {
    if (db_ == nullptr || command.predicate_group_sha256.size() != 64 ||
        command.execution_package_sha256.size() != 64 ||
        command.execution_package_blob.empty() ||
        command.phase_program_kind <= 0 || command.phase_program_version <= 0 ||
        command.phase_canonical_id.empty() ||
        command.phase_sha256.size() != 64 ||
        command.hook_contract_canonical_id.empty() ||
        command.hook_contract_sha256.size() != 64) {
        if (error_out) *error_out = "predicate execution package is incomplete";
        return false;
    }
    if (command.wave_id.has_value()) {
        if (const auto existing = GetBattlePredicateExecutionPackageForWave(
                *command.wave_id); existing.has_value()) {
            const bool exact =
                existing->predicate_group_revision_id == command.predicate_group_revision_id &&
                existing->predicate_group_sha256 == command.predicate_group_sha256 &&
                existing->execution_package_sha256 == command.execution_package_sha256 &&
                existing->execution_package_blob == command.execution_package_blob &&
                existing->phase_program_kind == command.phase_program_kind &&
                existing->phase_program_version == command.phase_program_version &&
                existing->phase_canonical_id == command.phase_canonical_id &&
                existing->phase_revision == command.phase_revision &&
                existing->phase_sha256 == command.phase_sha256 &&
                existing->hook_contract_canonical_id == command.hook_contract_canonical_id &&
                existing->hook_contract_revision == command.hook_contract_revision &&
                existing->hook_contract_sha256 == command.hook_contract_sha256;
            if (!exact) {
                if (error_out) *error_out =
                    "wave already has a different predicate execution package";
                return false;
            }
            if (binding_id_out)
                *binding_id_out = existing->predicate_execution_package_id;
            return true;
        }
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const auto rollback = [&] {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    };
    Statement insert;
    constexpr const char* sql =
        "INSERT INTO ab_predicate_execution_package_v1("
        "wave_id,predicate_group_revision_id,predicate_group_sha256,"
        "execution_package_sha256,execution_package_blob,"
        "phase_program_kind,phase_program_version,phase_canonical_id,phase_revision,"
        "phase_sha256,hook_contract_canonical_id,hook_contract_revision,"
        "hook_contract_sha256,created_at_utc) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &insert.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }
    if (command.wave_id) sqlite3_bind_int64(insert.st, 1, *command.wave_id);
    else sqlite3_bind_null(insert.st, 1);
    if (command.predicate_group_revision_id)
        sqlite3_bind_int64(insert.st, 2, *command.predicate_group_revision_id);
    else sqlite3_bind_null(insert.st, 2);
    sqlite3_bind_text(insert.st, 3, command.predicate_group_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.st, 4, command.execution_package_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(insert.st, 5, command.execution_package_blob.data(),
        static_cast<int>(command.execution_package_blob.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(insert.st, 6, command.phase_program_kind);
    sqlite3_bind_int(insert.st, 7, command.phase_program_version);
    sqlite3_bind_text(insert.st, 8, command.phase_canonical_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.st, 9, command.phase_revision);
    sqlite3_bind_text(insert.st, 10, command.phase_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.st, 11, command.hook_contract_canonical_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.st, 12, command.hook_contract_revision);
    sqlite3_bind_text(insert.st, 13, command.hook_contract_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.st, 14, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }
    const auto binding_id = sqlite3_last_insert_rowid(db_);
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }
    if (binding_id_out) *binding_id_out = binding_id;
    return true;
}

std::optional<BattlePredicateExecutionPackageSnapshot>
SqliteAnalysisDb::GetBattlePredicateExecutionPackageForWave(
    std::int64_t wave_id) const {
    if (db_ == nullptr || wave_id <= 0) return std::nullopt;
    Statement root;
    constexpr const char* sql =
        "SELECT predicate_execution_package_id,wave_id,predicate_group_revision_id,"
        "predicate_group_sha256,execution_package_sha256,execution_package_blob,"
        "phase_program_kind,phase_program_version,phase_canonical_id,"
        "phase_revision,phase_sha256,hook_contract_canonical_id,hook_contract_revision,"
        "hook_contract_sha256,created_at_utc FROM ab_predicate_execution_package_v1 WHERE wave_id=?1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &root.st, nullptr) != SQLITE_OK)
        return std::nullopt;
    sqlite3_bind_int64(root.st, 1, wave_id);
    if (sqlite3_step(root.st) != SQLITE_ROW) return std::nullopt;
    BattlePredicateExecutionPackageSnapshot out{};
    out.predicate_execution_package_id = sqlite3_column_int64(root.st, 0);
    out.wave_id = ColumnInt64Optional(root.st, 1);
    out.predicate_group_revision_id = ColumnInt64Optional(root.st, 2);
    out.predicate_group_sha256 = ColumnText(root.st, 3);
    out.execution_package_sha256 = ColumnText(root.st, 4);
    const auto blob = ColumnBlob(root.st, 5);
    out.execution_package_blob.assign(blob.begin(), blob.end());
    out.phase_program_kind = sqlite3_column_int(root.st, 6);
    out.phase_program_version = sqlite3_column_int(root.st, 7);
    out.phase_canonical_id = ColumnText(root.st, 8);
    out.phase_revision = static_cast<std::uint32_t>(sqlite3_column_int64(root.st, 9));
    out.phase_sha256 = ColumnText(root.st, 10);
    out.hook_contract_canonical_id = ColumnText(root.st, 11);
    out.hook_contract_revision = static_cast<std::uint32_t>(sqlite3_column_int64(root.st, 12));
    out.hook_contract_sha256 = ColumnText(root.st, 13);
    out.created_at_utc = types::UtcTimePoint(
        std::chrono::milliseconds(sqlite3_column_int64(root.st, 14)));
    return out;
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

    if (!command.materialization_key.empty()) {
        Statement existing;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT context_probe_id FROM ab_battle_context_probe WHERE materialization_key=?1;",
                -1,
                &existing.st,
                nullptr) != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_text(existing.st, 1, command.materialization_key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(existing.st) == SQLITE_ROW) {
            if (context_probe_id_out) *context_probe_id_out = sqlite3_column_int64(existing.st, 0);
            return true;
        }
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
            "INSERT INTO ab_battle_context_probe("
            "wave_id,source_savestate_id,exec_job_id,probe_status,context_blob,context_version,recorded_at_utc,created_at_utc,"
            "materialization_key,workflow_instance_id,workflow_step_id,full_phase_program_kind,full_phase_program_version,"
            "source_savestate_artifact_id,source_savestate_sha256,full_phase_canonical_id,full_phase_contract_revision,full_phase_sha256,"
            "module_canonical_id,module_revision,module_sha256) "
            "VALUES(?1,?2,NULL,?3,NULL,NULL,NULL,?4,?5,?6,?7,?10,?11,?8,?9,?12,?13,?14,?15,?16,?17);",
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
    if (!command.materialization_key.empty()) sqlite3_bind_text(insert_probe.st, 5, command.materialization_key.c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(insert_probe.st, 5);
    if (command.workflow_instance_id > 0) sqlite3_bind_int64(insert_probe.st, 6, command.workflow_instance_id);
    else sqlite3_bind_null(insert_probe.st, 6);
    if (command.workflow_step_id > 0) sqlite3_bind_int64(insert_probe.st, 7, command.workflow_step_id);
    else sqlite3_bind_null(insert_probe.st, 7);
    if (command.source_savestate_artifact_id > 0) sqlite3_bind_int64(insert_probe.st, 8, command.source_savestate_artifact_id);
    else sqlite3_bind_null(insert_probe.st, 8);
    if (!command.source_savestate_sha256.empty()) sqlite3_bind_text(insert_probe.st, 9, command.source_savestate_sha256.c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(insert_probe.st, 9);
    if (command.full_phase_program_kind > 0) sqlite3_bind_int64(insert_probe.st, 10, command.full_phase_program_kind);
    else sqlite3_bind_null(insert_probe.st, 10);
    if (command.full_phase_program_version > 0) sqlite3_bind_int64(insert_probe.st, 11, command.full_phase_program_version);
    else sqlite3_bind_null(insert_probe.st, 11);
    if (!command.full_phase_canonical_id.empty()) sqlite3_bind_text(insert_probe.st, 12, command.full_phase_canonical_id.c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(insert_probe.st, 12);
    if (command.full_phase_contract_revision > 0) sqlite3_bind_int64(insert_probe.st, 13, command.full_phase_contract_revision);
    else sqlite3_bind_null(insert_probe.st, 13);
    if (!command.full_phase_sha256.empty()) sqlite3_bind_text(insert_probe.st, 14, command.full_phase_sha256.c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(insert_probe.st, 14);
    if (!command.module_canonical_id.empty()) sqlite3_bind_text(insert_probe.st, 15, command.module_canonical_id.c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(insert_probe.st, 15);
    if (command.module_revision > 0) sqlite3_bind_int64(insert_probe.st, 16, command.module_revision);
    else sqlite3_bind_null(insert_probe.st, 16);
    if (!command.module_sha256.empty()) sqlite3_bind_text(insert_probe.st, 17, command.module_sha256.c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(insert_probe.st, 17);
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
    Statement lookup;
    if (sqlite3_prepare_v2(db_, "SELECT context_probe_id FROM ab_battle_context_probe WHERE exec_job_id=?1;", -1, &lookup.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(lookup.st, 1, command.exec_job_id);
    if (sqlite3_step(lookup.st) != SQLITE_ROW) {
        if (error_out) *error_out = "battle context probe does not exist";
        return false;
    }
    const auto context_probe_id = sqlite3_column_int64(lookup.st, 0);
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE ab_battle_context_probe SET probe_status=?2,context_blob=?3,context_version=?4,recorded_at_utc=?5,"
            "context_artifact_id=?6,worker_terminal_sha256=?7,entry_pc=?8,entry_vi_count=?9,entry_epoch=?10,"
            "capture_pc=?11,capture_vi_count=?12,capture_epoch=?13 WHERE exec_job_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
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
    if (command.context_artifact_id) sqlite3_bind_int64(st.st, 6, *command.context_artifact_id); else sqlite3_bind_null(st.st, 6);
    if (command.worker_terminal_sha256) sqlite3_bind_text(st.st, 7, command.worker_terminal_sha256->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st.st, 7);
    if (command.entry_pc) sqlite3_bind_int64(st.st, 8, *command.entry_pc); else sqlite3_bind_null(st.st, 8);
    if (command.entry_vi_count) sqlite3_bind_int64(st.st, 9, static_cast<sqlite3_int64>(*command.entry_vi_count)); else sqlite3_bind_null(st.st, 9);
    if (command.entry_epoch) sqlite3_bind_int64(st.st, 10, static_cast<sqlite3_int64>(*command.entry_epoch)); else sqlite3_bind_null(st.st, 10);
    if (command.capture_pc) sqlite3_bind_int64(st.st, 11, *command.capture_pc); else sqlite3_bind_null(st.st, 11);
    if (command.capture_vi_count) sqlite3_bind_int64(st.st, 12, static_cast<sqlite3_int64>(*command.capture_vi_count)); else sqlite3_bind_null(st.st, 12);
    if (command.capture_epoch) sqlite3_bind_int64(st.st, 13, static_cast<sqlite3_int64>(*command.capture_epoch)); else sqlite3_bind_null(st.st, 13);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    if (sqlite3_changes(db_) <= 0
        || !InsertBattleOutboxEvent(db_, "AnalysisBattle.ContextProbeCompleted.v1", "battle_context_probe",
            std::to_string(context_probe_id), "", "", command.recorded_at_utc.time_since_epoch().count(),
            "battle_context_probe", context_probe_id, error_out)
        || sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out && error_out->empty()) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
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

bool SqliteAnalysisDb::RecordBattleSingleTurnResult(
    const RecordBattleSingleTurnResultCommand& command,
    std::int64_t* result_id_out,
    std::string* error_out) {
    if (db_ == nullptr || command.turn_job_id <= 0 || command.exec_job_id <= 0 ||
        command.worker_terminal_sha256.size() != 64 || command.terminal_kind.empty()) {
        if (error_out) *error_out = "battle.single_turn result identity is incomplete";
        return false;
    }
    Statement insert;
    constexpr const char* sql =
        "INSERT INTO ab_battle_single_turn_result_v1("
        "turn_job_id,exec_job_id,worker_terminal_sha256,terminal_kind,domain_outcome,"
        "error_code,error_text,ending_rng,vi_start,vi_end,pred_passed,pred_total,"
        "cumulative_fake_attacks,successor_savestate_id,battle_context_artifact_id,"
        "predicate_group_revision_id,predicate_group_sha256,predicate_execution_package_sha256,"
        "predicate_evidence_blob,applied_input_artifact_id,input_trace_artifact_id,"
        "recorded_at_utc) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(turn_job_id) DO NOTHING;";
    if (sqlite3_prepare_v2(db_, sql, -1, &insert.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const auto bind_text = [&](int index, const std::optional<std::string>& value) {
        if (value) sqlite3_bind_text(insert.st, index, value->c_str(), -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(insert.st, index);
    };
    const auto bind_i64 = [&](int index, const auto& value) {
        if (value) sqlite3_bind_int64(insert.st, index, static_cast<sqlite3_int64>(*value));
        else sqlite3_bind_null(insert.st, index);
    };
    sqlite3_bind_int64(insert.st, 1, command.turn_job_id);
    sqlite3_bind_int64(insert.st, 2, command.exec_job_id);
    sqlite3_bind_text(insert.st, 3, command.worker_terminal_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.st, 4, command.terminal_kind.c_str(), -1, SQLITE_TRANSIENT);
    bind_text(5, command.domain_outcome);
    bind_text(6, command.error_code);
    bind_text(7, command.error_text);
    bind_i64(8, command.ending_rng);
    bind_i64(9, command.vi_start);
    bind_i64(10, command.vi_end);
    bind_i64(11, command.pred_passed);
    bind_i64(12, command.pred_total);
    bind_i64(13, command.cumulative_fake_attacks);
    bind_i64(14, command.successor_savestate_id);
    bind_i64(15, command.battle_context_artifact_id);
    bind_i64(16, command.predicate_group_revision_id);
    bind_text(17, command.predicate_group_sha256);
    bind_text(18, command.predicate_execution_package_sha256);
    if (!command.predicate_evidence_blob.empty())
        sqlite3_bind_blob(insert.st, 19, command.predicate_evidence_blob.data(),
            static_cast<int>(command.predicate_evidence_blob.size()), SQLITE_TRANSIENT);
    else sqlite3_bind_null(insert.st, 19);
    bind_i64(20, command.applied_input_artifact_id);
    bind_i64(21, command.input_trace_artifact_id);
    sqlite3_bind_int64(insert.st, 22, command.recorded_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const auto stored = GetBattleSingleTurnResultForExecJob(command.exec_job_id);
    if (!stored || stored->turn_job_id != command.turn_job_id ||
        stored->worker_terminal_sha256 != command.worker_terminal_sha256) {
        if (error_out) *error_out = "battle.single_turn result conflicts with a prior terminal";
        return false;
    }
    if (result_id_out) *result_id_out = stored->battle_single_turn_result_id;
    return true;
}

bool SqliteAnalysisDb::ReplaceFailedBattleSingleTurnResult(
    const ReplaceFailedBattleSingleTurnResultCommand& command,
    std::int64_t* result_id_out,
    std::string* error_out) {
    const auto& result = command.result;
    if (db_ == nullptr || result.turn_job_id <= 0 || result.exec_job_id <= 0
        || command.expected_worker_terminal_sha256.size() != 64
        || command.replacement_worker_terminal_sha256.size() != 64
        || result.worker_terminal_sha256 != command.replacement_worker_terminal_sha256
        || result.worker_terminal_sha256.size() != 64 || result.terminal_kind.empty()) {
        if (error_out) *error_out = "battle.single_turn failed-result replacement identity is incomplete";
        return false;
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const auto rollback = [&]() { (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr); };
    const auto fail = [&](std::string message) {
        rollback();
        if (error_out) *error_out = std::move(message);
        return false;
    };

    const auto existing = GetBattleSingleTurnResultForExecJob(result.exec_job_id);
    if (!existing || existing->turn_job_id != result.turn_job_id
        || existing->worker_terminal_sha256 != command.expected_worker_terminal_sha256
        || existing->terminal_kind != "FAILED") {
        return fail("battle.single_turn failed-result replacement precondition failed");
    }

    Statement archive;
    constexpr const char* archive_sql =
        "INSERT INTO ab_historical_failed_battle_single_turn_results("
        "superseded_battle_single_turn_result_id,turn_job_id,exec_job_id,"
        "prior_worker_terminal_sha256,replacement_worker_terminal_sha256,"
        "prior_terminal_kind,prior_domain_outcome,prior_error_code,prior_error_text,"
        "prior_recorded_at_utc,superseded_at_utc) VALUES(?,?,?,?,?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(db_, archive_sql, -1, &archive.st, nullptr) != SQLITE_OK) {
        return fail(sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(archive.st, 1, existing->battle_single_turn_result_id);
    sqlite3_bind_int64(archive.st, 2, existing->turn_job_id);
    sqlite3_bind_int64(archive.st, 3, existing->exec_job_id);
    sqlite3_bind_text(archive.st, 4, existing->worker_terminal_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(archive.st, 5, command.replacement_worker_terminal_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(archive.st, 6, existing->terminal_kind.c_str(), -1, SQLITE_TRANSIENT);
    const auto bind_existing_text = [&](int index, const std::optional<std::string>& value) {
        if (value) sqlite3_bind_text(archive.st, index, value->c_str(), -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(archive.st, index);
    };
    bind_existing_text(7, existing->domain_outcome);
    bind_existing_text(8, existing->error_code);
    bind_existing_text(9, existing->error_text);
    sqlite3_bind_int64(archive.st, 10, existing->recorded_at_utc.time_since_epoch().count());
    sqlite3_bind_int64(archive.st, 11, command.superseded_at_utc.time_since_epoch().count());
    if (sqlite3_step(archive.st) != SQLITE_DONE) {
        return fail(sqlite3_errmsg(db_));
    }

    Statement update;
    constexpr const char* update_sql =
        "UPDATE ab_battle_single_turn_result_v1 SET "
        "worker_terminal_sha256=?1,terminal_kind=?2,domain_outcome=?3,"
        "error_code=?4,error_text=?5,ending_rng=?6,vi_start=?7,vi_end=?8,"
        "pred_passed=?9,pred_total=?10,cumulative_fake_attacks=?11,"
        "successor_savestate_id=?12,battle_context_artifact_id=?13,"
        "predicate_group_revision_id=?14,predicate_group_sha256=?15,"
        "predicate_execution_package_sha256=?16,predicate_evidence_blob=?17,"
        "applied_input_artifact_id=?18,input_trace_artifact_id=?19,recorded_at_utc=?20 "
        "WHERE battle_single_turn_result_id=?21 AND worker_terminal_sha256=?22 "
        "AND terminal_kind='FAILED';";
    if (sqlite3_prepare_v2(db_, update_sql, -1, &update.st, nullptr) != SQLITE_OK) {
        return fail(sqlite3_errmsg(db_));
    }
    const auto bind_text = [&](int index, const std::optional<std::string>& value) {
        if (value) sqlite3_bind_text(update.st, index, value->c_str(), -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(update.st, index);
    };
    const auto bind_i64 = [&](int index, const auto& value) {
        if (value) sqlite3_bind_int64(update.st, index, static_cast<sqlite3_int64>(*value));
        else sqlite3_bind_null(update.st, index);
    };
    sqlite3_bind_text(update.st, 1, result.worker_terminal_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update.st, 2, result.terminal_kind.c_str(), -1, SQLITE_TRANSIENT);
    bind_text(3, result.domain_outcome);
    bind_text(4, result.error_code);
    bind_text(5, result.error_text);
    bind_i64(6, result.ending_rng);
    bind_i64(7, result.vi_start);
    bind_i64(8, result.vi_end);
    bind_i64(9, result.pred_passed);
    bind_i64(10, result.pred_total);
    bind_i64(11, result.cumulative_fake_attacks);
    bind_i64(12, result.successor_savestate_id);
    bind_i64(13, result.battle_context_artifact_id);
    bind_i64(14, result.predicate_group_revision_id);
    bind_text(15, result.predicate_group_sha256);
    bind_text(16, result.predicate_execution_package_sha256);
    if (!result.predicate_evidence_blob.empty()) {
        sqlite3_bind_blob(update.st, 17, result.predicate_evidence_blob.data(),
            static_cast<int>(result.predicate_evidence_blob.size()), SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(update.st, 17);
    }
    bind_i64(18, result.applied_input_artifact_id);
    bind_i64(19, result.input_trace_artifact_id);
    sqlite3_bind_int64(update.st, 20, result.recorded_at_utc.time_since_epoch().count());
    sqlite3_bind_int64(update.st, 21, existing->battle_single_turn_result_id);
    sqlite3_bind_text(update.st, 22, command.expected_worker_terminal_sha256.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(update.st) != SQLITE_DONE || sqlite3_changes(db_) != 1) {
        return fail(sqlite3_errmsg(db_));
    }
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return fail(sqlite3_errmsg(db_));
    }
    if (result_id_out) *result_id_out = existing->battle_single_turn_result_id;
    return true;
}

std::optional<BattleSingleTurnResultSnapshot>
SqliteAnalysisDb::GetBattleSingleTurnResultForExecJob(std::int64_t exec_job_id) const {
    if (db_ == nullptr || exec_job_id <= 0) return std::nullopt;
    Statement st;
    constexpr const char* sql =
        "SELECT battle_single_turn_result_id,turn_job_id,exec_job_id,"
        "worker_terminal_sha256,terminal_kind,domain_outcome,error_code,error_text,"
        "ending_rng,vi_start,vi_end,pred_passed,pred_total,cumulative_fake_attacks,"
        "successor_savestate_id,battle_context_artifact_id,predicate_group_revision_id,"
        "predicate_group_sha256,predicate_execution_package_sha256,predicate_evidence_blob,"
        "applied_input_artifact_id,input_trace_artifact_id,recorded_at_utc "
        "FROM ab_battle_single_turn_result_v1 WHERE exec_job_id=?1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st.st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(st.st, 1, exec_job_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    BattleSingleTurnResultSnapshot out{};
    out.battle_single_turn_result_id = sqlite3_column_int64(st.st, 0);
    out.turn_job_id = sqlite3_column_int64(st.st, 1);
    out.exec_job_id = sqlite3_column_int64(st.st, 2);
    out.worker_terminal_sha256 = ColumnText(st.st, 3);
    out.terminal_kind = ColumnText(st.st, 4);
    out.domain_outcome = ColumnTextOptional(st.st, 5);
    out.error_code = ColumnTextOptional(st.st, 6);
    out.error_text = ColumnTextOptional(st.st, 7);
    out.ending_rng = ColumnInt64Optional(st.st, 8);
    if (const auto value = ColumnInt64Optional(st.st, 9)) out.vi_start = static_cast<std::uint64_t>(*value);
    if (const auto value = ColumnInt64Optional(st.st, 10)) out.vi_end = static_cast<std::uint64_t>(*value);
    if (const auto value = ColumnInt64Optional(st.st, 11)) out.pred_passed = static_cast<std::uint32_t>(*value);
    if (const auto value = ColumnInt64Optional(st.st, 12)) out.pred_total = static_cast<std::uint32_t>(*value);
    if (const auto value = ColumnInt64Optional(st.st, 13)) out.cumulative_fake_attacks = static_cast<std::uint32_t>(*value);
    out.successor_savestate_id = ColumnInt64Optional(st.st, 14);
    out.battle_context_artifact_id = ColumnInt64Optional(st.st, 15);
    out.predicate_group_revision_id = ColumnInt64Optional(st.st, 16);
    out.predicate_group_sha256 = ColumnTextOptional(st.st, 17);
    out.predicate_execution_package_sha256 = ColumnTextOptional(st.st, 18);
    if (sqlite3_column_type(st.st, 19) != SQLITE_NULL) {
        const auto blob = ColumnBlob(st.st, 19);
        out.predicate_evidence_blob.assign(blob.begin(), blob.end());
    }
    out.applied_input_artifact_id = ColumnInt64Optional(st.st, 20);
    out.input_trace_artifact_id = ColumnInt64Optional(st.st, 21);
    out.recorded_at_utc = types::UtcTimePoint(
        std::chrono::milliseconds(sqlite3_column_int64(st.st, 22)));
    return out;
}

bool SqliteAnalysisDb::ApplyBattleTurnAdvancement(
    const ApplyBattleTurnAdvancementCommand& command,
    ApplyBattleTurnAdvancementReceipt* receipt_out,
    std::string* error_out) {
    if (receipt_out) *receipt_out = {};
    if (db_ == nullptr || command.battle_set_id <= 0 || command.turn_index <= 0
        || command.expected_results.empty() || command.wave_statuses.empty()
        || command.battle_set_status == BattleSetStatus::Unknown) {
        if (error_out) *error_out = "battle turn advancement command is incomplete";
        return false;
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const auto fail = [&](std::string message) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = std::move(message);
        return false;
    };

    Statement corpus_count;
    if (sqlite3_prepare_v2(db_,
            "SELECT COUNT(*) FROM ab_turn_job j JOIN ab_turn_wave w ON w.wave_id=j.wave_id "
            "WHERE w.battle_set_id=?1 AND w.turn_index=?2;",
            -1, &corpus_count.st, nullptr) != SQLITE_OK) {
        return fail(sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(corpus_count.st, 1, command.battle_set_id);
    sqlite3_bind_int(corpus_count.st, 2, command.turn_index);
    if (sqlite3_step(corpus_count.st) != SQLITE_ROW
        || sqlite3_column_int64(corpus_count.st, 0)
            != static_cast<sqlite3_int64>(command.expected_results.size())) {
        return fail("battle turn advancement expected-result corpus is incomplete");
    }

    std::set<std::int64_t> expected_exec_jobs;
    for (const auto& expected : command.expected_results) {
        if (expected.wave_id <= 0 || expected.turn_job_id <= 0
            || expected.exec_job_id <= 0 || expected.worker_terminal_sha256.empty()
            || !expected_exec_jobs.insert(expected.exec_job_id).second) {
            return fail("battle turn advancement expected-result identity is invalid");
        }
        Statement result;
        if (sqlite3_prepare_v2(db_,
                "SELECT r.turn_job_id,r.worker_terminal_sha256,r.terminal_kind,j.wave_id,w.battle_set_id,w.turn_index "
                "FROM ab_battle_single_turn_result_v1 r "
                "JOIN ab_turn_job j ON j.turn_job_id=r.turn_job_id "
                "JOIN ab_turn_wave w ON w.wave_id=j.wave_id WHERE r.exec_job_id=?1;",
                -1, &result.st, nullptr) != SQLITE_OK) {
            return fail(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(result.st, 1, expected.exec_job_id);
        if (sqlite3_step(result.st) != SQLITE_ROW
            || sqlite3_column_int64(result.st, 0) != expected.turn_job_id
            || ColumnText(result.st, 1) != expected.worker_terminal_sha256
            || ColumnText(result.st, 2) != "SUCCEEDED"
            || sqlite3_column_int64(result.st, 3) != expected.wave_id
            || sqlite3_column_int64(result.st, 4) != command.battle_set_id
            || sqlite3_column_int(result.st, 5) != command.turn_index) {
            return fail("battle turn advancement result fence rejected the current corpus");
        }
    }

    struct ResolvedPool {
        const BattleTurnAdvancementPoolPlan* plan = nullptr;
        std::int64_t id = 0;
        bool existed = false;
    };
    std::vector<ResolvedPool> pools;
    bool any_existing = false;
    bool all_existing = true;
    for (const auto& plan : command.pools) {
        if (plan.pool_name.empty()
            || plan.criterion_kind == BattleAdvancementCriterionKind::Unknown) {
            return fail("battle turn advancement pool plan is invalid");
        }
        Statement existing;
        if (sqlite3_prepare_v2(db_,
                "SELECT battle_advancement_pool_id,criterion_kind FROM ab_battle_advancement_pool "
                "WHERE battle_set_id=?1 AND turn_index=?2 AND pool_name=?3;",
                -1, &existing.st, nullptr) != SQLITE_OK) {
            return fail(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(existing.st, 1, command.battle_set_id);
        sqlite3_bind_int(existing.st, 2, command.turn_index);
        sqlite3_bind_text(existing.st, 3, plan.pool_name.c_str(), -1, SQLITE_TRANSIENT);
        ResolvedPool resolved{.plan = &plan};
        if (sqlite3_step(existing.st) == SQLITE_ROW) {
            resolved.id = sqlite3_column_int64(existing.st, 0);
            resolved.existed = true;
            any_existing = true;
            if (ColumnText(existing.st, 1) != ToDbString(plan.criterion_kind)) {
                return fail("battle turn advancement pool shape conflicts with the requested plan");
            }
            std::map<std::int64_t, std::pair<std::string, std::string>> actual;
            Statement decisions;
            if (sqlite3_prepare_v2(db_,
                    "SELECT turn_job_id,decision_kind,COALESCE(decision_reason,'') "
                    "FROM ab_battle_advancement_decision WHERE battle_advancement_pool_id=?1;",
                    -1, &decisions.st, nullptr) != SQLITE_OK) {
                return fail(sqlite3_errmsg(db_));
            }
            sqlite3_bind_int64(decisions.st, 1, resolved.id);
            while (sqlite3_step(decisions.st) == SQLITE_ROW) {
                actual.emplace(sqlite3_column_int64(decisions.st, 0),
                    std::pair{ColumnText(decisions.st, 1), ColumnText(decisions.st, 2)});
            }
            if (actual.size() != plan.decisions.size()) {
                return fail("battle turn advancement persisted pool is partial");
            }
            for (const auto& decision : plan.decisions) {
                const auto found = actual.find(decision.turn_job_id);
                if (found == actual.end()
                    || found->second.first != ToDbString(decision.decision_kind)
                    || found->second.second != decision.decision_reason.value_or("")) {
                    return fail("battle turn advancement persisted decision conflicts with the requested plan");
                }
            }
        } else {
            all_existing = false;
        }
        pools.push_back(resolved);
    }

    std::vector<std::optional<std::int64_t>> existing_children;
    existing_children.reserve(command.children.size());
    for (const auto& child : command.children) {
        if (child.parent_wave_id <= 0 || child.parent_turn_job_id <= 0
            || child.seed_candidate_id <= 0) {
            return fail("battle turn advancement child plan is invalid");
        }
        Statement existing;
        if (sqlite3_prepare_v2(db_,
                "SELECT wave_id,battle_set_id,seed_candidate_id,battle_advancement_pool_id "
                "FROM ab_turn_wave WHERE parent_wave_id=?1 AND parent_turn_job_id=?2 AND turn_index=?3;",
                -1, &existing.st, nullptr) != SQLITE_OK) {
            return fail(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(existing.st, 1, child.parent_wave_id);
        sqlite3_bind_int64(existing.st, 2, child.parent_turn_job_id);
        sqlite3_bind_int(existing.st, 3, command.turn_index + 1);
        if (sqlite3_step(existing.st) == SQLITE_ROW) {
            const auto wave_id = sqlite3_column_int64(existing.st, 0);
            any_existing = true;
            std::optional<std::int64_t> expected_pool;
            if (child.pool_name) {
                const auto pool = std::ranges::find(pools, *child.pool_name,
                    [](const ResolvedPool& item) { return item.plan->pool_name; });
                if (pool == pools.end() || pool->id <= 0)
                    return fail("battle turn advancement child pool is unresolved");
                expected_pool = pool->id;
            }
            const auto actual_pool = ColumnInt64Optional(existing.st, 3);
            if (sqlite3_column_int64(existing.st, 1) != command.battle_set_id
                || sqlite3_column_int64(existing.st, 2) != child.seed_candidate_id
                || actual_pool != expected_pool) {
                return fail("battle turn advancement persisted child conflicts with the requested plan");
            }
            existing_children.push_back(wave_id);
        } else {
            all_existing = false;
            existing_children.push_back(std::nullopt);
        }
    }

    for (const auto& status : command.wave_statuses) {
        if (status.wave_id <= 0 || status.status == BattleTurnWaveStatus::Unknown)
            return fail("battle turn advancement wave status plan is invalid");
        Statement existing;
        if (sqlite3_prepare_v2(db_,
                "SELECT status FROM ab_turn_wave WHERE wave_id=?1 AND battle_set_id=?2 AND turn_index=?3;",
                -1, &existing.st, nullptr) != SQLITE_OK) {
            return fail(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(existing.st, 1, status.wave_id);
        sqlite3_bind_int64(existing.st, 2, command.battle_set_id);
        sqlite3_bind_int(existing.st, 3, command.turn_index);
        if (sqlite3_step(existing.st) != SQLITE_ROW)
            return fail("battle turn advancement wave is missing");
        if (ColumnText(existing.st, 0) == ToDbString(status.status)) {
            any_existing = true;
        } else {
            all_existing = false;
        }
    }

    if (any_existing && !all_existing)
        return fail("battle turn advancement persisted shape is partial");
    if (all_existing) {
        ApplyBattleTurnAdvancementReceipt receipt{};
        receipt.disposition = ApplyBattleTurnAdvancementDisposition::AlreadyApplied;
        for (const auto wave_id : existing_children) receipt.child_wave_ids.push_back(*wave_id);
        if (receipt_out) *receipt_out = std::move(receipt);
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK)
            return fail(sqlite3_errmsg(db_));
        if (error_out) error_out->clear();
        return true;
    }

    for (auto& pool : pools) {
        Statement insert;
        if (sqlite3_prepare_v2(db_,
                "INSERT INTO ab_battle_advancement_pool(battle_set_id,turn_index,pool_name,criterion_kind,created_at_utc) "
                "VALUES(?1,?2,?3,?4,?5);", -1, &insert.st, nullptr) != SQLITE_OK)
            return fail(sqlite3_errmsg(db_));
        sqlite3_bind_int64(insert.st, 1, command.battle_set_id);
        sqlite3_bind_int(insert.st, 2, command.turn_index);
        sqlite3_bind_text(insert.st, 3, pool.plan->pool_name.c_str(), -1, SQLITE_TRANSIENT);
        const auto criterion = ToDbString(pool.plan->criterion_kind);
        sqlite3_bind_text(insert.st, 4, criterion.data(), static_cast<int>(criterion.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert.st, 5, command.applied_at_utc.time_since_epoch().count());
        if (sqlite3_step(insert.st) != SQLITE_DONE) return fail(sqlite3_errmsg(db_));
        pool.id = sqlite3_last_insert_rowid(db_);
        if (!InsertBattleOutboxEvent(db_, "AnalysisBattle.BattleAdvancementPoolCreated.v1",
                "battle_set", std::to_string(command.battle_set_id), command.correlation_id,
                command.causation_id, command.applied_at_utc.time_since_epoch().count(),
                "battle_advancement_pool", pool.id, error_out)) return fail(error_out ? *error_out : sqlite3_errmsg(db_));
        for (const auto& decision : pool.plan->decisions) {
            Statement insert_decision;
            if (sqlite3_prepare_v2(db_,
                    "INSERT INTO ab_battle_advancement_decision(battle_advancement_pool_id,turn_job_id,decision_kind,decision_reason,created_at_utc) "
                    "VALUES(?1,?2,?3,?4,?5);", -1, &insert_decision.st, nullptr) != SQLITE_OK)
                return fail(sqlite3_errmsg(db_));
            sqlite3_bind_int64(insert_decision.st, 1, pool.id);
            sqlite3_bind_int64(insert_decision.st, 2, decision.turn_job_id);
            const auto kind = ToDbString(decision.decision_kind);
            sqlite3_bind_text(insert_decision.st, 3, kind.data(), static_cast<int>(kind.size()), SQLITE_TRANSIENT);
            if (decision.decision_reason) sqlite3_bind_text(insert_decision.st, 4, decision.decision_reason->c_str(), -1, SQLITE_TRANSIENT);
            else sqlite3_bind_null(insert_decision.st, 4);
            sqlite3_bind_int64(insert_decision.st, 5, command.applied_at_utc.time_since_epoch().count());
            if (sqlite3_step(insert_decision.st) != SQLITE_DONE) return fail(sqlite3_errmsg(db_));
            const auto decision_id = sqlite3_last_insert_rowid(db_);
            if (!InsertBattleOutboxEvent(db_, "AnalysisBattle.BattleAdvancementDecisionRecorded.v1",
                    "battle_set", std::to_string(command.battle_set_id), command.correlation_id,
                    command.causation_id, command.applied_at_utc.time_since_epoch().count(),
                    "battle_advancement_decision", decision_id, error_out)) return fail(error_out ? *error_out : sqlite3_errmsg(db_));
        }
    }

    ApplyBattleTurnAdvancementReceipt receipt{};
    receipt.disposition = ApplyBattleTurnAdvancementDisposition::Applied;
    for (const auto& child : command.children) {
        std::optional<std::int64_t> pool_id;
        if (child.pool_name) {
            const auto pool = std::ranges::find(pools, *child.pool_name,
                [](const ResolvedPool& item) { return item.plan->pool_name; });
            if (pool == pools.end()) return fail("battle turn advancement child pool is unresolved");
            pool_id = pool->id;
        }
        Statement insert;
        if (sqlite3_prepare_v2(db_,
                "INSERT INTO ab_turn_wave(battle_set_id,turn_index,parent_wave_id,parent_turn_job_id,seed_candidate_id,battle_advancement_pool_id,status,created_at_utc) "
                "VALUES(?1,?2,?3,?4,?5,?6,'READY',?7);", -1, &insert.st, nullptr) != SQLITE_OK)
            return fail(sqlite3_errmsg(db_));
        sqlite3_bind_int64(insert.st, 1, command.battle_set_id);
        sqlite3_bind_int(insert.st, 2, command.turn_index + 1);
        sqlite3_bind_int64(insert.st, 3, child.parent_wave_id);
        sqlite3_bind_int64(insert.st, 4, child.parent_turn_job_id);
        sqlite3_bind_int64(insert.st, 5, child.seed_candidate_id);
        if (pool_id) sqlite3_bind_int64(insert.st, 6, *pool_id); else sqlite3_bind_null(insert.st, 6);
        sqlite3_bind_int64(insert.st, 7, command.applied_at_utc.time_since_epoch().count());
        if (sqlite3_step(insert.st) != SQLITE_DONE) return fail(sqlite3_errmsg(db_));
        const auto child_id = sqlite3_last_insert_rowid(db_);
        receipt.child_wave_ids.push_back(child_id);
        if (!InsertBattleOutboxEvent(db_, "AnalysisBattle.TurnWaveCreated.v1",
                "battle_set", std::to_string(command.battle_set_id), command.correlation_id,
                command.causation_id, command.applied_at_utc.time_since_epoch().count(),
                "turn_wave", child_id, error_out)) return fail(error_out ? *error_out : sqlite3_errmsg(db_));
    }

    for (const auto& status : command.wave_statuses) {
        Statement update;
        if (sqlite3_prepare_v2(db_,
                "UPDATE ab_turn_wave SET status=?2,completed_at_utc=?3 WHERE wave_id=?1;",
                -1, &update.st, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
        sqlite3_bind_int64(update.st, 1, status.wave_id);
        const auto value = ToDbString(status.status);
        sqlite3_bind_text(update.st, 2, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
        if (status.completed_at_utc) sqlite3_bind_int64(update.st, 3, status.completed_at_utc->time_since_epoch().count());
        else sqlite3_bind_null(update.st, 3);
        if (sqlite3_step(update.st) != SQLITE_DONE) return fail(sqlite3_errmsg(db_));
        if (!InsertBattleOutboxEvent(db_, "AnalysisBattle.TurnWaveStatusUpdated.v1",
                "battle_set", std::to_string(command.battle_set_id), command.correlation_id,
                command.causation_id, command.applied_at_utc.time_since_epoch().count(),
                "turn_wave", status.wave_id, error_out)) return fail(error_out ? *error_out : sqlite3_errmsg(db_));
    }

    Statement update_set;
    if (sqlite3_prepare_v2(db_,
            "UPDATE ab_battle_set SET status=?2,completed_at_utc=?3 WHERE battle_set_id=?1;",
            -1, &update_set.st, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    sqlite3_bind_int64(update_set.st, 1, command.battle_set_id);
    const auto set_status = ToDbString(command.battle_set_status);
    sqlite3_bind_text(update_set.st, 2, set_status.data(), static_cast<int>(set_status.size()), SQLITE_TRANSIENT);
    if (command.battle_set_completed_at_utc) sqlite3_bind_int64(update_set.st, 3, command.battle_set_completed_at_utc->time_since_epoch().count());
    else sqlite3_bind_null(update_set.st, 3);
    if (sqlite3_step(update_set.st) != SQLITE_DONE) return fail(sqlite3_errmsg(db_));
    if (!InsertBattleOutboxEvent(db_, "AnalysisBattle.BattleSetStatusUpdated.v1",
            "battle_set", std::to_string(command.battle_set_id), command.correlation_id,
            command.causation_id, command.applied_at_utc.time_since_epoch().count(),
            "battle_set", command.battle_set_id, error_out)) return fail(error_out ? *error_out : sqlite3_errmsg(db_));

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK)
        return fail(sqlite3_errmsg(db_));
    if (receipt_out) *receipt_out = std::move(receipt);
    if (error_out) error_out->clear();
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
        "SELECT battle_set_id,name,entry_savestate_id,battle_plan_id,battle_plan_fingerprint,"
        "continuation_mode,continue_automatic_exploration_after_victory,"
        "launch_fake_attack_min,launch_fake_attack_max,status,created_at_utc,completed_at_utc "
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
    out.battle_plan_id = sqlite3_column_int64(st.st, 3);
    out.battle_plan_fingerprint = ColumnText(st.st, 4);
    out.continuation_mode = ParseBattleContinuationMode(ColumnText(st.st, 5));
    out.continue_automatic_exploration_after_victory = sqlite3_column_int(st.st, 6) != 0;
    out.launch_fake_attack_min = sqlite3_column_int(st.st, 7);
    out.launch_fake_attack_max = sqlite3_column_int(st.st, 8);
    out.status = ParseBattleSetStatus(ColumnText(st.st, 9));
    out.created_at_utc = ColumnTime(st.st, 10);
    out.completed_at_utc = ColumnTimeOptional(st.st, 11);
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
    row.context_artifact_id = ColumnInt64Optional(st, 9);
    row.worker_terminal_sha256 = ColumnTextOptional(st, 10);
    if (const auto value = ColumnInt64Optional(st, 11)) row.entry_pc = static_cast<std::uint32_t>(*value);
    if (const auto value = ColumnInt64Optional(st, 12)) row.entry_vi_count = static_cast<std::uint64_t>(*value);
    if (const auto value = ColumnInt64Optional(st, 13)) row.entry_epoch = static_cast<std::uint64_t>(*value);
    if (const auto value = ColumnInt64Optional(st, 14)) row.capture_pc = static_cast<std::uint32_t>(*value);
    if (const auto value = ColumnInt64Optional(st, 15)) row.capture_vi_count = static_cast<std::uint64_t>(*value);
    if (const auto value = ColumnInt64Optional(st, 16)) row.capture_epoch = static_cast<std::uint64_t>(*value);
    row.materialization_key = ColumnTextOptional(st, 17);
    row.workflow_instance_id = ColumnInt64Optional(st, 18);
    row.workflow_step_id = ColumnInt64Optional(st, 19);
    row.source_savestate_artifact_id = ColumnInt64Optional(st, 20);
    row.source_savestate_sha256 = ColumnTextOptional(st, 21);
    row.full_phase_program_kind = ColumnInt64Optional(st, 22);
    row.full_phase_program_version = ColumnInt64Optional(st, 23);
    row.full_phase_canonical_id = ColumnTextOptional(st, 24);
    row.full_phase_contract_revision = ColumnInt64Optional(st, 25);
    row.full_phase_sha256 = ColumnTextOptional(st, 26);
    row.module_canonical_id = ColumnTextOptional(st, 27);
    row.module_revision = ColumnInt64Optional(st, 28);
    row.module_sha256 = ColumnTextOptional(st, 29);
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
        "SELECT context_probe_id,wave_id,source_savestate_id,exec_job_id,probe_status,context_blob,context_version,recorded_at_utc,created_at_utc,"
        "context_artifact_id,worker_terminal_sha256,entry_pc,entry_vi_count,entry_epoch,capture_pc,capture_vi_count,capture_epoch,"
        "materialization_key,workflow_instance_id,workflow_step_id,source_savestate_artifact_id,source_savestate_sha256,"
        "full_phase_program_kind,full_phase_program_version,full_phase_canonical_id,"
        "full_phase_contract_revision,full_phase_sha256,module_canonical_id,module_revision,module_sha256 "
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
        "SELECT context_probe_id,wave_id,source_savestate_id,exec_job_id,probe_status,context_blob,context_version,recorded_at_utc,created_at_utc,"
        "context_artifact_id,worker_terminal_sha256,entry_pc,entry_vi_count,entry_epoch,capture_pc,capture_vi_count,capture_epoch,"
        "materialization_key,workflow_instance_id,workflow_step_id,source_savestate_artifact_id,source_savestate_sha256,"
        "full_phase_program_kind,full_phase_program_version,full_phase_canonical_id,"
        "full_phase_contract_revision,full_phase_sha256,module_canonical_id,module_revision,module_sha256 "
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
        "SELECT context_probe_id,wave_id,source_savestate_id,exec_job_id,probe_status,context_blob,context_version,recorded_at_utc,created_at_utc,"
        "context_artifact_id,worker_terminal_sha256,entry_pc,entry_vi_count,entry_epoch,capture_pc,capture_vi_count,capture_epoch,"
        "materialization_key,workflow_instance_id,workflow_step_id,source_savestate_artifact_id,source_savestate_sha256,"
        "full_phase_program_kind,full_phase_program_version,full_phase_canonical_id,"
        "full_phase_contract_revision,full_phase_sha256,module_canonical_id,module_revision,module_sha256 "
        "FROM ab_battle_context_probe WHERE wave_id=?1 AND probe_status='SUCCEEDED' AND (context_blob IS NOT NULL OR context_artifact_id IS NOT NULL) "
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

std::vector<BattleAdvancementPoolRow> SqliteAnalysisDb::ListBattleAdvancementPoolsForBattleTurn(
    std::int64_t battle_set_id,
    int turn_index) const {
    std::vector<BattleAdvancementPoolRow> rows;
    if (db_ == nullptr || battle_set_id <= 0 || turn_index <= 0) return rows;
    Statement st;
    constexpr const char* kSql =
        "SELECT battle_advancement_pool_id,battle_set_id,turn_index,pool_name,"
        "criterion_kind,created_at_utc FROM ab_battle_advancement_pool "
        "WHERE battle_set_id=?1 AND turn_index=?2 ORDER BY battle_advancement_pool_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) return rows;
    sqlite3_bind_int64(st.st, 1, battle_set_id);
    sqlite3_bind_int(st.st, 2, turn_index);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        BattleAdvancementPoolRow row{};
        row.battle_advancement_pool_id = sqlite3_column_int64(st.st, 0);
        row.battle_set_id = sqlite3_column_int64(st.st, 1);
        row.turn_index = sqlite3_column_int(st.st, 2);
        row.pool_name = ColumnText(st.st, 3);
        row.criterion_kind = ParseBattleAdvancementCriterionKind(ColumnText(st.st, 4));
        row.created_at_utc = ColumnTime(st.st, 5);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::optional<std::int64_t> SqliteAnalysisDb::GetBattleWorkflowInstanceId(
    std::int64_t battle_set_id) const {
    if (db_ == nullptr || battle_set_id <= 0) return std::nullopt;
    Statement st;
    if (sqlite3_prepare_v2(
            db_, "SELECT workflow_instance_id FROM ab_battle_start WHERE battle_set_id=?1;",
            -1, &st.st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(st.st, 1, battle_set_id);
    return sqlite3_step(st.st) == SQLITE_ROW
        ? std::optional<std::int64_t>(sqlite3_column_int64(st.st, 0))
        : std::nullopt;
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
    if (envelope.event_type == "AnalysisBattle.BattleRecordingCreated.v1"
        || envelope.event_type == "AnalysisBattle.BattleRecordingCompleted.v1"
        || envelope.event_type == "AnalysisBattle.BattleRecordingFailed.v1") {
        const auto view = battle_row_resolver_.ResolveBattleRecording(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) return std::nullopt;
        BattlePayloadRecord record{};
        record.battle_completion_id = view->battle_completion_id;
        record.battle_recording_id = view->battle_recording_id;
        return record;
    }
    if (envelope.event_type == "AnalysisBattle.BattleReplayCreated.v1"
        || envelope.event_type == "AnalysisBattle.BattleReplayCompleted.v1"
        || envelope.event_type == "AnalysisBattle.BattleReplayFailed.v1") {
        const auto view = battle_row_resolver_.ResolveBattleReplay(
            envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) return std::nullopt;
        BattlePayloadRecord record{};
        record.battle_completion_id = view->battle_completion_id;
        record.battle_replay_id = view->battle_replay_id;
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
