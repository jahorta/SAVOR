#include "ArchiveWorkflowCommands.h"

#include <fstream>
#include <sstream>

namespace savor::runner::parallel::savordb {
namespace {

struct Statement {
    sqlite3_stmt* st = nullptr;
    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }
};

std::optional<std::string> ReadFileText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string Fnv1a64(const std::string& data) {
    constexpr std::uint64_t kOffset = 1469598103934665603ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t hash = kOffset;
    for (const auto c : data) {
        hash ^= static_cast<unsigned char>(c);
        hash *= kPrime;
    }

    std::ostringstream out;
    out << std::hex;
    out.width(16);
    out.fill('0');
    out << hash;
    return out.str();
}

} // namespace

ArchiveWorkflowCommands::ArchiveWorkflowCommands(
    sqlite3* archive_db,
    savor::db::IArchiveDb* archive_service,
    savor::db::archive::IArchivePackageService* archive_package_service,
    savor::db::archive::IRehydrateExecutor* rehydrate_executor)
    : archive_db_(archive_db)
    , archive_service_(archive_service)
    , archive_package_service_(archive_package_service)
    , rehydrate_executor_(rehydrate_executor) {
}

ArchiveCommandSummary ArchiveWorkflowCommands::ArchivePreview(const ArchiveCommandRequest& request) const {
    ArchiveCommandSummary summary{};
    if (archive_package_service_ == nullptr) {
        summary.errors.push_back("archive package service is null");
        return summary;
    }

    std::string error;
    const auto preview = archive_package_service_->PreviewArchiveBatch(
        request.older_than_utc,
        request.now_utc,
        request.max_candidates,
        request.outbox_policy,
        &error);
    summary.success = error.empty();
    summary.candidate_count = static_cast<int>(preview.candidates.size());
    summary.ui_safe_floor_outbox_id = preview.ui_safe_floor_outbox_id;
    summary.retention_safe_floor_outbox_id = preview.outbox_retention.safe_purge_floor_outbox_id;
    summary.purged_outbox_rows = static_cast<int>(preview.purgeable_published_outbox_rows);
    summary.blocking_reasons = preview.blocking_reasons;
    if (!error.empty()) {
        summary.errors.push_back(error);
    }
    return summary;
}

WorkflowArchiveCommandSummary ArchiveWorkflowCommands::WorkflowArchivePreview(const WorkflowArchiveCommandRequest& request) const {
    WorkflowArchiveCommandSummary summary{};
    if (archive_package_service_ == nullptr) {
        summary.errors.push_back("archive package service is null");
        return summary;
    }

    std::string error;
    summary.preview = archive_package_service_->PreviewWorkflowArchive(request.selection, &error);
    summary.success = summary.preview.success && error.empty();
    summary.blocking_reasons = summary.preview.purge_blockers;
    if (!error.empty()) {
        summary.errors.push_back(error);
    } else if (summary.preview.error.has_value()) {
        summary.errors.push_back(*summary.preview.error);
    }
    return summary;
}

WorkflowArchiveCommandSummary ArchiveWorkflowCommands::WorkflowArchiveExecute(const WorkflowArchiveCommandRequest& request) const {
    savor::db::archive::EmitArchiveProgress(
        request.progress_sink,
        savor::db::archive::ArchiveOperationPhase::Previewing,
        "Previewing selected workflows");
    WorkflowArchiveCommandSummary summary = WorkflowArchivePreview(request);
    if (!summary.success) {
        savor::db::archive::EmitArchiveProgress(
            request.progress_sink,
            savor::db::archive::ArchiveOperationPhase::Failed,
            "Workflow archive preview failed",
            0,
            0,
            false);
        return summary;
    }
    savor::db::archive::EmitArchiveProgress(
        request.progress_sink,
        savor::db::archive::ArchiveOperationPhase::Previewing,
        "Preview complete",
        summary.preview.workflow_count,
        summary.preview.workflow_count,
        false);

    savor::db::archive::CreateWorkflowArchivePackageRequest package_request{};
    package_request.selection = request.selection;
    package_request.created_at_utc = request.now_utc;
    package_request.archive_name = request.archive_name;
    package_request.archive_notes = request.archive_notes;
    package_request.correlation_id = request.trace_id.empty() ? "workflow-archive" : request.trace_id;
    package_request.causation_id = package_request.correlation_id;
    package_request.progress_sink = request.progress_sink;

    const auto package = archive_package_service_->CreateWorkflowPackage(package_request);
    if (!package.success) {
        summary.success = false;
        summary.errors.push_back(package.error.value_or("workflow archive package creation failed"));
        savor::db::archive::EmitArchiveProgress(
            request.progress_sink,
            savor::db::archive::ArchiveOperationPhase::Failed,
            summary.errors.back(),
            0,
            0,
            false);
        return summary;
    }
    summary.archive_package_id = package.archive_package_id;
    summary.package_root = package.package_root;

    savor::db::archive::EmitArchiveProgress(
        request.progress_sink,
        savor::db::archive::ArchiveOperationPhase::VerifyingPackage,
        "Verifying archive package");
    summary.verify = PackageVerify({ .archive_package_id = package.archive_package_id });
    if (!summary.verify.success) {
        summary.success = false;
        summary.blocking_reasons.insert(
            summary.blocking_reasons.end(),
            summary.verify.blocking_reasons.begin(),
            summary.verify.blocking_reasons.end());
        savor::db::archive::EmitArchiveProgress(
            request.progress_sink,
            savor::db::archive::ArchiveOperationPhase::Failed,
            "Archive package verification failed",
            0,
            0,
            false);
        return summary;
    }
    savor::db::archive::EmitArchiveProgress(
        request.progress_sink,
        savor::db::archive::ArchiveOperationPhase::VerifyingPackage,
        "Archive package verified",
        summary.verify.checksum_verified_files,
        summary.verify.manifest_file_count,
        false);

    if (request.purge_after_verify) {
        savor::db::archive::EmitArchiveProgress(
            request.progress_sink,
            savor::db::archive::ArchiveOperationPhase::PurgingSource,
            "Purging archived source rows");
        std::string purge_error;
        auto* sqlite_archive_service =
            dynamic_cast<savor::db::archive::SqliteArchivePackageService*>(
                archive_package_service_);
        if (sqlite_archive_service == nullptr) {
            summary.success = false;
            summary.errors.push_back(
                "workflow source deletion requires the internal SQLite archive service");
            return summary;
        }
        summary.purge = sqlite_archive_service->PurgeWorkflowArchiveSource(
            request.selection,
            package.archive_package_id,
            &purge_error);
        if (!summary.purge.success) {
            summary.success = false;
            summary.blocking_reasons.insert(
                summary.blocking_reasons.end(),
                summary.purge.blockers.begin(),
                summary.purge.blockers.end());
            if (!purge_error.empty()) {
                summary.errors.push_back(purge_error);
            } else if (summary.purge.error.has_value()) {
                summary.errors.push_back(*summary.purge.error);
            }
            savor::db::archive::EmitArchiveProgress(
                request.progress_sink,
                savor::db::archive::ArchiveOperationPhase::Failed,
                summary.errors.empty() ? "Archive source purge failed" : summary.errors.back(),
                0,
                0,
                false);
            return summary;
        }
        savor::db::archive::EmitArchiveProgress(
            request.progress_sink,
            savor::db::archive::ArchiveOperationPhase::PurgingSource,
            "Archived source rows purged",
            summary.purge.workflow_rows_deleted
                + summary.purge.execution_rows_deleted
                + summary.purge.analysis_rows_deleted
                + summary.purge.ui_read_rows_deleted
                + summary.purge.savestate_rows_deleted
                + summary.purge.artifact_rows_deleted
                + summary.purge.savestate_files_deleted,
            summary.purge.workflow_rows_deleted
                + summary.purge.execution_rows_deleted
                + summary.purge.analysis_rows_deleted
                + summary.purge.ui_read_rows_deleted
                + summary.purge.savestate_rows_deleted
                + summary.purge.artifact_rows_deleted
                + summary.purge.savestate_files_deleted,
            false);
    }

    summary.success = true;
    savor::db::archive::EmitArchiveProgress(
        request.progress_sink,
        savor::db::archive::ArchiveOperationPhase::Complete,
        "Archive operation complete",
        1,
        1,
        false);
    return summary;
}

PackageVerifySummary ArchiveWorkflowCommands::PackageVerify(const PackageVerifyRequest& request) const {
    PackageVerifySummary summary{};
    summary.archive_package_id = request.archive_package_id;

    std::string error;
    const auto manifest_path = ManifestPathForPackage(request.archive_package_id, &error);
    if (!manifest_path.has_value()) {
        summary.blocking_reasons.push_back(error.empty() ? "archive package not found" : error);
        return summary;
    }

    const auto manifest_text = ReadFileText(*manifest_path);
    if (!manifest_text.has_value()) {
        summary.blocking_reasons.push_back("manifest missing");
        return summary;
    }

    Statement manifest_rows;
    if (sqlite3_prepare_v2(
            archive_db_,
            "SELECT json_extract(value,'$.item_kind'), json_extract(value,'$.path'), json_extract(value,'$.row_count'), json_extract(value,'$.checksum') "
            "FROM json_each(json_extract(?1, '$.files'));",
            -1,
            &manifest_rows.st,
            nullptr)
        != SQLITE_OK) {
        summary.blocking_reasons.push_back("failed to parse manifest json");
        return summary;
    }

    sqlite3_bind_text(manifest_rows.st, 1, manifest_text->c_str(), static_cast<int>(manifest_text->size()), SQLITE_TRANSIENT);

    while (sqlite3_step(manifest_rows.st) == SQLITE_ROW) {
        const auto item_kind = reinterpret_cast<const char*>(sqlite3_column_text(manifest_rows.st, 0));
        const auto rel_path = reinterpret_cast<const char*>(sqlite3_column_text(manifest_rows.st, 1));
        const auto row_count = sqlite3_column_int(manifest_rows.st, 2);
        const auto checksum = reinterpret_cast<const char*>(sqlite3_column_text(manifest_rows.st, 3));

        ++summary.manifest_file_count;
        summary.manifest_row_total += row_count;

        Statement db_count;
        if (sqlite3_prepare_v2(
                archive_db_,
                "SELECT COALESCE(SUM(item_count),0) FROM ar_archive_item WHERE archive_package_id=?1 AND item_kind=?2;",
                -1,
                &db_count.st,
                nullptr)
            == SQLITE_OK) {
            sqlite3_bind_int64(db_count.st, 1, request.archive_package_id);
            sqlite3_bind_text(db_count.st, 2, item_kind ? item_kind : "", -1, SQLITE_TRANSIENT);
            if (sqlite3_step(db_count.st) == SQLITE_ROW) {
                const auto db_rows = sqlite3_column_int(db_count.st, 0);
                summary.archive_item_row_total += db_rows;
                if (db_rows != row_count) {
                    summary.blocking_reasons.push_back(std::string("row_count_mismatch:") + (item_kind ? item_kind : ""));
                }
            }
        }

        const auto full_path = manifest_path->parent_path() / (rel_path ? rel_path : "");
        const auto file_text = ReadFileText(full_path);
        if (!file_text.has_value()) {
            summary.blocking_reasons.push_back(std::string("missing_file:") + (rel_path ? rel_path : ""));
            continue;
        }

        const auto computed = Fnv1a64(*file_text);
        if (!checksum || computed != checksum) {
            summary.blocking_reasons.push_back(std::string("checksum_mismatch:") + (rel_path ? rel_path : ""));
        }
        ++summary.checksum_verified_files;
    }

    summary.success = summary.blocking_reasons.empty();
    return summary;
}

RehydratePreviewSummary ArchiveWorkflowCommands::RehydratePreview(const RehydratePreviewRequest& request) const {
    RehydratePreviewSummary summary{};

    const auto verify = PackageVerify({ .archive_package_id = request.archive_package_id });
    summary.success = verify.success;
    summary.manifest_file_count = verify.manifest_file_count;
    summary.manifest_row_total = verify.manifest_row_total;
    summary.blocking_reasons = verify.blocking_reasons;

    if (rehydrate_executor_ != nullptr) {
        const auto preview = rehydrate_executor_->PreviewPackage({
            .archive_package_id = request.archive_package_id,
            .target_namespace = request.target_namespace,
        });
        summary.success = summary.success && preview.success;
        summary.expected_jobs = preview.job_count;
        summary.expected_workflows = preview.workflow_count;
        summary.execution_row_count = preview.execution_row_count;
        summary.analysis_row_count = preview.analysis_row_count;
        summary.state_savestate_count = preview.state_savestate_count;
        summary.savestate_zip_entry_count = preview.savestate_zip_entry_count;
        summary.blocking_reasons.insert(summary.blocking_reasons.end(), preview.blocking_reasons.begin(), preview.blocking_reasons.end());
        if (preview.error.has_value()) {
            summary.blocking_reasons.push_back(*preview.error);
        }
    }

    return summary;
}

ArchiveCommandSummary ArchiveWorkflowCommands::RehydrateExecute(const RehydrateExecuteRequest& request) const {
    ArchiveCommandSummary summary{};
    if (archive_service_ == nullptr || rehydrate_executor_ == nullptr) {
        summary.errors.push_back("archive service or rehydrate executor is null");
        savor::db::archive::EmitArchiveProgress(
            request.progress_sink,
            savor::db::archive::ArchiveOperationPhase::Failed,
            summary.errors.back(),
            0,
            0,
            false);
        return summary;
    }

    const auto preview = RehydratePreview({
        .archive_package_id = request.archive_package_id,
        .target_namespace = request.target_namespace,
    });
    if (!preview.success) {
        summary.blocking_reasons = preview.blocking_reasons;
        summary.errors.push_back("rehydrate preflight failed");
        savor::db::archive::EmitArchiveProgress(
            request.progress_sink,
            savor::db::archive::ArchiveOperationPhase::Failed,
            "Rehydrate preflight failed",
            0,
            0,
            false);
        return summary;
    }

    savor::db::archive::EmitArchiveProgress(
        request.progress_sink,
        savor::db::archive::ArchiveOperationPhase::Previewing,
        "Creating rehydrate request");
    std::string error;
    std::int64_t request_id = 0;
    if (!archive_service_->RequestRehydrate(
            {
                .archive_package_id = request.archive_package_id,
                .status = "REQUESTED",
                .requested_at_utc = request.now_utc,
                .target_namespace = request.target_namespace,
                .correlation_id = request.trace_id,
                .causation_id = request.trace_id,
            },
            &request_id,
            &error)) {
        summary.errors.push_back(error.empty() ? "request_rehydrate failed" : error);
        savor::db::archive::EmitArchiveProgress(
            request.progress_sink,
            savor::db::archive::ArchiveOperationPhase::Failed,
            summary.errors.back(),
            0,
            0,
            false);
        return summary;
    }

    auto result = rehydrate_executor_->Execute({
        .rehydrate_request_id = request_id,
        .now_utc = request.now_utc,
        .correlation_id = request.trace_id,
        .causation_id = request.trace_id,
        .progress_sink = request.progress_sink,
    });

    summary.success = result.success;
    summary.request_ids.push_back(request_id);
    summary.package_count = static_cast<int>(result.restored_job_count);
    if (result.error.has_value()) {
        summary.errors.push_back(*result.error);
    }
    if (!summary.success) {
        savor::db::archive::EmitArchiveProgress(
            request.progress_sink,
            savor::db::archive::ArchiveOperationPhase::Failed,
            summary.errors.empty() ? "Rehydrate failed" : summary.errors.back(),
            0,
            0,
            false);
    }
    return summary;
}

ArchiveCommandSummary ArchiveWorkflowCommands::RehydrateCleanup(const RehydrateCleanupRequest& request) const {
    ArchiveCommandSummary summary{};
    if (archive_db_ == nullptr || request.rehydrate_request_id <= 0) {
        summary.errors.push_back("invalid cleanup request");
        return summary;
    }

    Statement status;
    std::string state;
    if (sqlite3_prepare_v2(
            archive_db_,
            "SELECT status FROM ar_rehydrate_request WHERE rehydrate_request_id=?1;",
            -1,
            &status.st,
            nullptr)
        == SQLITE_OK) {
        sqlite3_bind_int64(status.st, 1, request.rehydrate_request_id);
        if (sqlite3_step(status.st) == SQLITE_ROW && sqlite3_column_text(status.st, 0)) {
            state = reinterpret_cast<const char*>(sqlite3_column_text(status.st, 0));
        }
    }

    if (state.empty()) {
        summary.errors.push_back("rehydrate request not found");
        return summary;
    }
    if (state == "REQUESTED") {
        summary.blocking_reasons.push_back("rehydrate request still active");
        return summary;
    }

    char* errmsg = nullptr;
    const auto sql = std::string("BEGIN;DELETE FROM ar_rehydrate_map WHERE rehydrate_request_id=")
        + std::to_string(request.rehydrate_request_id)
        + ";DELETE FROM ar_rehydrate_request WHERE rehydrate_request_id="
        + std::to_string(request.rehydrate_request_id)
        + ";COMMIT;";
    if (sqlite3_exec(archive_db_, sql.c_str(), nullptr, nullptr, &errmsg) != SQLITE_OK) {
        summary.errors.push_back(errmsg ? errmsg : "cleanup failed");
        sqlite3_free(errmsg);
        return summary;
    }

    summary.success = true;
    summary.request_ids.push_back(request.rehydrate_request_id);
    return summary;
}

std::optional<std::filesystem::path> ArchiveWorkflowCommands::ManifestPathForPackage(
    std::int64_t archive_package_id,
    std::string* error_out) const {
    if (archive_db_ == nullptr || archive_package_id <= 0) {
        if (error_out != nullptr) *error_out = "invalid manifest lookup request";
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            archive_db_,
            "SELECT manifest_path FROM ar_archive_package WHERE archive_package_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(archive_db_);
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, archive_package_id);
    if (sqlite3_step(st.st) != SQLITE_ROW || sqlite3_column_text(st.st, 0) == nullptr) {
        if (error_out != nullptr) *error_out = "archive package not found";
        return std::nullopt;
    }
    return std::filesystem::path(reinterpret_cast<const char*>(sqlite3_column_text(st.st, 0)));
}

} // namespace savor::runner::parallel::savordb
