#include "ArchivePackageService.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <utility>

#include "../Common/Migrations/MigrationRunner.h"

namespace simcore::db::archive {

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

std::string MakeEventId(const CreateArchivePackageRequest& request, std::string_view suffix) {
    std::ostringstream out;
    if (!request.event_id.empty()) {
        out << request.event_id << '.' << suffix;
    } else {
        out << "archive-" << request.source_root_job_set_id << '-'
            << request.created_at_utc.time_since_epoch().count() << '-' << suffix;
    }
    return out.str();
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
    const auto root = std::to_string(request.source_root_job_set_id);
    const std::string scoped_job_sets =
        "WITH RECURSIVE scoped_job_sets(job_set_id) AS ("
        "SELECT job_set_id FROM exec_job_set WHERE job_set_id="
        + root
        + " UNION ALL "
          "SELECT child.job_set_id FROM exec_job_set child "
          "JOIN scoped_job_sets parent ON child.parent_job_set_id=parent.job_set_id"
          ") ";

    std::vector<ExportSpec> specs;
    specs.push_back(ExportSpec{
        "job_sets",
        scoped_job_sets
            + "SELECT * FROM exec_job_set WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets) ORDER BY job_set_id ASC;"
    });

    specs.push_back(ExportSpec{
        "jobs",
        scoped_job_sets
            + "SELECT * FROM exec_job WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets) ORDER BY job_id ASC;"
    });

    specs.push_back(ExportSpec{
        "job_events",
        scoped_job_sets
            + "SELECT e.* FROM exec_job_event e "
              "JOIN exec_job j ON j.job_id=e.job_id "
              "WHERE j.job_set_id IN (SELECT job_set_id FROM scoped_job_sets) "
              "ORDER BY e.job_event_id ASC;"
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
                  "ORDER BY outbox_id ASC;"
        });
    }

    return specs;
}

} // namespace

SqliteArchivePackageService::SqliteArchivePackageService(
    sqlite3* execution_db,
    simcore::db::IExecutionDb* execution_retention_db,
    simcore::db::IUiReadDb* ui_read_db,
    IArchiveDb* archive_db,
    DbConfigPaths config_paths)
    : execution_db_(execution_db)
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
    if (request.source_root_job_set_id <= 0) {
        result.error = "source_root_job_set_id must be positive";
        return result;
    }

    int manifest_schema_version = request.schema_version;
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
    const auto package_name = "package-" + std::to_string(request.source_root_job_set_id) + "-" + std::to_string(epoch);

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
             << "  \"source_root_job_set_id\": " << request.source_root_job_set_id << ",\n"
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

    const std::string package_event_id = MakeEventId(request, "package");
    CreateArchivePackageCommand create_command{};
    create_command.source_context = request.source_context;
    create_command.source_root_job_set_id = request.source_root_job_set_id;
    create_command.created_at_utc = request.created_at_utc;
    create_command.schema_version = manifest_schema_version;
    create_command.event_catalog_version = request.event_catalog_version;
    create_command.time_range_start_utc = min_time;
    create_command.time_range_end_utc = max_time;
    create_command.manifest_path = manifest_path.generic_string();
    create_command.checksum_status = "PASS";
    create_command.event_id = package_event_id;
    create_command.correlation_id = request.correlation_id;
    create_command.causation_id = request.causation_id;

    std::string archive_error;
    std::int64_t archive_package_id = 0;
    if (!archive_db_->CreateArchivePackage(create_command, &archive_package_id, &archive_error)) {
        result.error = archive_error;
        return result;
    }

    std::int64_t item_index = 0;
    for (const auto& file : context.files) {
        AddArchiveItemCommand item_command{};
        item_command.archive_package_id = archive_package_id;
        item_command.item_kind = file.item_kind;
        item_command.item_count = file.row_count;
        item_command.blob_path = file.relative_path.generic_string();
        item_command.checksum = file.checksum;
        item_command.indexed_at_utc = request.created_at_utc;
        item_command.event_id = MakeEventId(request, "item-" + std::to_string(item_index));
        item_command.correlation_id = request.correlation_id;
        item_command.causation_id = package_event_id;

        if (!archive_db_->AddArchiveItem(item_command, nullptr, &archive_error)) {
            result.error = archive_error;
            return result;
        }
        ++item_index;
    }

    result.success = true;
    result.archive_package_id = archive_package_id;
    result.package_root = context.package_root;
    result.files = context.files;
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
        "WITH RECURSIVE run_tree(root_job_set_id, job_set_id) AS ("
        "  SELECT job_set_id, job_set_id "
        "  FROM exec_job_set "
        "  WHERE parent_job_set_id IS NULL "
        "  UNION ALL "
        "  SELECT t.root_job_set_id, c.job_set_id "
        "  FROM exec_job_set c "
        "  JOIN run_tree t ON c.parent_job_set_id=t.job_set_id"
        "), run_jobs AS ("
        "  SELECT t.root_job_set_id, j.job_id, j.state, j.claimed_by_token, j.lease_expires_at_utc, "
        "         COALESCE(j.ended_at_utc, j.started_at_utc, j.queued_at_utc, 0) AS terminal_or_activity_utc, "
        "         j.ended_at_utc "
        "  FROM run_tree t "
        "  JOIN exec_job j ON j.job_set_id=t.job_set_id"
        "), run_stats AS ("
        "  SELECT root_job_set_id, "
        "         COUNT(job_id) AS total_jobs, "
        "         SUM(CASE WHEN ended_at_utc IS NULL "
        "                   AND state NOT IN ('COMPLETED','FAILED','CANCELED','SUCCEEDED','SUCCEEDED_WINNER','SUCCEEDED_DUPLICATE','SUPERSEDED') "
        "                  THEN 1 ELSE 0 END) AS non_terminal_jobs, "
        "         SUM(CASE WHEN claimed_by_token IS NOT NULL AND claimed_by_token<>'' "
        "                   AND COALESCE(lease_expires_at_utc, 0) > ?1 "
        "                  THEN 1 ELSE 0 END) AS active_leases, "
        "         MAX(terminal_or_activity_utc) AS terminal_at_utc "
        "  FROM run_jobs "
        "  GROUP BY root_job_set_id"
        ") "
        "SELECT root_job_set_id, terminal_at_utc "
        "FROM run_stats "
        "WHERE total_jobs > 0 "
        "  AND non_terminal_jobs = 0 "
        "  AND active_leases = 0 "
        "  AND terminal_at_utc > 0 "
        "  AND terminal_at_utc < ?2 "
        "ORDER BY terminal_at_utc ASC, root_job_set_id ASC "
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
        candidate.root_job_set_id = sqlite3_column_int64(st.st, 0);
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

bool SqliteArchivePackageService::ApplySourcePurgePolicyForRoot(
    std::int64_t root_job_set_id,
    ArchiveSourcePurgeAction action,
    std::int64_t archive_package_id,
    int* job_sets_affected_out,
    std::string* error_out) const {
    if (job_sets_affected_out != nullptr) *job_sets_affected_out = 0;
    if (execution_db_ == nullptr) {
        if (error_out != nullptr) *error_out = "execution db is null";
        return false;
    }
    if (root_job_set_id <= 0) {
        if (error_out != nullptr) *error_out = "root_job_set_id must be > 0";
        return false;
    }
    if (action == ArchiveSourcePurgeAction::None) {
        return true;
    }

    Statement st;
    if (action == ArchiveSourcePurgeAction::MarkArchived) {
        if (sqlite3_prepare_v2(
                execution_db_,
                "WITH RECURSIVE scoped_job_sets(job_set_id) AS ("
                "  SELECT job_set_id FROM exec_job_set WHERE job_set_id=?1 "
                "  UNION ALL "
                "  SELECT c.job_set_id FROM exec_job_set c "
                "  JOIN scoped_job_sets p ON c.parent_job_set_id=p.job_set_id"
                ") "
                "UPDATE exec_job_set "
                "SET meta_note=CASE "
                "  WHEN COALESCE(meta_note,'')='' THEN ?2 "
                "  ELSE meta_note || ' | ' || ?2 END "
                "WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets);",
                -1,
                &st.st,
                nullptr)
            != SQLITE_OK) {
            if (error_out != nullptr) *error_out = sqlite3_errmsg(execution_db_);
            return false;
        }
        const std::string marker = "ARCHIVED(package_id=" + std::to_string(archive_package_id) + ")";
        sqlite3_bind_int64(st.st, 1, root_job_set_id);
        sqlite3_bind_text(st.st, 2, marker.c_str(), -1, SQLITE_TRANSIENT);
    }
    else if (action == ArchiveSourcePurgeAction::DeleteRows) {
        char* err_msg = nullptr;
        if (sqlite3_exec(execution_db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &err_msg) != SQLITE_OK) {
            if (error_out != nullptr) *error_out = err_msg == nullptr ? "begin transaction failed" : err_msg;
            sqlite3_free(err_msg);
            return false;
        }
        sqlite3_free(err_msg);

        bool ok = true;
        const auto run_delete = [&](const char* sql) {
            sqlite3_stmt* del = nullptr;
            if (sqlite3_prepare_v2(execution_db_, sql, -1, &del, nullptr) != SQLITE_OK) {
                return false;
            }
            sqlite3_bind_int64(del, 1, root_job_set_id);
            const int rc = sqlite3_step(del);
            sqlite3_finalize(del);
            return rc == SQLITE_DONE;
        };

        ok = ok && run_delete(
            "WITH RECURSIVE scoped_job_sets(job_set_id) AS ("
            "  SELECT job_set_id FROM exec_job_set WHERE job_set_id=?1 "
            "  UNION ALL "
            "  SELECT c.job_set_id FROM exec_job_set c "
            "  JOIN scoped_job_sets p ON c.parent_job_set_id=p.job_set_id"
            "), scoped_instances(workflow_instance_id) AS ("
            "  SELECT workflow_instance_id FROM exec_workflow_instance "
            "  WHERE root_scope_kind='job_set' AND root_scope_id IN (SELECT job_set_id FROM scoped_job_sets) "
            "  UNION "
            "  SELECT DISTINCT workflow_instance_id FROM exec_workflow_step "
            "  WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets)"
            ") "
            "DELETE FROM exec_workflow_event "
            "WHERE workflow_instance_id IN (SELECT workflow_instance_id FROM scoped_instances);");
        ok = ok && run_delete(
            "WITH RECURSIVE scoped_job_sets(job_set_id) AS ("
            "  SELECT job_set_id FROM exec_job_set WHERE job_set_id=?1 "
            "  UNION ALL "
            "  SELECT c.job_set_id FROM exec_job_set c "
            "  JOIN scoped_job_sets p ON c.parent_job_set_id=p.job_set_id"
            "), scoped_instances(workflow_instance_id) AS ("
            "  SELECT workflow_instance_id FROM exec_workflow_instance "
            "  WHERE root_scope_kind='job_set' AND root_scope_id IN (SELECT job_set_id FROM scoped_job_sets) "
            "  UNION "
            "  SELECT DISTINCT workflow_instance_id FROM exec_workflow_step "
            "  WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets)"
            ") "
            "DELETE FROM exec_workflow_edge "
            "WHERE workflow_instance_id IN (SELECT workflow_instance_id FROM scoped_instances);");
        ok = ok && run_delete(
            "WITH RECURSIVE scoped_job_sets(job_set_id) AS ("
            "  SELECT job_set_id FROM exec_job_set WHERE job_set_id=?1 "
            "  UNION ALL "
            "  SELECT c.job_set_id FROM exec_job_set c "
            "  JOIN scoped_job_sets p ON c.parent_job_set_id=p.job_set_id"
            "), scoped_instances(workflow_instance_id) AS ("
            "  SELECT workflow_instance_id FROM exec_workflow_instance "
            "  WHERE root_scope_kind='job_set' AND root_scope_id IN (SELECT job_set_id FROM scoped_job_sets) "
            "  UNION "
            "  SELECT DISTINCT workflow_instance_id FROM exec_workflow_step "
            "  WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets)"
            ") "
            "DELETE FROM exec_workflow_unit_activation_edge "
            "WHERE workflow_instance_id IN (SELECT workflow_instance_id FROM scoped_instances);");
        ok = ok && run_delete(
            "WITH RECURSIVE scoped_job_sets(job_set_id) AS ("
            "  SELECT job_set_id FROM exec_job_set WHERE job_set_id=?1 "
            "  UNION ALL "
            "  SELECT c.job_set_id FROM exec_job_set c "
            "  JOIN scoped_job_sets p ON c.parent_job_set_id=p.job_set_id"
            "), scoped_instances(workflow_instance_id) AS ("
            "  SELECT workflow_instance_id FROM exec_workflow_instance "
            "  WHERE root_scope_kind='job_set' AND root_scope_id IN (SELECT job_set_id FROM scoped_job_sets) "
            "  UNION "
            "  SELECT DISTINCT workflow_instance_id FROM exec_workflow_step "
            "  WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets)"
            ") "
            "UPDATE exec_workflow_step SET workflow_unit_activation_id=NULL "
            "WHERE workflow_instance_id IN (SELECT workflow_instance_id FROM scoped_instances);");
        ok = ok && run_delete(
            "WITH RECURSIVE scoped_job_sets(job_set_id) AS ("
            "  SELECT job_set_id FROM exec_job_set WHERE job_set_id=?1 "
            "  UNION ALL "
            "  SELECT c.job_set_id FROM exec_job_set c "
            "  JOIN scoped_job_sets p ON c.parent_job_set_id=p.job_set_id"
            "), scoped_instances(workflow_instance_id) AS ("
            "  SELECT workflow_instance_id FROM exec_workflow_instance "
            "  WHERE root_scope_kind='job_set' AND root_scope_id IN (SELECT job_set_id FROM scoped_job_sets) "
            "  UNION "
            "  SELECT DISTINCT workflow_instance_id FROM exec_workflow_step "
            "  WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets)"
            ") "
            "DELETE FROM exec_workflow_unit_activation "
            "WHERE workflow_instance_id IN (SELECT workflow_instance_id FROM scoped_instances);");
        ok = ok && run_delete(
            "WITH RECURSIVE scoped_job_sets(job_set_id) AS ("
            "  SELECT job_set_id FROM exec_job_set WHERE job_set_id=?1 "
            "  UNION ALL "
            "  SELECT c.job_set_id FROM exec_job_set c "
            "  JOIN scoped_job_sets p ON c.parent_job_set_id=p.job_set_id"
            "), scoped_instances(workflow_instance_id) AS ("
            "  SELECT workflow_instance_id FROM exec_workflow_instance "
            "  WHERE root_scope_kind='job_set' AND root_scope_id IN (SELECT job_set_id FROM scoped_job_sets) "
            "  UNION "
            "  SELECT DISTINCT workflow_instance_id FROM exec_workflow_step "
            "  WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets)"
            ") "
            "DELETE FROM exec_workflow_step "
            "WHERE workflow_instance_id IN (SELECT workflow_instance_id FROM scoped_instances);");
        ok = ok && run_delete(
            "WITH RECURSIVE scoped_job_sets(job_set_id) AS ("
            "  SELECT job_set_id FROM exec_job_set WHERE job_set_id=?1 "
            "  UNION ALL "
            "  SELECT c.job_set_id FROM exec_job_set c "
            "  JOIN scoped_job_sets p ON c.parent_job_set_id=p.job_set_id"
            "), scoped_instances(workflow_instance_id) AS ("
            "  SELECT workflow_instance_id FROM exec_workflow_instance "
            "  WHERE root_scope_kind='job_set' AND root_scope_id IN (SELECT job_set_id FROM scoped_job_sets) "
            "  UNION "
            "  SELECT DISTINCT workflow_instance_id FROM exec_workflow_step "
            "  WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets)"
            ") "
            "DELETE FROM exec_workflow_instance "
            "WHERE workflow_instance_id IN (SELECT workflow_instance_id FROM scoped_instances);");
        ok = ok && run_delete(
            "WITH RECURSIVE scoped_job_sets(job_set_id) AS ("
            "  SELECT job_set_id FROM exec_job_set WHERE job_set_id=?1 "
            "  UNION ALL "
            "  SELECT c.job_set_id FROM exec_job_set c "
            "  JOIN scoped_job_sets p ON c.parent_job_set_id=p.job_set_id"
            "), scoped_jobs(job_id) AS ("
            "  SELECT job_id FROM exec_job WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets)"
            ") "
            "DELETE FROM exec_job_event WHERE job_id IN (SELECT job_id FROM scoped_jobs);");
        ok = ok && run_delete(
            "WITH RECURSIVE scoped_job_sets(job_set_id) AS ("
            "  SELECT job_set_id FROM exec_job_set WHERE job_set_id=?1 "
            "  UNION ALL "
            "  SELECT c.job_set_id FROM exec_job_set c "
            "  JOIN scoped_job_sets p ON c.parent_job_set_id=p.job_set_id"
            "), scoped_jobs(job_id) AS ("
            "  SELECT job_id FROM exec_job WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets)"
            ") "
            "DELETE FROM exec_trigger "
            "WHERE (scope_kind='job_set' AND scope_id IN (SELECT job_set_id FROM scoped_job_sets)) "
            "   OR (scope_kind='job' AND scope_id IN (SELECT job_id FROM scoped_jobs));");
        ok = ok && run_delete(
            "WITH RECURSIVE scoped_job_sets(job_set_id) AS ("
            "  SELECT job_set_id FROM exec_job_set WHERE job_set_id=?1 "
            "  UNION ALL "
            "  SELECT c.job_set_id FROM exec_job_set c "
            "  JOIN scoped_job_sets p ON c.parent_job_set_id=p.job_set_id"
            "), scoped_jobs(job_id) AS ("
            "  SELECT job_id FROM exec_job WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets)"
            ") "
            "DELETE FROM exec_outbox_message "
            "WHERE (aggregate_kind='job_set' AND CAST(aggregate_id AS INTEGER) IN (SELECT job_set_id FROM scoped_job_sets)) "
            "   OR (aggregate_kind='job' AND CAST(aggregate_id AS INTEGER) IN (SELECT job_id FROM scoped_jobs));");
        ok = ok && run_delete(
            "WITH RECURSIVE scoped_job_sets(job_set_id) AS ("
            "  SELECT job_set_id FROM exec_job_set WHERE job_set_id=?1 "
            "  UNION ALL "
            "  SELECT c.job_set_id FROM exec_job_set c "
            "  JOIN scoped_job_sets p ON c.parent_job_set_id=p.job_set_id"
            ") "
            "DELETE FROM exec_job WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets);");
        ok = ok && run_delete(
            "WITH RECURSIVE scoped_job_sets(job_set_id) AS ("
            "  SELECT job_set_id FROM exec_job_set WHERE job_set_id=?1 "
            "  UNION ALL "
            "  SELECT c.job_set_id FROM exec_job_set c "
            "  JOIN scoped_job_sets p ON c.parent_job_set_id=p.job_set_id"
            ") "
            "DELETE FROM exec_job_set WHERE job_set_id IN (SELECT job_set_id FROM scoped_job_sets);");

        if (!ok) {
            sqlite3_exec(execution_db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            if (error_out != nullptr) *error_out = sqlite3_errmsg(execution_db_);
            return false;
        }
        if (sqlite3_exec(execution_db_, "COMMIT;", nullptr, nullptr, &err_msg) != SQLITE_OK) {
            if (error_out != nullptr) *error_out = err_msg == nullptr ? "commit transaction failed" : err_msg;
            sqlite3_free(err_msg);
            return false;
        }
        sqlite3_free(err_msg);
        if (job_sets_affected_out != nullptr) {
            *job_sets_affected_out = sqlite3_changes(execution_db_);
        }
        return true;
    }

    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(execution_db_);
        return false;
    }
    if (job_sets_affected_out != nullptr) *job_sets_affected_out = sqlite3_changes(execution_db_);
    return true;
}

ArchiveBatchResult SqliteArchivePackageService::ExecuteArchiveBatch(
    types::UtcTimePoint older_than_utc,
    types::UtcTimePoint now_utc,
    int max_candidates,
    int max_outbox_purge_rows,
    const retention::OutboxRetentionPolicy& outbox_policy,
    const ArchiveSourcePurgePolicy& source_purge_policy) {
    ArchiveBatchResult result{};

    std::string preview_error;
    const auto preview = PreviewArchiveBatch(older_than_utc, now_utc, max_candidates, outbox_policy, &preview_error);
    if (!preview_error.empty()) {
        result.errors.push_back(preview_error);
        return result;
    }
    result.candidates_considered = static_cast<int>(preview.candidates.size());

    for (const auto& candidate : preview.candidates) {
        CreateArchivePackageRequest request{};
        request.source_root_job_set_id = candidate.root_job_set_id;
        request.created_at_utc = now_utc;
        const auto package_result = CreatePackage(request);
        if (!package_result.success) {
            result.errors.push_back(
                "failed to archive root " + std::to_string(candidate.root_job_set_id) + ": "
                + package_result.error.value_or("unknown error"));
            continue;
        }

        ++result.packages_written;
        result.archived_root_job_set_ids.push_back(candidate.root_job_set_id);

        int source_rows = 0;
        std::string source_error;
        if (!ApplySourcePurgePolicyForRoot(
                candidate.root_job_set_id,
                source_purge_policy.source_action,
                package_result.archive_package_id,
                &source_rows,
                &source_error)) {
            result.errors.push_back(
                "source purge policy failed for root " + std::to_string(candidate.root_job_set_id) + ": " + source_error);
            continue;
        }
        result.source_job_sets_purged += source_rows;
    }

    if (max_outbox_purge_rows > 0 && preview.outbox_retention.safe_purge_floor_outbox_id.has_value()) {
        int rows_deleted = 0;
        std::string purge_error;
        if (!PurgePublishedOutboxRowsBeforeFloor(
                preview.outbox_retention.safe_purge_floor_outbox_id.value(),
                max_outbox_purge_rows,
                &rows_deleted,
                &purge_error)) {
            result.errors.push_back("outbox purge failed: " + purge_error);
        }
        result.outbox_rows_purged = rows_deleted;
    }

    if (execution_db_ != nullptr) {
        Statement upsert_cursor;
        if (sqlite3_prepare_v2(
                execution_db_,
                "INSERT INTO exec_archive_cursor(cursor_id,cursor_kind,last_scanned_at_utc,last_job_set_id) "
                "VALUES(1, 'stage4.archive', ?1, ?2) "
                "ON CONFLICT(cursor_id) DO UPDATE SET "
                "cursor_kind=excluded.cursor_kind,last_scanned_at_utc=excluded.last_scanned_at_utc,last_job_set_id=excluded.last_job_set_id;",
                -1,
                &upsert_cursor.st,
                nullptr)
            == SQLITE_OK) {
            sqlite3_bind_int64(upsert_cursor.st, 1, ToEpochMillis(now_utc));
            sqlite3_bind_int64(
                upsert_cursor.st,
                2,
                result.archived_root_job_set_ids.empty() ? 0 : result.archived_root_job_set_ids.back());
            sqlite3_step(upsert_cursor.st);
        }
    }

    result.success = result.errors.empty();
    return result;
}

} // namespace simcore::db::archive
