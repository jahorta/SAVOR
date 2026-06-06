#include "RehydrateExecutor.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_set>

#include "../Common/Migrations/MigrationRunner.h"

namespace simcore::db::archive {
namespace {

struct Statement {
    sqlite3_stmt* st = nullptr;
    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }
};

struct StreamFile {
    std::string item_kind;
    std::filesystem::path rel_path;
    std::string checksum;
    int row_count = 0;
};

struct PackageSpec {
    std::int64_t archive_package_id = 0;
    std::string target_namespace;
    std::filesystem::path package_root;
    std::int64_t schema_version = 0;
    std::int64_t event_catalog_version = 0;
    std::vector<StreamFile> stream_files;
};

struct StructuredError {
    std::string code;
    std::string message;
    std::string detail;

    std::string ToJson() const {
        std::ostringstream out;
        out << "{\"code\":\"" << code << "\",\"message\":\"" << message << "\",\"detail\":\"" << detail << "\"}";
        return out.str();
    }
};

bool Prepare(sqlite3* db, const char* sql, Statement* st, std::string* error_out) {
    if (sqlite3_prepare_v2(db, sql, -1, &st->st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    return true;
}

bool StepDone(sqlite3* db, sqlite3_stmt* st, std::string* error_out) {
    if (sqlite3_step(st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    return true;
}

bool ReadFile(const std::filesystem::path& path, std::string* content, std::string* error_out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        if (error_out) *error_out = "failed to open " + path.string();
        return false;
    }
    std::ostringstream out;
    out << in.rdbuf();
    *content = out.str();
    return true;
}

std::string Hex64(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex;
    out.width(16);
    out.fill('0');
    out << value;
    return out.str();
}

std::string Fnv1a64(std::string_view payload) {
    std::uint64_t hash = 1469598103934665603ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    for (unsigned char ch : payload) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= kPrime;
    }
    return Hex64(hash);
}

std::int64_t JsonExtractInt(sqlite3* db, std::string_view json, std::string_view path, bool* ok) {
    Statement st;
    *ok = false;
    if (sqlite3_prepare_v2(db, "SELECT json_extract(?1, ?2);", -1, &st.st, nullptr) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st.st, 1, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 2, path.data(), static_cast<int>(path.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(st.st) != SQLITE_ROW || sqlite3_column_type(st.st, 0) == SQLITE_NULL) {
        return 0;
    }
    *ok = true;
    return sqlite3_column_int64(st.st, 0);
}

std::string JsonExtractText(sqlite3* db, std::string_view json, std::string_view path, bool* ok) {
    Statement st;
    *ok = false;
    if (sqlite3_prepare_v2(db, "SELECT json_extract(?1, ?2);", -1, &st.st, nullptr) != SQLITE_OK) {
        return {};
    }
    sqlite3_bind_text(st.st, 1, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 2, path.data(), static_cast<int>(path.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(st.st) != SQLITE_ROW || sqlite3_column_type(st.st, 0) == SQLITE_NULL) {
        return {};
    }
    const char* text = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 0));
    const int bytes = sqlite3_column_bytes(st.st, 0);
    *ok = true;
    return text == nullptr ? std::string{} : std::string(text, bytes);
}

std::int64_t AllocateId(std::string_view ns, std::string_view kind, std::int64_t old_id) {
    const auto seed = std::string(ns) + ":" + std::string(kind) + ":" + std::to_string(old_id);
    std::uint64_t hash = 1469598103934665603ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    for (unsigned char ch : seed) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= kPrime;
    }
    hash &= 0x7fffffffffffffffULL;
    if (hash < 1000000ULL) {
        hash += 1000000ULL;
    }
    if (hash > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        hash = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    }
    return static_cast<std::int64_t>(hash);
}

bool InsertMap(sqlite3* archive_db, std::int64_t request_id, std::string_view kind, std::int64_t old_id, std::int64_t new_id, std::string* error_out) {
    Statement st;
    if (!Prepare(archive_db,
        "INSERT INTO ar_rehydrate_map(rehydrate_request_id,entity_kind,old_id,new_id) VALUES(?1,?2,?3,?4);",
        &st,
        error_out)) {
        return false;
    }
    sqlite3_bind_int64(st.st, 1, request_id);
    sqlite3_bind_text(st.st, 2, kind.data(), static_cast<int>(kind.size()), SQLITE_TRANSIENT);
    const auto old_s = std::to_string(old_id);
    const auto new_s = std::to_string(new_id);
    sqlite3_bind_text(st.st, 3, old_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 4, new_s.c_str(), -1, SQLITE_TRANSIENT);
    return StepDone(archive_db, st.st, error_out);
}

bool InsertExecutionOutbox(
    sqlite3* execution_db,
    std::string_view event_id,
    std::string_view event_type,
    std::string_view aggregate_kind,
    std::string_view aggregate_id,
    std::string_view correlation_id,
    std::string_view causation_id,
    std::int64_t occurred_at_utc,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id,
    std::string* error_out) {
    Statement st;
    if (!Prepare(execution_db,
        "INSERT INTO exec_outbox_message(event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id) "
        "VALUES(?1,?2,1,'Execution',?3,?4,?5,?6,?7,?8,?9);",
        &st,
        error_out)) {
        return false;
    }

    sqlite3_bind_text(st.st, 1, event_id.data(), static_cast<int>(event_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 2, event_type.data(), static_cast<int>(event_type.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 3, aggregate_kind.data(), static_cast<int>(aggregate_kind.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 4, aggregate_id.data(), static_cast<int>(aggregate_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 5, correlation_id.data(), static_cast<int>(correlation_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 6, causation_id.data(), static_cast<int>(causation_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 7, occurred_at_utc);
    sqlite3_bind_text(st.st, 8, payload_ref_kind.data(), static_cast<int>(payload_ref_kind.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 9, payload_ref_id);

    return StepDone(execution_db, st.st, error_out);
}

} // namespace

SqliteRehydrateExecutor::SqliteRehydrateExecutor(
    sqlite3* execution_db,
    sqlite3* archive_db,
    simcore::db::IArchiveDb* archive_service,
    std::filesystem::path archive_store_root)
    : execution_db_(execution_db)
    , archive_db_(archive_db)
    , archive_service_(archive_service)
    , archive_store_root_(std::move(archive_store_root)) {
}

RehydrateExecutionResult SqliteRehydrateExecutor::Execute(const RehydrateExecutionRequest& request) {
    RehydrateExecutionResult result{};
    if (execution_db_ == nullptr || archive_db_ == nullptr || archive_service_ == nullptr) {
        result.error = StructuredError{ "DEPENDENCY_NULL", "rehydrate dependencies are missing", "database/service dependency is null" }.ToJson();
        return result;
    }
    if (request.rehydrate_request_id <= 0) {
        result.error = StructuredError{ "INVALID_REQUEST", "rehydrate_request_id must be positive", "invalid request id" }.ToJson();
        return result;
    }

    PackageSpec spec{};
    {
        Statement st;
        std::string error;
        if (!Prepare(
                archive_db_,
                "SELECT rr.archive_package_id, rr.target_namespace, p.manifest_path "
                "FROM ar_rehydrate_request rr JOIN ar_archive_package p ON p.archive_package_id=rr.archive_package_id "
                "WHERE rr.rehydrate_request_id=?1;",
                &st,
                &error)) {
            result.error = StructuredError{ "DB_QUERY_ERROR", "failed reading rehydrate request", error }.ToJson();
            return result;
        }
        sqlite3_bind_int64(st.st, 1, request.rehydrate_request_id);
        if (sqlite3_step(st.st) != SQLITE_ROW) {
            result.error = StructuredError{ "REQUEST_NOT_FOUND", "rehydrate request missing", "request row not found" }.ToJson();
            return result;
        }
        spec.archive_package_id = sqlite3_column_int64(st.st, 0);
        const char* ns = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 1));
        const char* manifest = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 2));
        spec.target_namespace = ns == nullptr ? std::string{} : std::string(ns);
        const auto manifest_path = manifest == nullptr ? std::filesystem::path{} : std::filesystem::path(manifest);
        spec.package_root = manifest_path.parent_path();
    }

    if (spec.target_namespace.empty()) {
        spec.target_namespace = "rh-" + std::to_string(request.rehydrate_request_id);
    }
    result.namespace_token = spec.target_namespace;

    std::string manifest_text;
    std::string io_error;
    if (!ReadFile(spec.package_root / "manifest.json", &manifest_text, &io_error)) {
        result.error = StructuredError{ "PACKAGE_READ_ERROR", "manifest missing", io_error }.ToJson();
    } else {
        bool ok_schema = false;
        bool ok_catalog = false;
        spec.schema_version = JsonExtractInt(execution_db_, manifest_text, "$.schema_version", &ok_schema);
        spec.event_catalog_version = JsonExtractInt(execution_db_, manifest_text, "$.event_catalog_version", &ok_catalog);
        if (!ok_schema || !ok_catalog) {
            result.error = StructuredError{ "PACKAGE_SCHEMA_ERROR", "manifest required fields missing", "schema_version/event_catalog_version missing" }.ToJson();
        }
    }

    if (!result.error.has_value()) {
        std::string checksums_text;
        if (!ReadFile(spec.package_root / "checksums.json", &checksums_text, &io_error)) {
            result.error = StructuredError{ "PACKAGE_READ_ERROR", "checksums missing", io_error }.ToJson();
        } else {
            Statement st_files;
            if (sqlite3_prepare_v2(
                    execution_db_,
                    "SELECT json_extract(value,'$.item_kind'), json_extract(value,'$.path'), json_extract(value,'$.checksum'), json_extract(value,'$.row_count') "
                    "FROM json_each(json_extract(?1, '$.files'));",
                    -1,
                    &st_files.st,
                    nullptr)
                == SQLITE_OK) {
                sqlite3_bind_text(st_files.st, 1, manifest_text.c_str(), -1, SQLITE_TRANSIENT);
                while (sqlite3_step(st_files.st) == SQLITE_ROW) {
                    StreamFile f{};
                    const char* kind = reinterpret_cast<const char*>(sqlite3_column_text(st_files.st, 0));
                    const char* path = reinterpret_cast<const char*>(sqlite3_column_text(st_files.st, 1));
                    const char* checksum = reinterpret_cast<const char*>(sqlite3_column_text(st_files.st, 2));
                    f.item_kind = kind == nullptr ? std::string{} : std::string(kind);
                    f.rel_path = path == nullptr ? std::filesystem::path{} : std::filesystem::path(path);
                    f.checksum = checksum == nullptr ? std::string{} : std::string(checksum);
                    f.row_count = sqlite3_column_type(st_files.st, 3) == SQLITE_NULL ? 0 : sqlite3_column_int(st_files.st, 3);
                    if (f.rel_path.extension() == ".jsonl") {
                        spec.stream_files.push_back(std::move(f));
                    }
                }
            }

            Statement st_checksums;
            std::unordered_map<std::string, std::string> checksum_index;
            if (sqlite3_prepare_v2(
                    execution_db_,
                    "SELECT json_extract(value,'$.path'), json_extract(value,'$.checksum') FROM json_each(json_extract(?1, '$.files'));",
                    -1,
                    &st_checksums.st,
                    nullptr)
                == SQLITE_OK) {
                sqlite3_bind_text(st_checksums.st, 1, checksums_text.c_str(), -1, SQLITE_TRANSIENT);
                while (sqlite3_step(st_checksums.st) == SQLITE_ROW) {
                    const char* path = reinterpret_cast<const char*>(sqlite3_column_text(st_checksums.st, 0));
                    const char* checksum = reinterpret_cast<const char*>(sqlite3_column_text(st_checksums.st, 1));
                    if (path != nullptr && checksum != nullptr) {
                        checksum_index[path] = checksum;
                    }
                }
            }

            for (const auto& file : spec.stream_files) {
                std::string content;
                if (!ReadFile(spec.package_root / file.rel_path, &content, &io_error)) {
                    result.error = StructuredError{ "PACKAGE_READ_ERROR", "stream file missing", io_error }.ToJson();
                    break;
                }
                const auto digest = Fnv1a64(content);
                const auto path_key = file.rel_path.generic_string();
                const auto it = checksum_index.find(path_key);
                if (it == checksum_index.end() || it->second != digest) {
                    result.error = StructuredError{ "CHECKSUM_MISMATCH", "archive checksum mismatch", path_key }.ToJson();
                    break;
                }
            }
        }
    }

    if (!result.error.has_value()) {
        std::string migration_error;
        const auto execution_schema = migrations::GetCurrentContextSchemaVersion(
            execution_db_,
            migrations::MigrationContext::Execution,
            &migration_error);

        if (!execution_schema.has_value()) {
            result.error = StructuredError{ "SCHEMA_READ_ERROR", "failed reading execution schema version", migration_error }.ToJson();
        } else if (*execution_schema != spec.schema_version || spec.event_catalog_version != 1) {
            result.error = StructuredError{
                "COMPATIBILITY_MISMATCH",
                "archive package is not compatible with current execution/event catalog",
                "archive schema=" + std::to_string(spec.schema_version)
                    + ", execution schema=" + std::to_string(*execution_schema)
                    + ", archive catalog=" + std::to_string(spec.event_catalog_version)
            }.ToJson();
        }
    }

    std::vector<std::int64_t> restored_jobs;
    std::unordered_map<std::string, std::unordered_map<std::int64_t, std::int64_t>> id_map;

    if (!result.error.has_value()) {
        sqlite3_exec(execution_db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
        auto rollback = [&]() {
            sqlite3_exec(execution_db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        };

        std::string db_error;
        std::vector<std::string> order = {
            "job_sets", "jobs", "job_events", "workflow_instances", "workflow_steps", "workflow_edges", "workflow_events", "triggers"
        };

        for (const auto& kind : order) {
            const auto it = std::find_if(spec.stream_files.begin(), spec.stream_files.end(), [&](const StreamFile& f) {
                return f.item_kind == kind;
            });
            if (it == spec.stream_files.end()) {
                continue;
            }

            std::ifstream in(spec.package_root / it->rel_path, std::ios::binary);
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty()) continue;

                auto map_id = [&](std::string_view map_kind, std::int64_t old_id) -> std::int64_t {
                    auto& per_kind = id_map[std::string(map_kind)];
                    auto existing = per_kind.find(old_id);
                    if (existing != per_kind.end()) {
                        return existing->second;
                    }
                    const auto next = AllocateId(spec.target_namespace, map_kind, old_id);
                    per_kind.emplace(old_id, next);
                    if (!InsertMap(archive_db_, request.rehydrate_request_id, map_kind, old_id, next, &db_error)) {
                        return 0;
                    }
                    return next;
                };

                if (kind == "job_sets") {
                    bool ok_id = false;
                    bool ok_parent = false;
                    const auto old_id = JsonExtractInt(execution_db_, line, "$.job_set_id", &ok_id);
                    const auto old_parent = JsonExtractInt(execution_db_, line, "$.parent_job_set_id", &ok_parent);
                    if (!ok_id) continue;
                    const auto new_id = map_id("job_set", old_id);
                    const auto new_parent = ok_parent ? map_id("job_set", old_parent) : 0;
                    if (new_id == 0 || (ok_parent && new_parent == 0)) break;

                    Statement st;
                    if (!Prepare(execution_db_,
                            "INSERT INTO exec_job_set(job_set_id,parent_job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note) "
                            "VALUES(?1,?2,json_extract(?3,'$.program_kind'),json_extract(?3,'$.purpose'),?4,json_extract(?3,'$.created_at_utc'),json_extract(?3,'$.priority_boost'),json_extract(?3,'$.expected_total'),json_extract(?3,'$.domain_ref_kind'),json_extract(?3,'$.domain_ref_id'),?5);",
                            &st,
                            &db_error)) {
                        break;
                    }
                    sqlite3_bind_int64(st.st, 1, new_id);
                    if (ok_parent) sqlite3_bind_int64(st.st, 2, new_parent); else sqlite3_bind_null(st.st, 2);
                    sqlite3_bind_text(st.st, 3, line.c_str(), -1, SQLITE_TRANSIENT);
                    const auto created_by = spec.target_namespace + ":rehydrate";
                    const auto note = "rehydrated:" + spec.target_namespace;
                    sqlite3_bind_text(st.st, 4, created_by.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(st.st, 5, note.c_str(), -1, SQLITE_TRANSIENT);
                    if (!StepDone(execution_db_, st.st, &db_error)) break;
                } else if (kind == "jobs") {
                    bool ok_id = false;
                    bool ok_set = false;
                    bool ok_parent = false;
                    const auto old_id = JsonExtractInt(execution_db_, line, "$.job_id", &ok_id);
                    const auto old_set = JsonExtractInt(execution_db_, line, "$.job_set_id", &ok_set);
                    const auto old_parent = JsonExtractInt(execution_db_, line, "$.parent_job_id", &ok_parent);
                    if (!ok_id || !ok_set) continue;
                    const auto new_id = map_id("job", old_id);
                    const auto new_set = map_id("job_set", old_set);
                    const auto new_parent = ok_parent ? map_id("job", old_parent) : 0;
                    if (new_id == 0 || new_set == 0 || (ok_parent && new_parent == 0)) break;

                    const auto base_fingerprint = JsonExtractText(execution_db_, line, "$.fingerprint", &ok_id);
                    const auto safe_fingerprint = Fnv1a64(spec.target_namespace + ":job:" + std::to_string(new_id) + ":" + base_fingerprint);

                    Statement st;
                    if (!Prepare(execution_db_,
                            "INSERT INTO exec_job(job_id,job_set_id,parent_job_id,program_kind,program_version,program_ref_kind,program_ref_id,fingerprint,priority,state,attempts,max_attempts,claimed_by_token,lease_expires_at_utc,queued_at_utc,started_at_utc,ended_at_utc,error_code,error_text) "
                            "VALUES(?1,?2,?3,json_extract(?4,'$.program_kind'),json_extract(?4,'$.program_version'),json_extract(?4,'$.program_ref_kind'),json_extract(?4,'$.program_ref_id'),?5,json_extract(?4,'$.priority'),json_extract(?4,'$.state'),json_extract(?4,'$.attempts'),json_extract(?4,'$.max_attempts'),json_extract(?4,'$.claimed_by_token'),json_extract(?4,'$.lease_expires_at_utc'),json_extract(?4,'$.queued_at_utc'),json_extract(?4,'$.started_at_utc'),json_extract(?4,'$.ended_at_utc'),json_extract(?4,'$.error_code'),json_extract(?4,'$.error_text'));",
                            &st,
                            &db_error)) {
                        break;
                    }
                    sqlite3_bind_int64(st.st, 1, new_id);
                    sqlite3_bind_int64(st.st, 2, new_set);
                    if (ok_parent) sqlite3_bind_int64(st.st, 3, new_parent); else sqlite3_bind_null(st.st, 3);
                    sqlite3_bind_text(st.st, 4, line.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(st.st, 5, safe_fingerprint.c_str(), -1, SQLITE_TRANSIENT);
                    if (!StepDone(execution_db_, st.st, &db_error)) break;
                    restored_jobs.push_back(new_id);
                } else if (kind == "job_events") {
                    bool ok_event = false;
                    bool ok_job = false;
                    const auto old_event = JsonExtractInt(execution_db_, line, "$.job_event_id", &ok_event);
                    const auto old_job = JsonExtractInt(execution_db_, line, "$.job_id", &ok_job);
                    if (!ok_event || !ok_job) continue;
                    const auto new_event = map_id("job_event", old_event);
                    const auto new_job = map_id("job", old_job);
                    if (new_event == 0 || new_job == 0) break;
                    Statement st;
                    if (!Prepare(execution_db_,
                            "INSERT INTO exec_job_event(job_event_id,job_id,event_kind,event_ts_utc,message,artifact_id) "
                            "VALUES(?1,?2,json_extract(?3,'$.event_kind'),json_extract(?3,'$.event_ts_utc'),json_extract(?3,'$.message'),json_extract(?3,'$.artifact_id'));",
                            &st,
                            &db_error)) break;
                    sqlite3_bind_int64(st.st, 1, new_event);
                    sqlite3_bind_int64(st.st, 2, new_job);
                    sqlite3_bind_text(st.st, 3, line.c_str(), -1, SQLITE_TRANSIENT);
                    if (!StepDone(execution_db_, st.st, &db_error)) break;
                } else if (kind == "workflow_instances") {
                    bool ok_id = false;
                    const auto old_id = JsonExtractInt(execution_db_, line, "$.workflow_instance_id", &ok_id);
                    bool ok_root = false;
                    const auto old_root = JsonExtractInt(execution_db_, line, "$.root_scope_id", &ok_root);
                    if (!ok_id) continue;
                    const auto new_id = map_id("workflow_instance", old_id);
                    const auto scope_kind = JsonExtractText(execution_db_, line, "$.root_scope_kind", &ok_id);
                    const auto new_root = ok_root && scope_kind == "job_set" ? map_id("job_set", old_root) : old_root;
                    if (new_id == 0 || (ok_root && scope_kind == "job_set" && new_root == 0)) break;
                    Statement st;
                    if (!Prepare(execution_db_,
                            "INSERT INTO exec_workflow_instance(workflow_instance_id,workflow_kind,state,root_scope_kind,root_scope_id,created_by,created_at_utc,started_at_utc,completed_at_utc,failure_code,failure_text) "
                            "VALUES(?1,json_extract(?2,'$.workflow_kind'),json_extract(?2,'$.state'),json_extract(?2,'$.root_scope_kind'),?3,?4,json_extract(?2,'$.created_at_utc'),json_extract(?2,'$.started_at_utc'),json_extract(?2,'$.completed_at_utc'),json_extract(?2,'$.failure_code'),json_extract(?2,'$.failure_text'));",
                            &st,
                            &db_error)) break;
                    sqlite3_bind_int64(st.st, 1, new_id);
                    sqlite3_bind_text(st.st, 2, line.c_str(), -1, SQLITE_TRANSIENT);
                    if (ok_root) sqlite3_bind_int64(st.st, 3, new_root); else sqlite3_bind_null(st.st, 3);
                    const auto created_by = spec.target_namespace + ":rehydrate";
                    sqlite3_bind_text(st.st, 4, created_by.c_str(), -1, SQLITE_TRANSIENT);
                    if (!StepDone(execution_db_, st.st, &db_error)) break;
                } else if (kind == "workflow_steps") {
                    bool ok_id = false;
                    bool ok_instance = false;
                    bool ok_set = false;
                    const auto old_id = JsonExtractInt(execution_db_, line, "$.workflow_step_id", &ok_id);
                    const auto old_instance = JsonExtractInt(execution_db_, line, "$.workflow_instance_id", &ok_instance);
                    const auto old_set = JsonExtractInt(execution_db_, line, "$.job_set_id", &ok_set);
                    if (!ok_id || !ok_instance) continue;
                    const auto new_id = map_id("workflow_step", old_id);
                    const auto new_instance = map_id("workflow_instance", old_instance);
                    const auto new_set = ok_set ? map_id("job_set", old_set) : 0;
                    if (new_id == 0 || new_instance == 0 || (ok_set && new_set == 0)) break;
                    Statement st;
                    if (!Prepare(execution_db_,
                            "INSERT INTO exec_workflow_step(workflow_step_id,workflow_instance_id,step_key,step_kind,state,guard_kind,guard_value,priority,attempts,max_attempts,job_set_id,input_ref_kind,input_ref_id,output_ref_kind,output_ref_id,blocked_reason,ready_at_utc,started_at_utc,completed_at_utc,failed_at_utc,created_at_utc) "
                            "VALUES(?1,?2,json_extract(?3,'$.step_key'),json_extract(?3,'$.step_kind'),json_extract(?3,'$.state'),json_extract(?3,'$.guard_kind'),json_extract(?3,'$.guard_value'),json_extract(?3,'$.priority'),json_extract(?3,'$.attempts'),json_extract(?3,'$.max_attempts'),?4,json_extract(?3,'$.input_ref_kind'),json_extract(?3,'$.input_ref_id'),json_extract(?3,'$.output_ref_kind'),json_extract(?3,'$.output_ref_id'),json_extract(?3,'$.blocked_reason'),json_extract(?3,'$.ready_at_utc'),json_extract(?3,'$.started_at_utc'),json_extract(?3,'$.completed_at_utc'),json_extract(?3,'$.failed_at_utc'),json_extract(?3,'$.created_at_utc'));",
                            &st,
                            &db_error)) break;
                    sqlite3_bind_int64(st.st, 1, new_id);
                    sqlite3_bind_int64(st.st, 2, new_instance);
                    sqlite3_bind_text(st.st, 3, line.c_str(), -1, SQLITE_TRANSIENT);
                    if (ok_set) sqlite3_bind_int64(st.st, 4, new_set); else sqlite3_bind_null(st.st, 4);
                    if (!StepDone(execution_db_, st.st, &db_error)) break;
                } else if (kind == "workflow_edges") {
                    bool ok_id = false;
                    bool ok_instance = false;
                    bool ok_from = false;
                    bool ok_to = false;
                    const auto old_id = JsonExtractInt(execution_db_, line, "$.workflow_edge_id", &ok_id);
                    const auto old_instance = JsonExtractInt(execution_db_, line, "$.workflow_instance_id", &ok_instance);
                    const auto old_from = JsonExtractInt(execution_db_, line, "$.from_step_id", &ok_from);
                    const auto old_to = JsonExtractInt(execution_db_, line, "$.to_step_id", &ok_to);
                    if (!ok_id || !ok_instance || !ok_from || !ok_to) continue;
                    const auto new_id = map_id("workflow_edge", old_id);
                    const auto new_instance = map_id("workflow_instance", old_instance);
                    const auto new_from = map_id("workflow_step", old_from);
                    const auto new_to = map_id("workflow_step", old_to);
                    if (new_id == 0 || new_instance == 0 || new_from == 0 || new_to == 0) break;
                    Statement st;
                    if (!Prepare(execution_db_,
                            "INSERT INTO exec_workflow_edge(workflow_edge_id,workflow_instance_id,from_step_id,to_step_id,condition_kind,condition_value,created_at_utc) "
                            "VALUES(?1,?2,?3,?4,json_extract(?5,'$.condition_kind'),json_extract(?5,'$.condition_value'),json_extract(?5,'$.created_at_utc'));",
                            &st,
                            &db_error)) break;
                    sqlite3_bind_int64(st.st, 1, new_id);
                    sqlite3_bind_int64(st.st, 2, new_instance);
                    sqlite3_bind_int64(st.st, 3, new_from);
                    sqlite3_bind_int64(st.st, 4, new_to);
                    sqlite3_bind_text(st.st, 5, line.c_str(), -1, SQLITE_TRANSIENT);
                    if (!StepDone(execution_db_, st.st, &db_error)) break;
                } else if (kind == "workflow_events") {
                    bool ok_id = false;
                    bool ok_instance = false;
                    bool ok_step = false;
                    const auto old_id = JsonExtractInt(execution_db_, line, "$.workflow_event_id", &ok_id);
                    const auto old_instance = JsonExtractInt(execution_db_, line, "$.workflow_instance_id", &ok_instance);
                    const auto old_step = JsonExtractInt(execution_db_, line, "$.workflow_step_id", &ok_step);
                    if (!ok_id || !ok_instance) continue;
                    const auto new_id = map_id("workflow_event", old_id);
                    const auto new_instance = map_id("workflow_instance", old_instance);
                    const auto new_step = ok_step ? map_id("workflow_step", old_step) : 0;
                    if (new_id == 0 || new_instance == 0 || (ok_step && new_step == 0)) break;
                    Statement st;
                    if (!Prepare(execution_db_,
                            "INSERT INTO exec_workflow_event(workflow_event_id,workflow_instance_id,workflow_step_id,event_kind,event_ts_utc,message,detail_ref_kind,detail_ref_id) "
                            "VALUES(?1,?2,?3,json_extract(?4,'$.event_kind'),json_extract(?4,'$.event_ts_utc'),json_extract(?4,'$.message'),json_extract(?4,'$.detail_ref_kind'),json_extract(?4,'$.detail_ref_id'));",
                            &st,
                            &db_error)) break;
                    sqlite3_bind_int64(st.st, 1, new_id);
                    sqlite3_bind_int64(st.st, 2, new_instance);
                    if (ok_step) sqlite3_bind_int64(st.st, 3, new_step); else sqlite3_bind_null(st.st, 3);
                    sqlite3_bind_text(st.st, 4, line.c_str(), -1, SQLITE_TRANSIENT);
                    if (!StepDone(execution_db_, st.st, &db_error)) break;
                } else if (kind == "triggers") {
                    bool ok_id = false;
                    bool ok_scope = false;
                    const auto old_id = JsonExtractInt(execution_db_, line, "$.trigger_id", &ok_id);
                    const auto old_scope_id = JsonExtractInt(execution_db_, line, "$.scope_id", &ok_scope);
                    if (!ok_id || !ok_scope) continue;
                    const auto new_id = map_id("trigger", old_id);
                    const auto scope_kind = JsonExtractText(execution_db_, line, "$.scope_kind", &ok_id);
                    const auto new_scope_id = scope_kind == "job" ? map_id("job", old_scope_id) : map_id("job_set", old_scope_id);
                    if (new_id == 0 || new_scope_id == 0) break;
                    Statement st;
                    if (!Prepare(execution_db_,
                            "INSERT INTO exec_trigger(trigger_id,scope_kind,scope_id,condition_kind,condition_value,action_kind,action_value,active,created_at_utc) "
                            "VALUES(?1,json_extract(?2,'$.scope_kind'),?3,json_extract(?2,'$.condition_kind'),json_extract(?2,'$.condition_value'),json_extract(?2,'$.action_kind'),json_extract(?2,'$.action_value'),json_extract(?2,'$.active'),json_extract(?2,'$.created_at_utc'));",
                            &st,
                            &db_error)) break;
                    sqlite3_bind_int64(st.st, 1, new_id);
                    sqlite3_bind_text(st.st, 2, line.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(st.st, 3, new_scope_id);
                    if (!StepDone(execution_db_, st.st, &db_error)) break;
                }
            }

            if (!db_error.empty()) {
                result.error = StructuredError{ "DB_INSERT_ERROR", "failed restoring execution rows", db_error }.ToJson();
                break;
            }
        }

        if (!result.error.has_value()) {
            std::string db_error2;
            const auto now_epoch = request.now_utc.time_since_epoch().count();
            std::string event_prefix = request.event_id_prefix.empty()
                ? ("rehydrate-" + std::to_string(request.rehydrate_request_id))
                : request.event_id_prefix;
            std::string correlation = request.correlation_id.empty()
                ? ("rehydrate-request-" + std::to_string(request.rehydrate_request_id))
                : request.correlation_id;
            std::string causation = request.causation_id.empty()
                ? correlation
                : request.causation_id;

            for (const auto job_id : restored_jobs) {
                const auto ev = event_prefix + ".job-restored." + std::to_string(job_id);
                if (!InsertExecutionOutbox(
                        execution_db_,
                        ev,
                        "Execution.JobRestored.v1",
                        "job",
                        std::to_string(job_id),
                        correlation,
                        causation,
                        now_epoch,
                        "job",
                        job_id,
                        &db_error2)) {
                    break;
                }
            }

            if (db_error2.empty()) {
                sqlite3_exec(execution_db_, "COMMIT;", nullptr, nullptr, nullptr);

                CompleteRehydrateCommand done{};
                done.rehydrate_request_id = request.rehydrate_request_id;
                done.status = "COMPLETED";
                done.completed_at_utc = request.now_utc;
                done.event_id = event_prefix + ".completed";
                done.correlation_id = correlation;
                done.causation_id = causation;
                std::string archive_error;
                if (!archive_service_->CompleteRehydrate(done, &archive_error)) {
                    result.error = StructuredError{ "ARCHIVE_COMPLETE_ERROR", "rehydrate complete status write failed", archive_error }.ToJson();
                } else {
                    result.success = true;
                    result.restored_job_count = static_cast<std::int64_t>(restored_jobs.size());
                }
            } else {
                result.error = StructuredError{ "OUTBOX_ERROR", "failed emitting job restored outbox rows", db_error2 }.ToJson();
            }
        }

        if (result.error.has_value()) {
            rollback();
        }
    }

    if (result.error.has_value()) {
        FailRehydrateCommand fail{};
        fail.rehydrate_request_id = request.rehydrate_request_id;
        fail.status = "FAILED";
        fail.completed_at_utc = request.now_utc;
        fail.error_text = *result.error;
        const auto prefix = request.event_id_prefix.empty()
            ? ("rehydrate-" + std::to_string(request.rehydrate_request_id))
            : request.event_id_prefix;
        fail.event_id = prefix + ".failed";
        fail.correlation_id = request.correlation_id.empty()
            ? ("rehydrate-request-" + std::to_string(request.rehydrate_request_id))
            : request.correlation_id;
        fail.causation_id = request.causation_id.empty() ? fail.correlation_id : request.causation_id;
        std::string archive_error;
        archive_service_->FailRehydrate(fail, &archive_error);
    }

    return result;
}

} // namespace simcore::db::archive
