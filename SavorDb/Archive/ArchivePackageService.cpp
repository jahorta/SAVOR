#include "ArchivePackageService.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

#include "../Common/Migrations/MigrationRunner.h"
#include "../State/ArtifactObjectStore.h"

namespace savor::db::archive {

namespace {

struct Statement {
    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }

    sqlite3_stmt* st = nullptr;
};

struct ExportSpec {
    std::string item_kind;
    std::string query;
};

struct ExportContext {
    std::filesystem::path package_root;
    std::filesystem::path data_dir;
    std::filesystem::path blob_dir;
    std::vector<ArchivePackageFileSummary> files;
    std::vector<ArchivePackageFileSummary> blobs;
    types::UtcTimePoint min_time{};
    types::UtcTimePoint max_time{};
    bool has_time = false;
};

std::string EscapeJson(std::string_view value) {
    std::ostringstream out;
    out << '"';
    for (char ch : value) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(ch))
                    << std::dec << std::setfill(' ');
            } else {
                out << ch;
            }
            break;
        }
    }
    out << '"';
    return out.str();
}

std::string Hex64(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

std::string SlugForPackageName(std::string_view value) {
    std::string slug;
    slug.reserve(value.size());
    bool last_dash = false;
    for (char ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            slug.push_back(static_cast<char>(std::tolower(c)));
            last_dash = false;
        } else if (!last_dash && !slug.empty()) {
            slug.push_back('-');
            last_dash = true;
        }
        if (slug.size() >= 64) {
            break;
        }
    }
    while (!slug.empty() && slug.back() == '-') {
        slug.pop_back();
    }
    return slug.empty() ? std::string("archive") : slug;
}

std::string Fnv1a64(const std::string& payload) {
    std::uint64_t hash = 1469598103934665603ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    for (unsigned char ch : payload) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= kPrime;
    }
    return Hex64(hash);
}

std::string ChecksumForFile(const std::filesystem::path& file_path) {
    std::ifstream in(file_path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return Fnv1a64(buffer.str());
}

bool IsTablePresent(sqlite3* db, std::string_view table_name, std::string* error_out) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db);
        }
        return false;
    }

    sqlite3_bind_text(st.st, 1, table_name.data(), static_cast<int>(table_name.size()), SQLITE_TRANSIENT);
    const int step = sqlite3_step(st.st);
    if (step == SQLITE_ROW) {
        return true;
    }
    if (step == SQLITE_DONE) {
        return false;
    }

    if (error_out != nullptr) {
        *error_out = sqlite3_errmsg(db);
    }
    return false;
}

bool WriteFileText(const std::filesystem::path& path, const std::string& content, std::string* error_out) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (error_out != nullptr) {
            *error_out = "failed to open output file: " + path.string();
        }
        return false;
    }
    out << content;
    if (!out.good()) {
        if (error_out != nullptr) {
            *error_out = "failed to write output file: " + path.string();
        }
        return false;
    }
    return true;
}

std::string ToStableSlug(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        } else if (ch == '_' || ch == '-') {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    return out;
}

std::string BuildObjectJson(sqlite3_stmt* st, const ArchivePackageRetentionPolicy& policy, ExportContext* context, int* blob_count) {
    std::ostringstream json;
    json << '{';

    bool first = true;
    const int column_count = sqlite3_column_count(st);
    for (int i = 0; i < column_count; ++i) {
        const char* raw_name = sqlite3_column_name(st, i);
        const std::string column_name = raw_name == nullptr ? "" : raw_name;

        if (!first) {
            json << ',';
        }
        first = false;

        json << EscapeJson(column_name) << ':';

        const int type = sqlite3_column_type(st, i);
        if (type == SQLITE_NULL) {
            json << "null";
            continue;
        }

        if (type == SQLITE_INTEGER) {
            const auto number = sqlite3_column_int64(st, i);
            json << number;
            if (column_name.size() >= 7 && column_name.rfind("_at_utc") == column_name.size() - 7) {
                const auto ts = types::UtcTimePoint(std::chrono::milliseconds(number));
                if (!context->has_time || ts < context->min_time) {
                    context->min_time = ts;
                }
                if (!context->has_time || ts > context->max_time) {
                    context->max_time = ts;
                }
                context->has_time = true;
            }
            continue;
        }

        if (type == SQLITE_FLOAT) {
            json << sqlite3_column_double(st, i);
            continue;
        }

        const auto* text_ptr = reinterpret_cast<const char*>(sqlite3_column_text(st, i));
        const int text_bytes = sqlite3_column_bytes(st, i);
        const std::string text_value = text_ptr == nullptr ? "" : std::string(text_ptr, text_bytes);

        const bool payload_candidate =
            column_name == "message"
            || column_name == "error_text"
            || column_name == "worker_terminal_error_text"
            || column_name == "result_processing_error_text"
            || column_name == "cancellation_reason_text"
            || column_name == "failure_text"
            || column_name == "last_error"
            || column_name == "meta_note"
            || column_name == "condition_value"
            || column_name == "action_value"
            || column_name == "guard_value"
            || column_name == "blocked_reason";

        if (payload_candidate && text_value.size() > policy.inline_payload_max_bytes) {
            const auto blob_hash = Fnv1a64(text_value);
            const auto blob_name = ToStableSlug(column_name) + "-" + blob_hash + ".blob";
            const auto rel_blob_path = std::filesystem::path("blobs") / blob_name;
            const auto full_blob_path = context->package_root / rel_blob_path;
            std::string write_error;
            if (WriteFileText(full_blob_path, text_value, &write_error)) {
                ArchivePackageFileSummary blob_summary{};
                blob_summary.item_kind = "blobs";
                blob_summary.relative_path = rel_blob_path;
                blob_summary.row_count = 1;
                blob_summary.checksum = ChecksumForFile(full_blob_path);
                context->blobs.push_back(std::move(blob_summary));

                ++(*blob_count);
                json << "{\"blob_ref\":" << EscapeJson(rel_blob_path.generic_string())
                     << ",\"checksum\":" << EscapeJson(blob_hash)
                     << ",\"bytes\":" << text_value.size() << '}';
                continue;
            }
        }

        json << EscapeJson(text_value);
    }

    json << '}';
    return json.str();
}

bool ExportTable(
    sqlite3* db,
    const ExportSpec& spec,
    const ArchivePackageRetentionPolicy& policy,
    ExportContext* context,
    std::string* error_out) {
    Statement st;
    if (sqlite3_prepare_v2(db, spec.query.c_str(), -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db);
        }
        return false;
    }

    const auto out_rel_path = std::filesystem::path("data") / (spec.item_kind + ".jsonl");
    const auto out_path = context->package_root / out_rel_path;
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (error_out != nullptr) {
            *error_out = "failed to open " + out_path.string();
        }
        return false;
    }

    int row_count = 0;
    int blob_count = 0;
    while (true) {
        const int rc = sqlite3_step(st.st);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            if (error_out != nullptr) {
                *error_out = sqlite3_errmsg(db);
            }
            return false;
        }

        const std::string object_json = BuildObjectJson(st.st, policy, context, &blob_count);
        out << object_json << '\n';
        ++row_count;
    }

    out.flush();
    if (!out.good()) {
        if (error_out != nullptr) {
            *error_out = "failed to flush " + out_path.string();
        }
        return false;
    }

    ArchivePackageFileSummary summary{};
    summary.item_kind = spec.item_kind;
    summary.relative_path = out_rel_path;
    summary.row_count = row_count;
    summary.checksum = ChecksumForFile(out_path);
    context->files.push_back(std::move(summary));
    return true;
}

bool ValidateChecksums(
    const std::filesystem::path& package_root,
    const std::vector<ArchivePackageFileSummary>& files,
    std::string* error_out) {
    for (const auto& file : files) {
        const auto current = ChecksumForFile(package_root / file.relative_path);
        if (current != file.checksum) {
            if (error_out != nullptr) {
                *error_out = "checksum mismatch for " + file.relative_path.generic_string();
            }
            return false;
        }
    }
    return true;
}

std::int64_t ToEpochMillis(types::UtcTimePoint value) {
    return value.time_since_epoch().count();
}

types::UtcTimePoint FromEpochMillis(std::int64_t value) {
    return types::UtcTimePoint(std::chrono::milliseconds(value));
}

std::vector<ExportSpec> BuildExportSpecs(const CreateArchivePackageRequest& request, bool has_workflow_event, bool has_trigger, bool has_outbox) {
    const auto root = std::to_string(request.source_job_set_id);
    const std::string scoped_job_sets =
        "WITH scoped_job_sets(job_set_id) AS ("
        "SELECT job_set_id FROM exec_job_set WHERE job_set_id=" + root + ") ";

    std::vector<ExportSpec> specs;
    specs.push_back(ExportSpec{
        "job_sets",
        scoped_job_sets
            + "SELECT * FROM exec_job_set WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets) ORDER BY job_set_id ASC;"
    });

    specs.push_back(ExportSpec{
        "worksets",
        scoped_job_sets
            + "SELECT workset_id,job_set_id,workflow_step_id,workset_key,program_kind,program_version,"
              "contract_key,module_canonical_id,module_version,module_sha256,entrypoint,verified_dependency_sha256,"
              "runtime_profile_sha256,program_package_sha256,execution_affinity_key,estimated_payload_bytes,priority,"
              "item_count,published_at_utc,derived_state_binding_sha256,"
              "CASE WHEN derived_state_binding_payload IS NULL THEN NULL ELSE hex(derived_state_binding_payload) END AS derived_state_binding_payload_hex,"
              "capture_binding_sha256,"
              "CASE WHEN capture_binding_payload IS NULL THEN NULL ELSE hex(capture_binding_payload) END AS capture_binding_payload_hex,"
              "progress_plan_sha256,CASE WHEN progress_plan_payload IS NULL THEN NULL ELSE hex(progress_plan_payload) END AS progress_plan_payload_hex "
              "FROM exec_workset WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets) ORDER BY workset_id ASC;"
    });

    specs.push_back(ExportSpec{
        "workset_dispatch_attempts",
        scoped_job_sets
            + "SELECT a.* FROM exec_workset_dispatch_attempt a "
              "JOIN exec_workset w ON w.workset_id=a.workset_id "
              "WHERE w.job_set_id IN (SELECT job_set_id FROM scoped_job_sets) "
              "ORDER BY a.dispatch_attempt_id ASC;"
    });

    specs.push_back(ExportSpec{
        "jobs",
        scoped_job_sets
            + "SELECT job_id,job_set_id,parent_job_id,program_kind,program_version,program_ref_kind,program_ref_id,"
              "fingerprint,priority,state,attempts,max_attempts,claimed_by_token,lease_expires_at_utc,queued_at_utc,"
              "started_at_utc,ended_at_utc,error_code,error_text,savestate_id,input_ini,workset_id,workset_item_ordinal,"
              "dispatch_attempt_id,reserved_attempt_id,execution_finished_at_utc,worker_terminal_status,"
              "worker_terminal_fingerprint,worker_terminal_id,worker_terminal_error_code,worker_terminal_error_text,"
              "worker_terminal_unstarted,NULL AS worker_result_blob_id,result_processing_state,"
              "result_processing_attempts,result_processing_failures,"
              "result_processing_error_code,result_processing_error_text,result_processing_failed_at_utc,"
              "result_processed_at_utc,cancellation_group_key,cancellation_state,cancellation_request_key,cancellation_reason_code,"
              "cancellation_reason_text,cancellation_requested_by,cancellation_caused_by_job_id,"
              "cancellation_requested_at_utc,cancellation_delivery_attempts,"
              "cancellation_last_delivery_error_code,cancellation_last_delivery_error_text,"
              "cancellation_last_delivery_failed_at_utc,cancellation_delivered_at_utc,cancellation_resolved_at_utc,"
              "cancellation_resolution_code "
              "FROM exec_job WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets) ORDER BY job_id ASC;"
    });

    specs.push_back(ExportSpec{
        "job_events",
        scoped_job_sets
            + "SELECT e.* FROM exec_job_event e "
              "JOIN exec_job j ON j.job_id=e.job_id "
              "WHERE j.job_set_id IN (SELECT job_set_id FROM scoped_job_sets) "
              "ORDER BY e.job_event_id ASC;"
    });

    specs.push_back(ExportSpec{
        "job_cancellation_requests",
        scoped_job_sets
            + "SELECT c.* FROM exec_job_cancellation_request c "
              "JOIN exec_job j ON j.job_id=c.job_id "
              "WHERE j.job_set_id IN (SELECT job_set_id FROM scoped_job_sets) "
              "ORDER BY c.cancellation_request_id ASC;"
    });

    const std::string scoped_instances =
        scoped_job_sets
        + ", scoped_instances(workflow_instance_id) AS ("
          "SELECT workflow_instance_id FROM exec_workflow_instance "
          "WHERE root_scope_kind='job_set' AND root_scope_id IN (SELECT job_set_id FROM scoped_job_sets) "
          "UNION "
          "SELECT DISTINCT s.workflow_instance_id FROM exec_workflow_step s "
          "WHERE s.job_set_id IN (SELECT job_set_id FROM scoped_job_sets)"
          ") ";

    specs.push_back(ExportSpec{
        "workflow_instances",
        scoped_instances
            + "SELECT * FROM exec_workflow_instance "
              "WHERE workflow_instance_id IN (SELECT workflow_instance_id FROM scoped_instances) "
              "ORDER BY workflow_instance_id ASC;"
    });

    specs.push_back(ExportSpec{
        "workflow_unit_activations",
        scoped_instances
            + "SELECT * FROM exec_workflow_unit_activation "
              "WHERE workflow_instance_id IN (SELECT workflow_instance_id FROM scoped_instances) "
              "ORDER BY workflow_unit_activation_id ASC;"
    });

    specs.push_back(ExportSpec{
        "workflow_unit_activation_edges",
        scoped_instances
            + "SELECT * FROM exec_workflow_unit_activation_edge "
              "WHERE workflow_instance_id IN (SELECT workflow_instance_id FROM scoped_instances) "
              "ORDER BY workflow_unit_activation_edge_id ASC;"
    });

    specs.push_back(ExportSpec{
        "workflow_steps",
        scoped_instances
            + "SELECT * FROM exec_workflow_step "
              "WHERE workflow_instance_id IN (SELECT workflow_instance_id FROM scoped_instances) "
              "ORDER BY workflow_step_id ASC;"
    });

    specs.push_back(ExportSpec{
        "workflow_edges",
        scoped_instances
            + "SELECT * FROM exec_workflow_edge "
              "WHERE workflow_instance_id IN (SELECT workflow_instance_id FROM scoped_instances) "
              "ORDER BY workflow_edge_id ASC;"
    });

    if (request.retention_policy.include_workflow_event && has_workflow_event) {
        specs.push_back(ExportSpec{
            "workflow_events",
            scoped_instances
                + "SELECT * FROM exec_workflow_event "
                  "WHERE workflow_instance_id IN (SELECT workflow_instance_id FROM scoped_instances) "
                  "ORDER BY workflow_event_id ASC;"
        });
    }

    if (request.retention_policy.include_trigger && has_trigger) {
        specs.push_back(ExportSpec{
            "triggers",
            scoped_job_sets
                + "SELECT * FROM exec_trigger "
                  "WHERE scope_kind='job_set' AND scope_id IN (SELECT job_set_id FROM scoped_job_sets) "
                  "OR scope_kind='job' AND scope_id IN ("
                  "SELECT job_id FROM exec_job WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets)) "
                  "ORDER BY trigger_id ASC;"
        });
    }

    if (request.retention_policy.include_outbox_message && has_outbox) {
        specs.push_back(ExportSpec{
            "outbox",
            scoped_job_sets
                + "SELECT * FROM exec_outbox_message "
                  "WHERE aggregate_kind='job_set' AND CAST(aggregate_id AS INTEGER) IN (SELECT job_set_id FROM scoped_job_sets) "
                  "OR aggregate_kind='job' AND CAST(aggregate_id AS INTEGER) IN ("
                  "SELECT job_id FROM exec_job WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets)) "
                  "OR aggregate_kind='workset' AND CAST(aggregate_id AS INTEGER) IN ("
                  "SELECT workset_id FROM exec_workset WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets)) "
                  "OR aggregate_kind='workset_dispatch_attempt' AND CAST(aggregate_id AS INTEGER) IN ("
                  "SELECT a.dispatch_attempt_id FROM exec_workset_dispatch_attempt a "
                  "JOIN exec_workset w ON w.workset_id=a.workset_id "
                  "WHERE w.job_set_id IN (SELECT job_set_id FROM scoped_job_sets)) "
                  "ORDER BY outbox_id ASC;"
        });
    }

    return specs;
}

std::vector<std::int64_t> NormalizeWorkflowSelection(const ArchiveWorkflowSelection& selection) {
    std::set<std::int64_t> excluded;
    for (const auto id : selection.explicit_exclusions) {
        if (id > 0) {
            excluded.insert(id);
        }
    }

    std::set<std::int64_t> normalized;
    for (const auto id : selection.workflow_instance_ids) {
        if (id > 0 && excluded.find(id) == excluded.end()) {
            normalized.insert(id);
        }
    }
    return std::vector<std::int64_t>(normalized.begin(), normalized.end());
}

std::string JoinIds(const std::vector<std::int64_t>& ids) {
    std::ostringstream out;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << ids[i];
    }
    return out.str();
}

std::string WorkflowJobSetCte(
    const std::string& workflow_ids,
    const std::vector<std::int64_t>& additional_job_set_ids = {}) {
    const auto additional_job_sets = additional_job_set_ids.empty()
        ? std::string{}
        : " UNION SELECT job_set_id FROM exec_job_set WHERE job_set_id IN ("
            + JoinIds(additional_job_set_ids) + ")";
    return "WITH scoped_job_sets(job_set_id) AS ("
           "SELECT job_set_id FROM exec_workflow_step WHERE workflow_instance_id IN (" + workflow_ids + ") AND job_set_id IS NOT NULL"
           + additional_job_sets + "), "
           "scoped_jobs(job_id) AS ("
           "SELECT job_id FROM exec_job WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets)) ";
}

std::string JobSetCte(std::int64_t job_set_id) {
    return "WITH scoped_job_sets(job_set_id) AS ("
           "SELECT job_set_id FROM exec_job_set WHERE job_set_id=" + std::to_string(job_set_id) + "), "
           "scoped_jobs(job_id) AS ("
           "SELECT job_id FROM exec_job WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets)) ";
}

std::vector<std::int64_t> QueryInt64Column(sqlite3* db, const std::string& query, std::string* error_out) {
    std::vector<std::int64_t> values;
    if (db == nullptr || query.empty()) {
        return values;
    }

    Statement st;
    if (sqlite3_prepare_v2(db, query.c_str(), -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return values;
    }

    while (true) {
        const int rc = sqlite3_step(st.st);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            if (error_out) *error_out = sqlite3_errmsg(db);
            values.clear();
            return values;
        }
        if (sqlite3_column_type(st.st, 0) != SQLITE_NULL) {
            values.push_back(sqlite3_column_int64(st.st, 0));
        }
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

std::int64_t QuerySingleInt64(sqlite3* db, const std::string& query, std::string* error_out) {
    Statement st;
    if (db == nullptr || sqlite3_prepare_v2(db, query.c_str(), -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out && db != nullptr) *error_out = sqlite3_errmsg(db);
        return 0;
    }
    const int rc = sqlite3_step(st.st);
    if (rc == SQLITE_ROW) {
        return sqlite3_column_int64(st.st, 0);
    }
    if (rc != SQLITE_DONE && error_out) {
        *error_out = sqlite3_errmsg(db);
    }
    return 0;
}

bool ValidateFinalWorkflowSelection(
    sqlite3* execution_db,
    const std::vector<std::int64_t>& workflow_ids,
    std::string* error_out) {
    if (execution_db == nullptr || workflow_ids.empty()) {
        if (error_out != nullptr) *error_out = "invalid workflow archive selection";
        return false;
    }

    std::string query_error;
    const auto final_count = QuerySingleInt64(
        execution_db,
        "SELECT COUNT(1) FROM exec_workflow_instance WHERE workflow_instance_id IN ("
            + JoinIds(workflow_ids) + ") AND state IN ('COMPLETED','CANCELED');",
        &query_error);
    if (!query_error.empty()) {
        if (error_out != nullptr) *error_out = query_error;
        return false;
    }
    if (final_count != static_cast<std::int64_t>(workflow_ids.size())) {
        if (error_out != nullptr) {
            *error_out = "workflow archive selection contains a missing or non-final workflow; only COMPLETED and CANCELED workflows are eligible";
        }
        return false;
    }
    return true;
}

bool CollectArchiveReadinessBlockers(
    sqlite3* execution_db,
    const std::string& scoped_job_sets_and_jobs,
    std::vector<std::string>* blockers,
    std::string* error_out) {
    if (execution_db == nullptr || blockers == nullptr || scoped_job_sets_and_jobs.empty()) {
        if (error_out != nullptr) *error_out = "invalid archive readiness request";
        return false;
    }

    const auto add_blocker_if_any = [&](const std::string& query, std::string message) {
        std::string query_error;
        const auto count = QuerySingleInt64(execution_db, query, &query_error);
        if (!query_error.empty()) {
            if (error_out != nullptr) *error_out = query_error;
            return false;
        }
        if (count > 0) {
            blockers->push_back(std::move(message) + " (" + std::to_string(count) + ")");
        }
        return true;
    };

    return add_blocker_if_any(
               scoped_job_sets_and_jobs
                   + "SELECT COUNT(1) FROM exec_job_set "
                     "WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets) "
                     "AND COALESCE(materialization_state,'')<>'WORKSET_PUBLICATION_COMPLETE';",
               "job-set workset publication is incomplete")
        && add_blocker_if_any(
               scoped_job_sets_and_jobs
                   + "SELECT COUNT(1) FROM exec_job "
                     "WHERE job_id IN (SELECT job_id FROM scoped_jobs) "
                     "AND (workset_id IS NULL OR workset_item_ordinal IS NULL);",
               "published workset membership is incomplete")
        && add_blocker_if_any(
               scoped_job_sets_and_jobs
                   + "SELECT COUNT(1) FROM exec_workset w "
                     "WHERE w.job_set_id IN (SELECT job_set_id FROM scoped_job_sets) "
                     "AND ("
                     "  (SELECT COUNT(1) FROM exec_job j WHERE j.workset_id=w.workset_id)<>w.item_count "
                     "  OR (SELECT COALESCE(MIN(j.workset_item_ordinal),-1) FROM exec_job j WHERE j.workset_id=w.workset_id)<>0 "
                     "  OR (SELECT COALESCE(MAX(j.workset_item_ordinal),-1) FROM exec_job j WHERE j.workset_id=w.workset_id)<>w.item_count-1"
                     ");",
               "published workset item membership is inconsistent")
        && add_blocker_if_any(
               scoped_job_sets_and_jobs
                   + "SELECT COUNT(1) FROM exec_workset_dispatch_attempt a "
                     "JOIN exec_workset w ON w.workset_id=a.workset_id "
                     "WHERE w.job_set_id IN (SELECT job_set_id FROM scoped_job_sets) "
                     "AND a.state IN ('CLAIMED','ACTIVE','DRAINING');",
               "workset dispatch is active")
        && add_blocker_if_any(
               scoped_job_sets_and_jobs
                   + "SELECT COUNT(1) FROM exec_job "
                     "WHERE job_id IN (SELECT job_id FROM scoped_jobs) "
                     "AND state='EXECUTION_FINISHED';",
               "job result processing is not terminal")
        && add_blocker_if_any(
               scoped_job_sets_and_jobs
                   + "SELECT COUNT(1) FROM exec_job "
                     "WHERE job_id IN (SELECT job_id FROM scoped_jobs) "
                     "AND cancellation_state IS NOT NULL "
                     "AND cancellation_state<>'RESOLVED';",
               "job cancellation is unresolved")
        && add_blocker_if_any(
               scoped_job_sets_and_jobs
                   + "SELECT COUNT(1) FROM exec_job_cancellation_request "
                     "WHERE job_id IN (SELECT job_id FROM scoped_jobs) "
                     "AND state<>'RESOLVED';",
               "job cancellation request history is unresolved")
        && add_blocker_if_any(
               scoped_job_sets_and_jobs
                   + "SELECT COUNT(1) FROM exec_job j "
                     "WHERE j.job_id IN (SELECT job_id FROM scoped_jobs) "
                     "AND j.cancellation_request_key IS NOT NULL "
                     "AND NOT EXISTS ("
                     "  SELECT 1 FROM exec_job_cancellation_request c "
                     "  WHERE c.job_id=j.job_id "
                     "    AND c.request_key=j.cancellation_request_key"
                     ");",
               "job cancellation summary has no authoritative history row")
        && add_blocker_if_any(
               scoped_job_sets_and_jobs
                   + "SELECT COUNT(1) FROM exec_job j "
                     "JOIN exec_temp_blob b ON b.temp_blob_id=j.worker_result_blob_id "
                     "WHERE j.job_id IN (SELECT job_id FROM scoped_jobs) "
                     "AND b.cleanup_state<>'DELETED';",
               "temporary result-blob cleanup is pending")
        && add_blocker_if_any(
               scoped_job_sets_and_jobs
                   + "SELECT COUNT(1) FROM exec_job "
                     "WHERE job_id IN (SELECT job_id FROM scoped_jobs) "
                     "AND cancellation_caused_by_job_id IS NOT NULL "
                     "AND cancellation_caused_by_job_id NOT IN (SELECT job_id FROM scoped_jobs);",
               "cancellation history depends on a job outside the archive scope")
        && add_blocker_if_any(
               scoped_job_sets_and_jobs
                   + "SELECT COUNT(1) FROM exec_job "
                     "WHERE job_id NOT IN (SELECT job_id FROM scoped_jobs) "
                     "AND cancellation_caused_by_job_id IN (SELECT job_id FROM scoped_jobs);",
               "a job outside the archive scope depends on scoped cancellation history")
        && add_blocker_if_any(
               scoped_job_sets_and_jobs
                   + "SELECT COUNT(1) FROM exec_job_cancellation_request "
                     "WHERE job_id IN (SELECT job_id FROM scoped_jobs) "
                     "AND caused_by_job_id IS NOT NULL "
                     "AND caused_by_job_id NOT IN (SELECT job_id FROM scoped_jobs);",
               "cancellation request history depends on a job outside the archive scope")
        && add_blocker_if_any(
               scoped_job_sets_and_jobs
                   + "SELECT COUNT(1) FROM exec_job_cancellation_request "
                     "WHERE job_id NOT IN (SELECT job_id FROM scoped_jobs) "
                     "AND caused_by_job_id IN (SELECT job_id FROM scoped_jobs);",
               "a cancellation request outside the archive scope depends on a scoped job");
}

std::string JoinBlockers(const std::vector<std::string>& blockers) {
    std::ostringstream out;
    for (std::size_t i = 0; i < blockers.size(); ++i) {
        if (i != 0) out << "; ";
        out << blockers[i];
    }
    return out.str();
}

bool ColumnPresent(sqlite3* db, std::string_view table_name, std::string_view column_name) {
    Statement st;
    const std::string pragma = "PRAGMA table_info(" + std::string(table_name) + ");";
    if (db == nullptr || sqlite3_prepare_v2(db, pragma.c_str(), -1, &st.st, nullptr) != SQLITE_OK) {
        return false;
    }
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 1));
        if (text != nullptr && column_name == text) {
            return true;
        }
    }
    return false;
}

std::vector<ExportSpec> BuildWorkflowExecutionSpecs(
    const std::vector<std::int64_t>& workflow_ids,
    const ArchivePackageRetentionPolicy& policy,
    sqlite3* execution_db,
    const std::vector<std::int64_t>& additional_job_set_ids = {}) {
    const auto ids = JoinIds(workflow_ids);
    const auto scoped =
        WorkflowJobSetCte(ids, additional_job_set_ids);
    std::vector<ExportSpec> specs;
    specs.push_back({"workflow_instances", "SELECT * FROM exec_workflow_instance WHERE workflow_instance_id IN (" + ids + ") ORDER BY workflow_instance_id ASC;"});
    specs.push_back({"workflow_steps", "SELECT * FROM exec_workflow_step WHERE workflow_instance_id IN (" + ids + ") ORDER BY workflow_step_id ASC;"});
    specs.push_back({"workflow_edges", "SELECT * FROM exec_workflow_edge WHERE workflow_instance_id IN (" + ids + ") ORDER BY workflow_edge_id ASC;"});
    if (IsTablePresent(execution_db, "exec_workflow_unit_activation", nullptr)) {
        specs.push_back({"workflow_unit_activations", "SELECT * FROM exec_workflow_unit_activation WHERE workflow_instance_id IN (" + ids + ") ORDER BY workflow_unit_activation_id ASC;"});
    }
    if (IsTablePresent(execution_db, "exec_workflow_unit_activation_edge", nullptr)) {
        specs.push_back({"workflow_unit_activation_edges", "SELECT * FROM exec_workflow_unit_activation_edge WHERE workflow_instance_id IN (" + ids + ") ORDER BY workflow_unit_activation_edge_id ASC;"});
    }
    if (IsTablePresent(execution_db, "exec_workflow_step_output", nullptr)) {
        specs.push_back({"workflow_step_outputs", "SELECT o.* FROM exec_workflow_step_output o JOIN exec_workflow_step s ON s.workflow_step_id=o.workflow_step_id WHERE s.workflow_instance_id IN (" + ids + ") ORDER BY o.workflow_step_output_id ASC;"});
    }
    if (IsTablePresent(execution_db, "exec_workflow_instance_input_binding", nullptr)) {
        specs.push_back({"workflow_instance_input_bindings", "SELECT * FROM exec_workflow_instance_input_binding WHERE workflow_instance_id IN (" + ids + ") ORDER BY workflow_instance_input_binding_id ASC;"});
    }
    if (IsTablePresent(execution_db, "exec_workflow_instance_argument", nullptr)) {
        specs.push_back({"workflow_instance_arguments", "SELECT * FROM exec_workflow_instance_argument WHERE workflow_instance_id IN (" + ids + ") ORDER BY workflow_instance_argument_id ASC;"});
    }
    if (IsTablePresent(execution_db, "exec_workflow_event", nullptr)) {
        specs.push_back({"workflow_events", "SELECT * FROM exec_workflow_event WHERE workflow_instance_id IN (" + ids + ") ORDER BY workflow_event_id ASC;"});
    }
    specs.push_back({"job_sets", scoped + "SELECT * FROM exec_job_set WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets) ORDER BY job_set_id ASC;"});
    specs.push_back({"worksets", scoped
        + "SELECT workset_id,job_set_id,workflow_step_id,workset_key,program_kind,program_version,"
          "contract_key,module_canonical_id,module_version,module_sha256,entrypoint,verified_dependency_sha256,"
          "runtime_profile_sha256,program_package_sha256,execution_affinity_key,estimated_payload_bytes,priority,"
          "item_count,published_at_utc,derived_state_binding_sha256,"
          "CASE WHEN derived_state_binding_payload IS NULL THEN NULL ELSE hex(derived_state_binding_payload) END AS derived_state_binding_payload_hex,"
          "capture_binding_sha256,"
          "CASE WHEN capture_binding_payload IS NULL THEN NULL ELSE hex(capture_binding_payload) END AS capture_binding_payload_hex,"
          "progress_plan_sha256,CASE WHEN progress_plan_payload IS NULL THEN NULL ELSE hex(progress_plan_payload) END AS progress_plan_payload_hex "
          "FROM exec_workset WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets) ORDER BY workset_id ASC;"});
    specs.push_back({"workset_dispatch_attempts", scoped + "SELECT a.* FROM exec_workset_dispatch_attempt a JOIN exec_workset w ON w.workset_id=a.workset_id WHERE w.job_set_id IN (SELECT job_set_id FROM scoped_job_sets) ORDER BY a.dispatch_attempt_id ASC;"});
    specs.push_back({
        "jobs",
        scoped
            + "SELECT job_id,job_set_id,parent_job_id,program_kind,program_version,program_ref_kind,program_ref_id,"
              "fingerprint,priority,state,attempts,max_attempts,claimed_by_token,lease_expires_at_utc,queued_at_utc,"
              "started_at_utc,ended_at_utc,error_code,error_text,savestate_id,input_ini,workset_id,workset_item_ordinal,"
              "dispatch_attempt_id,reserved_attempt_id,execution_finished_at_utc,worker_terminal_status,"
              "worker_terminal_fingerprint,worker_terminal_id,worker_terminal_error_code,worker_terminal_error_text,"
              "worker_terminal_unstarted,NULL AS worker_result_blob_id,result_processing_state,"
              "result_processing_attempts,result_processing_failures,"
              "result_processing_error_code,result_processing_error_text,result_processing_failed_at_utc,"
              "result_processed_at_utc,cancellation_group_key,cancellation_state,cancellation_request_key,cancellation_reason_code,"
              "cancellation_reason_text,cancellation_requested_by,cancellation_caused_by_job_id,"
              "cancellation_requested_at_utc,cancellation_delivery_attempts,"
              "cancellation_last_delivery_error_code,cancellation_last_delivery_error_text,"
              "cancellation_last_delivery_failed_at_utc,cancellation_delivered_at_utc,cancellation_resolved_at_utc,"
              "cancellation_resolution_code "
              "FROM exec_job WHERE job_id IN (SELECT job_id FROM scoped_jobs) ORDER BY job_id ASC;"
    });
    specs.push_back({
        "job_progress",
        scoped
            + "SELECT job_id,attempt_id,ordinal,dispatch_attempt_id,workset_item_ordinal,workset_id,item_id,invocation_id,"
              "library_id,library_revision,progress_point_id,has_routed_provenance,routed_sequence,sample_snapshot_id,"
              "trigger_epoch,schema_id,schema_revision,schema_sha256,hex(typed_payload) AS typed_payload_hex,"
              "display_text,recorded_at_utc FROM exec_job_progress "
              "WHERE job_id IN (SELECT job_id FROM scoped_jobs) ORDER BY job_id ASC,attempt_id ASC,ordinal ASC;"});
    specs.push_back({"job_events", scoped + "SELECT * FROM exec_job_event WHERE job_id IN (SELECT job_id FROM scoped_jobs) ORDER BY job_event_id ASC;"});
    specs.push_back({"job_cancellation_requests", scoped + "SELECT * FROM exec_job_cancellation_request WHERE job_id IN (SELECT job_id FROM scoped_jobs) ORDER BY cancellation_request_id ASC;"});
    if (policy.include_trigger && IsTablePresent(execution_db, "exec_trigger", nullptr)) {
        specs.push_back({"triggers", scoped + "SELECT * FROM exec_trigger WHERE (scope_kind='job_set' AND scope_id IN (SELECT job_set_id FROM scoped_job_sets)) OR (scope_kind='job' AND scope_id IN (SELECT job_id FROM scoped_jobs)) ORDER BY trigger_id ASC;"});
    }
    if (policy.include_outbox_message && IsTablePresent(execution_db, "exec_outbox_message", nullptr)) {
        specs.push_back({
            "outbox",
            scoped
                + "SELECT * FROM exec_outbox_message WHERE "
                  "(aggregate_kind='job_set' AND CAST(aggregate_id AS INTEGER) IN (SELECT job_set_id FROM scoped_job_sets)) "
                  "OR (aggregate_kind='job' AND CAST(aggregate_id AS INTEGER) IN (SELECT job_id FROM scoped_jobs)) "
                  "OR (aggregate_kind='workset' AND CAST(aggregate_id AS INTEGER) IN (SELECT workset_id FROM exec_workset WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets))) "
                  "OR (aggregate_kind='workset_dispatch_attempt' AND CAST(aggregate_id AS INTEGER) IN (SELECT a.dispatch_attempt_id FROM exec_workset_dispatch_attempt a JOIN exec_workset w ON w.workset_id=a.workset_id WHERE w.job_set_id IN (SELECT job_set_id FROM scoped_job_sets))) "
                  "ORDER BY outbox_id ASC;"
        });
    }
    return specs;
}

std::vector<ExportSpec> BuildWorkflowUiReadSpecs(sqlite3* ui_db, const std::vector<std::int64_t>& workflow_ids, const std::vector<std::int64_t>& job_ids) {
    const auto ids = JoinIds(workflow_ids);
    std::vector<ExportSpec> specs;
    if (ui_db == nullptr || ids.empty()) {
        return specs;
    }
    if (IsTablePresent(ui_db, "ui_workflow_instance", nullptr)) {
        specs.push_back({"ui_workflow_instances", "SELECT * FROM ui_workflow_instance WHERE workflow_instance_id IN (" + ids + ") ORDER BY workflow_instance_id ASC;"});
    }
    if (IsTablePresent(ui_db, "ui_workflow_step", nullptr)) {
        specs.push_back({"ui_workflow_steps", "SELECT * FROM ui_workflow_step WHERE workflow_instance_id IN (" + ids + ") ORDER BY workflow_step_id ASC;"});
    }
    if (IsTablePresent(ui_db, "ui_workflow_edge", nullptr)) {
        specs.push_back({"ui_workflow_edges", "SELECT * FROM ui_workflow_edge WHERE workflow_instance_id IN (" + ids + ") ORDER BY workflow_edge_id ASC;"});
    }
    if (IsTablePresent(ui_db, "ui_workflow_alert", nullptr)) {
        specs.push_back({"ui_workflow_alerts", "SELECT * FROM ui_workflow_alert WHERE workflow_instance_id IN (" + ids + ") ORDER BY workflow_alert_id ASC;"});
    }
    if (!job_ids.empty() && IsTablePresent(ui_db, "ui_battle_turn_job_replication", nullptr)) {
        specs.push_back({"ui_battle_turn_job_replications", "SELECT * FROM ui_battle_turn_job_replication WHERE exec_job_id IN (" + JoinIds(job_ids) + ") ORDER BY turn_job_id ASC;"});
    }
    return specs;
}

std::vector<ExportSpec> BuildWorkflowAnalysisSpecs(
    sqlite3* execution_db,
    sqlite3* analysis_db,
    const std::vector<std::int64_t>& job_ids,
    const std::vector<std::int64_t>& workflow_ids,
    std::vector<std::int64_t>* battle_set_ids_out) {
    std::vector<ExportSpec> specs;
    if (analysis_db == nullptr || (job_ids.empty() && workflow_ids.empty())) {
        return specs;
    }

    const auto job_id_list = job_ids.empty() ? std::string("0") : JoinIds(job_ids);
    const auto workflow_id_list = workflow_ids.empty() ? std::string("0") : JoinIds(workflow_ids);
    std::string error;

    if (!workflow_ids.empty() && IsTablePresent(analysis_db, "ab_battle_completion", nullptr)) {
        specs.push_back({
            "analysis_battle_completions",
            "SELECT battle_completion_id,workflow_instance_id,workflow_step_id,exec_job_id,battle_set_id,wave_id,"
                "selected_turn_job_id,selected_execution_job_id,entry_savestate_id,completion_savestate_id,manifest_version,"
                "CASE WHEN manifest_blob IS NULL THEN NULL ELSE hex(manifest_blob) END AS manifest_blob_hex,"
                "manifest_sha256,manifest_artifact_id,route_kind,transition_filename,worker_terminal_sha256,error_code,error_text,status,"
                "created_at_utc,completed_at_utc FROM ab_battle_completion WHERE workflow_instance_id IN (" + workflow_id_list
                + ") ORDER BY battle_completion_id ASC;"
        });
    }
    if (!workflow_ids.empty() && IsTablePresent(analysis_db, "ab_battle_recording", nullptr)) {
        specs.push_back({
            "analysis_battle_recordings",
            "SELECT battle_recording_id,battle_completion_id,workflow_instance_id,workflow_step_id,exec_job_id,"
                "source_savestate_id,source_dtm_artifact_id,source_itinerary_artifact_id,replay_plan_version,"
                "source_binding_version,hex(source_binding_blob) AS source_binding_blob_hex,source_binding_sha256,"
                "hex(replay_plan_blob) AS replay_plan_blob_hex,replay_plan_sha256,outcome,recorded_dtm_artifact_id,"
                "recorded_itinerary_artifact_id,paired_checkpoint_savestate_id,timing_anchor_version,"
                "CASE WHEN timing_anchor_blob IS NULL THEN NULL ELSE hex(timing_anchor_blob) END AS timing_anchor_blob_hex,"
                "tas_movie_tree_id,validation_request_id,sterilization_request_id,worker_terminal_sha256,error_code,error_text,status,"
                "created_at_utc,completed_at_utc FROM ab_battle_recording WHERE workflow_instance_id IN (" + workflow_id_list
                + ") ORDER BY battle_recording_id ASC;"
        });
    }
    if (!workflow_ids.empty() && IsTablePresent(analysis_db, "ab_battle_replay", nullptr)) {
        specs.push_back({
            "analysis_battle_replays",
            "SELECT battle_replay_id,battle_completion_id,workflow_instance_id,workflow_step_id,exec_job_id,"
                "source_savestate_id,source_dtm_artifact_id,source_itinerary_artifact_id,replay_plan_version,"
                "source_binding_version,hex(source_binding_blob) AS source_binding_blob_hex,source_binding_sha256,"
                "hex(replay_plan_blob) AS replay_plan_blob_hex,replay_plan_sha256,outcome,mismatch_turn,expected_rng,observed_rng,"
                "CASE WHEN observed_completion_blob IS NULL THEN NULL ELSE hex(observed_completion_blob) END AS observed_completion_blob_hex,"
                "observed_completion_sha256,CASE WHEN observed_transition_blob IS NULL THEN NULL ELSE hex(observed_transition_blob) END AS observed_transition_blob_hex,"
                "observed_transition_sha256,worker_terminal_sha256,error_code,error_text,status,created_at_utc,completed_at_utc "
                "FROM ab_battle_replay WHERE workflow_instance_id IN (" + workflow_id_list +
                ") ORDER BY battle_replay_id ASC;"
        });
    }
    std::vector<std::int64_t> wave_ids;
    if (IsTablePresent(analysis_db, "ab_turn_job", nullptr)) {
        wave_ids = QueryInt64Column(analysis_db, "SELECT wave_id FROM ab_turn_job WHERE exec_job_id IN (" + job_id_list + ") AND wave_id IS NOT NULL;", &error);
    }
    if (IsTablePresent(analysis_db, "ab_battle_context_probe", nullptr)) {
        auto probe_wave_ids = QueryInt64Column(analysis_db, "SELECT wave_id FROM ab_battle_context_probe WHERE exec_job_id IN (" + job_id_list + ") AND wave_id IS NOT NULL;", &error);
        wave_ids.insert(wave_ids.end(), probe_wave_ids.begin(), probe_wave_ids.end());
    }
    if (!wave_ids.empty()) {
        std::sort(wave_ids.begin(), wave_ids.end());
        wave_ids.erase(std::unique(wave_ids.begin(), wave_ids.end()), wave_ids.end());
    }

    std::vector<std::int64_t> battle_set_ids;
    if (!wave_ids.empty() && IsTablePresent(analysis_db, "ab_turn_wave", nullptr)) {
        battle_set_ids = QueryInt64Column(analysis_db, "SELECT battle_set_id FROM ab_turn_wave WHERE wave_id IN (" + JoinIds(wave_ids) + ") AND battle_set_id IS NOT NULL;", &error);
    }
    if (battle_set_ids_out != nullptr) {
        *battle_set_ids_out = battle_set_ids;
    }

    if (!battle_set_ids.empty() && IsTablePresent(analysis_db, "ab_battle_set", nullptr)) {
        specs.push_back({"analysis_battle_sets", "SELECT * FROM ab_battle_set WHERE battle_set_id IN (" + JoinIds(battle_set_ids) + ") ORDER BY battle_set_id ASC;"});
        if (IsTablePresent(analysis_db, "ab_seed_candidate", nullptr)) {
            specs.push_back({"analysis_seed_candidates", "SELECT * FROM ab_seed_candidate WHERE battle_set_id IN (" + JoinIds(battle_set_ids) + ") ORDER BY seed_candidate_id ASC;"});
        }
        if (IsTablePresent(analysis_db, "ab_battle_advancement_pool", nullptr)) {
            specs.push_back({"analysis_battle_advancement_pools", "SELECT * FROM ab_battle_advancement_pool WHERE battle_set_id IN (" + JoinIds(battle_set_ids) + ") ORDER BY battle_advancement_pool_id ASC;"});
        } else if (IsTablePresent(analysis_db, "ab_selection_pool", nullptr)) {
            specs.push_back({"analysis_battle_advancement_pools", "SELECT selection_pool_id AS battle_advancement_pool_id,battle_set_id,turn_index,pool_name,criterion_kind,created_at_utc FROM ab_selection_pool WHERE battle_set_id IN (" + JoinIds(battle_set_ids) + ") ORDER BY selection_pool_id ASC;"});
        }
    }
    if (!wave_ids.empty() && IsTablePresent(analysis_db, "ab_turn_wave", nullptr)) {
        specs.push_back({"analysis_turn_waves", "SELECT * FROM ab_turn_wave WHERE wave_id IN (" + JoinIds(wave_ids) + ") ORDER BY wave_id ASC;"});
    }
    if (!wave_ids.empty() && IsTablePresent(analysis_db, "ab_predicate_execution_package_v1", nullptr)) {
        specs.push_back({
            "analysis_predicate_execution_packages",
            "SELECT predicate_execution_package_id,wave_id,predicate_group_revision_id,"
            "predicate_group_sha256,execution_package_sha256,"
            "hex(execution_package_blob) AS execution_package_blob_hex,"
            "phase_program_kind,phase_program_version,phase_canonical_id,phase_revision,phase_sha256,"
            "hook_contract_canonical_id,hook_contract_revision,hook_contract_sha256,created_at_utc "
            "FROM ab_predicate_execution_package_v1 WHERE wave_id IN (" + JoinIds(wave_ids) + ") "
            "ORDER BY predicate_execution_package_id ASC;"
        });
    }
    if (IsTablePresent(analysis_db, "ab_battle_context_probe", nullptr)) {
        specs.push_back({"analysis_battle_context_probes", "SELECT * FROM ab_battle_context_probe WHERE exec_job_id IN (" + job_id_list + ") ORDER BY context_probe_id ASC;"});
    }
    if (IsTablePresent(analysis_db, "ab_turn_job", nullptr)) {
        specs.push_back({"analysis_battle_turn_jobs", "SELECT * FROM ab_turn_job WHERE exec_job_id IN (" + job_id_list + ") ORDER BY turn_job_id ASC;"});
    }
    if (IsTablePresent(analysis_db, "ab_battle_advancement_decision", nullptr)) {
        specs.push_back({"analysis_battle_advancement_decisions", "SELECT d.* FROM ab_battle_advancement_decision d JOIN ab_turn_job tj ON tj.turn_job_id=d.turn_job_id WHERE tj.exec_job_id IN (" + job_id_list + ") ORDER BY d.battle_advancement_decision_id ASC;"});
    } else if (IsTablePresent(analysis_db, "ab_selection_decision", nullptr)) {
        specs.push_back({"analysis_battle_advancement_decisions", "SELECT selection_decision_id AS battle_advancement_decision_id,selection_pool_id AS battle_advancement_pool_id,d.turn_job_id,CASE d.decision_kind WHEN 'WINNER' THEN 'SELECTED' WHEN 'DUPLICATE' THEN 'NOT_SELECTED' ELSE d.decision_kind END AS decision_kind,d.decision_reason,d.created_at_utc FROM ab_selection_decision d JOIN ab_turn_job tj ON tj.turn_job_id=d.turn_job_id WHERE tj.exec_job_id IN (" + job_id_list + ") ORDER BY d.selection_decision_id ASC;"});
    }
    if (IsTablePresent(analysis_db, "ab_manual_followup", nullptr)) {
        specs.push_back({"analysis_manual_followups", "SELECT f.* FROM ab_manual_followup f JOIN ab_turn_job tj ON tj.turn_job_id=f.turn_job_id WHERE tj.exec_job_id IN (" + job_id_list + ") ORDER BY f.manual_followup_id ASC;"});
    } else if (IsTablePresent(analysis_db, "ab_terminal_followup", nullptr)) {
        specs.push_back({"analysis_manual_followups", "SELECT terminal_followup_id AS manual_followup_id,turn_job_id,manual_followup_status,recorded_dtm_artifact_id,recorded_dtmini_artifact_id,recorded_sav_artifact_id,note,updated_at_utc FROM ab_terminal_followup f JOIN ab_turn_job tj ON tj.turn_job_id=f.turn_job_id WHERE tj.exec_job_id IN (" + job_id_list + ") ORDER BY f.terminal_followup_id ASC;"});
    }

    std::vector<std::int64_t> probe_run_ids;
    if (execution_db != nullptr && !job_ids.empty()) {
        auto values = QueryInt64Column(
            execution_db,
            "SELECT program_ref_id FROM exec_job WHERE job_id IN (" + job_id_list + ") AND program_ref_kind='sp_probe_run';",
            &error);
        probe_run_ids.insert(probe_run_ids.end(), values.begin(), values.end());
    }
    if (execution_db != nullptr && !workflow_ids.empty()) {
        auto values = QueryInt64Column(
            execution_db,
            "SELECT input_ref_id FROM exec_workflow_step WHERE workflow_instance_id IN (" + workflow_id_list + ") AND input_ref_kind='sp_probe_run' "
            "UNION SELECT output_ref_id FROM exec_workflow_step WHERE workflow_instance_id IN (" + workflow_id_list + ") AND output_ref_kind='sp_probe_run';",
            &error);
        probe_run_ids.insert(probe_run_ids.end(), values.begin(), values.end());
        if (IsTablePresent(execution_db, "exec_workflow_instance_input_binding", nullptr)) {
            values = QueryInt64Column(
                execution_db,
                "SELECT ref_id FROM exec_workflow_instance_input_binding WHERE workflow_instance_id IN (" + workflow_id_list + ") AND ref_kind='sp_probe_run';",
                &error);
            probe_run_ids.insert(probe_run_ids.end(), values.begin(), values.end());
        }
        if (IsTablePresent(execution_db, "exec_workflow_step_output", nullptr)) {
            values = QueryInt64Column(
                execution_db,
                "SELECT ref_id FROM exec_workflow_step_output WHERE workflow_instance_id IN (" + workflow_id_list + ") AND ref_kind='sp_probe_run';",
                &error);
            probe_run_ids.insert(probe_run_ids.end(), values.begin(), values.end());
        }
    }
    std::sort(probe_run_ids.begin(), probe_run_ids.end());
    probe_run_ids.erase(std::unique(probe_run_ids.begin(), probe_run_ids.end()), probe_run_ids.end());
    if (!probe_run_ids.empty() && IsTablePresent(analysis_db, "sp_probe_run", nullptr)) {
        const auto run_ids = JoinIds(probe_run_ids);
        const auto probe_set_ids = QueryInt64Column(analysis_db, "SELECT probe_set_id FROM sp_probe_run WHERE probe_run_id IN (" + run_ids + ") ORDER BY probe_set_id ASC;", &error);
        const auto input_set_ids = QueryInt64Column(analysis_db, "SELECT accepted_input_set_id FROM sp_probe_run WHERE probe_run_id IN (" + run_ids + ") ORDER BY accepted_input_set_id ASC;", &error);
        const auto probe_result_ids = QueryInt64Column(analysis_db, "SELECT probe_result_id FROM sp_probe_result WHERE probe_run_id IN (" + run_ids + ") ORDER BY probe_result_id ASC;", &error);

        if (!probe_set_ids.empty() && IsTablePresent(analysis_db, "sp_probe_set", nullptr)) {
            specs.push_back({"analysis_seed_probe_sets", "SELECT * FROM sp_probe_set WHERE probe_set_id IN (" + JoinIds(probe_set_ids) + ") ORDER BY probe_set_id ASC;"});
        }
        if (!input_set_ids.empty() && IsTablePresent(analysis_db, "an_input_set", nullptr)) {
            specs.push_back({"analysis_input_sets", "SELECT * FROM an_input_set WHERE input_set_id IN (" + JoinIds(input_set_ids) + ") ORDER BY input_set_id ASC;"});
        }

        std::vector<std::int64_t> input_frame_ids;
        if (!input_set_ids.empty() && IsTablePresent(analysis_db, "an_input_set_frame", nullptr)) {
            input_frame_ids = QueryInt64Column(analysis_db, "SELECT input_frame_id FROM an_input_set_frame WHERE input_set_id IN (" + JoinIds(input_set_ids) + ") ORDER BY input_frame_id ASC;", &error);
        }
        if (!probe_result_ids.empty()) {
            auto result_frame_ids = QueryInt64Column(analysis_db, "SELECT input_frame_id FROM sp_probe_result WHERE probe_result_id IN (" + JoinIds(probe_result_ids) + ") ORDER BY input_frame_id ASC;", &error);
            input_frame_ids.insert(input_frame_ids.end(), result_frame_ids.begin(), result_frame_ids.end());
        }
        std::sort(input_frame_ids.begin(), input_frame_ids.end());
        input_frame_ids.erase(std::unique(input_frame_ids.begin(), input_frame_ids.end()), input_frame_ids.end());

        std::vector<std::int64_t> axis_ids;
        if (!input_frame_ids.empty() && IsTablePresent(analysis_db, "sp_input_frame", nullptr)) {
            axis_ids = QueryInt64Column(
                analysis_db,
                "SELECT main_axis_xy_id FROM sp_input_frame WHERE input_frame_id IN (" + JoinIds(input_frame_ids) + ") "
                "UNION SELECT cstick_axis_xy_id FROM sp_input_frame WHERE input_frame_id IN (" + JoinIds(input_frame_ids) + ") "
                "UNION SELECT trigger_axis_xy_id FROM sp_input_frame WHERE input_frame_id IN (" + JoinIds(input_frame_ids) + ");",
                &error);
        }
        std::sort(axis_ids.begin(), axis_ids.end());
        axis_ids.erase(std::unique(axis_ids.begin(), axis_ids.end()), axis_ids.end());

        if (!axis_ids.empty() && IsTablePresent(analysis_db, "sp_axis_xy", nullptr)) {
            specs.push_back({"analysis_seed_probe_axis_xy", "SELECT * FROM sp_axis_xy WHERE axis_xy_id IN (" + JoinIds(axis_ids) + ") ORDER BY axis_xy_id ASC;"});
        }
        if (!input_frame_ids.empty() && IsTablePresent(analysis_db, "sp_input_frame", nullptr)) {
            specs.push_back({"analysis_seed_probe_input_frames", "SELECT * FROM sp_input_frame WHERE input_frame_id IN (" + JoinIds(input_frame_ids) + ") ORDER BY input_frame_id ASC;"});
        }
        if (!input_set_ids.empty() && IsTablePresent(analysis_db, "an_input_set_frame", nullptr)) {
            specs.push_back({"analysis_input_set_frames", "SELECT * FROM an_input_set_frame WHERE input_set_id IN (" + JoinIds(input_set_ids) + ") ORDER BY input_set_id ASC, ordinal ASC;"});
        }
        specs.push_back({"analysis_seed_probe_runs", "SELECT * FROM sp_probe_run WHERE probe_run_id IN (" + run_ids + ") ORDER BY probe_run_id ASC;"});
        if (!probe_result_ids.empty() && IsTablePresent(analysis_db, "sp_probe_result", nullptr)) {
            specs.push_back({"analysis_seed_probe_results", "SELECT * FROM sp_probe_result WHERE probe_result_id IN (" + JoinIds(probe_result_ids) + ") ORDER BY probe_result_id ASC;"});
        }
        if (IsTablePresent(analysis_db, "sp_encounter_projection", nullptr)) {
            specs.push_back({"analysis_seed_probe_encounter_projections", "SELECT * FROM sp_encounter_projection WHERE probe_run_id IN (" + run_ids + ") ORDER BY encounter_projection_id ASC;"});
        }
    }

    if (!workflow_ids.empty()
        && IsTablePresent(analysis_db, "tmv_validation_request", nullptr)
        && IsTablePresent(analysis_db, "tmv_validation_attempt", nullptr)) {
        const auto request_ids = QueryInt64Column(
            analysis_db,
            "WITH RECURSIVE selected(validation_request_id) AS ("
            "SELECT validation_request_id FROM tmv_validation_request WHERE workflow_instance_id IN (" + workflow_id_list + ") "
            "UNION SELECT parent.validation_request_id FROM tmv_validation_request child "
            "JOIN tmv_validation_attempt source_attempt ON child.source_kind='ROOT_ESTABLISHMENT' "
            "AND source_attempt.validation_attempt_id=child.source_ref_id "
            "JOIN tmv_validation_request parent ON parent.validation_request_id=source_attempt.validation_request_id "
            "JOIN selected current ON current.validation_request_id=child.validation_request_id) "
            "SELECT validation_request_id FROM selected ORDER BY validation_request_id;",
            &error);
        if (!request_ids.empty()) {
            const auto ids = JoinIds(request_ids);
            specs.push_back({
                "analysis_tas_movie_validation_requests",
                "SELECT * FROM tmv_validation_request WHERE validation_request_id IN (" + ids
                    + ") ORDER BY validation_request_id ASC;"});
            specs.push_back({
                "analysis_tas_movie_validation_attempts",
                "SELECT * FROM tmv_validation_attempt WHERE validation_request_id IN (" + ids
                    + ") ORDER BY validation_attempt_id ASC;"});
            if (IsTablePresent(analysis_db, "tmv_dtm_validation_status", nullptr)) {
                specs.push_back({
                    "analysis_tas_movie_validation_statuses",
                    "SELECT DISTINCT s.* FROM tmv_dtm_validation_status s "
                    "JOIN tmv_validation_request r ON r.effective_dtm_sha256=s.effective_dtm_sha256 "
                    "WHERE r.validation_request_id IN (" + ids
                        + ") ORDER BY s.effective_dtm_sha256 ASC;"});
            }
        }
    }
    if (!workflow_ids.empty()
        && IsTablePresent(analysis_db, "tmv_checkpoint_sterilization_request", nullptr)
        && IsTablePresent(analysis_db, "tmv_checkpoint_sterilization_attempt", nullptr)) {
        const auto request_ids = QueryInt64Column(
            analysis_db,
            "SELECT sterilization_request_id FROM tmv_checkpoint_sterilization_request "
            "WHERE workflow_instance_id IN (" + workflow_id_list
                + ") ORDER BY sterilization_request_id;",
            &error);
        if (!request_ids.empty()) {
            const auto ids = JoinIds(request_ids);
            specs.push_back({
                "analysis_tas_movie_checkpoint_sterilization_requests",
                "SELECT * FROM tmv_checkpoint_sterilization_request "
                "WHERE sterilization_request_id IN (" + ids
                    + ") ORDER BY sterilization_request_id ASC;"});
            specs.push_back({
                "analysis_tas_movie_checkpoint_sterilization_attempts",
                "SELECT * FROM tmv_checkpoint_sterilization_attempt "
                "WHERE sterilization_request_id IN (" + ids
                    + ") ORDER BY sterilization_attempt_id ASC;"});
        }
    }
    return specs;
}

std::uint32_t Crc32(const std::string& payload) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char ch : payload) {
        crc ^= ch;
        for (int i = 0; i < 8; ++i) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

void WriteLe16(std::ofstream& out, std::uint16_t value) {
    out.put(static_cast<char>(value & 0xFF));
    out.put(static_cast<char>((value >> 8) & 0xFF));
}

void WriteLe32(std::ofstream& out, std::uint32_t value) {
    WriteLe16(out, static_cast<std::uint16_t>(value & 0xFFFF));
    WriteLe16(out, static_cast<std::uint16_t>((value >> 16) & 0xFFFF));
}

struct ZipSourceEntry {
    std::string entry_name;
    std::filesystem::path source_path;
};

struct ZipCentralEntry {
    std::string entry_name;
    std::uint32_t crc = 0;
    std::uint32_t size = 0;
    std::uint32_t local_offset = 0;
};

std::string SafeArchiveExtension(std::string_view artifact_kind, std::string_view file_ext) {
    std::string upper_kind(artifact_kind);
    std::transform(upper_kind.begin(), upper_kind.end(), upper_kind.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    if (upper_kind == "SAV") return ".sav";
    std::string extension(file_ext);
    if (!extension.empty() && extension.front() != '.') extension.insert(extension.begin(), '.');
    if (extension.size() < 2 || extension.size() > 17) return ".bin";
    for (std::size_t i = 1; i < extension.size(); ++i) {
        const auto ch = static_cast<unsigned char>(extension[i]);
        if (!std::isalnum(ch) && ch != '_' && ch != '-') return ".bin";
        extension[i] = static_cast<char>(std::tolower(ch));
    }
    return extension;
}

bool WriteStoredZip(
    const std::filesystem::path& zip_path,
    const std::vector<ZipSourceEntry>& entries,
    std::string* error_out) {
    if (entries.size() > 0xFFFFu) {
        if (error_out) *error_out = "too many state artifacts for zip";
        return false;
    }

    std::ofstream out(zip_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (error_out) *error_out = "failed to open " + zip_path.string();
        return false;
    }

    std::vector<ZipCentralEntry> central_entries;
    for (const auto& entry : entries) {
        std::ifstream in(entry.source_path, std::ios::binary);
        if (!in.is_open()) {
            if (error_out) *error_out = "failed to read savestate file: " + entry.source_path.string();
            return false;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        const std::string payload = buffer.str();
        if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
            if (error_out) *error_out = "state artifact file too large for stored zip entry: " + entry.source_path.string();
            return false;
        }

        ZipCentralEntry central{};
        central.entry_name = entry.entry_name;
        central.crc = Crc32(payload);
        central.size = static_cast<std::uint32_t>(payload.size());
        central.local_offset = static_cast<std::uint32_t>(out.tellp());

        WriteLe32(out, 0x04034b50u);
        WriteLe16(out, 20);
        WriteLe16(out, 0);
        WriteLe16(out, 0);
        WriteLe16(out, 0);
        WriteLe16(out, 0);
        WriteLe32(out, central.crc);
        WriteLe32(out, central.size);
        WriteLe32(out, central.size);
        WriteLe16(out, static_cast<std::uint16_t>(central.entry_name.size()));
        WriteLe16(out, 0);
        out.write(central.entry_name.data(), static_cast<std::streamsize>(central.entry_name.size()));
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        central_entries.push_back(std::move(central));
    }

    const auto central_offset = static_cast<std::uint32_t>(out.tellp());
    for (const auto& entry : central_entries) {
        WriteLe32(out, 0x02014b50u);
        WriteLe16(out, 20);
        WriteLe16(out, 20);
        WriteLe16(out, 0);
        WriteLe16(out, 0);
        WriteLe16(out, 0);
        WriteLe16(out, 0);
        WriteLe32(out, entry.crc);
        WriteLe32(out, entry.size);
        WriteLe32(out, entry.size);
        WriteLe16(out, static_cast<std::uint16_t>(entry.entry_name.size()));
        WriteLe16(out, 0);
        WriteLe16(out, 0);
        WriteLe16(out, 0);
        WriteLe16(out, 0);
        WriteLe32(out, 0);
        WriteLe32(out, entry.local_offset);
        out.write(entry.entry_name.data(), static_cast<std::streamsize>(entry.entry_name.size()));
    }
    const auto central_size = static_cast<std::uint32_t>(static_cast<std::uint32_t>(out.tellp()) - central_offset);

    WriteLe32(out, 0x06054b50u);
    WriteLe16(out, 0);
    WriteLe16(out, 0);
    WriteLe16(out, static_cast<std::uint16_t>(central_entries.size()));
    WriteLe16(out, static_cast<std::uint16_t>(central_entries.size()));
    WriteLe32(out, central_size);
    WriteLe32(out, central_offset);
    WriteLe16(out, 0);
    out.flush();
    if (!out.good()) {
        if (error_out) *error_out = "failed to write " + zip_path.string();
        return false;
    }
    return true;
}

struct SavestateArchiveRow {
    std::int64_t savestate_id = 0;
    std::int64_t artifact_id = 0;
    std::optional<std::int64_t> dtm_artifact_id;
    std::string sha256;
    std::int64_t size_bytes = 0;
    std::string filename;
    std::string object_relpath;
    std::string file_ext;
    std::string artifact_kind;
};

std::optional<std::filesystem::path> ResolveArchiveArtifactPath(
    const DbConfigPaths& config_paths,
    const SavestateArchiveRow& row,
    std::string* error_out) {
    if (!row.object_relpath.empty()) {
        return state::ResolveArtifactObjectPath(
            config_paths.object_store_root, row.object_relpath, error_out);
    }
    return state::ResolveLegacyArtifactSource(
        config_paths.object_store_root, row.filename, error_out);
}

std::vector<std::int64_t> CollectWorkflowSavestateIds(
    sqlite3* execution_db,
    sqlite3* analysis_db,
    const std::vector<std::int64_t>& workflow_ids,
    const std::vector<std::int64_t>& job_ids,
    const std::vector<std::int64_t>& battle_set_ids) {
    std::vector<std::int64_t> ids;
    if (execution_db != nullptr && !workflow_ids.empty()) {
        const auto workflow_id_list = JoinIds(workflow_ids);
        std::string error;
        auto input_ids = QueryInt64Column(
            execution_db,
            "SELECT input_ref_id FROM exec_workflow_step WHERE workflow_instance_id IN (" + workflow_id_list + ") AND input_ref_id IS NOT NULL AND LOWER(COALESCE(input_ref_kind,'')) LIKE '%savestate%';",
            &error);
        ids.insert(ids.end(), input_ids.begin(), input_ids.end());
        auto output_ids = QueryInt64Column(
            execution_db,
            "SELECT output_ref_id FROM exec_workflow_step WHERE workflow_instance_id IN (" + workflow_id_list + ") AND output_ref_id IS NOT NULL AND LOWER(COALESCE(output_ref_kind,'')) LIKE '%savestate%';",
            &error);
        ids.insert(ids.end(), output_ids.begin(), output_ids.end());
        if (ColumnPresent(execution_db, "exec_job", "savestate_id")) {
            const auto scoped = WorkflowJobSetCte(workflow_id_list);
            auto job_savestate_ids = QueryInt64Column(
                execution_db,
                scoped + "SELECT savestate_id FROM exec_job WHERE job_id IN (SELECT job_id FROM scoped_jobs) AND savestate_id IS NOT NULL;",
                &error);
            ids.insert(ids.end(), job_savestate_ids.begin(), job_savestate_ids.end());
        }
    }

    if (analysis_db != nullptr) {
        std::string error;
        if (!workflow_ids.empty()) {
            const auto workflow_id_list = JoinIds(workflow_ids);
            if (IsTablePresent(analysis_db, "ab_battle_completion", nullptr)) {
                auto values = QueryInt64Column(
                    analysis_db,
                    "SELECT entry_savestate_id FROM ab_battle_completion WHERE workflow_instance_id IN (" + workflow_id_list + ") "
                    "UNION SELECT completion_savestate_id FROM ab_battle_completion WHERE workflow_instance_id IN (" + workflow_id_list + ") AND completion_savestate_id IS NOT NULL;",
                    &error);
                ids.insert(ids.end(), values.begin(), values.end());
            }
            if (IsTablePresent(analysis_db, "ab_battle_recording", nullptr)) {
                auto values = QueryInt64Column(
                    analysis_db,
                    "SELECT source_savestate_id FROM ab_battle_recording WHERE workflow_instance_id IN (" + workflow_id_list + ") "
                    "UNION SELECT paired_checkpoint_savestate_id FROM ab_battle_recording WHERE workflow_instance_id IN (" + workflow_id_list + ") AND paired_checkpoint_savestate_id IS NOT NULL;",
                    &error);
                ids.insert(ids.end(), values.begin(), values.end());
            }
            if (IsTablePresent(analysis_db, "ab_battle_replay", nullptr)) {
                auto values = QueryInt64Column(
                    analysis_db,
                    "SELECT source_savestate_id FROM ab_battle_replay WHERE workflow_instance_id IN (" + workflow_id_list + ");",
                    &error);
                ids.insert(ids.end(), values.begin(), values.end());
            }
        }
        if (!job_ids.empty()) {
            const auto job_id_list = JoinIds(job_ids);
            if (IsTablePresent(analysis_db, "ab_turn_job", nullptr)) {
                if (ColumnPresent(analysis_db, "ab_turn_job", "source_savestate_id")) {
                    auto values = QueryInt64Column(
                        analysis_db,
                        "SELECT source_savestate_id FROM ab_turn_job WHERE exec_job_id IN (" + job_id_list + ") AND source_savestate_id IS NOT NULL;",
                        &error);
                    ids.insert(ids.end(), values.begin(), values.end());
                }
                if (ColumnPresent(analysis_db, "ab_turn_job", "output_savestate_id")) {
                    auto values = QueryInt64Column(
                        analysis_db,
                        "SELECT output_savestate_id FROM ab_turn_job WHERE exec_job_id IN (" + job_id_list + ") AND output_savestate_id IS NOT NULL;",
                        &error);
                    ids.insert(ids.end(), values.begin(), values.end());
                }
            }
            if (IsTablePresent(analysis_db, "ab_battle_context_probe", nullptr)) {
                auto values = QueryInt64Column(
                    analysis_db,
                    "SELECT source_savestate_id FROM ab_battle_context_probe WHERE exec_job_id IN (" + job_id_list + ") AND source_savestate_id IS NOT NULL;",
                    &error);
                ids.insert(ids.end(), values.begin(), values.end());
            }
        }
        if (!battle_set_ids.empty() && IsTablePresent(analysis_db, "ab_battle_set", nullptr)) {
            auto values = QueryInt64Column(
                analysis_db,
                "SELECT entry_savestate_id FROM ab_battle_set WHERE battle_set_id IN (" + JoinIds(battle_set_ids) + ") AND entry_savestate_id IS NOT NULL;",
                &error);
            ids.insert(ids.end(), values.begin(), values.end());
        }
        if (execution_db != nullptr && IsTablePresent(analysis_db, "sp_probe_run", nullptr)) {
            std::vector<std::int64_t> probe_run_ids;
            if (!job_ids.empty()) {
                const auto job_id_list = JoinIds(job_ids);
                auto values = QueryInt64Column(
                    execution_db,
                    "SELECT program_ref_id FROM exec_job WHERE job_id IN (" + job_id_list + ") AND program_ref_kind='sp_probe_run';",
                    &error);
                probe_run_ids.insert(probe_run_ids.end(), values.begin(), values.end());
            }
            if (!workflow_ids.empty()) {
                const auto workflow_id_list = JoinIds(workflow_ids);
                auto values = QueryInt64Column(
                    execution_db,
                    "SELECT input_ref_id FROM exec_workflow_step WHERE workflow_instance_id IN (" + workflow_id_list + ") AND input_ref_kind='sp_probe_run' "
                    "UNION SELECT output_ref_id FROM exec_workflow_step WHERE workflow_instance_id IN (" + workflow_id_list + ") AND output_ref_kind='sp_probe_run';",
                    &error);
                probe_run_ids.insert(probe_run_ids.end(), values.begin(), values.end());
                if (IsTablePresent(execution_db, "exec_workflow_instance_input_binding", nullptr)) {
                    values = QueryInt64Column(
                        execution_db,
                        "SELECT ref_id FROM exec_workflow_instance_input_binding WHERE workflow_instance_id IN (" + workflow_id_list + ") AND ref_kind='sp_probe_run';",
                        &error);
                    probe_run_ids.insert(probe_run_ids.end(), values.begin(), values.end());
                }
                if (IsTablePresent(execution_db, "exec_workflow_step_output", nullptr)) {
                    values = QueryInt64Column(
                        execution_db,
                        "SELECT ref_id FROM exec_workflow_step_output WHERE workflow_instance_id IN (" + workflow_id_list + ") AND ref_kind='sp_probe_run';",
                        &error);
                    probe_run_ids.insert(probe_run_ids.end(), values.begin(), values.end());
                }
            }
            std::sort(probe_run_ids.begin(), probe_run_ids.end());
            probe_run_ids.erase(std::unique(probe_run_ids.begin(), probe_run_ids.end()), probe_run_ids.end());
            if (!probe_run_ids.empty()) {
                auto values = QueryInt64Column(
                    analysis_db,
                    "SELECT entry_savestate_id FROM sp_probe_run WHERE probe_run_id IN (" + JoinIds(probe_run_ids) + ") AND entry_savestate_id IS NOT NULL;",
                    &error);
                ids.insert(ids.end(), values.begin(), values.end());
            }
        }
    }

    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

std::vector<std::int64_t> CollectWorkflowAggregateArtifactIds(
    sqlite3* analysis_db,
    const std::vector<std::int64_t>& workflow_ids) {
    std::vector<std::int64_t> ids;
    if (analysis_db == nullptr || workflow_ids.empty()) return ids;
    const auto workflows = JoinIds(workflow_ids);
    std::string error;
    if (IsTablePresent(analysis_db, "ab_battle_completion", nullptr)) {
        auto values = QueryInt64Column(
            analysis_db,
            "SELECT manifest_artifact_id FROM ab_battle_completion WHERE workflow_instance_id IN (" + workflows
                + ") AND manifest_artifact_id IS NOT NULL;",
            &error);
        ids.insert(ids.end(), values.begin(), values.end());
    }
    if (IsTablePresent(analysis_db, "ab_battle_recording", nullptr)) {
        auto values = QueryInt64Column(
            analysis_db,
            "SELECT source_dtm_artifact_id FROM ab_battle_recording WHERE workflow_instance_id IN (" + workflows + ") "
                "UNION SELECT source_itinerary_artifact_id FROM ab_battle_recording WHERE workflow_instance_id IN (" + workflows + ") "
                "UNION SELECT recorded_dtm_artifact_id FROM ab_battle_recording WHERE workflow_instance_id IN (" + workflows + ") AND recorded_dtm_artifact_id IS NOT NULL "
                "UNION SELECT recorded_itinerary_artifact_id FROM ab_battle_recording WHERE workflow_instance_id IN (" + workflows + ") AND recorded_itinerary_artifact_id IS NOT NULL;",
            &error);
        ids.insert(ids.end(), values.begin(), values.end());
    }
    if (IsTablePresent(analysis_db, "ab_battle_replay", nullptr)) {
        auto values = QueryInt64Column(
            analysis_db,
            "SELECT source_dtm_artifact_id FROM ab_battle_replay WHERE workflow_instance_id IN (" + workflows + ") "
                "UNION SELECT source_itinerary_artifact_id FROM ab_battle_replay WHERE workflow_instance_id IN (" + workflows + ");",
            &error);
        ids.insert(ids.end(), values.begin(), values.end());
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

std::vector<std::int64_t> CollectWorkflowSeedProbeRunIds(
    sqlite3* execution_db,
    sqlite3* analysis_db,
    const std::vector<std::int64_t>& workflow_ids,
    const std::vector<std::int64_t>& job_ids,
    const std::vector<std::int64_t>& job_set_ids) {
    std::vector<std::int64_t> ids;
    if (analysis_db == nullptr) return ids;
    std::string error;
    const auto jobs = job_ids.empty() ? std::string("0") : JoinIds(job_ids);
    const auto job_sets = job_set_ids.empty() ? std::string("0") : JoinIds(job_set_ids);
    const auto workflows = workflow_ids.empty() ? std::string("0") : JoinIds(workflow_ids);
    std::vector<std::int64_t> result_ids;
    if (execution_db != nullptr) {
        auto values = QueryInt64Column(
            execution_db,
            "SELECT program_ref_id FROM exec_job WHERE job_id IN (" + jobs
                + ") AND program_ref_kind='sp_probe_run' "
                  "UNION SELECT domain_ref_id FROM exec_job_set WHERE job_set_id IN (" + job_sets
                + ") AND domain_ref_kind='sp_probe_run';",
            &error);
        ids.insert(ids.end(), values.begin(), values.end());
        values = QueryInt64Column(
            execution_db,
            "SELECT input_ref_id FROM exec_workflow_step WHERE workflow_instance_id IN (" + workflows
                + ") AND input_ref_kind='sp_probe_run' "
                "UNION SELECT output_ref_id FROM exec_workflow_step WHERE workflow_instance_id IN (" + workflows
                + ") AND output_ref_kind='sp_probe_run';",
            &error);
        ids.insert(ids.end(), values.begin(), values.end());
        if (IsTablePresent(execution_db, "exec_workflow_instance_input_binding", nullptr)) {
            values = QueryInt64Column(
                execution_db,
                "SELECT ref_id FROM exec_workflow_instance_input_binding WHERE workflow_instance_id IN ("
                    + workflows + ") AND ref_kind='sp_probe_run';",
                &error);
            ids.insert(ids.end(), values.begin(), values.end());
        }
        if (IsTablePresent(execution_db, "exec_workflow_step_output", nullptr)) {
            values = QueryInt64Column(
                execution_db,
                "SELECT ref_id FROM exec_workflow_step_output WHERE workflow_instance_id IN (" + workflows
                    + ") AND ref_kind='sp_probe_run';",
                &error);
            ids.insert(ids.end(), values.begin(), values.end());
        }

        values = QueryInt64Column(
            execution_db,
            "SELECT program_ref_id FROM exec_job WHERE job_id IN (" + jobs
                + ") AND program_ref_kind='analysisseedprobe.confirmed_result' "
                "UNION SELECT domain_ref_id FROM exec_job_set WHERE job_set_id IN (" + job_sets
                + ") AND domain_ref_kind='analysisseedprobe.confirmed_result';",
            &error);
        result_ids.insert(result_ids.end(), values.begin(), values.end());

        if (IsTablePresent(execution_db, "exec_job_output", nullptr)) {
            values = QueryInt64Column(
                execution_db,
                "SELECT ref_id FROM exec_job_output WHERE job_id IN (" + jobs
                    + ") AND ref_kind='sp_probe_run';",
                &error);
            ids.insert(ids.end(), values.begin(), values.end());
            values = QueryInt64Column(
                execution_db,
                "SELECT ref_id FROM exec_job_output WHERE job_id IN (" + jobs
                    + ") AND ref_kind='analysisseedprobe.confirmed_result';",
                &error);
            result_ids.insert(result_ids.end(), values.begin(), values.end());
        }
    }
    if (!result_ids.empty() && IsTablePresent(analysis_db, "sp_probe_result", nullptr)) {
        auto values = QueryInt64Column(
            analysis_db,
            "SELECT DISTINCT probe_run_id FROM sp_probe_result WHERE probe_result_id IN ("
                + JoinIds(result_ids) + ");",
            &error);
        ids.insert(ids.end(), values.begin(), values.end());
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

std::vector<std::int64_t> CollectSeedProbeExecutionJobSetIds(
    sqlite3* execution_db,
    sqlite3* analysis_db,
    const std::vector<std::int64_t>& workflow_ids,
    const std::vector<std::int64_t>& job_ids,
    const std::vector<std::int64_t>& job_set_ids,
    std::string* error_out) {
    // SeedProbe result facts live in Analysis, while their durable stage,
    // ordinal, and desired-delta intent lives on the source Execution jobs.
    // Export each producer job set so workset membership and dispatch history
    // remain internally complete without importing the producer workflow.
    std::vector<std::int64_t> execution_job_set_ids;
    if (execution_db == nullptr || analysis_db == nullptr) {
        return execution_job_set_ids;
    }

    const auto probe_run_ids = CollectWorkflowSeedProbeRunIds(
        execution_db,
        analysis_db,
        workflow_ids,
        job_ids,
        job_set_ids);
    if (probe_run_ids.empty()
        || !IsTablePresent(analysis_db, "sp_probe_result", nullptr)) {
        return execution_job_set_ids;
    }

    std::string query_error;
    const auto source_job_ids = QueryInt64Column(
        analysis_db,
        "SELECT DISTINCT source_job_id FROM sp_probe_result "
        "WHERE probe_run_id IN (" + JoinIds(probe_run_ids)
            + ") ORDER BY source_job_id ASC;",
        &query_error);
    if (!query_error.empty()) {
        if (error_out != nullptr) {
            *error_out = query_error;
        }
        return {};
    }
    if (source_job_ids.empty()) {
        return execution_job_set_ids;
    }

    const auto source_jobs = JoinIds(source_job_ids);
    const auto present_source_job_ids = QueryInt64Column(
        execution_db,
        "SELECT job_id FROM exec_job WHERE job_id IN (" + source_jobs
            + ") ORDER BY job_id ASC;",
        &query_error);
    if (!query_error.empty()) {
        if (error_out != nullptr) {
            *error_out = query_error;
        }
        return {};
    }
    if (present_source_job_ids != source_job_ids) {
        if (error_out != nullptr) {
            *error_out =
                "SeedProbe archive evidence references a missing source "
                "execution job";
        }
        return {};
    }

    execution_job_set_ids = QueryInt64Column(
        execution_db,
        "SELECT DISTINCT job_set_id FROM exec_job WHERE job_id IN (" + source_jobs
            + ") ORDER BY job_set_id ASC;",
        &query_error);
    if (!query_error.empty()) {
        if (error_out != nullptr) {
            *error_out = query_error;
        }
        return {};
    }
    if (execution_job_set_ids.empty()) {
        if (error_out != nullptr) {
            *error_out =
                "SeedProbe archive evidence source jobs have no job set";
        }
        return {};
    }
    return execution_job_set_ids;
}

std::vector<std::int64_t> FilterExclusiveWorkflowSeedProbeRunIds(
    sqlite3* execution_db,
    sqlite3* analysis_db,
    sqlite3* state_db,
    const std::vector<std::int64_t>& selected_workflow_ids,
    const std::vector<std::int64_t>& selected_job_ids,
    const std::vector<std::int64_t>& selected_job_set_ids,
    const std::vector<std::int64_t>& selected_battle_set_ids,
    const std::vector<std::int64_t>& exclusive_savestate_ids,
    const std::vector<std::int64_t>& probe_run_ids) {
    std::vector<std::int64_t> exclusive;
    if (analysis_db == nullptr || probe_run_ids.empty()) return exclusive;

    const auto workflows = selected_workflow_ids.empty() ? std::string("0") : JoinIds(selected_workflow_ids);
    const auto jobs = selected_job_ids.empty() ? std::string("0") : JoinIds(selected_job_ids);
    const auto job_sets = selected_job_set_ids.empty() ? std::string("0") : JoinIds(selected_job_set_ids);
    const auto battle_sets = selected_battle_set_ids.empty() ? std::string("0") : JoinIds(selected_battle_set_ids);
    const auto exclusive_states = exclusive_savestate_ids.empty()
        ? std::string("0")
        : JoinIds(exclusive_savestate_ids);
    std::string error;

    for (const auto probe_run_id : probe_run_ids) {
        const auto run = std::to_string(probe_run_id);
        bool referenced = false;
        const auto result_ids = QueryInt64Column(
            analysis_db,
            "SELECT probe_result_id FROM sp_probe_result WHERE probe_run_id=" + run + ";",
            &error);
        const auto results = result_ids.empty() ? std::string("0") : JoinIds(result_ids);

        if (execution_db != nullptr) {
            referenced = QuerySingleInt64(
                execution_db,
                "SELECT COUNT(1) FROM exec_job WHERE job_id NOT IN (" + jobs
                    + ") AND program_ref_kind='sp_probe_run' AND program_ref_id=" + run + ";",
                &error) > 0;
            referenced = referenced || QuerySingleInt64(
                execution_db,
                "SELECT COUNT(1) FROM exec_job_set WHERE job_set_id NOT IN (" + job_sets
                    + ") AND domain_ref_kind='sp_probe_run' AND domain_ref_id=" + run + ";",
                &error) > 0;
            referenced = referenced || QuerySingleInt64(
                execution_db,
                "SELECT COUNT(1) FROM exec_workflow_step WHERE workflow_instance_id NOT IN (" + workflows
                    + ") AND ((input_ref_kind='sp_probe_run' AND input_ref_id=" + run
                    + ") OR (output_ref_kind='sp_probe_run' AND output_ref_id=" + run + "));",
                &error) > 0;
            if (IsTablePresent(execution_db, "exec_workflow_instance_input_binding", nullptr)) {
                referenced = referenced || QuerySingleInt64(
                    execution_db,
                    "SELECT COUNT(1) FROM exec_workflow_instance_input_binding WHERE workflow_instance_id NOT IN ("
                        + workflows + ") AND ref_kind='sp_probe_run' AND ref_id=" + run + ";",
                    &error) > 0;
            }
            if (IsTablePresent(execution_db, "exec_workflow_step_output", nullptr)) {
                referenced = referenced || QuerySingleInt64(
                    execution_db,
                    "SELECT COUNT(1) FROM exec_workflow_step_output WHERE workflow_instance_id NOT IN ("
                        + workflows + ") AND ref_kind='sp_probe_run' AND ref_id=" + run + ";",
                    &error) > 0;
            }
            if (IsTablePresent(execution_db, "exec_job_output", nullptr)) {
                referenced = referenced || QuerySingleInt64(
                    execution_db,
                    "SELECT COUNT(1) FROM exec_job_output WHERE job_id NOT IN (" + jobs
                        + ") AND ref_kind='sp_probe_run' AND ref_id=" + run + ";",
                    &error) > 0;
            }

            referenced = referenced || QuerySingleInt64(
                execution_db,
                "SELECT COUNT(1) FROM exec_job WHERE job_id NOT IN (" + jobs
                    + ") AND program_ref_kind='analysisseedprobe.confirmed_result' "
                      "AND program_ref_id IN (" + results + ");",
                &error) > 0;
            referenced = referenced || QuerySingleInt64(
                execution_db,
                "SELECT COUNT(1) FROM exec_job_set WHERE job_set_id NOT IN (" + job_sets
                    + ") AND domain_ref_kind='analysisseedprobe.confirmed_result' "
                      "AND domain_ref_id IN (" + results + ");",
                &error) > 0;
            if (IsTablePresent(execution_db, "exec_job_output", nullptr)) {
                referenced = referenced || QuerySingleInt64(
                    execution_db,
                    "SELECT COUNT(1) FROM exec_job_output WHERE job_id NOT IN (" + jobs
                        + ") AND ref_kind='analysisseedprobe.confirmed_result' "
                          "AND ref_id IN (" + results + ");",
                    &error) > 0;
            }
        }

        if (!referenced && IsTablePresent(analysis_db, "ab_seed_candidate", nullptr)) {
            referenced = QuerySingleInt64(
                analysis_db,
                "SELECT COUNT(1) FROM ab_seed_candidate c WHERE c.source_probe_result_id IN (" + results + ") AND ("
                    "c.battle_set_id NOT IN (" + battle_sets + ") OR EXISTS ("
                    "SELECT 1 FROM ab_turn_wave w WHERE w.battle_set_id=c.battle_set_id AND ("
                    "EXISTS (SELECT 1 FROM ab_turn_job t WHERE t.wave_id=w.wave_id AND "
                    "(t.exec_job_id IS NULL OR t.exec_job_id NOT IN (" + jobs + "))) OR "
                    "EXISTS (SELECT 1 FROM ab_battle_context_probe p WHERE p.wave_id=w.wave_id AND "
                    "(p.exec_job_id IS NULL OR p.exec_job_id NOT IN (" + jobs + "))))));",
                &error) > 0;
        }
        if (!referenced && IsTablePresent(analysis_db, "sp_probe_run", nullptr)) {
            referenced = QuerySingleInt64(
                analysis_db,
                "SELECT COUNT(1) FROM sp_probe_run other JOIN sp_probe_run candidate "
                    "ON other.accepted_input_set_id=candidate.accepted_input_set_id "
                    "WHERE candidate.probe_run_id=" + run + " AND other.probe_run_id<>candidate.probe_run_id;",
                &error) > 0;
        }
        if (!referenced && state_db != nullptr && IsTablePresent(state_db, "state_savestate_derivation", nullptr)) {
            referenced = QuerySingleInt64(
                state_db,
                "SELECT COUNT(1) FROM state_savestate_derivation WHERE "
                    "source_context_kind='analysisseedprobe.confirmed_result' "
                    "AND source_context_id IN (" + results + ") "
                    "AND (from_savestate_id NOT IN (" + exclusive_states + ") OR to_savestate_id NOT IN ("
                    + exclusive_states + "));",
                &error) > 0;
        }
        if (!referenced) exclusive.push_back(probe_run_id);
    }
    return exclusive;
}

std::vector<SavestateArchiveRow> LoadSavestateRows(
    sqlite3* state_db,
    const std::vector<std::int64_t>& savestate_ids,
    std::string* error_out) {
    std::vector<SavestateArchiveRow> rows;
    if (state_db == nullptr || savestate_ids.empty()) {
        return rows;
    }

    const std::string query =
        "SELECT s.savestate_id,s.artifact_id,a.sha256,a.size_bytes,a.filename,a.object_relpath,a.file_ext,a.artifact_kind,s.dtm_artifact_id "
        "FROM state_savestate s JOIN state_artifact a ON a.artifact_id=s.artifact_id "
        "WHERE s.savestate_id IN (" + JoinIds(savestate_ids) + ") "
        "AND (UPPER(a.artifact_kind)='SAV' OR LOWER(a.file_ext)='.sav') "
        "ORDER BY s.savestate_id ASC;";

    Statement st;
    if (sqlite3_prepare_v2(state_db, query.c_str(), -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(state_db);
        return rows;
    }

    while (true) {
        const int rc = sqlite3_step(st.st);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            if (error_out) *error_out = sqlite3_errmsg(state_db);
            rows.clear();
            return rows;
        }

        SavestateArchiveRow row{};
        row.savestate_id = sqlite3_column_int64(st.st, 0);
        row.artifact_id = sqlite3_column_int64(st.st, 1);
        const auto* sha = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 2));
        row.sha256 = sha == nullptr ? "" : sha;
        row.size_bytes = sqlite3_column_int64(st.st, 3);
        const auto* filename = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 4));
        row.filename = filename == nullptr ? "" : filename;
        const auto* object_relpath = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 5));
        row.object_relpath = object_relpath == nullptr ? "" : object_relpath;
        const auto* file_ext = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 6));
        row.file_ext = file_ext == nullptr ? "" : file_ext;
        const auto* artifact_kind = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 7));
        row.artifact_kind = artifact_kind == nullptr ? "" : artifact_kind;
        if (sqlite3_column_type(st.st, 8) != SQLITE_NULL)
            row.dtm_artifact_id = sqlite3_column_int64(st.st, 8);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<SavestateArchiveRow> LoadStandaloneArtifactRows(
    sqlite3* state_db,
    const std::vector<std::int64_t>& artifact_ids,
    std::string* error_out) {
    std::vector<SavestateArchiveRow> rows;
    if (state_db == nullptr || artifact_ids.empty()) return rows;
    const std::string query =
        "SELECT 0,a.artifact_id,a.sha256,a.size_bytes,a.filename,a.object_relpath,a.file_ext,a.artifact_kind "
        "FROM state_artifact a WHERE a.artifact_id IN (" + JoinIds(artifact_ids) + ") ORDER BY a.artifact_id ASC;";
    Statement st;
    if (sqlite3_prepare_v2(state_db, query.c_str(), -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(state_db);
        return rows;
    }
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        SavestateArchiveRow row{};
        row.artifact_id = sqlite3_column_int64(st.st, 1);
        const auto* sha = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 2));
        row.sha256 = sha == nullptr ? "" : sha;
        row.size_bytes = sqlite3_column_int64(st.st, 3);
        const auto* filename = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 4));
        row.filename = filename == nullptr ? "" : filename;
        const auto* object_relpath = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 5));
        row.object_relpath = object_relpath == nullptr ? "" : object_relpath;
        const auto* file_ext = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 6));
        row.file_ext = file_ext == nullptr ? "" : file_ext;
        const auto* artifact_kind = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 7));
        row.artifact_kind = artifact_kind == nullptr ? "" : artifact_kind;
        rows.push_back(std::move(row));
    }
    if (rows.size() != artifact_ids.size()) {
        if (error_out) *error_out = "one or more battle-end aggregate artifacts are missing from State DB";
        rows.clear();
    }
    return rows;
}

void MergeArtifactRows(
    std::vector<SavestateArchiveRow>* rows,
    std::vector<SavestateArchiveRow> additional) {
    if (rows == nullptr) return;
    std::set<std::int64_t> existing;
    for (const auto& row : *rows) existing.insert(row.artifact_id);
    for (auto& row : additional) {
        if (existing.insert(row.artifact_id).second) rows->push_back(std::move(row));
    }
}

struct TasMovieArchiveClosure {
    std::vector<std::int64_t> root_ids;
    std::vector<std::int64_t> tree_ids;
    std::vector<std::int64_t> savestate_ids;
    std::vector<std::int64_t> artifact_ids;
};

TasMovieArchiveClosure CollectTasMovieArchiveClosure(
    sqlite3* analysis_db,
    sqlite3* state_db,
    const std::vector<std::int64_t>& workflow_ids,
    std::string* error_out) {
    TasMovieArchiveClosure closure;
    if (analysis_db == nullptr || state_db == nullptr || workflow_ids.empty()
        || !IsTablePresent(analysis_db, "tmv_validation_request", nullptr)) {
        return closure;
    }
    std::string error;
    const auto workflows = JoinIds(workflow_ids);
    const auto request_ids = QueryInt64Column(
        analysis_db,
        "WITH RECURSIVE selected(validation_request_id) AS ("
        "SELECT validation_request_id FROM tmv_validation_request WHERE workflow_instance_id IN (" + workflows + ") "
        "UNION SELECT parent.validation_request_id FROM tmv_validation_request child "
        "JOIN tmv_validation_attempt source_attempt ON child.source_kind='ROOT_ESTABLISHMENT' "
        "AND source_attempt.validation_attempt_id=child.source_ref_id "
        "JOIN tmv_validation_request parent ON parent.validation_request_id=source_attempt.validation_request_id "
        "JOIN selected current ON current.validation_request_id=child.validation_request_id) "
        "SELECT validation_request_id FROM selected;",
        &error);
    if (!error.empty() || request_ids.empty()) {
        if (error_out != nullptr && !error.empty()) *error_out = error;
        return closure;
    }
    const auto requests = JoinIds(request_ids);
    auto append = [](std::vector<std::int64_t>* destination, std::vector<std::int64_t> values) {
        destination->insert(destination->end(), values.begin(), values.end());
    };
    append(&closure.artifact_ids, QueryInt64Column(
        analysis_db,
        "SELECT source_dtm_artifact_id FROM tmv_validation_request WHERE validation_request_id IN (" + requests + ") "
        "UNION SELECT itinerary_artifact_id FROM tmv_validation_request WHERE validation_request_id IN (" + requests + ") AND itinerary_artifact_id IS NOT NULL "
        "UNION SELECT candidate_itinerary_artifact_id FROM tmv_validation_attempt WHERE validation_request_id IN (" + requests + ") AND candidate_itinerary_artifact_id IS NOT NULL;",
        &error));
    append(&closure.savestate_ids, QueryInt64Column(
        analysis_db,
        "SELECT last_known_good_savestate_id FROM tmv_validation_attempt WHERE validation_request_id IN (" + requests + ") AND last_known_good_savestate_id IS NOT NULL;",
        &error));
    append(&closure.root_ids, QueryInt64Column(
        analysis_db,
        "SELECT produced_tas_movie_root_id FROM tmv_validation_attempt WHERE validation_request_id IN (" + requests + ") AND produced_tas_movie_root_id IS NOT NULL;",
        &error));
    append(&closure.tree_ids, QueryInt64Column(
        analysis_db,
        "SELECT source_ref_id FROM tmv_validation_request WHERE validation_request_id IN (" + requests + ") AND source_kind='TREE';",
        &error));
    if (!error.empty()) {
        if (error_out != nullptr) *error_out = error;
        return {};
    }

    if (!closure.tree_ids.empty()) {
        const auto trees = JoinIds(closure.tree_ids);
        closure.tree_ids = QueryInt64Column(
            state_db,
            "WITH RECURSIVE lineage(tas_movie_tree_id) AS ("
            "SELECT tas_movie_tree_id FROM state_tas_movie_trees WHERE tas_movie_tree_id IN (" + trees + ") "
            "UNION SELECT t.parent_tas_movie_tree_id FROM state_tas_movie_trees t JOIN lineage l "
            "ON t.tas_movie_tree_id=l.tas_movie_tree_id WHERE t.parent_tas_movie_tree_id IS NOT NULL) "
            "SELECT tas_movie_tree_id FROM lineage ORDER BY tas_movie_tree_id;",
            &error);
        if (!closure.tree_ids.empty()) {
            const auto lineage = JoinIds(closure.tree_ids);
            append(&closure.root_ids, QueryInt64Column(
                state_db,
                "SELECT tas_movie_root_id FROM state_tas_movie_trees WHERE tas_movie_tree_id IN (" + lineage + ");",
                &error));
            append(&closure.artifact_ids, QueryInt64Column(
                state_db,
                "SELECT dtm_artifact_id FROM state_tas_movie_trees WHERE tas_movie_tree_id IN (" + lineage + ") "
                "UNION SELECT itinerary_artifact_id FROM state_tas_movie_trees WHERE tas_movie_tree_id IN (" + lineage + ");",
                &error));
            append(&closure.savestate_ids, QueryInt64Column(
                state_db,
                "SELECT checkpoint_savestate_id FROM state_tas_movie_trees WHERE tas_movie_tree_id IN (" + lineage + ");",
                &error));
        }
    }
    std::sort(closure.root_ids.begin(), closure.root_ids.end());
    closure.root_ids.erase(std::unique(closure.root_ids.begin(), closure.root_ids.end()), closure.root_ids.end());
    if (!closure.root_ids.empty()) {
        const auto roots = JoinIds(closure.root_ids);
        append(&closure.artifact_ids, QueryInt64Column(
            state_db,
            "SELECT source_dtm_artifact_id FROM state_tas_movie_root WHERE tas_movie_root_id IN (" + roots + ") "
            "UNION SELECT dtm_artifact_id FROM state_tas_movie_root WHERE tas_movie_root_id IN (" + roots + ") "
            "UNION SELECT itinerary_artifact_id FROM state_tas_movie_root WHERE tas_movie_root_id IN (" + roots + ");",
            &error));
        append(&closure.savestate_ids, QueryInt64Column(
            state_db,
            "SELECT checkpoint_savestate_id FROM state_tas_movie_root WHERE tas_movie_root_id IN (" + roots + ");",
            &error));
    }
    if (!error.empty()) {
        if (error_out != nullptr) *error_out = error;
        return {};
    }
    for (auto* values : {&closure.tree_ids, &closure.savestate_ids, &closure.artifact_ids}) {
        std::sort(values->begin(), values->end());
        values->erase(std::unique(values->begin(), values->end()), values->end());
    }
    return closure;
}

std::vector<ExportSpec> BuildStateSavestateSpecs(
    const std::vector<SavestateArchiveRow>& rows,
    const std::vector<std::int64_t>& tas_movie_root_ids,
    const std::vector<std::int64_t>& tas_movie_tree_ids) {
    std::vector<ExportSpec> specs;
    if (rows.empty()) {
        return specs;
    }
    std::vector<std::int64_t> savestate_ids;
    std::vector<std::int64_t> artifact_ids;
    for (const auto& row : rows) {
        if (row.savestate_id > 0) savestate_ids.push_back(row.savestate_id);
        artifact_ids.push_back(row.artifact_id);
        if (row.dtm_artifact_id) artifact_ids.push_back(*row.dtm_artifact_id);
    }
    std::sort(artifact_ids.begin(), artifact_ids.end());
    artifact_ids.erase(std::unique(artifact_ids.begin(), artifact_ids.end()), artifact_ids.end());
    const auto art_ids = JoinIds(artifact_ids);
    specs.push_back({"state_artifacts", "SELECT * FROM state_artifact WHERE artifact_id IN (" + art_ids + ") ORDER BY artifact_id ASC;"});
    if (!savestate_ids.empty()) {
        const auto sav_ids = JoinIds(savestate_ids);
        specs.push_back({"state_savestates", "SELECT * FROM state_savestate WHERE savestate_id IN (" + sav_ids + ") ORDER BY savestate_id ASC;"});
        specs.push_back({"state_savestate_derivations", "SELECT * FROM state_savestate_derivation WHERE from_savestate_id IN (" + sav_ids + ") AND to_savestate_id IN (" + sav_ids + ") ORDER BY from_savestate_id ASC,to_savestate_id ASC;"});
    }
    if (!tas_movie_root_ids.empty()) {
        specs.push_back({"state_tas_movie_roots", "SELECT * FROM state_tas_movie_root WHERE tas_movie_root_id IN (" + JoinIds(tas_movie_root_ids) + ") ORDER BY tas_movie_root_id ASC;"});
    }
    if (!tas_movie_tree_ids.empty()) {
        specs.push_back({"state_tas_movie_trees", "SELECT * FROM state_tas_movie_trees WHERE tas_movie_tree_id IN (" + JoinIds(tas_movie_tree_ids) + ") ORDER BY tas_movie_tree_id ASC;"});
    }
    return specs;
}

bool ExecuteSql(sqlite3* db, const std::string& sql, std::string* error_out) {
    char* err_msg = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err_msg) != SQLITE_OK) {
        if (error_out) *error_out = err_msg == nullptr ? sqlite3_errmsg(db) : err_msg;
        sqlite3_free(err_msg);
        return false;
    }
    sqlite3_free(err_msg);
    return true;
}

int ExecuteDeleteSql(sqlite3* db, const std::string& sql, std::string* error_out) {
    if (!ExecuteSql(db, sql, error_out)) {
        return -1;
    }
    return sqlite3_changes(db);
}

std::vector<std::int64_t> FilterExclusiveSavestateIds(
    sqlite3* execution_db,
    sqlite3* analysis_db,
    sqlite3* state_db,
    const std::vector<std::int64_t>& selected_workflow_ids,
    const std::vector<std::int64_t>& selected_job_ids,
    const std::vector<std::int64_t>& selected_battle_set_ids,
    const std::vector<std::int64_t>& savestate_ids) {
    std::vector<std::int64_t> exclusive;
    if (savestate_ids.empty()) {
        return exclusive;
    }
    const auto sav_ids = JoinIds(savestate_ids);
    const auto workflow_ids = JoinIds(selected_workflow_ids);
    const auto job_ids = JoinIds(selected_job_ids);
    const auto battle_set_ids = JoinIds(selected_battle_set_ids);
    std::string error;

    for (const auto savestate_id : savestate_ids) {
        const auto id = std::to_string(savestate_id);
        bool referenced = false;
        if (execution_db != nullptr) {
            referenced = referenced || QuerySingleInt64(
                execution_db,
                "SELECT COUNT(1) FROM exec_workflow_step "
                "WHERE workflow_instance_id NOT IN (" + workflow_ids + ") "
                "AND ((input_ref_id=" + id + " AND LOWER(COALESCE(input_ref_kind,'')) LIKE '%savestate%') "
                "OR (output_ref_id=" + id + " AND LOWER(COALESCE(output_ref_kind,'')) LIKE '%savestate%'));",
                &error) > 0;
            if (ColumnPresent(execution_db, "exec_job", "savestate_id") && !job_ids.empty()) {
                referenced = referenced || QuerySingleInt64(
                    execution_db,
                    "SELECT COUNT(1) FROM exec_job WHERE savestate_id=" + id + " AND job_id NOT IN (" + job_ids + ");",
                    &error) > 0;
            }
        }
        if (analysis_db != nullptr) {
            if (IsTablePresent(analysis_db, "ab_turn_job", nullptr)) {
                if (ColumnPresent(analysis_db, "ab_turn_job", "source_savestate_id") && !job_ids.empty()) {
                    referenced = referenced || QuerySingleInt64(
                        analysis_db,
                        "SELECT COUNT(1) FROM ab_turn_job WHERE source_savestate_id=" + id + " AND (exec_job_id IS NULL OR exec_job_id NOT IN (" + job_ids + "));",
                        &error) > 0;
                }
                if (ColumnPresent(analysis_db, "ab_turn_job", "output_savestate_id") && !job_ids.empty()) {
                    referenced = referenced || QuerySingleInt64(
                        analysis_db,
                        "SELECT COUNT(1) FROM ab_turn_job WHERE output_savestate_id=" + id + " AND (exec_job_id IS NULL OR exec_job_id NOT IN (" + job_ids + "));",
                        &error) > 0;
                }
            }
            if (IsTablePresent(analysis_db, "ab_battle_context_probe", nullptr) && !job_ids.empty()) {
                referenced = referenced || QuerySingleInt64(
                    analysis_db,
                    "SELECT COUNT(1) FROM ab_battle_context_probe WHERE source_savestate_id=" + id + " AND (exec_job_id IS NULL OR exec_job_id NOT IN (" + job_ids + "));",
                    &error) > 0;
            }
            if (IsTablePresent(analysis_db, "ab_battle_set", nullptr)) {
                std::string battle_filter = battle_set_ids.empty() ? "0" : battle_set_ids;
                referenced = referenced || QuerySingleInt64(
                    analysis_db,
                    "SELECT COUNT(1) FROM ab_battle_set WHERE entry_savestate_id=" + id + " AND battle_set_id NOT IN (" + battle_filter + ");",
                    &error) > 0;
            }
            if (IsTablePresent(analysis_db, "ab_battle_completion", nullptr)) {
                const auto workflow_filter = workflow_ids.empty() ? std::string("0") : workflow_ids;
                referenced = referenced || QuerySingleInt64(
                    analysis_db,
                    "SELECT COUNT(1) FROM ab_battle_completion WHERE workflow_instance_id NOT IN (" + workflow_filter + ") "
                    "AND (entry_savestate_id=" + id + " OR completion_savestate_id=" + id + ");",
                    &error) > 0;
            }
            if (IsTablePresent(analysis_db, "ab_battle_recording", nullptr)) {
                const auto workflow_filter = workflow_ids.empty() ? std::string("0") : workflow_ids;
                referenced = referenced || QuerySingleInt64(
                    analysis_db,
                    "SELECT COUNT(1) FROM ab_battle_recording WHERE workflow_instance_id NOT IN (" + workflow_filter + ") "
                    "AND (source_savestate_id=" + id + " OR paired_checkpoint_savestate_id=" + id + ");",
                    &error) > 0;
            }
            if (IsTablePresent(analysis_db, "ab_battle_replay", nullptr)) {
                const auto workflow_filter = workflow_ids.empty() ? std::string("0") : workflow_ids;
                referenced = referenced || QuerySingleInt64(
                    analysis_db,
                    "SELECT COUNT(1) FROM ab_battle_replay WHERE workflow_instance_id NOT IN (" + workflow_filter + ") "
                    "AND source_savestate_id=" + id + ";",
                    &error) > 0;
            }
        }
        if (state_db != nullptr) {
            referenced = referenced || QuerySingleInt64(
                state_db,
                "SELECT COUNT(1) FROM state_savestate_derivation "
                "WHERE (from_savestate_id=" + id + " AND to_savestate_id NOT IN (" + sav_ids + ")) "
                "OR (to_savestate_id=" + id + " AND from_savestate_id NOT IN (" + sav_ids + "));",
                &error) > 0;
            if (!referenced && IsTablePresent(state_db, "state_tas_movie_root", nullptr)) {
                referenced = QuerySingleInt64(
                    state_db,
                    "SELECT COUNT(1) FROM state_tas_movie_root WHERE checkpoint_savestate_id=" + id + ";",
                    &error) > 0;
            }
            if (!referenced && IsTablePresent(state_db, "state_tas_movie_trees", nullptr)) {
                referenced = QuerySingleInt64(
                    state_db,
                    "SELECT COUNT(1) FROM state_tas_movie_trees WHERE checkpoint_savestate_id=" + id + ";",
                    &error) > 0;
            }
        }
        if (!referenced) {
            exclusive.push_back(savestate_id);
        }
    }
    return exclusive;
}

std::vector<std::int64_t> FilterExclusiveAggregateArtifactIds(
    sqlite3* analysis_db,
    sqlite3* state_db,
    const std::vector<std::int64_t>& selected_workflow_ids,
    const std::vector<std::int64_t>& selected_job_ids,
    const std::vector<std::int64_t>& artifact_ids) {
    std::vector<std::int64_t> exclusive;
    if (artifact_ids.empty()) return exclusive;
    const auto workflow_filter = selected_workflow_ids.empty()
        ? std::string("0")
        : JoinIds(selected_workflow_ids);
    const auto job_filter = selected_job_ids.empty()
        ? std::string("0")
        : JoinIds(selected_job_ids);
    std::string error;
    for (const auto artifact_id : artifact_ids) {
        const auto id = std::to_string(artifact_id);
        bool referenced = false;
        if (state_db != nullptr) {
            referenced = QuerySingleInt64(
                state_db,
                "SELECT COUNT(1) FROM state_savestate WHERE artifact_id=" + id + ";",
                &error) > 0;
            if (!referenced && IsTablePresent(state_db, "state_tas_movie_root", nullptr)) {
                referenced = QuerySingleInt64(
                    state_db,
                    "SELECT COUNT(1) FROM state_tas_movie_root WHERE source_dtm_artifact_id=" + id
                        + " OR dtm_artifact_id=" + id + " OR itinerary_artifact_id=" + id + ";",
                    &error) > 0;
            }
            if (!referenced && IsTablePresent(state_db, "state_tas_movie_trees", nullptr)) {
                referenced = QuerySingleInt64(
                    state_db,
                    "SELECT COUNT(1) FROM state_tas_movie_trees WHERE dtm_artifact_id=" + id
                        + " OR itinerary_artifact_id=" + id + ";",
                    &error) > 0;
            }
        }
        if (!referenced && analysis_db != nullptr
            && IsTablePresent(analysis_db, "ab_battle_completion", nullptr)) {
            referenced = QuerySingleInt64(
                analysis_db,
                "SELECT COUNT(1) FROM ab_battle_completion WHERE workflow_instance_id NOT IN ("
                    + workflow_filter + ") AND manifest_artifact_id=" + id + ";",
                &error) > 0;
        }
        if (!referenced && analysis_db != nullptr
            && IsTablePresent(analysis_db, "ab_battle_recording", nullptr)) {
            referenced = QuerySingleInt64(
                analysis_db,
                "SELECT COUNT(1) FROM ab_battle_recording WHERE workflow_instance_id NOT IN ("
                    + workflow_filter + ") AND (source_dtm_artifact_id=" + id
                    + " OR source_itinerary_artifact_id=" + id
                    + " OR recorded_dtm_artifact_id=" + id
                    + " OR recorded_itinerary_artifact_id=" + id + ");",
                &error) > 0;
        }
        if (!referenced && analysis_db != nullptr
            && IsTablePresent(analysis_db, "ab_battle_replay", nullptr)) {
            referenced = QuerySingleInt64(
                analysis_db,
                "SELECT COUNT(1) FROM ab_battle_replay WHERE workflow_instance_id NOT IN ("
                    + workflow_filter + ") AND (source_dtm_artifact_id=" + id
                    + " OR source_itinerary_artifact_id=" + id + ");",
                &error) > 0;
        }
        if (!referenced && analysis_db != nullptr
            && IsTablePresent(analysis_db, "ab_turn_job", nullptr)) {
            referenced = QuerySingleInt64(
                analysis_db,
                "SELECT COUNT(1) FROM ab_turn_job WHERE (exec_job_id IS NULL OR exec_job_id NOT IN ("
                    + job_filter + ")) AND (input_trace_artifact_id=" + id
                    + " OR applied_input_artifact_id=" + id + ");",
                &error) > 0;
        }
        if (!referenced && analysis_db != nullptr
            && IsTablePresent(analysis_db, "ab_manual_followup", nullptr)) {
            referenced = QuerySingleInt64(
                analysis_db,
                "SELECT COUNT(1) FROM ab_manual_followup f JOIN ab_turn_job t ON t.turn_job_id=f.turn_job_id "
                    "WHERE (t.exec_job_id IS NULL OR t.exec_job_id NOT IN (" + job_filter
                    + ")) AND (f.recorded_dtm_artifact_id=" + id
                    + " OR f.recorded_dtmini_artifact_id=" + id
                    + " OR f.recorded_sav_artifact_id=" + id + ");",
                &error) > 0;
        }
        if (!referenced && analysis_db != nullptr
            && IsTablePresent(analysis_db, "tmv_validation_request", nullptr)) {
            referenced = QuerySingleInt64(
                analysis_db,
                "SELECT COUNT(1) FROM tmv_validation_request WHERE workflow_instance_id NOT IN ("
                    + workflow_filter + ") AND (source_dtm_artifact_id=" + id
                    + " OR itinerary_artifact_id=" + id + ");",
                &error) > 0;
        }
        if (!referenced && analysis_db != nullptr
            && IsTablePresent(analysis_db, "tmv_validation_attempt", nullptr)) {
            referenced = QuerySingleInt64(
                analysis_db,
                "SELECT COUNT(1) FROM tmv_validation_attempt a JOIN tmv_validation_request r "
                    "ON r.validation_request_id=a.validation_request_id WHERE r.workflow_instance_id NOT IN ("
                    + workflow_filter + ") AND a.candidate_itinerary_artifact_id=" + id + ";",
                &error) > 0;
        }
        if (!referenced) exclusive.push_back(artifact_id);
    }
    return exclusive;
}

std::string BuildSelectionSummaryJson(const ArchiveWorkflowSelection& selection, const std::vector<std::int64_t>& selected_ids) {
    std::ostringstream json;
    json << "{";
    json << "\"workflow_count\":" << selected_ids.size();
    if (!selection.created_by_filter_snapshot.empty()) {
        json << ",\"created_by_filter_snapshot\":" << EscapeJson(selection.created_by_filter_snapshot);
    }
    json << ",\"explicit_exclusions\":[";
    for (std::size_t i = 0; i < selection.explicit_exclusions.size(); ++i) {
        if (i != 0) json << ',';
        json << selection.explicit_exclusions[i];
    }
    json << "]}";
    return json.str();
}

} // namespace

SqliteArchivePackageService::SqliteArchivePackageService(
    sqlite3* execution_db,
    savor::db::IExecutionDb* execution_retention_db,
    savor::db::IUiReadDb* ui_read_db,
    IArchiveDb* archive_db,
    DbConfigPaths config_paths,
    sqlite3* state_db,
    sqlite3* analysis_db,
    sqlite3* ui_read_sqlite_db)
    : execution_db_(execution_db)
    , state_db_(state_db)
    , analysis_db_(analysis_db)
    , ui_read_sqlite_db_(ui_read_sqlite_db)
    , execution_retention_db_(execution_retention_db)
    , ui_read_db_(ui_read_db)
    , archive_db_(archive_db)
    , config_paths_(std::move(config_paths)) {
}

CreateArchivePackageResult SqliteArchivePackageService::CreatePackage(const CreateArchivePackageRequest& request) {
    CreateArchivePackageResult result{};

    if (execution_db_ == nullptr || archive_db_ == nullptr) {
        result.error = "null db dependency";
        return result;
    }
    if (request.source_job_set_id <= 0) {
        result.error = "source_job_set_id must be positive";
        return result;
    }

    std::vector<std::string> readiness_blockers;
    std::string readiness_error;
    if (!CollectArchiveReadinessBlockers(
            execution_db_,
            JobSetCte(request.source_job_set_id),
            &readiness_blockers,
            &readiness_error)) {
        result.error = readiness_error;
        return result;
    }
    if (!readiness_blockers.empty()) {
        result.error = "archive source is not quiescent: " + JoinBlockers(readiness_blockers);
        return result;
    }

    std::int64_t manifest_schema_version = request.schema_version;
    if (manifest_schema_version <= 0) {
        std::string schema_error;
        const auto execution_schema = migrations::GetCurrentContextSchemaVersion(
            execution_db_,
            migrations::MigrationContext::Execution,
            &schema_error);
        if (!execution_schema.has_value()) {
            result.error = "failed reading execution schema version: " + schema_error;
            return result;
        }
        manifest_schema_version = *execution_schema;
    }

    const auto epoch = request.created_at_utc.time_since_epoch().count();
    const auto package_name = request.archive_name.empty()
        ? "package-" + std::to_string(request.source_job_set_id) + "-" + std::to_string(epoch)
        : "package-" + SlugForPackageName(request.archive_name) + "-" + std::to_string(request.source_job_set_id) + "-" + std::to_string(epoch);

    ExportContext context{};
    context.package_root = config_paths_.archive_store_root / package_name;
    context.data_dir = context.package_root / "data";
    context.blob_dir = context.package_root / "blobs";

    std::string filesystem_error;
    try {
        std::filesystem::create_directories(context.data_dir);
        std::filesystem::create_directories(context.blob_dir);
    } catch (const std::exception& ex) {
        result.error = ex.what();
        return result;
    }

    std::string table_error;
    const bool has_workflow_event = IsTablePresent(execution_db_, "exec_workflow_event", &table_error);
    if (!table_error.empty()) {
        result.error = table_error;
        return result;
    }
    const bool has_trigger = IsTablePresent(execution_db_, "exec_trigger", &table_error);
    if (!table_error.empty()) {
        result.error = table_error;
        return result;
    }
    const bool has_outbox = IsTablePresent(execution_db_, "exec_outbox_message", &table_error);
    if (!table_error.empty()) {
        result.error = table_error;
        return result;
    }

    const auto export_specs = BuildExportSpecs(request, has_workflow_event, has_trigger, has_outbox);
    for (const auto& spec : export_specs) {
        std::string export_error;
        if (!ExportTable(execution_db_, spec, request.retention_policy, &context, &export_error)) {
            result.error = export_error;
            return result;
        }
    }

    context.files.insert(context.files.end(), context.blobs.begin(), context.blobs.end());

    std::sort(
        context.files.begin(),
        context.files.end(),
        [](const ArchivePackageFileSummary& lhs, const ArchivePackageFileSummary& rhs) {
            return lhs.relative_path.generic_string() < rhs.relative_path.generic_string();
        });

    std::ostringstream checksums_json;
    checksums_json << "{\n  \"algorithm\": \"fnv1a64\",\n  \"files\": [\n";
    for (std::size_t i = 0; i < context.files.size(); ++i) {
        const auto& file = context.files[i];
        checksums_json << "    {\"path\": " << EscapeJson(file.relative_path.generic_string())
                       << ", \"checksum\": " << EscapeJson(file.checksum) << "}";
        if (i + 1 < context.files.size()) {
            checksums_json << ',';
        }
        checksums_json << "\n";
    }
    checksums_json << "  ]\n}\n";

    const auto checksums_rel = std::filesystem::path("checksums.json");
    const auto checksums_path = context.package_root / checksums_rel;
    std::string write_error;
    if (!WriteFileText(checksums_path, checksums_json.str(), &write_error)) {
        result.error = write_error;
        return result;
    }

    ArchivePackageFileSummary checksums_summary{};
    checksums_summary.item_kind = "checksums";
    checksums_summary.relative_path = checksums_rel;
    checksums_summary.row_count = static_cast<int>(context.files.size());
    checksums_summary.checksum = ChecksumForFile(checksums_path);
    context.files.push_back(checksums_summary);

    std::string checksum_validation_error;
    if (!ValidateChecksums(context.package_root, context.files, &checksum_validation_error)) {
        result.error = checksum_validation_error;
        return result;
    }

    const auto min_time = context.has_time ? context.min_time : request.created_at_utc;
    const auto max_time = context.has_time ? context.max_time : request.created_at_utc;

    std::ostringstream manifest;
    manifest << "{\n"
             << "  \"source_context\": " << EscapeJson(request.source_context) << ",\n"
             << "  \"archive_name\": " << EscapeJson(request.archive_name) << ",\n"
             << "  \"archive_notes\": " << (request.archive_notes.has_value() ? EscapeJson(*request.archive_notes) : std::string("null")) << ",\n"
             << "  \"source_job_set_id\": " << request.source_job_set_id << ",\n"
             << "  \"created_at_utc\": " << request.created_at_utc.time_since_epoch().count() << ",\n"
             << "  \"time_range_start_utc\": " << min_time.time_since_epoch().count() << ",\n"
             << "  \"time_range_end_utc\": " << max_time.time_since_epoch().count() << ",\n"
             << "  \"schema_version\": " << manifest_schema_version << ",\n"
             << "  \"event_catalog_version\": " << request.event_catalog_version << ",\n"
             << "  \"retention_policy\": {\n"
             << "    \"include_workflow_event\": " << (request.retention_policy.include_workflow_event ? "true" : "false") << ",\n"
             << "    \"include_trigger\": " << (request.retention_policy.include_trigger ? "true" : "false") << ",\n"
             << "    \"include_outbox_message\": " << (request.retention_policy.include_outbox_message ? "true" : "false") << ",\n"
             << "    \"inline_payload_max_bytes\": " << request.retention_policy.inline_payload_max_bytes << "\n"
             << "  },\n"
             << "  \"files\": [\n";

    for (std::size_t i = 0; i < context.files.size(); ++i) {
        const auto& file = context.files[i];
        manifest << "    {\"item_kind\": " << EscapeJson(file.item_kind)
                 << ", \"path\": " << EscapeJson(file.relative_path.generic_string())
                 << ", \"row_count\": " << file.row_count
                 << ", \"checksum\": " << EscapeJson(file.checksum) << "}";
        if (i + 1 < context.files.size()) {
            manifest << ',';
        }
        manifest << "\n";
    }
    manifest << "  ]\n}\n";

    const auto manifest_rel = std::filesystem::path("manifest.json");
    const auto manifest_path = context.package_root / manifest_rel;
    if (!WriteFileText(manifest_path, manifest.str(), &write_error)) {
        result.error = write_error;
        return result;
    }

    CreateArchivePackageCommand create_command{};
    create_command.source_context = request.source_context;
    create_command.source_job_set_id = request.source_job_set_id;
    create_command.archive_name = request.archive_name;
    create_command.archive_notes = request.archive_notes;
    create_command.created_at_utc = request.created_at_utc;
    create_command.schema_version = manifest_schema_version;
    create_command.event_catalog_version = request.event_catalog_version;
    create_command.time_range_start_utc = min_time;
    create_command.time_range_end_utc = max_time;
    create_command.manifest_path = manifest_path.generic_string();
    create_command.checksum_status = "PASS";
    create_command.correlation_id = request.correlation_id;
    create_command.causation_id = request.causation_id;

    std::string archive_error;
    std::int64_t archive_package_id = 0;
    if (!archive_db_->CreateArchivePackage(create_command, &archive_package_id, &archive_error)) {
        result.error = archive_error;
        return result;
    }

    for (const auto& file : context.files) {
        AddArchiveItemCommand item_command{};
        item_command.archive_package_id = archive_package_id;
        item_command.item_kind = file.item_kind;
        item_command.item_count = file.row_count;
        item_command.blob_path = file.relative_path.generic_string();
        item_command.checksum = file.checksum;
        item_command.indexed_at_utc = request.created_at_utc;
        item_command.correlation_id = request.correlation_id;
        item_command.causation_id = request.causation_id;

        if (!archive_db_->AddArchiveItem(item_command, nullptr, &archive_error)) {
            result.error = archive_error;
            return result;
        }
    }

    result.success = true;
    result.archive_package_id = archive_package_id;
    result.package_root = context.package_root;
    result.files = context.files;
    return result;
}

WorkflowArchivePreview SqliteArchivePackageService::PreviewWorkflowArchive(
    const ArchiveWorkflowSelection& selection,
    std::string* error_out) const {
    WorkflowArchivePreview preview{};
    if (execution_db_ == nullptr) {
        preview.error = "execution db is null";
        if (error_out) *error_out = *preview.error;
        return preview;
    }

    const auto workflow_ids = NormalizeWorkflowSelection(selection);
    if (workflow_ids.empty()) {
        preview.error = "workflow selection is empty";
        if (error_out) *error_out = *preview.error;
        return preview;
    }
    std::string final_selection_error;
    if (!ValidateFinalWorkflowSelection(execution_db_, workflow_ids, &final_selection_error)) {
        preview.error = final_selection_error;
        if (error_out) *error_out = final_selection_error;
        return preview;
    }
    preview.workflow_count = static_cast<int>(workflow_ids.size());

    const auto workflow_id_list = JoinIds(workflow_ids);
    std::string query_error;
    const auto base_scoped = WorkflowJobSetCte(workflow_id_list);
    const auto base_job_ids = QueryInt64Column(
        execution_db_,
        base_scoped
            + "SELECT job_id FROM scoped_jobs ORDER BY job_id ASC;",
        &query_error);
    const auto base_job_set_ids = QueryInt64Column(
        execution_db_,
        base_scoped
            + "SELECT job_set_id FROM scoped_job_sets "
              "ORDER BY job_set_id ASC;",
        &query_error);
    const auto seed_probe_execution_job_set_ids =
        CollectSeedProbeExecutionJobSetIds(
            execution_db_,
            analysis_db_,
            workflow_ids,
            base_job_ids,
            base_job_set_ids,
            &query_error);
    if (!query_error.empty()) {
        preview.error = query_error;
        if (error_out) *error_out = query_error;
        return preview;
    }
    const auto scoped = WorkflowJobSetCte(
        workflow_id_list,
        seed_probe_execution_job_set_ids);
    if (!CollectArchiveReadinessBlockers(
            execution_db_,
            scoped,
            &preview.purge_blockers,
            &query_error)) {
        preview.error = query_error;
        if (error_out) *error_out = query_error;
        return preview;
    }
    const auto job_ids = QueryInt64Column(
        execution_db_,
        scoped + "SELECT job_id FROM scoped_jobs ORDER BY job_id ASC;",
        &query_error);
    if (!query_error.empty()) {
        preview.error = query_error;
        if (error_out) *error_out = query_error;
        return preview;
    }

    auto count_rows = [](sqlite3* db, const ExportSpec& spec, std::string* error) -> std::int64_t {
        std::string query = spec.query;
        while (!query.empty() && (query.back() == ';' || std::isspace(static_cast<unsigned char>(query.back())))) {
            query.pop_back();
        }
        return QuerySingleInt64(db, "SELECT COUNT(1) FROM (" + query + ") archive_count_subquery;", error);
    };

    auto execution_policy = ArchivePackageRetentionPolicy{};
    execution_policy.inline_payload_max_bytes = std::numeric_limits<std::size_t>::max();
    for (const auto& spec : BuildWorkflowExecutionSpecs(
             workflow_ids,
             execution_policy,
             execution_db_,
             seed_probe_execution_job_set_ids)) {
        preview.execution_row_count += static_cast<int>(count_rows(execution_db_, spec, &query_error));
    }

    std::vector<std::int64_t> battle_set_ids;
    if (analysis_db_ != nullptr) {
        for (const auto& spec : BuildWorkflowAnalysisSpecs(execution_db_, analysis_db_, job_ids, workflow_ids, &battle_set_ids)) {
            preview.analysis_row_count += static_cast<int>(count_rows(analysis_db_, spec, &query_error));
        }
    }
    if (ui_read_sqlite_db_ != nullptr) {
        for (const auto& spec : BuildWorkflowUiReadSpecs(ui_read_sqlite_db_, workflow_ids, job_ids)) {
            preview.ui_read_snapshot_row_count += static_cast<int>(count_rows(ui_read_sqlite_db_, spec, &query_error));
        }
    }

    if (state_db_ != nullptr) {
        const auto savestate_ids = CollectWorkflowSavestateIds(execution_db_, analysis_db_, workflow_ids, job_ids, battle_set_ids);
        const auto savestate_rows = LoadSavestateRows(state_db_, savestate_ids, &query_error);
        preview.savestate_count = static_cast<int>(savestate_rows.size());
        for (const auto& row : savestate_rows) {
            preview.savestate_bytes += static_cast<std::uint64_t>(std::max<std::int64_t>(0, row.size_bytes));
        }
        preview.shared_savestate_count = 0;
        preview.exclusive_savestate_count = preview.savestate_count;
    }

    if (!query_error.empty()) {
        preview.error = query_error;
        if (error_out) *error_out = query_error;
        return preview;
    }

    preview.success = true;
    return preview;
}

CreateArchivePackageResult SqliteArchivePackageService::CreateWorkflowPackage(
    const CreateWorkflowArchivePackageRequest& request) {
    CreateArchivePackageResult result{};
    if (execution_db_ == nullptr || archive_db_ == nullptr) {
        result.error = "null db dependency";
        return result;
    }

    const auto workflow_ids = NormalizeWorkflowSelection(request.selection);
    if (workflow_ids.empty()) {
        result.error = "workflow selection is empty";
        return result;
    }
    std::string final_selection_error;
    if (!ValidateFinalWorkflowSelection(execution_db_, workflow_ids, &final_selection_error)) {
        result.error = final_selection_error;
        return result;
    }
    if (request.include_state_savestates && state_db_ == nullptr) {
        result.error = "state db is required when include_state_savestates is true";
        return result;
    }

    const auto workflow_id_list = JoinIds(workflow_ids);
    std::string query_error;
    const auto base_scoped = WorkflowJobSetCte(workflow_id_list);
    const auto base_job_ids = QueryInt64Column(
        execution_db_,
        base_scoped
            + "SELECT job_id FROM scoped_jobs ORDER BY job_id ASC;",
        &query_error);
    const auto base_job_set_ids = QueryInt64Column(
        execution_db_,
        base_scoped
            + "SELECT job_set_id FROM scoped_job_sets "
              "ORDER BY job_set_id ASC;",
        &query_error);
    const auto seed_probe_execution_job_set_ids = request.include_analysis
        ? CollectSeedProbeExecutionJobSetIds(
              execution_db_,
              analysis_db_,
              workflow_ids,
              base_job_ids,
              base_job_set_ids,
              &query_error)
        : std::vector<std::int64_t>{};
    if (!query_error.empty()) {
        result.error = query_error;
        return result;
    }
    if (!seed_probe_execution_job_set_ids.empty()
        && !request.include_execution) {
        result.error =
            "SeedProbe analysis archive requires execution context because "
            "sp_probe_result intent is stored in its source jobs";
        return result;
    }

    const auto scoped = WorkflowJobSetCte(
        workflow_id_list,
        seed_probe_execution_job_set_ids);
    std::vector<std::string> readiness_blockers;
    std::string readiness_error;
    if (!CollectArchiveReadinessBlockers(
            execution_db_,
            scoped,
            &readiness_blockers,
            &readiness_error)) {
        result.error = readiness_error;
        return result;
    }
    if (!readiness_blockers.empty()) {
        result.error = "archive source is not quiescent: " + JoinBlockers(readiness_blockers);
        return result;
    }

    std::int64_t execution_schema_version = request.schema_version;
    if (execution_schema_version <= 0) {
        std::string schema_error;
        const auto execution_schema = migrations::GetCurrentContextSchemaVersion(
            execution_db_,
            migrations::MigrationContext::Execution,
            &schema_error);
        if (!execution_schema.has_value()) {
            result.error = "failed reading execution schema version: " + schema_error;
            return result;
        }
        execution_schema_version = *execution_schema;
    }

    const auto epoch = request.created_at_utc.time_since_epoch().count();
    const auto package_name = "workflow-package-"
        + SlugForPackageName(request.archive_name)
        + "-"
        + std::to_string(workflow_ids.size())
        + "-"
        + std::to_string(epoch);

    ExportContext context{};
    context.package_root = config_paths_.archive_store_root / package_name;
    context.data_dir = context.package_root / "data";
    context.blob_dir = context.package_root / "blobs";

    try {
        std::filesystem::create_directories(context.data_dir);
        std::filesystem::create_directories(context.blob_dir);
    } catch (const std::exception& ex) {
        result.error = ex.what();
        return result;
    }

    auto package_policy = request.retention_policy;
    package_policy.inline_payload_max_bytes = std::numeric_limits<std::size_t>::max();

    const auto job_ids = QueryInt64Column(
        execution_db_,
        scoped + "SELECT job_id FROM scoped_jobs ORDER BY job_id ASC;",
        &query_error);
    if (!query_error.empty()) {
        result.error = query_error;
        return result;
    }

    const auto execution_specs = request.include_execution
        ? BuildWorkflowExecutionSpecs(
              workflow_ids,
              package_policy,
              execution_db_,
              seed_probe_execution_job_set_ids)
        : std::vector<ExportSpec>{};
    std::vector<std::int64_t> battle_set_ids;
    const auto analysis_specs = (request.include_analysis && analysis_db_ != nullptr)
        ? BuildWorkflowAnalysisSpecs(execution_db_, analysis_db_, job_ids, workflow_ids, &battle_set_ids)
        : std::vector<ExportSpec>{};
    const auto ui_read_specs = (request.include_ui_read_snapshot && ui_read_sqlite_db_ != nullptr)
        ? BuildWorkflowUiReadSpecs(ui_read_sqlite_db_, workflow_ids, job_ids)
        : std::vector<ExportSpec>{};
    const auto aggregate_artifact_ids = request.include_analysis
        ? CollectWorkflowAggregateArtifactIds(analysis_db_, workflow_ids)
        : std::vector<std::int64_t>{};
    if (!request.include_state_savestates && !aggregate_artifact_ids.empty()) {
        result.error = "battle-end aggregate artifacts require include_state_savestates";
        return result;
    }

    std::vector<SavestateArchiveRow> savestate_rows;
    std::vector<SavestateArchiveRow> state_artifact_rows;
    std::vector<ExportSpec> state_specs;
    if (request.include_state_savestates && state_db_ != nullptr) {
        const auto tas_movie_closure = CollectTasMovieArchiveClosure(
            analysis_db_, state_db_, workflow_ids, &query_error);
        if (!query_error.empty()) {
            result.error = query_error;
            return result;
        }
        auto savestate_ids = CollectWorkflowSavestateIds(execution_db_, analysis_db_, workflow_ids, job_ids, battle_set_ids);
        savestate_ids.insert(savestate_ids.end(), tas_movie_closure.savestate_ids.begin(), tas_movie_closure.savestate_ids.end());
        std::sort(savestate_ids.begin(), savestate_ids.end());
        savestate_ids.erase(std::unique(savestate_ids.begin(), savestate_ids.end()), savestate_ids.end());
        savestate_rows = LoadSavestateRows(state_db_, savestate_ids, &query_error);
        if (!query_error.empty()) {
            result.error = query_error;
            return result;
        }
        state_artifact_rows = savestate_rows;
        auto standalone_artifact_ids = aggregate_artifact_ids;
        standalone_artifact_ids.insert(
            standalone_artifact_ids.end(),
            tas_movie_closure.artifact_ids.begin(),
            tas_movie_closure.artifact_ids.end());
        std::sort(standalone_artifact_ids.begin(), standalone_artifact_ids.end());
        standalone_artifact_ids.erase(
            std::unique(standalone_artifact_ids.begin(), standalone_artifact_ids.end()),
            standalone_artifact_ids.end());
        auto aggregate_artifact_rows = LoadStandaloneArtifactRows(state_db_, standalone_artifact_ids, &query_error);
        if (!query_error.empty()) {
            result.error = query_error;
            return result;
        }
        MergeArtifactRows(&state_artifact_rows, std::move(aggregate_artifact_rows));
        state_specs = BuildStateSavestateSpecs(
            state_artifact_rows,
            tas_movie_closure.root_ids,
            tas_movie_closure.tree_ids);
    }

    const auto export_total = static_cast<std::int64_t>(
        execution_specs.size() + analysis_specs.size() + ui_read_specs.size() + state_specs.size());
    std::int64_t export_completed = 0;
    EmitArchiveProgress(
        request.progress_sink,
        ArchiveOperationPhase::ExportingRows,
        "Exporting archive row streams",
        export_completed,
        export_total,
        export_total == 0);

    if (request.include_execution) {
        for (const auto& spec : execution_specs) {
            std::string export_error;
            if (!ExportTable(execution_db_, spec, package_policy, &context, &export_error)) {
                result.error = export_error;
                return result;
            }
            ++export_completed;
            EmitArchiveProgress(
                request.progress_sink,
                ArchiveOperationPhase::ExportingRows,
                "Exported " + spec.item_kind,
                export_completed,
                export_total,
                false);
        }
    }

    if (request.include_analysis && analysis_db_ != nullptr) {
        for (const auto& spec : analysis_specs) {
            std::string export_error;
            if (!ExportTable(analysis_db_, spec, package_policy, &context, &export_error)) {
                result.error = export_error;
                return result;
            }
            ++export_completed;
            EmitArchiveProgress(
                request.progress_sink,
                ArchiveOperationPhase::ExportingRows,
                "Exported " + spec.item_kind,
                export_completed,
                export_total,
                false);
        }
    }

    if (request.include_ui_read_snapshot && ui_read_sqlite_db_ != nullptr) {
        for (const auto& spec : ui_read_specs) {
            std::string export_error;
            if (!ExportTable(ui_read_sqlite_db_, spec, package_policy, &context, &export_error)) {
                result.error = export_error;
                return result;
            }
            ++export_completed;
            EmitArchiveProgress(
                request.progress_sink,
                ArchiveOperationPhase::ExportingRows,
                "Exported " + spec.item_kind,
                export_completed,
                export_total,
                false);
        }
    }

    if (request.include_state_savestates && state_db_ != nullptr) {
        for (const auto& spec : state_specs) {
            std::string export_error;
            if (!ExportTable(state_db_, spec, package_policy, &context, &export_error)) {
                result.error = export_error;
                return result;
            }
            ++export_completed;
            EmitArchiveProgress(
                request.progress_sink,
                ArchiveOperationPhase::ExportingRows,
                "Exported " + spec.item_kind,
                export_completed,
                export_total,
                false);
        }

        std::set<std::string> emitted_sha;
        std::vector<ZipSourceEntry> zip_entries;
        std::uint64_t zip_bytes = 0;
        for (const auto& row : state_artifact_rows) {
            if (row.sha256.empty() || row.filename.empty() || emitted_sha.find(row.sha256) != emitted_sha.end()) {
                continue;
            }
            emitted_sha.insert(row.sha256);
            ZipSourceEntry entry{};
            entry.entry_name = row.sha256 + SafeArchiveExtension(row.artifact_kind, row.file_ext);
            std::string locator_error;
            const auto source_path = ResolveArchiveArtifactPath(
                config_paths_, row, &locator_error);
            if (!source_path) {
                result.error = "invalid archived artifact locator: " + locator_error;
                return result;
            }
            entry.source_path = *source_path;
            std::error_code ec;
            const auto bytes = std::filesystem::file_size(entry.source_path, ec);
            if (!ec) {
                zip_bytes += static_cast<std::uint64_t>(bytes);
            }
            zip_entries.push_back(std::move(entry));
        }
        EmitArchiveProgress(
            request.progress_sink,
            ArchiveOperationPhase::WritingSavestates,
            zip_entries.empty() ? "No savestate files to write" : "Writing savestate zip",
            0,
            static_cast<std::int64_t>(zip_entries.size()),
            zip_entries.empty(),
            0,
            zip_bytes);
        if (!zip_entries.empty()) {
            const auto zip_rel = std::filesystem::path("savestates.zip");
            const auto zip_path = context.package_root / zip_rel;
            std::string zip_error;
            if (!WriteStoredZip(zip_path, zip_entries, &zip_error)) {
                result.error = zip_error;
                return result;
            }
            ArchivePackageFileSummary zip_summary{};
            zip_summary.item_kind = "state_savestate_zip";
            zip_summary.relative_path = zip_rel;
            zip_summary.row_count = static_cast<int>(zip_entries.size());
            zip_summary.checksum = ChecksumForFile(zip_path);
            context.files.push_back(std::move(zip_summary));
            EmitArchiveProgress(
                request.progress_sink,
                ArchiveOperationPhase::WritingSavestates,
                "Savestate zip written",
                static_cast<std::int64_t>(zip_entries.size()),
                static_cast<std::int64_t>(zip_entries.size()),
                false,
                zip_bytes,
                zip_bytes);
        }
    }

    context.files.insert(context.files.end(), context.blobs.begin(), context.blobs.end());
    std::sort(
        context.files.begin(),
        context.files.end(),
        [](const ArchivePackageFileSummary& lhs, const ArchivePackageFileSummary& rhs) {
            return lhs.relative_path.generic_string() < rhs.relative_path.generic_string();
        });

    std::ostringstream checksums_json;
    checksums_json << "{\n  \"algorithm\": \"fnv1a64\",\n  \"files\": [\n";
    for (std::size_t i = 0; i < context.files.size(); ++i) {
        const auto& file = context.files[i];
        checksums_json << "    {\"path\": " << EscapeJson(file.relative_path.generic_string())
                       << ", \"checksum\": " << EscapeJson(file.checksum) << "}";
        if (i + 1 < context.files.size()) {
            checksums_json << ',';
        }
        checksums_json << "\n";
    }
    checksums_json << "  ]\n}\n";

    const auto checksums_rel = std::filesystem::path("checksums.json");
    const auto checksums_path = context.package_root / checksums_rel;
    std::string write_error;
    if (!WriteFileText(checksums_path, checksums_json.str(), &write_error)) {
        result.error = write_error;
        return result;
    }

    ArchivePackageFileSummary checksums_summary{};
    checksums_summary.item_kind = "checksums";
    checksums_summary.relative_path = checksums_rel;
    checksums_summary.row_count = static_cast<int>(context.files.size());
    checksums_summary.checksum = ChecksumForFile(checksums_path);
    context.files.push_back(checksums_summary);

    std::string checksum_validation_error;
    if (!ValidateChecksums(context.package_root, context.files, &checksum_validation_error)) {
        result.error = checksum_validation_error;
        return result;
    }

    const auto min_time = context.has_time ? context.min_time : request.created_at_utc;
    const auto max_time = context.has_time ? context.max_time : request.created_at_utc;
    const auto selection_summary = BuildSelectionSummaryJson(request.selection, workflow_ids);

    std::ostringstream manifest;
    manifest << "{\n"
             << "  \"source_context\": " << EscapeJson(request.source_context) << ",\n"
             << "  \"archive_name\": " << EscapeJson(request.archive_name) << ",\n"
             << "  \"archive_notes\": " << (request.archive_notes.has_value() ? EscapeJson(*request.archive_notes) : std::string("null")) << ",\n"
             << "  \"source_scope_kind\": \"workflow_selection\",\n"
             << "  \"source_workflow_count\": " << workflow_ids.size() << ",\n"
             << "  \"selection\": " << selection_summary << ",\n"
             << "  \"workflow_instance_ids\": [";
    for (std::size_t i = 0; i < workflow_ids.size(); ++i) {
        if (i != 0) manifest << ',';
        manifest << workflow_ids[i];
    }
    manifest << "],\n"
             << "  \"created_at_utc\": " << request.created_at_utc.time_since_epoch().count() << ",\n"
             << "  \"time_range_start_utc\": " << min_time.time_since_epoch().count() << ",\n"
             << "  \"time_range_end_utc\": " << max_time.time_since_epoch().count() << ",\n"
             << "  \"schema_version\": " << execution_schema_version << ",\n"
             << "  \"state_schema_version\": " << request.state_schema_version << ",\n"
             << "  \"analysis_schema_version\": " << request.analysis_schema_version << ",\n"
             << "  \"ui_read_schema_version\": " << request.ui_read_schema_version << ",\n"
             << "  \"event_catalog_version\": " << request.event_catalog_version << ",\n"
             << "  \"savestate_zip\": " << EscapeJson("savestates.zip") << ",\n"
             << "  \"files\": [\n";
    for (std::size_t i = 0; i < context.files.size(); ++i) {
        const auto& file = context.files[i];
        manifest << "    {\"item_kind\": " << EscapeJson(file.item_kind)
                 << ", \"path\": " << EscapeJson(file.relative_path.generic_string())
                 << ", \"row_count\": " << file.row_count
                 << ", \"checksum\": " << EscapeJson(file.checksum) << "}";
        if (i + 1 < context.files.size()) {
            manifest << ',';
        }
        manifest << "\n";
    }
    manifest << "  ]\n}\n";

    const auto manifest_rel = std::filesystem::path("manifest.json");
    const auto manifest_path = context.package_root / manifest_rel;
    if (!WriteFileText(manifest_path, manifest.str(), &write_error)) {
        result.error = write_error;
        return result;
    }

    CreateArchivePackageCommand create_command{};
    create_command.source_context = request.source_context;
    create_command.source_job_set_id = 0;
    create_command.source_scope_kind = "workflow_selection";
    create_command.source_workflow_count = static_cast<std::int64_t>(workflow_ids.size());
    create_command.selection_summary = selection_summary;
    create_command.archive_name = request.archive_name;
    create_command.archive_notes = request.archive_notes;
    create_command.created_at_utc = request.created_at_utc;
    create_command.schema_version = execution_schema_version;
    create_command.event_catalog_version = request.event_catalog_version;
    create_command.time_range_start_utc = min_time;
    create_command.time_range_end_utc = max_time;
    create_command.manifest_path = manifest_path.generic_string();
    create_command.checksum_status = "PASS";
    create_command.correlation_id = request.correlation_id;
    create_command.causation_id = request.causation_id;

    std::string archive_error;
    std::int64_t archive_package_id = 0;
    EmitArchiveProgress(
        request.progress_sink,
        ArchiveOperationPhase::RegisteringPackage,
        "Registering archive package",
        0,
        static_cast<std::int64_t>(context.files.size() + workflow_ids.size() + 1),
        false);
    if (!archive_db_->CreateArchivePackage(create_command, &archive_package_id, &archive_error)) {
        result.error = archive_error;
        return result;
    }

    std::int64_t register_completed = 1;
    for (const auto& file : context.files) {
        AddArchiveItemCommand item_command{};
        item_command.archive_package_id = archive_package_id;
        item_command.item_kind = file.item_kind;
        item_command.item_count = file.row_count;
        item_command.blob_path = file.relative_path.generic_string();
        item_command.checksum = file.checksum;
        item_command.indexed_at_utc = request.created_at_utc;
        item_command.correlation_id = request.correlation_id;
        item_command.causation_id = request.causation_id;
        if (!archive_db_->AddArchiveItem(item_command, nullptr, &archive_error)) {
            result.error = archive_error;
            return result;
        }
        ++register_completed;
        EmitArchiveProgress(
            request.progress_sink,
            ArchiveOperationPhase::RegisteringPackage,
            "Registered " + file.item_kind,
            register_completed,
            static_cast<std::int64_t>(context.files.size() + workflow_ids.size() + 1),
            false);
    }

    Statement member_st;
    const std::string member_sql =
        (ui_read_sqlite_db_ != nullptr && IsTablePresent(ui_read_sqlite_db_, "ui_workflow_instance", nullptr))
            ? "SELECT workflow_instance_id,workflow_kind,state,root_scope_kind,root_scope_id,created_at_utc,completed_at_utc,COALESCE(battle_final_victory_count,0) FROM ui_workflow_instance WHERE workflow_instance_id IN (" + workflow_id_list + ") ORDER BY workflow_instance_id ASC;"
            : "SELECT workflow_instance_id,workflow_kind,state,root_scope_kind,root_scope_id,created_at_utc,completed_at_utc,0 FROM exec_workflow_instance WHERE workflow_instance_id IN (" + workflow_id_list + ") ORDER BY workflow_instance_id ASC;";
    sqlite3* member_db = (ui_read_sqlite_db_ != nullptr && IsTablePresent(ui_read_sqlite_db_, "ui_workflow_instance", nullptr)) ? ui_read_sqlite_db_ : execution_db_;
    if (sqlite3_prepare_v2(member_db, member_sql.c_str(), -1, &member_st.st, nullptr) != SQLITE_OK) {
        result.error = sqlite3_errmsg(member_db);
        return result;
    }
    while (true) {
        const int rc = sqlite3_step(member_st.st);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            result.error = sqlite3_errmsg(member_db);
            return result;
        }
        AddArchiveWorkflowPackageMemberCommand member{};
        member.archive_package_id = archive_package_id;
        member.workflow_instance_id = sqlite3_column_int64(member_st.st, 0);
        const auto* workflow_kind = reinterpret_cast<const char*>(sqlite3_column_text(member_st.st, 1));
        if (workflow_kind != nullptr) member.workflow_kind = std::string(workflow_kind);
        const auto* display_state = reinterpret_cast<const char*>(sqlite3_column_text(member_st.st, 2));
        if (display_state != nullptr) member.display_state = std::string(display_state);
        const auto* root_scope_kind = reinterpret_cast<const char*>(sqlite3_column_text(member_st.st, 3));
        if (root_scope_kind != nullptr) member.root_scope_kind = std::string(root_scope_kind);
        if (sqlite3_column_type(member_st.st, 4) != SQLITE_NULL) member.root_scope_id = sqlite3_column_int64(member_st.st, 4);
        if (sqlite3_column_type(member_st.st, 5) != SQLITE_NULL) member.created_at_utc = FromEpochMillis(sqlite3_column_int64(member_st.st, 5));
        if (sqlite3_column_type(member_st.st, 6) != SQLITE_NULL) member.completed_at_utc = FromEpochMillis(sqlite3_column_int64(member_st.st, 6));
        member.battle_final_victory_count = sqlite3_column_int(member_st.st, 7);
        if (!archive_db_->AddArchiveWorkflowPackageMember(member, &archive_error)) {
            result.error = archive_error;
            return result;
        }
        ++register_completed;
        EmitArchiveProgress(
            request.progress_sink,
            ArchiveOperationPhase::RegisteringPackage,
            "Registered workflow package member",
            register_completed,
            static_cast<std::int64_t>(context.files.size() + workflow_ids.size() + 1),
            false);
    }

    result.success = true;
    result.archive_package_id = archive_package_id;
    result.package_root = context.package_root;
    result.files = context.files;
    return result;
}

WorkflowArchivePurgeResult SqliteArchivePackageService::PurgeWorkflowArchiveSource(
    const ArchiveWorkflowSelection& selection,
    std::int64_t archive_package_id,
    std::string* error_out) {
    WorkflowArchivePurgeResult result{};
    if (execution_db_ == nullptr) {
        result.error = "execution db is null";
        if (error_out) *error_out = *result.error;
        return result;
    }
    if (archive_package_id <= 0) {
        result.error = "archive_package_id must be positive";
        if (error_out) *error_out = *result.error;
        return result;
    }

    const auto workflow_ids = NormalizeWorkflowSelection(selection);
    if (workflow_ids.empty()) {
        result.error = "workflow selection is empty";
        if (error_out) *error_out = *result.error;
        return result;
    }
    std::string final_selection_error;
    if (!ValidateFinalWorkflowSelection(execution_db_, workflow_ids, &final_selection_error)) {
        result.error = final_selection_error;
        if (error_out) *error_out = final_selection_error;
        return result;
    }
    const auto workflow_id_list = JoinIds(workflow_ids);

    std::string query_error;
    const auto scoped = WorkflowJobSetCte(workflow_id_list);
    if (!CollectArchiveReadinessBlockers(
            execution_db_,
            scoped,
            &result.blockers,
            &query_error)) {
        result.error = query_error;
        if (error_out) *error_out = query_error;
        return result;
    }
    if (!result.blockers.empty()) {
        return result;
    }
    const auto job_ids = QueryInt64Column(execution_db_, scoped + "SELECT job_id FROM scoped_jobs ORDER BY job_id ASC;", &query_error);
    if (!query_error.empty()) {
        result.error = query_error;
        if (error_out) *error_out = query_error;
        return result;
    }
    const auto job_set_ids = QueryInt64Column(execution_db_, scoped + "SELECT job_set_id FROM scoped_job_sets ORDER BY job_set_id ASC;", &query_error);
    if (!query_error.empty()) {
        result.error = query_error;
        if (error_out) *error_out = query_error;
        return result;
    }

    std::vector<std::int64_t> battle_set_ids;
    if (analysis_db_ != nullptr) {
        (void)BuildWorkflowAnalysisSpecs(execution_db_, analysis_db_, job_ids, workflow_ids, &battle_set_ids);
    }
    const auto candidate_probe_run_ids = CollectWorkflowSeedProbeRunIds(
        execution_db_, analysis_db_, workflow_ids, job_ids, job_set_ids);
    auto savestate_ids = CollectWorkflowSavestateIds(
        execution_db_, analysis_db_, workflow_ids, job_ids, battle_set_ids);
    if (analysis_db_ != nullptr && !candidate_probe_run_ids.empty()
        && IsTablePresent(analysis_db_, "sp_probe_run", nullptr)) {
        auto probe_entry_states = QueryInt64Column(
            analysis_db_,
            "SELECT entry_savestate_id FROM sp_probe_run WHERE probe_run_id IN ("
                + JoinIds(candidate_probe_run_ids) + ") AND entry_savestate_id IS NOT NULL;",
            &query_error);
        if (!query_error.empty()) {
            result.error = query_error;
            if (error_out) *error_out = query_error;
            return result;
        }
        savestate_ids.insert(savestate_ids.end(), probe_entry_states.begin(), probe_entry_states.end());
        std::sort(savestate_ids.begin(), savestate_ids.end());
        savestate_ids.erase(std::unique(savestate_ids.begin(), savestate_ids.end()), savestate_ids.end());
    }
    const auto exclusive_savestate_ids = FilterExclusiveSavestateIds(
        execution_db_,
        analysis_db_,
        state_db_,
        workflow_ids,
        job_ids,
        battle_set_ids,
        savestate_ids);
    const auto exclusive_probe_run_ids = FilterExclusiveWorkflowSeedProbeRunIds(
        execution_db_,
        analysis_db_,
        state_db_,
        workflow_ids,
        job_ids,
        job_set_ids,
        battle_set_ids,
        exclusive_savestate_ids,
        candidate_probe_run_ids);
    const auto aggregate_artifact_ids = CollectWorkflowAggregateArtifactIds(analysis_db_, workflow_ids);
    const auto exclusive_aggregate_artifact_ids = FilterExclusiveAggregateArtifactIds(
        analysis_db_, state_db_, workflow_ids, job_ids, aggregate_artifact_ids);

    std::vector<std::int64_t> selected_completion_ids;
    std::vector<std::int64_t> selected_recording_ids;
    std::vector<std::int64_t> selected_replay_ids;
    std::vector<std::int64_t> exclusive_probe_result_ids;
    std::vector<std::int64_t> exclusive_encounter_projection_ids;
    std::vector<std::int64_t> exclusive_probe_set_ids;
    std::vector<std::int64_t> exclusive_input_set_ids;
    std::vector<std::int64_t> exclusive_input_frame_ids;
    std::vector<std::int64_t> exclusive_axis_ids;
    if (analysis_db_ != nullptr) {
        if (IsTablePresent(analysis_db_, "ab_battle_completion", nullptr)) {
            selected_completion_ids = QueryInt64Column(
                analysis_db_,
                "SELECT battle_completion_id FROM ab_battle_completion WHERE workflow_instance_id IN ("
                    + workflow_id_list + ");",
                &query_error);
        }
        if (query_error.empty() && IsTablePresent(analysis_db_, "ab_battle_recording", nullptr)) {
            selected_recording_ids = QueryInt64Column(
                analysis_db_,
                "SELECT battle_recording_id FROM ab_battle_recording WHERE workflow_instance_id IN ("
                    + workflow_id_list + ");",
                &query_error);
        }
        if (query_error.empty() && IsTablePresent(analysis_db_, "ab_battle_replay", nullptr)) {
            selected_replay_ids = QueryInt64Column(
                analysis_db_,
                "SELECT battle_replay_id FROM ab_battle_replay WHERE workflow_instance_id IN ("
                    + workflow_id_list + ");",
                &query_error);
        }
        if (query_error.empty() && !exclusive_probe_run_ids.empty()
            && IsTablePresent(analysis_db_, "sp_probe_run", nullptr)) {
            const auto runs = JoinIds(exclusive_probe_run_ids);
            exclusive_probe_set_ids = QueryInt64Column(
                analysis_db_,
                "SELECT probe_set_id FROM sp_probe_run WHERE probe_run_id IN (" + runs + ");",
                &query_error);
            if (query_error.empty()) {
                exclusive_input_set_ids = QueryInt64Column(
                    analysis_db_,
                    "SELECT accepted_input_set_id FROM sp_probe_run WHERE probe_run_id IN (" + runs + ");",
                    &query_error);
            }
            if (query_error.empty() && IsTablePresent(analysis_db_, "sp_probe_result", nullptr)) {
                exclusive_probe_result_ids = QueryInt64Column(
                    analysis_db_,
                    "SELECT probe_result_id FROM sp_probe_result WHERE probe_run_id IN (" + runs + ");",
                    &query_error);
            }
        }
        if (query_error.empty() && !exclusive_probe_result_ids.empty()) {
            const auto results = JoinIds(exclusive_probe_result_ids);
            auto frames = QueryInt64Column(
                analysis_db_,
                "SELECT input_frame_id FROM sp_probe_result WHERE probe_result_id IN (" + results + ");",
                &query_error);
            exclusive_input_frame_ids.insert(
                exclusive_input_frame_ids.end(), frames.begin(), frames.end());
        }
        if (query_error.empty() && !exclusive_probe_run_ids.empty()
            && IsTablePresent(analysis_db_, "sp_encounter_projection", nullptr)) {
            exclusive_encounter_projection_ids = QueryInt64Column(
                analysis_db_,
                "SELECT encounter_projection_id FROM sp_encounter_projection WHERE probe_run_id IN ("
                    + JoinIds(exclusive_probe_run_ids) + ");",
                &query_error);
        }
        if (query_error.empty() && !exclusive_input_set_ids.empty()
            && IsTablePresent(analysis_db_, "an_input_set_frame", nullptr)) {
            auto frames = QueryInt64Column(
                analysis_db_,
                "SELECT input_frame_id FROM an_input_set_frame WHERE input_set_id IN ("
                    + JoinIds(exclusive_input_set_ids) + ");",
                &query_error);
            exclusive_input_frame_ids.insert(
                exclusive_input_frame_ids.end(), frames.begin(), frames.end());
        }
        std::sort(exclusive_input_frame_ids.begin(), exclusive_input_frame_ids.end());
        exclusive_input_frame_ids.erase(
            std::unique(exclusive_input_frame_ids.begin(), exclusive_input_frame_ids.end()),
            exclusive_input_frame_ids.end());
        if (query_error.empty() && !exclusive_input_frame_ids.empty()
            && IsTablePresent(analysis_db_, "sp_input_frame", nullptr)) {
            auto axes = QueryInt64Column(
                analysis_db_,
                "SELECT main_axis_xy_id FROM sp_input_frame WHERE input_frame_id IN ("
                    + JoinIds(exclusive_input_frame_ids) + ") UNION "
                    "SELECT cstick_axis_xy_id FROM sp_input_frame WHERE input_frame_id IN ("
                    + JoinIds(exclusive_input_frame_ids) + ") UNION "
                    "SELECT trigger_axis_xy_id FROM sp_input_frame WHERE input_frame_id IN ("
                    + JoinIds(exclusive_input_frame_ids) + ");",
                &query_error);
            exclusive_axis_ids.insert(exclusive_axis_ids.end(), axes.begin(), axes.end());
        }
        std::sort(exclusive_axis_ids.begin(), exclusive_axis_ids.end());
        exclusive_axis_ids.erase(
            std::unique(exclusive_axis_ids.begin(), exclusive_axis_ids.end()),
            exclusive_axis_ids.end());
        if (!query_error.empty()) {
            result.error = query_error;
            if (error_out) *error_out = query_error;
            return result;
        }
    }

    const auto has_external_aggregate_reference = [&](const std::vector<std::int64_t>& aggregate_ids,
                                                       const std::string& ref_kinds) {
        if (aggregate_ids.empty()) return false;
        const auto ids = JoinIds(aggregate_ids);
        const auto jobs = job_ids.empty() ? std::string("0") : JoinIds(job_ids);
        const auto job_sets = job_set_ids.empty() ? std::string("0") : JoinIds(job_set_ids);
        const auto count = [&](const std::string& sql) {
            if (!query_error.empty()) return std::int64_t{0};
            return QuerySingleInt64(execution_db_, sql, &query_error);
        };
        bool referenced = count(
            "SELECT COUNT(1) FROM exec_job WHERE job_id NOT IN (" + jobs
                + ") AND program_ref_kind IN (" + ref_kinds + ") AND program_ref_id IN (" + ids + ");") > 0;
        referenced = referenced || count(
            "SELECT COUNT(1) FROM exec_job_set WHERE job_set_id NOT IN (" + job_sets
                + ") AND domain_ref_kind IN (" + ref_kinds + ") AND domain_ref_id IN (" + ids + ");") > 0;
        referenced = referenced || count(
            "SELECT COUNT(1) FROM exec_workflow_step WHERE workflow_instance_id NOT IN (" + workflow_id_list
                + ") AND ((input_ref_kind IN (" + ref_kinds + ") AND input_ref_id IN (" + ids + ")) OR "
                "(output_ref_kind IN (" + ref_kinds + ") AND output_ref_id IN (" + ids + ")));" ) > 0;
        if (IsTablePresent(execution_db_, "exec_workflow_instance_input_binding", nullptr)) {
            referenced = referenced || count(
                "SELECT COUNT(1) FROM exec_workflow_instance_input_binding WHERE workflow_instance_id NOT IN ("
                    + workflow_id_list + ") AND ref_kind IN (" + ref_kinds + ") AND ref_id IN (" + ids + ");") > 0;
        }
        if (IsTablePresent(execution_db_, "exec_workflow_step_output", nullptr)) {
            referenced = referenced || count(
                "SELECT COUNT(1) FROM exec_workflow_step_output WHERE workflow_instance_id NOT IN ("
                    + workflow_id_list + ") AND ref_kind IN (" + ref_kinds + ") AND ref_id IN (" + ids + ");") > 0;
        }
        if (IsTablePresent(execution_db_, "exec_job_output", nullptr)) {
            referenced = referenced || count(
                "SELECT COUNT(1) FROM exec_job_output WHERE job_id NOT IN (" + jobs
                    + ") AND ref_kind IN (" + ref_kinds + ") AND ref_id IN (" + ids + ");") > 0;
        }
        if (IsTablePresent(execution_db_, "exec_workflow_event", nullptr)) {
            referenced = referenced || count(
                "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_instance_id NOT IN ("
                    + workflow_id_list + ") AND detail_ref_kind IN (" + ref_kinds
                    + ") AND detail_ref_id IN (" + ids + ");") > 0;
        }
        return referenced;
    };
    const auto completion_ref_kinds =
        "'analysis_battle.battle_completion_id','analysis_battle.battle_completion',"
        "'analysisbattle.battle_completion','ab_battle_completion'";
    const auto recording_ref_kinds =
        "'analysis_battle.battle_recording_id','analysis_battle.battle_recording',"
        "'analysisbattle.battle_recording','ab_battle_recording'";
    const auto replay_ref_kinds =
        "'analysis_battle.battle_replay_id','analysis_battle.battle_replay',"
        "'analysisbattle.battle_replay','ab_battle_replay'";
    bool external_completion_reference = has_external_aggregate_reference(
        selected_completion_ids, completion_ref_kinds);
    if (!external_completion_reference && analysis_db_ != nullptr
        && !selected_completion_ids.empty()
        && IsTablePresent(analysis_db_, "ab_battle_recording", nullptr)) {
        external_completion_reference = QuerySingleInt64(
            analysis_db_,
            "SELECT COUNT(1) FROM ab_battle_recording WHERE workflow_instance_id NOT IN ("
                + workflow_id_list + ") AND battle_completion_id IN ("
                + JoinIds(selected_completion_ids) + ");",
            &query_error) > 0;
    }
    if (!external_completion_reference && analysis_db_ != nullptr
        && !selected_completion_ids.empty()
        && IsTablePresent(analysis_db_, "ab_battle_replay", nullptr)) {
        external_completion_reference = QuerySingleInt64(
            analysis_db_,
            "SELECT COUNT(1) FROM ab_battle_replay WHERE workflow_instance_id NOT IN ("
                + workflow_id_list + ") AND battle_completion_id IN ("
                + JoinIds(selected_completion_ids) + ");",
            &query_error) > 0;
    }
    const bool external_recording_reference = has_external_aggregate_reference(
        selected_recording_ids, recording_ref_kinds);
    const bool external_replay_reference = has_external_aggregate_reference(
        selected_replay_ids, replay_ref_kinds);
    if (!query_error.empty()) {
        result.error = query_error;
        if (error_out) *error_out = query_error;
        return result;
    }
    if (external_completion_reference || external_recording_reference
        || external_replay_reference) {
        result.blockers.push_back(
            "selected battle-end aggregate is referenced by an unselected workflow or execution record");
        return result;
    }

    std::vector<std::filesystem::path> savestate_files_to_delete;
    std::vector<std::int64_t> exclusive_artifact_ids;
    if (state_db_ != nullptr && !exclusive_savestate_ids.empty()) {
        const auto rows = LoadSavestateRows(state_db_, exclusive_savestate_ids, &query_error);
        if (!query_error.empty()) {
            result.error = query_error;
            if (error_out) *error_out = query_error;
            return result;
        }
        for (const auto& row : rows) {
            exclusive_artifact_ids.push_back(row.artifact_id);
            if (!row.filename.empty()) {
                const auto path = ResolveArchiveArtifactPath(
                    config_paths_, row, &query_error);
                if (!path) {
                    result.error = query_error;
                    if (error_out) *error_out = query_error;
                    return result;
                }
                savestate_files_to_delete.emplace_back(*path);
            }
        }
        std::sort(exclusive_artifact_ids.begin(), exclusive_artifact_ids.end());
        exclusive_artifact_ids.erase(std::unique(exclusive_artifact_ids.begin(), exclusive_artifact_ids.end()), exclusive_artifact_ids.end());
        std::sort(savestate_files_to_delete.begin(), savestate_files_to_delete.end());
        savestate_files_to_delete.erase(std::unique(savestate_files_to_delete.begin(), savestate_files_to_delete.end()), savestate_files_to_delete.end());
    }
    if (state_db_ != nullptr && !exclusive_aggregate_artifact_ids.empty()) {
        const auto rows = LoadStandaloneArtifactRows(state_db_, exclusive_aggregate_artifact_ids, &query_error);
        if (!query_error.empty()) {
            result.error = query_error;
            if (error_out) *error_out = query_error;
            return result;
        }
        for (const auto& row : rows) {
            exclusive_artifact_ids.push_back(row.artifact_id);
            if (!row.filename.empty()) {
                const auto path = ResolveArchiveArtifactPath(
                    config_paths_, row, &query_error);
                if (!path) {
                    result.error = query_error;
                    if (error_out) *error_out = query_error;
                    return result;
                }
                savestate_files_to_delete.emplace_back(*path);
            }
        }
        std::sort(exclusive_artifact_ids.begin(), exclusive_artifact_ids.end());
        exclusive_artifact_ids.erase(
            std::unique(exclusive_artifact_ids.begin(), exclusive_artifact_ids.end()),
            exclusive_artifact_ids.end());
        std::sort(savestate_files_to_delete.begin(), savestate_files_to_delete.end());
        savestate_files_to_delete.erase(
            std::unique(savestate_files_to_delete.begin(), savestate_files_to_delete.end()),
            savestate_files_to_delete.end());
    }

    auto fail = [&](std::string message) {
        result.error = std::move(message);
        if (error_out) *error_out = *result.error;
        return result;
    };

    if (analysis_db_ != nullptr && (!job_ids.empty() || !workflow_ids.empty())) {
        const auto job_id_list = job_ids.empty() ? std::string("0") : JoinIds(job_ids);
        if (!ExecuteSql(analysis_db_, "BEGIN IMMEDIATE;", &query_error)) return fail(query_error);
        bool ok = true;
        int deleted = 0;
        auto del = [&](const std::string& sql) {
            if (!ok) return;
            std::string err;
            const int rows = ExecuteDeleteSql(analysis_db_, sql, &err);
            if (rows < 0) {
                ok = false;
                query_error = err;
            } else {
                deleted += rows;
            }
        };
        if (IsTablePresent(analysis_db_, "ab_outbox_message", nullptr)) {
            if (IsTablePresent(analysis_db_, "ab_battle_replay", nullptr)) {
                del("DELETE FROM ab_outbox_message WHERE aggregate_kind='battle_replay' "
                    "AND CAST(aggregate_id AS INTEGER) IN (SELECT battle_replay_id FROM ab_battle_replay WHERE workflow_instance_id IN (" + workflow_id_list + "));" );
            }
            if (!selected_recording_ids.empty()) {
                del("DELETE FROM ab_outbox_message WHERE aggregate_kind='battle_recording' "
                    "AND CAST(aggregate_id AS INTEGER) IN (" + JoinIds(selected_recording_ids) + ");");
            }
            if (!selected_completion_ids.empty()) {
                del("DELETE FROM ab_outbox_message WHERE aggregate_kind='battle_completion' "
                    "AND CAST(aggregate_id AS INTEGER) IN (" + JoinIds(selected_completion_ids) + ");");
            }
        }
        if (IsTablePresent(analysis_db_, "ab_battle_replay", nullptr)) {
            del("DELETE FROM ab_battle_replay WHERE workflow_instance_id IN (" + workflow_id_list + ");");
        }
        if (IsTablePresent(analysis_db_, "ab_battle_recording", nullptr)) {
            del("DELETE FROM ab_battle_recording WHERE workflow_instance_id IN (" + workflow_id_list + ");");
        }
        if (IsTablePresent(analysis_db_, "ab_battle_completion", nullptr)) {
            del("DELETE FROM ab_battle_completion WHERE workflow_instance_id IN (" + workflow_id_list + ");");
        }
        if (IsTablePresent(analysis_db_, "ab_terminal_followup", nullptr)) {
            del("DELETE FROM ab_terminal_followup WHERE turn_job_id IN (SELECT turn_job_id FROM ab_turn_job WHERE exec_job_id IN (" + job_id_list + "));");
        }
        if (IsTablePresent(analysis_db_, "ab_selection_decision", nullptr)) {
            del("DELETE FROM ab_selection_decision WHERE turn_job_id IN (SELECT turn_job_id FROM ab_turn_job WHERE exec_job_id IN (" + job_id_list + "));");
        }
        if (IsTablePresent(analysis_db_, "ab_turn_job", nullptr)) {
            del("DELETE FROM ab_turn_job WHERE exec_job_id IN (" + job_id_list + ");");
        }
        if (IsTablePresent(analysis_db_, "ab_battle_context_probe", nullptr)) {
            del("DELETE FROM ab_battle_context_probe WHERE exec_job_id IN (" + job_id_list + ");");
        }
        if (!battle_set_ids.empty() && IsTablePresent(analysis_db_, "ab_turn_wave", nullptr)) {
            const auto battle_id_list = JoinIds(battle_set_ids);
            if (IsTablePresent(analysis_db_, "ab_predicate_execution_package_v1", nullptr)) {
                del("DELETE FROM ab_predicate_execution_package_v1 WHERE wave_id IN ("
                    "SELECT wave_id FROM ab_turn_wave WHERE battle_set_id IN (" + battle_id_list + ") "
                    "AND wave_id NOT IN (SELECT DISTINCT wave_id FROM ab_turn_job WHERE wave_id IS NOT NULL) "
                    "AND wave_id NOT IN (SELECT DISTINCT wave_id FROM ab_battle_context_probe WHERE wave_id IS NOT NULL));");
            }
            del("DELETE FROM ab_turn_wave WHERE battle_set_id IN (" + battle_id_list + ") "
                "AND wave_id NOT IN (SELECT DISTINCT wave_id FROM ab_turn_job WHERE wave_id IS NOT NULL) "
                "AND wave_id NOT IN (SELECT DISTINCT wave_id FROM ab_battle_context_probe WHERE wave_id IS NOT NULL);");
            if (IsTablePresent(analysis_db_, "ab_selection_pool", nullptr)) {
                del("DELETE FROM ab_selection_pool WHERE battle_set_id IN (" + battle_id_list + ") "
                    "AND battle_set_id NOT IN (SELECT DISTINCT battle_set_id FROM ab_turn_wave WHERE battle_set_id IS NOT NULL);");
            }
            if (IsTablePresent(analysis_db_, "ab_seed_candidate", nullptr)) {
                del("DELETE FROM ab_seed_candidate WHERE battle_set_id IN (" + battle_id_list + ") "
                    "AND battle_set_id NOT IN (SELECT DISTINCT battle_set_id FROM ab_turn_wave WHERE battle_set_id IS NOT NULL);");
            }
            if (IsTablePresent(analysis_db_, "ab_battle_set", nullptr)) {
                del("DELETE FROM ab_battle_set WHERE battle_set_id IN (" + battle_id_list + ") "
                    "AND battle_set_id NOT IN (SELECT DISTINCT battle_set_id FROM ab_turn_wave WHERE battle_set_id IS NOT NULL);");
            }
        }
        if (!exclusive_probe_run_ids.empty() && IsTablePresent(analysis_db_, "sp_probe_run", nullptr)) {
            const auto run_id_list = JoinIds(exclusive_probe_run_ids);
            if (IsTablePresent(analysis_db_, "sp_outbox_message", nullptr)) {
                del("DELETE FROM sp_outbox_message WHERE aggregate_kind='probe_run' "
                    "AND CAST(aggregate_id AS INTEGER) IN (" + run_id_list + ");");
            }
            if (IsTablePresent(analysis_db_, "sp_encounter_projection", nullptr)) {
                del("DELETE FROM sp_encounter_projection WHERE probe_run_id IN (" + run_id_list + ");");
            }
            if (IsTablePresent(analysis_db_, "sp_probe_result", nullptr)) {
                del("DELETE FROM sp_probe_result WHERE probe_run_id IN (" + run_id_list + ");");
            }
            del("DELETE FROM sp_probe_run WHERE probe_run_id IN (" + run_id_list + ");");

            if (!exclusive_input_set_ids.empty()
                && IsTablePresent(analysis_db_, "an_input_set_frame", nullptr)) {
                const auto input_set_id_list = JoinIds(exclusive_input_set_ids);
                del("DELETE FROM an_input_set_frame WHERE input_set_id IN (" + input_set_id_list + ") "
                    "AND input_set_id NOT IN (SELECT DISTINCT accepted_input_set_id FROM sp_probe_run);");
                if (IsTablePresent(analysis_db_, "an_input_set", nullptr)) {
                    del("DELETE FROM an_input_set WHERE input_set_id IN (" + input_set_id_list + ") "
                        "AND input_set_id NOT IN (SELECT DISTINCT accepted_input_set_id FROM sp_probe_run);");
                }
            }
            if (!exclusive_input_frame_ids.empty()
                && IsTablePresent(analysis_db_, "sp_input_frame", nullptr)) {
                std::string retained_candidate_clause;
                if (IsTablePresent(analysis_db_, "ab_seed_candidate", nullptr)) {
                    retained_candidate_clause =
                        "AND input_frame_id NOT IN (SELECT DISTINCT source_input_frame_id FROM ab_seed_candidate "
                        "WHERE source_input_frame_id IS NOT NULL) ";
                }
                del("DELETE FROM sp_input_frame WHERE input_frame_id IN ("
                    + JoinIds(exclusive_input_frame_ids) + ") "
                    "AND input_frame_id NOT IN (SELECT DISTINCT input_frame_id FROM sp_probe_result) "
                    "AND input_frame_id NOT IN (SELECT DISTINCT input_frame_id FROM an_input_set_frame) "
                    + retained_candidate_clause + ";");
            }
            if (!exclusive_axis_ids.empty() && IsTablePresent(analysis_db_, "sp_axis_xy", nullptr)) {
                del("DELETE FROM sp_axis_xy WHERE axis_xy_id IN (" + JoinIds(exclusive_axis_ids) + ") "
                    "AND axis_xy_id NOT IN (SELECT main_axis_xy_id FROM sp_input_frame "
                    "UNION SELECT cstick_axis_xy_id FROM sp_input_frame "
                    "UNION SELECT trigger_axis_xy_id FROM sp_input_frame);");
            }
            if (!exclusive_probe_set_ids.empty() && IsTablePresent(analysis_db_, "sp_probe_set", nullptr)) {
                const auto probe_set_id_list = JoinIds(exclusive_probe_set_ids);
                del("DELETE FROM sp_probe_set WHERE probe_set_id IN (" + probe_set_id_list + ") "
                    "AND probe_set_id NOT IN (SELECT DISTINCT probe_set_id FROM sp_probe_run);");
                if (IsTablePresent(analysis_db_, "sp_outbox_message", nullptr)) {
                    del("DELETE FROM sp_outbox_message WHERE aggregate_kind='probe_set' "
                        "AND CAST(aggregate_id AS INTEGER) IN (" + probe_set_id_list + ") "
                        "AND CAST(aggregate_id AS INTEGER) NOT IN (SELECT probe_set_id FROM sp_probe_set);");
                }
            }
        }
        if (ok) {
            if (!ExecuteSql(analysis_db_, "COMMIT;", &query_error)) return fail(query_error);
            result.analysis_rows_deleted = deleted;
        } else {
            (void)ExecuteSql(analysis_db_, "ROLLBACK;", nullptr);
            return fail(query_error);
        }
    }

    if (ui_read_sqlite_db_ != nullptr) {
        if (!ExecuteSql(ui_read_sqlite_db_, "BEGIN IMMEDIATE;", &query_error)) return fail(query_error);
        bool ok = true;
        int deleted = 0;
        auto del = [&](const std::string& sql) {
            if (!ok) return;
            std::string err;
            const int rows = ExecuteDeleteSql(ui_read_sqlite_db_, sql, &err);
            if (rows < 0) {
                ok = false;
                query_error = err;
            } else {
                deleted += rows;
            }
        };
        if (!job_ids.empty() && IsTablePresent(ui_read_sqlite_db_, "ui_battle_turn_job_replication", nullptr)) {
            del("DELETE FROM ui_battle_turn_job_replication WHERE exec_job_id IN (" + JoinIds(job_ids) + ");");
        }
        if (IsTablePresent(ui_read_sqlite_db_, "ui_workflow_alert", nullptr)) {
            del("DELETE FROM ui_workflow_alert WHERE workflow_instance_id IN (" + workflow_id_list + ");");
        }
        if (IsTablePresent(ui_read_sqlite_db_, "ui_workflow_edge", nullptr)) {
            del("DELETE FROM ui_workflow_edge WHERE workflow_instance_id IN (" + workflow_id_list + ");");
        }
        if (IsTablePresent(ui_read_sqlite_db_, "ui_workflow_step", nullptr)) {
            del("DELETE FROM ui_workflow_step WHERE workflow_instance_id IN (" + workflow_id_list + ");");
        }
        if (IsTablePresent(ui_read_sqlite_db_, "ui_workflow_instance", nullptr)) {
            del("DELETE FROM ui_workflow_instance WHERE workflow_instance_id IN (" + workflow_id_list + ");");
        }
        if (ok) {
            if (!ExecuteSql(ui_read_sqlite_db_, "COMMIT;", &query_error)) return fail(query_error);
            result.ui_read_rows_deleted = deleted;
        } else {
            (void)ExecuteSql(ui_read_sqlite_db_, "ROLLBACK;", nullptr);
            return fail(query_error);
        }
    }

    if (!ExecuteSql(execution_db_, "BEGIN IMMEDIATE;", &query_error)) return fail(query_error);
    std::vector<std::string> final_readiness_blockers;
    if (!CollectArchiveReadinessBlockers(
            execution_db_, scoped, &final_readiness_blockers, &query_error)) {
        (void)ExecuteSql(execution_db_, "ROLLBACK;", nullptr);
        return fail(query_error);
    }
    if (!final_readiness_blockers.empty()) {
        (void)ExecuteSql(execution_db_, "ROLLBACK;", nullptr);
        result.blockers.insert(
            result.blockers.end(),
            final_readiness_blockers.begin(),
            final_readiness_blockers.end());
        return fail("workflow source changed after archive package creation");
    }
    bool exec_ok = true;
    int exec_deleted = 0;
    auto exec_del = [&](const std::string& sql) {
        if (!exec_ok) return;
        std::string err;
        const int rows = ExecuteDeleteSql(execution_db_, sql, &err);
        if (rows < 0) {
            exec_ok = false;
            query_error = err;
        } else {
            exec_deleted += rows;
        }
    };
    const auto job_id_list_for_delete = job_ids.empty() ? std::string("0") : JoinIds(job_ids);
    const auto job_set_id_list_for_delete = job_set_ids.empty() ? std::string("0") : JoinIds(job_set_ids);
    exec_del("DELETE FROM exec_workflow_event WHERE workflow_instance_id IN (" + workflow_id_list + ");");
    exec_del("DELETE FROM exec_workflow_edge WHERE workflow_instance_id IN (" + workflow_id_list + ");");
    if (IsTablePresent(execution_db_, "exec_workflow_unit_activation_edge", nullptr)) {
        exec_del("DELETE FROM exec_workflow_unit_activation_edge WHERE workflow_instance_id IN (" + workflow_id_list + ");");
    }
    if (IsTablePresent(execution_db_, "exec_workflow_step_output", nullptr)) {
        exec_del("DELETE FROM exec_workflow_step_output WHERE workflow_step_id IN (SELECT workflow_step_id FROM exec_workflow_step WHERE workflow_instance_id IN (" + workflow_id_list + "));");
    }
    if (IsTablePresent(execution_db_, "exec_workflow_instance_input_binding", nullptr)) {
        exec_del("DELETE FROM exec_workflow_instance_input_binding WHERE workflow_instance_id IN (" + workflow_id_list + ");");
    }
    if (IsTablePresent(execution_db_, "exec_workflow_instance_argument", nullptr)) {
        exec_del("DELETE FROM exec_workflow_instance_argument WHERE workflow_instance_id IN (" + workflow_id_list + ");");
    }
    if (IsTablePresent(execution_db_, "exec_workflow_unit_activation", nullptr)) {
        exec_del("UPDATE exec_workflow_step SET workflow_unit_activation_id=NULL WHERE workflow_instance_id IN (" + workflow_id_list + ");");
        exec_del("DELETE FROM exec_workflow_unit_activation WHERE workflow_instance_id IN (" + workflow_id_list + ");");
    }
    exec_del("DELETE FROM exec_workflow_step WHERE workflow_instance_id IN (" + workflow_id_list + ");");
    exec_del("DELETE FROM exec_workflow_instance WHERE workflow_instance_id IN (" + workflow_id_list + ");");
    exec_del("DELETE FROM exec_job_progress WHERE job_id IN (" + job_id_list_for_delete + ");");
    exec_del("DELETE FROM exec_job_event WHERE job_id IN (" + job_id_list_for_delete + ");");
    exec_del("DELETE FROM exec_job_cancellation_request WHERE job_id IN (" + job_id_list_for_delete + ");");
    if (IsTablePresent(execution_db_, "exec_trigger", nullptr)) {
        exec_del("DELETE FROM exec_trigger WHERE (scope_kind='job_set' AND scope_id IN (" + job_set_id_list_for_delete + ")) OR (scope_kind='job' AND scope_id IN (" + job_id_list_for_delete + "));");
    }
    if (IsTablePresent(execution_db_, "exec_outbox_message", nullptr)) {
        exec_del(
            "DELETE FROM exec_outbox_message WHERE "
            "(aggregate_kind='job_set' AND CAST(aggregate_id AS INTEGER) IN (" + job_set_id_list_for_delete + ")) "
            "OR (aggregate_kind='job' AND CAST(aggregate_id AS INTEGER) IN (" + job_id_list_for_delete + ")) "
            "OR (aggregate_kind='workset' AND CAST(aggregate_id AS INTEGER) IN (SELECT workset_id FROM exec_workset WHERE job_set_id IN (" + job_set_id_list_for_delete + "))) "
            "OR (aggregate_kind='workset_dispatch_attempt' AND CAST(aggregate_id AS INTEGER) IN (SELECT a.dispatch_attempt_id FROM exec_workset_dispatch_attempt a JOIN exec_workset w ON w.workset_id=a.workset_id WHERE w.job_set_id IN (" + job_set_id_list_for_delete + ")));");
    }
    exec_del("DELETE FROM exec_job WHERE job_id IN (" + job_id_list_for_delete + ");");
    exec_del("DELETE FROM exec_workset_dispatch_attempt WHERE workset_id IN (SELECT workset_id FROM exec_workset WHERE job_set_id IN (" + job_set_id_list_for_delete + "));");
    exec_del("DELETE FROM exec_workset WHERE job_set_id IN (" + job_set_id_list_for_delete + ");");
    exec_del("DELETE FROM exec_job_set WHERE job_set_id IN (" + job_set_id_list_for_delete + ");");
    if (exec_ok) {
        const auto remaining = QuerySingleInt64(
            execution_db_,
            "SELECT "
            "(SELECT COUNT(1) FROM exec_job_progress WHERE job_id IN (" + job_id_list_for_delete + ")) + "
            "(SELECT COUNT(1) FROM exec_job_cancellation_request WHERE job_id IN (" + job_id_list_for_delete + ")) + "
            "(SELECT COUNT(1) FROM exec_job_event WHERE job_id IN (" + job_id_list_for_delete + ")) + "
            "(SELECT COUNT(1) FROM exec_job WHERE job_id IN (" + job_id_list_for_delete + ")) + "
            "(SELECT COUNT(1) FROM exec_workset_dispatch_attempt WHERE workset_id IN (SELECT workset_id FROM exec_workset WHERE job_set_id IN (" + job_set_id_list_for_delete + "))) + "
            "(SELECT COUNT(1) FROM exec_workset WHERE job_set_id IN (" + job_set_id_list_for_delete + ")) + "
            "(SELECT COUNT(1) FROM exec_job_set WHERE job_set_id IN (" + job_set_id_list_for_delete + "));",
            &query_error);
        if (!query_error.empty() || remaining != 0) {
            exec_ok = false;
            if (query_error.empty()) {
                query_error = "execution coordination rows remain after archive purge";
            }
        }
    }
    if (exec_ok) {
        if (!ExecuteSql(execution_db_, "COMMIT;", &query_error)) return fail(query_error);
        result.workflow_rows_deleted = static_cast<int>(workflow_ids.size());
        result.execution_rows_deleted = exec_deleted;
    } else {
        (void)ExecuteSql(execution_db_, "ROLLBACK;", nullptr);
        return fail(query_error);
    }

    if (state_db_ != nullptr
        && (!exclusive_savestate_ids.empty()
            || !exclusive_artifact_ids.empty()
            || !selected_completion_ids.empty()
            || !selected_recording_ids.empty()
            || !selected_replay_ids.empty()
            || !exclusive_probe_result_ids.empty())) {
        if (!ExecuteSql(state_db_, "BEGIN IMMEDIATE;", &query_error)) return fail(query_error);
        bool state_ok = true;
        int sav_deleted = 0;
        int artifact_deleted = 0;
        auto state_del = [&](const std::string& sql, int* accumulator) {
            if (!state_ok) return;
            std::string err;
            const int rows = ExecuteDeleteSql(state_db_, sql, &err);
            if (rows < 0) {
                state_ok = false;
                query_error = err;
            } else if (accumulator != nullptr) {
                *accumulator += rows;
            }
        };
        if (!selected_completion_ids.empty()) {
            state_del(
                "DELETE FROM state_savestate_derivation WHERE source_context_id IN ("
                    + JoinIds(selected_completion_ids) + ") AND source_context_kind IN ("
                    "'analysis_battle.battle_completion_id','analysis_battle.battle_completion',"
                    "'analysisbattle.battle_completion','ab_battle_completion');",
                nullptr);
        }
        if (!selected_recording_ids.empty()) {
            state_del(
                "DELETE FROM state_savestate_derivation WHERE source_context_id IN ("
                    + JoinIds(selected_recording_ids) + ") AND source_context_kind IN ("
                    "'analysis_battle.battle_recording_id','analysis_battle.battle_recording',"
                    "'analysisbattle.battle_recording','ab_battle_recording');",
                nullptr);
        }
        if (!exclusive_probe_result_ids.empty()) {
            state_del(
                "DELETE FROM state_savestate_derivation WHERE source_context_kind="
                    "'analysisseedprobe.confirmed_result' AND source_context_id IN ("
                    + JoinIds(exclusive_probe_result_ids) + ");",
                nullptr);
        }
        if (!selected_replay_ids.empty()) {
            state_del(
                "DELETE FROM state_savestate_derivation WHERE source_context_id IN ("
                    + JoinIds(selected_replay_ids) + ") AND source_context_kind IN ("
                    "'analysis_battle.battle_replay_id','analysis_battle.battle_replay',"
                    "'analysisbattle.battle_replay','ab_battle_replay');",
                nullptr);
        }
        if (!exclusive_savestate_ids.empty()) {
            const auto sav_id_list = JoinIds(exclusive_savestate_ids);
            state_del("DELETE FROM state_savestate_derivation WHERE from_savestate_id IN (" + sav_id_list + ") OR to_savestate_id IN (" + sav_id_list + ");", nullptr);
            state_del("DELETE FROM state_savestate WHERE savestate_id IN (" + sav_id_list + ");", &sav_deleted);
        }
        if (!exclusive_artifact_ids.empty()) {
            state_del("DELETE FROM state_artifact WHERE artifact_id IN (" + JoinIds(exclusive_artifact_ids) + ") "
                "AND artifact_id NOT IN (SELECT DISTINCT artifact_id FROM state_savestate WHERE artifact_id IS NOT NULL);",
                &artifact_deleted);
        }
        if (state_ok) {
            if (!ExecuteSql(state_db_, "COMMIT;", &query_error)) return fail(query_error);
            result.savestate_rows_deleted = sav_deleted;
            result.artifact_rows_deleted = artifact_deleted;
        } else {
            (void)ExecuteSql(state_db_, "ROLLBACK;", nullptr);
            return fail(query_error);
        }

        for (const auto& file : savestate_files_to_delete) {
            const auto relative_file = file.lexically_relative(
                config_paths_.object_store_root);
            const auto relative_locator =
                state::IsValidArtifactObjectRelativePath(relative_file)
                    ? relative_file.generic_string()
                    : std::string{};
            Statement retained_artifact;
            if (sqlite3_prepare_v2(
                    state_db_,
                    "SELECT COUNT(1) FROM state_artifact "
                    "WHERE object_relpath=?1 OR (object_relpath='' AND filename=?2);",
                    -1,
                    &retained_artifact.st,
                    nullptr) != SQLITE_OK) {
                result.blockers.push_back(
                    "failed checking retained artifact before file deletion " + file.string()
                    + ": " + sqlite3_errmsg(state_db_));
                continue;
            }
            sqlite3_bind_text(
                retained_artifact.st, 1, relative_locator.c_str(), -1,
                SQLITE_TRANSIENT);
            const auto legacy_filename = file.string();
            sqlite3_bind_text(
                retained_artifact.st, 2, legacy_filename.c_str(), -1,
                SQLITE_TRANSIENT);
            if (sqlite3_step(retained_artifact.st) != SQLITE_ROW) {
                result.blockers.push_back(
                    "failed checking retained artifact before file deletion " + file.string()
                    + ": " + sqlite3_errmsg(state_db_));
                continue;
            }
            if (sqlite3_column_int64(retained_artifact.st, 0) > 0) continue;
            std::error_code ec;
            if (std::filesystem::exists(file, ec)) {
                if (std::filesystem::remove(file, ec) && !ec) {
                    ++result.savestate_files_deleted;
                } else {
                    result.blockers.push_back("failed deleting savestate file " + file.string() + ": " + ec.message());
                }
            } else if (ec) {
                result.blockers.push_back("failed checking savestate file " + file.string() + ": " + ec.message());
            }
        }
    }

    result.success = result.blockers.empty();
    if (!result.success && result.error.has_value() && error_out) {
        *error_out = *result.error;
    }
    return result;
}

std::vector<ArchiveCandidateRoot> SqliteArchivePackageService::ListArchiveCandidateRoots(
    types::UtcTimePoint older_than_utc,
    types::UtcTimePoint now_utc,
    int max_candidates,
    std::string* error_out) const {
    std::vector<ArchiveCandidateRoot> candidates;
    if (execution_db_ == nullptr) {
        if (error_out != nullptr) *error_out = "execution db is null";
        return candidates;
    }
    if (max_candidates <= 0) {
        if (error_out != nullptr) *error_out = "max_candidates must be > 0";
        return candidates;
    }

    Statement st;
    constexpr const char* kSql =
        "WITH run_jobs AS ("
        "  SELECT j.job_set_id, j.job_id, j.state, j.claimed_by_token, j.lease_expires_at_utc, "
        "         COALESCE(j.ended_at_utc, j.started_at_utc, j.queued_at_utc, 0) AS terminal_or_activity_utc, "
        "         j.ended_at_utc "
        "  FROM exec_job j"
        "), run_stats AS ("
        "  SELECT job_set_id, "
        "         COUNT(job_id) AS total_jobs, "
        "         SUM(CASE WHEN state='EXECUTION_FINISHED' "
        "                   OR (ended_at_utc IS NULL "
        "                   AND state NOT IN ('FAILED','INTERRUPTED','CANCELED','SUCCEEDED','SUCCEEDED_WINNER','SUCCEEDED_DUPLICATE','SUPERSEDED') "
        "                  ) THEN 1 ELSE 0 END) AS non_terminal_jobs, "
        "         SUM(CASE WHEN claimed_by_token IS NOT NULL AND claimed_by_token<>'' "
        "                   AND COALESCE(lease_expires_at_utc, 0) > ?1 "
        "                  THEN 1 ELSE 0 END) AS active_leases, "
        "         MAX(terminal_or_activity_utc) AS terminal_at_utc "
        "  FROM run_jobs "
        "  GROUP BY job_set_id"
        ") "
        "SELECT job_set_id, terminal_at_utc "
        "FROM run_stats "
        "WHERE total_jobs > 0 "
        "  AND non_terminal_jobs = 0 "
        "  AND active_leases = 0 "
        "  AND NOT EXISTS ("
        "    SELECT 1 FROM exec_job_set s "
        "    WHERE s.job_set_id=run_stats.job_set_id "
        "      AND COALESCE(s.materialization_state,'')<>'WORKSET_PUBLICATION_COMPLETE'"
        "  ) "
        "  AND NOT EXISTS ("
        "    SELECT 1 FROM exec_workset w "
        "    JOIN exec_workset_dispatch_attempt a ON a.workset_id=w.workset_id "
        "    WHERE w.job_set_id=run_stats.job_set_id "
        "      AND a.state IN ('CLAIMED','ACTIVE','DRAINING')"
        "  ) "
        "  AND NOT EXISTS ("
        "    SELECT 1 FROM exec_job j "
        "    LEFT JOIN exec_job_cancellation_request c ON c.job_id=j.job_id "
        "    WHERE j.job_set_id=run_stats.job_set_id "
        "      AND ((j.cancellation_state IS NOT NULL AND j.cancellation_state<>'RESOLVED') "
        "        OR (c.cancellation_request_id IS NOT NULL AND c.state<>'RESOLVED'))"
        "  ) "
        "  AND NOT EXISTS ("
        "    SELECT 1 FROM exec_job j "
        "    JOIN exec_temp_blob b ON b.temp_blob_id=j.worker_result_blob_id "
        "    WHERE j.job_set_id=run_stats.job_set_id "
        "      AND b.cleanup_state<>'DELETED'"
        "  ) "
        "  AND terminal_at_utc > 0 "
        "  AND terminal_at_utc < ?2 "
        "ORDER BY terminal_at_utc ASC, job_set_id ASC "
        "LIMIT ?3;";
    if (sqlite3_prepare_v2(execution_db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(execution_db_);
        return candidates;
    }

    sqlite3_bind_int64(st.st, 1, ToEpochMillis(now_utc));
    sqlite3_bind_int64(st.st, 2, ToEpochMillis(older_than_utc));
    sqlite3_bind_int(st.st, 3, max_candidates);
    while (true) {
        const int rc = sqlite3_step(st.st);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            if (error_out != nullptr) *error_out = sqlite3_errmsg(execution_db_);
            candidates.clear();
            return candidates;
        }

        ArchiveCandidateRoot candidate{};
        candidate.job_set_id = sqlite3_column_int64(st.st, 0);
        candidate.terminal_at_utc = FromEpochMillis(sqlite3_column_int64(st.st, 1));
        candidates.push_back(candidate);
    }

    return candidates;
}

ArchiveBatchPreview SqliteArchivePackageService::PreviewArchiveBatch(
    types::UtcTimePoint older_than_utc,
    types::UtcTimePoint now_utc,
    int max_candidates,
    const retention::OutboxRetentionPolicy& outbox_policy,
    std::string* error_out) const {
    ArchiveBatchPreview preview{};
    preview.candidates = ListArchiveCandidateRoots(older_than_utc, now_utc, max_candidates, error_out);
    if (error_out != nullptr && !error_out->empty()) {
        return preview;
    }

    if (ui_read_db_ == nullptr || execution_retention_db_ == nullptr) {
        preview.blocking_reasons.push_back("missing UIRead/Execution retention db dependencies");
        if (error_out != nullptr) *error_out = "retention preview dependencies are null";
        return preview;
    }

    const auto ui_subscriptions = ui_read_db_->ListProjectionSubscriptions("Execution", "exec_outbox_message");
    preview.ui_safe_floor_outbox_id = ui_read_db_->ComputeSafeFloorOutboxId("Execution", "exec_outbox_message");

    std::vector<retention::OutboxSubscriptionSnapshot> snapshots;
    snapshots.reserve(ui_subscriptions.size());
    for (const auto& subscription : ui_subscriptions) {
        retention::OutboxSubscriptionSnapshot snapshot{};
        snapshot.projector_name = subscription.projector_name;
        snapshot.last_outbox_id = subscription.last_outbox_id;
        snapshot.updated_at_utc = subscription.updated_at_utc;
        snapshot.status = subscription.status;
        snapshot.last_error = subscription.last_error;
        snapshot.required = true;
        snapshots.push_back(std::move(snapshot));
    }

    preview.outbox_retention = execution_retention_db_->PreviewOutboxRetention(snapshots, now_utc, outbox_policy);
    if (preview.ui_safe_floor_outbox_id.has_value()) {
        if (preview.outbox_retention.safe_purge_floor_outbox_id.has_value()) {
            preview.outbox_retention.safe_purge_floor_outbox_id = std::min(
                preview.outbox_retention.safe_purge_floor_outbox_id.value(),
                preview.ui_safe_floor_outbox_id.value());
        }
        else {
            preview.outbox_retention.safe_purge_floor_outbox_id = preview.ui_safe_floor_outbox_id.value();
        }
    }

    if (preview.outbox_retention.IsPurgeBlocked()) {
        for (const auto& blocked : preview.outbox_retention.blocking_required_subscriptions) {
            preview.blocking_reasons.push_back(
                blocked.projector_name + " status=" + blocked.status + " blocks purge");
        }
    }
    if (!preview.outbox_retention.safe_purge_floor_outbox_id.has_value()) {
        preview.blocking_reasons.push_back("safe purge floor unavailable");
    }

    if (preview.outbox_retention.safe_purge_floor_outbox_id.has_value() && execution_db_ != nullptr) {
        Statement st;
        if (sqlite3_prepare_v2(
                execution_db_,
                "SELECT COUNT(1) FROM exec_outbox_message "
                "WHERE published_at_utc IS NOT NULL AND outbox_id < ?1;",
                -1,
                &st.st,
                nullptr)
            == SQLITE_OK) {
            sqlite3_bind_int64(st.st, 1, preview.outbox_retention.safe_purge_floor_outbox_id.value());
            if (sqlite3_step(st.st) == SQLITE_ROW) {
                preview.purgeable_published_outbox_rows = sqlite3_column_int64(st.st, 0);
            }
        }
    }

    return preview;
}

bool SqliteArchivePackageService::PurgePublishedOutboxRowsBeforeFloor(
    std::int64_t safe_floor_outbox_id,
    int max_rows,
    int* rows_deleted_out,
    std::string* error_out) const {
    if (rows_deleted_out != nullptr) *rows_deleted_out = 0;
    if (execution_db_ == nullptr) {
        if (error_out != nullptr) *error_out = "execution db is null";
        return false;
    }
    if (safe_floor_outbox_id <= 0 || max_rows <= 0) {
        if (error_out != nullptr) *error_out = "safe_floor_outbox_id/max_rows must be > 0";
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            execution_db_,
            "DELETE FROM exec_outbox_message "
            "WHERE outbox_id IN ("
            "  SELECT outbox_id FROM exec_outbox_message "
            "  WHERE published_at_utc IS NOT NULL AND outbox_id < ?1 "
            "  ORDER BY outbox_id ASC LIMIT ?2"
            ");",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(execution_db_);
        return false;
    }

    sqlite3_bind_int64(st.st, 1, safe_floor_outbox_id);
    sqlite3_bind_int(st.st, 2, max_rows);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(execution_db_);
        return false;
    }

    if (rows_deleted_out != nullptr) *rows_deleted_out = sqlite3_changes(execution_db_);
    return true;
}

} // namespace savor::db::archive
