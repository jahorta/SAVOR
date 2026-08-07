#include "RehydrateExecutor.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_set>

#include "../Common/Migrations/MigrationRunner.h"
#include "../Common/Events/OutboxEventIds.h"
#include "../Execution/ProgramDB/SeedProbe/SeedProbeJobSpec.h"
#include "../../SavorCore/Utils/Hash.h"

namespace savor::db::archive {
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

std::optional<std::string> DecodeHex(std::string_view hex) {
    if ((hex.size() % 2) != 0) return std::nullopt;
    auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
        if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
        return -1;
    };
    std::string bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const auto hi = nibble(hex[i]);
        const auto lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        bytes.push_back(static_cast<char>((hi << 4) | lo));
    }
    return bytes;
}

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

std::uint16_t ReadLe16(const std::string& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        static_cast<unsigned char>(bytes[offset])
        | (static_cast<unsigned char>(bytes[offset + 1]) << 8));
}

std::uint32_t ReadLe32(const std::string& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(ReadLe16(bytes, offset))
        | (static_cast<std::uint32_t>(ReadLe16(bytes, offset + 2)) << 16);
}

bool ExtractStoredZipEntry(
    const std::filesystem::path& zip_path,
    std::string_view entry_name,
    const std::filesystem::path& output_path,
    std::string* error_out) {
    std::string zip;
    if (!ReadFile(zip_path, &zip, error_out)) {
        return false;
    }

    std::size_t offset = 0;
    while (offset + 30 <= zip.size()) {
        const auto sig = ReadLe32(zip, offset);
        if (sig == 0x02014b50u || sig == 0x06054b50u) {
            break;
        }
        if (sig != 0x04034b50u) {
            if (error_out) *error_out = "unsupported zip layout";
            return false;
        }
        const auto compression = ReadLe16(zip, offset + 8);
        const auto compressed_size = ReadLe32(zip, offset + 18);
        const auto uncompressed_size = ReadLe32(zip, offset + 22);
        const auto name_len = ReadLe16(zip, offset + 26);
        const auto extra_len = ReadLe16(zip, offset + 28);
        const auto name_offset = offset + 30;
        const auto data_offset = name_offset + name_len + extra_len;
        if (data_offset + compressed_size > zip.size()) {
            if (error_out) *error_out = "zip entry is truncated";
            return false;
        }
        const std::string current_name = zip.substr(name_offset, name_len);
        if (current_name == entry_name) {
            if (compression != 0 || compressed_size != uncompressed_size) {
                if (error_out) *error_out = "state artifact zip entry is not stored";
                return false;
            }
            std::filesystem::create_directories(output_path.parent_path());
            std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                if (error_out) *error_out = "failed opening extracted state artifact path";
                return false;
            }
            out.write(zip.data() + data_offset, compressed_size);
            return out.good();
        }
        offset = data_offset + compressed_size;
    }

    if (error_out) *error_out = "state artifact zip entry not found";
    return false;
}

std::optional<std::int64_t> FindArtifactBySha(sqlite3* db, const std::string& sha, std::string* error_out) {
    Statement st;
    if (!Prepare(db, "SELECT artifact_id FROM state_artifact WHERE sha256=?1 LIMIT 1;", &st, error_out)) {
        return std::nullopt;
    }
    sqlite3_bind_text(st.st, 1, sha.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(st.st);
    if (rc == SQLITE_ROW) {
        return sqlite3_column_int64(st.st, 0);
    }
    if (rc != SQLITE_DONE && error_out) {
        *error_out = sqlite3_errmsg(db);
    }
    return std::nullopt;
}

std::optional<std::int64_t> FindSavestateForArtifact(sqlite3* db, std::int64_t artifact_id, const std::string& savestate_type, std::string* error_out) {
    Statement st;
    if (!Prepare(db, "SELECT savestate_id FROM state_savestate WHERE artifact_id=?1 AND savestate_type=?2 LIMIT 1;", &st, error_out)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, artifact_id);
    sqlite3_bind_text(st.st, 2, savestate_type.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(st.st);
    if (rc == SQLITE_ROW) {
        return sqlite3_column_int64(st.st, 0);
    }
    if (rc != SQLITE_DONE && error_out) {
        *error_out = sqlite3_errmsg(db);
    }
    return std::nullopt;
}

bool TableHasId(sqlite3* db, std::string_view table_name, std::string_view column_name, std::int64_t id, std::string* error_out) {
    if (db == nullptr) {
        return false;
    }
    const std::string sql = "SELECT 1 FROM " + std::string(table_name) + " WHERE " + std::string(column_name) + "=?1 LIMIT 1;";
    Statement st;
    if (!Prepare(db, sql.c_str(), &st, error_out)) {
        return false;
    }
    sqlite3_bind_int64(st.st, 1, id);
    return sqlite3_step(st.st) == SQLITE_ROW;
}

bool TableExists(sqlite3* db, std::string_view table_name) {
    if (db == nullptr) {
        return false;
    }
    Statement st;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1;", -1, &st.st, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(st.st, 1, table_name.data(), static_cast<int>(table_name.size()), SQLITE_TRANSIENT);
    return sqlite3_step(st.st) == SQLITE_ROW;
}

const StreamFile* FindStream(const PackageSpec& spec, std::string_view item_kind) {
    const auto it = std::find_if(spec.stream_files.begin(), spec.stream_files.end(), [&](const StreamFile& file) {
        return file.item_kind == item_kind;
    });
    return it == spec.stream_files.end() ? nullptr : &*it;
}

bool JsonPathPresent(sqlite3* db, std::string_view json, std::string_view path) {
    Statement st;
    if (db == nullptr
        || sqlite3_prepare_v2(db, "SELECT json_type(?1, ?2) IS NOT NULL;", -1, &st.st, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(st.st, 1, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 2, path.data(), static_cast<int>(path.size()), SQLITE_TRANSIENT);
    return sqlite3_step(st.st) == SQLITE_ROW && sqlite3_column_int(st.st, 0) != 0;
}

std::vector<std::string> ReadStreamLines(const PackageSpec& spec, const StreamFile& file) {
    std::vector<std::string> lines;
    std::ifstream in(spec.package_root / file.rel_path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
    }
    return lines;
}

void ValidateExecutionPackageContract(
    sqlite3* json_db,
    const PackageSpec& spec,
    std::vector<std::string>* blockers) {
    if (json_db == nullptr || blockers == nullptr) {
        return;
    }

    const std::array<std::string_view, 6> required_streams = {
        "job_sets",
        "worksets",
        "workset_dispatch_attempts",
        "jobs",
        "job_events",
        "job_cancellation_requests",
    };
    const bool has_execution = std::any_of(
        required_streams.begin(),
        required_streams.end(),
        [&](std::string_view kind) { return FindStream(spec, kind) != nullptr; });
    if (!has_execution) {
        return;
    }

    const std::array<std::string_view, 6> required_tables = {
        "exec_job_set",
        "exec_workset",
        "exec_workset_dispatch_attempt",
        "exec_job",
        "exec_job_event",
        "exec_job_cancellation_request",
    };
    for (const auto table : required_tables) {
        if (!TableExists(json_db, table)) {
            blockers->push_back("required_execution_table_missing:" + std::string(table));
        }
    }

    for (const auto kind : required_streams) {
        if (FindStream(spec, kind) == nullptr) {
            blockers->push_back("required_execution_stream_missing:" + std::string(kind));
        }
    }
    if (FindStream(spec, "temp_blobs") != nullptr
        || FindStream(spec, "temporary_result_blobs") != nullptr) {
        blockers->push_back("temporary_result_blob_stream_forbidden");
    }

    for (const auto& file : spec.stream_files) {
        if (file.rel_path.extension() != ".jsonl") {
            continue;
        }
        const auto lines = ReadStreamLines(spec, file);
        if (static_cast<int>(lines.size()) != file.row_count) {
            blockers->push_back(
                "stream_row_count_mismatch:" + file.item_kind
                + ":manifest=" + std::to_string(file.row_count)
                + ":actual=" + std::to_string(lines.size()));
        }
    }

    if (const auto* job_sets = FindStream(spec, "job_sets")) {
        for (const auto& line : ReadStreamLines(spec, *job_sets)) {
            bool ok = false;
            const auto state = JsonExtractText(json_db, line, "$.materialization_state", &ok);
            if (!ok || state != "WORKSET_PUBLICATION_COMPLETE") {
                blockers->push_back("job_set_publication_incomplete");
                break;
            }
        }
    }
    if (const auto* attempts = FindStream(spec, "workset_dispatch_attempts")) {
        for (const auto& line : ReadStreamLines(spec, *attempts)) {
            bool ok = false;
            const auto state = JsonExtractText(json_db, line, "$.state", &ok);
            if (!ok || state != "CLOSED") {
                blockers->push_back("active_workset_dispatch_in_package");
                break;
            }
        }
    }
    if (const auto* jobs = FindStream(spec, "jobs")) {
        std::unordered_map<std::int64_t, std::vector<std::int64_t>> ordinals_by_workset;
        for (const auto& line : ReadStreamLines(spec, *jobs)) {
            bool ok_state = false;
            const auto state = JsonExtractText(json_db, line, "$.state", &ok_state);
            if (!ok_state || state == "EXECUTION_FINISHED") {
                blockers->push_back("non_archivable_job_state_in_package");
                break;
            }
            bool has_blob_id = false;
            (void)JsonExtractInt(json_db, line, "$.worker_result_blob_id", &has_blob_id);
            if (has_blob_id) {
                blockers->push_back("temporary_result_blob_reference_forbidden");
                break;
            }
            bool ok_cancellation = false;
            const auto cancellation = JsonExtractText(
                json_db, line, "$.cancellation_state", &ok_cancellation);
            if (ok_cancellation && cancellation != "RESOLVED") {
                blockers->push_back("unresolved_job_cancellation_in_package");
                break;
            }
            if (!JsonPathPresent(json_db, line, "$.result_processing_attempts")
                || !JsonPathPresent(json_db, line, "$.result_processing_failures")
                || !JsonPathPresent(json_db, line, "$.cancellation_delivery_attempts")) {
                blockers->push_back("current_job_coordination_fields_missing");
                break;
            }
            bool ok_workset = false;
            bool ok_ordinal = false;
            const auto workset_id = JsonExtractInt(json_db, line, "$.workset_id", &ok_workset);
            const auto ordinal = JsonExtractInt(
                json_db, line, "$.workset_item_ordinal", &ok_ordinal);
            if (!ok_workset || !ok_ordinal || ordinal < 0) {
                blockers->push_back("published_job_membership_missing");
                break;
            }
            ordinals_by_workset[workset_id].push_back(ordinal);
        }

        if (const auto* worksets = FindStream(spec, "worksets")) {
            for (const auto& line : ReadStreamLines(spec, *worksets)) {
                bool ok_id = false;
                bool ok_count = false;
                const auto workset_id = JsonExtractInt(
                    json_db, line, "$.workset_id", &ok_id);
                const auto item_count = JsonExtractInt(
                    json_db, line, "$.item_count", &ok_count);
                if (!ok_id || !ok_count || item_count <= 0) {
                    blockers->push_back("published_workset_shape_invalid");
                    break;
                }
                auto ordinals = ordinals_by_workset[workset_id];
                std::sort(ordinals.begin(), ordinals.end());
                if (ordinals.size() != static_cast<std::size_t>(item_count)
                    || ordinals.front() != 0
                    || ordinals.back() != item_count - 1
                    || std::adjacent_find(ordinals.begin(), ordinals.end())
                        != ordinals.end()) {
                    blockers->push_back("published_workset_membership_inconsistent");
                    break;
                }
                ordinals_by_workset.erase(workset_id);
            }
            if (!ordinals_by_workset.empty()) {
                blockers->push_back("job_references_missing_workset_stream_row");
            }
        }
    }
    if (const auto* cancellations = FindStream(spec, "job_cancellation_requests")) {
        for (const auto& line : ReadStreamLines(spec, *cancellations)) {
            bool ok = false;
            const auto state = JsonExtractText(json_db, line, "$.state", &ok);
            if (!ok || state != "RESOLVED") {
                blockers->push_back("unresolved_cancellation_history_in_package");
                break;
            }
        }
    }
}

bool LoadPackageSpecFromManifest(
    sqlite3* json_db,
    const std::filesystem::path& manifest_path,
    std::string target_namespace,
    std::int64_t archive_package_id,
    PackageSpec* spec,
    std::string* error_out) {
    if (json_db == nullptr || spec == nullptr || manifest_path.empty()) {
        if (error_out) *error_out = "invalid package spec request";
        return false;
    }
    std::string manifest_text;
    if (!ReadFile(manifest_path, &manifest_text, error_out)) {
        return false;
    }

    PackageSpec loaded{};
    loaded.archive_package_id = archive_package_id;
    loaded.target_namespace = std::move(target_namespace);
    loaded.package_root = manifest_path.parent_path();
    bool ok_schema = false;
    bool ok_catalog = false;
    loaded.schema_version = JsonExtractInt(json_db, manifest_text, "$.schema_version", &ok_schema);
    loaded.event_catalog_version = JsonExtractInt(json_db, manifest_text, "$.event_catalog_version", &ok_catalog);
    if (!ok_schema || !ok_catalog) {
        if (error_out) *error_out = "schema_version/event_catalog_version missing";
        return false;
    }

    Statement st_files;
    if (sqlite3_prepare_v2(
            json_db,
            "SELECT json_extract(value,'$.item_kind'), json_extract(value,'$.path'), json_extract(value,'$.checksum'), json_extract(value,'$.row_count') "
            "FROM json_each(json_extract(?1, '$.files'));",
            -1,
            &st_files.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(json_db);
        return false;
    }
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
            loaded.stream_files.push_back(std::move(f));
        } else if (f.item_kind == "state_savestate_zip") {
            loaded.stream_files.push_back(std::move(f));
        }
    }
    *spec = std::move(loaded);
    return true;
}

bool LoadPackageSpecByPackageId(
    sqlite3* archive_db,
    sqlite3* json_db,
    std::int64_t archive_package_id,
    std::string target_namespace,
    PackageSpec* spec,
    std::string* error_out) {
    Statement st;
    if (!Prepare(archive_db, "SELECT manifest_path FROM ar_archive_package WHERE archive_package_id=?1;", &st, error_out)) {
        return false;
    }
    sqlite3_bind_int64(st.st, 1, archive_package_id);
    if (sqlite3_step(st.st) != SQLITE_ROW || sqlite3_column_text(st.st, 0) == nullptr) {
        if (error_out) *error_out = "archive package not found";
        return false;
    }
    return LoadPackageSpecFromManifest(
        json_db,
        std::filesystem::path(reinterpret_cast<const char*>(sqlite3_column_text(st.st, 0))),
        std::move(target_namespace),
        archive_package_id,
        spec,
        error_out);
}

struct IdRule {
    const char* item_kind;
    const char* json_id;
    const char* map_kind;
    const char* table_name;
    const char* column_name;
    sqlite3* db;
};

void ScanCollisionRules(
    const PackageSpec& spec,
    const std::vector<IdRule>& rules,
    std::vector<std::string>* blockers) {
    if (blockers == nullptr) {
        return;
    }
    std::unordered_set<std::string> seen;
    for (const auto& rule : rules) {
        if (rule.db == nullptr || !TableExists(rule.db, rule.table_name)) {
            continue;
        }
        const auto* stream = FindStream(spec, rule.item_kind);
        if (stream == nullptr || stream->rel_path.extension() != ".jsonl") {
            continue;
        }
        std::string error;
        for (const auto& line : ReadStreamLines(spec, *stream)) {
            bool ok = false;
            const auto old_id = JsonExtractInt(rule.db, line, rule.json_id, &ok);
            if (!ok) {
                continue;
            }
            const auto new_id = AllocateId(spec.target_namespace, rule.map_kind, old_id);
            const auto key = std::string(rule.table_name) + ":" + std::to_string(new_id);
            if (seen.insert(key).second && TableHasId(rule.db, rule.table_name, rule.column_name, new_id, &error)) {
                blockers->push_back(
                    "target_id_collision:" + std::string(rule.table_name) + "." + rule.column_name
                    + ":" + std::to_string(new_id));
            }
        }
    }
}

bool InsertExecutionOutbox(
    sqlite3* execution_db,
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
                execution_db,
                "Execution",
                event_type,
                payload_ref_kind,
                payload_ref_id,
                &event_id,
                error_out)) {
            return false;
        }

        Statement st;
        if (!Prepare(execution_db,
            "INSERT INTO exec_outbox_message(event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id) "
            "VALUES(?1,?2,1,'Execution',?3,?4,?5,?6,?7,?8,?9);",
            &st,
            error_out)) {
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
        if (!outbox::IsUniqueConstraint(execution_db)) {
            if (error_out) *error_out = sqlite3_errmsg(execution_db);
            return false;
        }
    }
    if (error_out) *error_out = "failed to generate a unique execution outbox event id";
    return false;
}

} // namespace

SqliteRehydrateExecutor::SqliteRehydrateExecutor(
    sqlite3* execution_db,
    sqlite3* archive_db,
    savor::db::IArchiveDb* archive_service,
    std::filesystem::path archive_store_root,
    sqlite3* state_db,
    sqlite3* analysis_db)
    : execution_db_(execution_db)
    , archive_db_(archive_db)
    , state_db_(state_db)
    , analysis_db_(analysis_db)
    , archive_service_(archive_service)
    , archive_store_root_(std::move(archive_store_root)) {
}

RehydratePackagePreviewResult SqliteRehydrateExecutor::PreviewPackage(const RehydratePackagePreviewRequest& request) {
    RehydratePackagePreviewResult result{};
    if (execution_db_ == nullptr || archive_db_ == nullptr || request.archive_package_id <= 0) {
        result.error = "invalid rehydrate preview request";
        result.blocking_reasons.push_back(result.error.value());
        return result;
    }

    PackageSpec spec{};
    std::string error;
    auto ns = request.target_namespace.empty()
        ? ("preview-" + std::to_string(request.archive_package_id))
        : request.target_namespace;
    if (!LoadPackageSpecByPackageId(archive_db_, execution_db_, request.archive_package_id, ns, &spec, &error)) {
        result.error = error.empty() ? "failed loading archive package" : error;
        result.blocking_reasons.push_back(*result.error);
        return result;
    }
    result.namespace_token = spec.target_namespace;

    for (const auto& file : spec.stream_files) {
        ++result.manifest_file_count;
        result.manifest_row_total += file.row_count;
        if (file.item_kind == "workflow_instances") {
            result.workflow_count += file.row_count;
        } else if (file.item_kind == "jobs") {
            result.job_count += file.row_count;
        } else if (file.item_kind == "state_savestates") {
            result.state_savestate_count += file.row_count;
        } else if (file.item_kind == "state_savestate_zip") {
            result.savestate_zip_entry_count += file.row_count;
        }
        if (file.item_kind.rfind("analysis_", 0) == 0) {
            result.analysis_row_count += file.row_count;
        } else if (file.item_kind != "state_savestate_zip"
            && file.item_kind.rfind("state_", 0) != 0
            && file.item_kind.rfind("ui_", 0) != 0) {
            result.execution_row_count += file.row_count;
        }
    }

    if (result.analysis_row_count > 0 && analysis_db_ == nullptr) {
        result.blocking_reasons.push_back("analysis_db_missing");
    }
    if (result.state_savestate_count > 0 && state_db_ == nullptr) {
        result.blocking_reasons.push_back("state_db_missing");
    }

    ValidateExecutionPackageContract(execution_db_, spec, &result.blocking_reasons);

    std::vector<IdRule> rules = {
        {"job_sets", "$.job_set_id", "job_set", "exec_job_set", "job_set_id", execution_db_},
        {"worksets", "$.workset_id", "workset", "exec_workset", "workset_id", execution_db_},
        {"workset_dispatch_attempts", "$.dispatch_attempt_id", "workset_dispatch_attempt", "exec_workset_dispatch_attempt", "dispatch_attempt_id", execution_db_},
        {"jobs", "$.job_id", "job", "exec_job", "job_id", execution_db_},
        {"job_events", "$.job_event_id", "job_event", "exec_job_event", "job_event_id", execution_db_},
        {"job_cancellation_requests", "$.cancellation_request_id", "job_cancellation_request", "exec_job_cancellation_request", "cancellation_request_id", execution_db_},
        {"workflow_instances", "$.workflow_instance_id", "workflow_instance", "exec_workflow_instance", "workflow_instance_id", execution_db_},
        {"workflow_unit_activations", "$.workflow_unit_activation_id", "workflow_unit_activation", "exec_workflow_unit_activation", "workflow_unit_activation_id", execution_db_},
        {"workflow_unit_activation_edges", "$.workflow_unit_activation_edge_id", "workflow_unit_activation_edge", "exec_workflow_unit_activation_edge", "workflow_unit_activation_edge_id", execution_db_},
        {"workflow_steps", "$.workflow_step_id", "workflow_step", "exec_workflow_step", "workflow_step_id", execution_db_},
        {"workflow_edges", "$.workflow_edge_id", "workflow_edge", "exec_workflow_edge", "workflow_edge_id", execution_db_},
        {"workflow_events", "$.workflow_event_id", "workflow_event", "exec_workflow_event", "workflow_event_id", execution_db_},
        {"workflow_step_outputs", "$.workflow_step_output_id", "workflow_step_output", "exec_workflow_step_output", "workflow_step_output_id", execution_db_},
        {"workflow_instance_input_bindings", "$.workflow_instance_input_binding_id", "workflow_instance_input_binding", "exec_workflow_instance_input_binding", "workflow_instance_input_binding_id", execution_db_},
        {"workflow_instance_arguments", "$.workflow_instance_argument_id", "workflow_instance_argument", "exec_workflow_instance_argument", "workflow_instance_argument_id", execution_db_},
        {"analysis_battle_sets", "$.battle_set_id", "analysis_battle_set", "ab_battle_set", "battle_set_id", analysis_db_},
        {"analysis_seed_candidates", "$.seed_candidate_id", "analysis_seed_candidate", "ab_seed_candidate", "seed_candidate_id", analysis_db_},
        {"analysis_battle_advancement_pools", "$.battle_advancement_pool_id", "analysis_battle_advancement_pool", "ab_battle_advancement_pool", "battle_advancement_pool_id", analysis_db_},
        {"analysis_turn_waves", "$.wave_id", "analysis_turn_wave", "ab_turn_wave", "wave_id", analysis_db_},
        {"analysis_battle_context_probes", "$.context_probe_id", "analysis_battle_context_probe", "ab_battle_context_probe", "context_probe_id", analysis_db_},
        {"analysis_battle_turn_jobs", "$.turn_job_id", "analysis_battle_turn_job", "ab_turn_job", "turn_job_id", analysis_db_},
        {"analysis_battle_advancement_decisions", "$.battle_advancement_decision_id", "analysis_battle_advancement_decision", "ab_battle_advancement_decision", "battle_advancement_decision_id", analysis_db_},
        {"analysis_manual_followups", "$.manual_followup_id", "analysis_manual_followup", "ab_manual_followup", "manual_followup_id", analysis_db_},
        {"analysis_battle_completions", "$.battle_completion_id", "analysis_battle_completion", "ab_battle_completion", "battle_completion_id", analysis_db_},
        {"analysis_battle_results", "$.battle_results_id", "analysis_battle_results", "ab_battle_results", "battle_results_id", analysis_db_},
        {"analysis_seed_probe_sets", "$.probe_set_id", "analysis_seed_probe_set", "sp_probe_set", "probe_set_id", analysis_db_},
        {"analysis_input_sets", "$.input_set_id", "analysis_input_set", "an_input_set", "input_set_id", analysis_db_},
        {"analysis_seed_probe_axis_xy", "$.axis_xy_id", "analysis_seed_probe_axis_xy", "sp_axis_xy", "axis_xy_id", analysis_db_},
        {"analysis_seed_probe_input_frames", "$.input_frame_id", "analysis_seed_probe_input_frame", "sp_input_frame", "input_frame_id", analysis_db_},
        {"analysis_seed_probe_runs", "$.probe_run_id", "analysis_seed_probe_run", "sp_probe_run", "probe_run_id", analysis_db_},
        {"analysis_seed_probe_results", "$.probe_result_id", "analysis_seed_probe_result", "sp_probe_result", "probe_result_id", analysis_db_},
        {"analysis_seed_probe_encounter_projections", "$.encounter_projection_id", "analysis_seed_probe_encounter_projection", "sp_encounter_projection", "encounter_projection_id", analysis_db_},
        {"analysis_tas_movie_checkpoint_sterilization_requests", "$.sterilization_request_id", "analysis_tas_movie_checkpoint_sterilization_request", "tmv_checkpoint_sterilization_request", "sterilization_request_id", analysis_db_},
        {"analysis_tas_movie_checkpoint_sterilization_attempts", "$.sterilization_attempt_id", "analysis_tas_movie_checkpoint_sterilization_attempt", "tmv_checkpoint_sterilization_attempt", "sterilization_attempt_id", analysis_db_},
    };
    ScanCollisionRules(spec, rules, &result.blocking_reasons);

    result.success = result.blocking_reasons.empty();
    return result;
}

RehydrateExecutionResult SqliteRehydrateExecutor::Execute(const RehydrateExecutionRequest& request) {
    RehydrateExecutionResult result{};
    if (execution_db_ == nullptr || archive_db_ == nullptr || archive_service_ == nullptr) {
        result.error = StructuredError{ "DEPENDENCY_NULL", "rehydrate dependencies are missing", "database/service dependency is null" }.ToJson();
        EmitArchiveProgress(
            request.progress_sink,
            ArchiveOperationPhase::Failed,
            "Rehydrate dependencies are missing",
            0,
            0,
            false);
        return result;
    }
    if (request.rehydrate_request_id <= 0) {
        result.error = StructuredError{ "INVALID_REQUEST", "rehydrate_request_id must be positive", "invalid request id" }.ToJson();
        EmitArchiveProgress(
            request.progress_sink,
            ArchiveOperationPhase::Failed,
            "Invalid rehydrate request",
            0,
            0,
            false);
        return result;
    }

    EmitArchiveProgress(
        request.progress_sink,
        ArchiveOperationPhase::Previewing,
        "Reading rehydrate request");
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
            EmitArchiveProgress(
                request.progress_sink,
                ArchiveOperationPhase::Failed,
                "Failed reading rehydrate request",
                0,
                0,
                false);
            return result;
        }
        sqlite3_bind_int64(st.st, 1, request.rehydrate_request_id);
        if (sqlite3_step(st.st) != SQLITE_ROW) {
            result.error = StructuredError{ "REQUEST_NOT_FOUND", "rehydrate request missing", "request row not found" }.ToJson();
            EmitArchiveProgress(
                request.progress_sink,
                ArchiveOperationPhase::Failed,
                "Rehydrate request not found",
                0,
                0,
                false);
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

    EmitArchiveProgress(
        request.progress_sink,
        ArchiveOperationPhase::VerifyingPackage,
        "Verifying archive package before rehydrate");
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

    if (!result.error.has_value()) {
        auto preflight = PreviewPackage({
            .archive_package_id = spec.archive_package_id,
            .target_namespace = spec.target_namespace,
        });
        if (!preflight.success) {
            std::ostringstream detail;
            for (std::size_t i = 0; i < preflight.blocking_reasons.size(); ++i) {
                if (i != 0) detail << ';';
                detail << preflight.blocking_reasons[i];
            }
            if (detail.str().empty() && preflight.error.has_value()) {
                detail << *preflight.error;
            }
            result.error = StructuredError{ "REHYDRATE_PREFLIGHT_FAILED", "rehydrate preflight failed", detail.str() }.ToJson();
        }
    }

    std::vector<std::int64_t> restored_jobs;
    std::unordered_map<std::string, std::unordered_map<std::int64_t, std::int64_t>> id_map;
    std::vector<std::string> pending_state_derivation_lines;
    std::vector<std::int64_t> inserted_tas_movie_root_ids;
    std::vector<std::int64_t> inserted_tas_movie_tree_ids;

    auto map_existing_id = [&](std::string_view map_kind, std::int64_t old_id, std::int64_t new_id, std::string* error_out) -> bool {
        id_map[std::string(map_kind)][old_id] = new_id;
        return InsertMap(archive_db_, request.rehydrate_request_id, map_kind, old_id, new_id, error_out);
    };

    if (!result.error.has_value() && state_db_ != nullptr) {
        EmitArchiveProgress(
            request.progress_sink,
            ArchiveOperationPhase::ExportingRows,
            "Rehydrating State DB savestates");
        const auto artifact_file = std::find_if(spec.stream_files.begin(), spec.stream_files.end(), [](const StreamFile& f) {
            return f.item_kind == "state_artifacts";
        });
        const auto savestate_file = std::find_if(spec.stream_files.begin(), spec.stream_files.end(), [](const StreamFile& f) {
            return f.item_kind == "state_savestates";
        });
        const auto derivation_file = std::find_if(spec.stream_files.begin(), spec.stream_files.end(), [](const StreamFile& f) {
            return f.item_kind == "state_savestate_derivations";
        });
        const auto tas_movie_root_file = std::find_if(spec.stream_files.begin(), spec.stream_files.end(), [](const StreamFile& f) {
            return f.item_kind == "state_tas_movie_roots";
        });
        const auto tas_movie_tree_file = std::find_if(spec.stream_files.begin(), spec.stream_files.end(), [](const StreamFile& f) {
            return f.item_kind == "state_tas_movie_trees";
        });
        if (artifact_file != spec.stream_files.end() || savestate_file != spec.stream_files.end()
            || tas_movie_root_file != spec.stream_files.end() || tas_movie_tree_file != spec.stream_files.end()) {
            sqlite3_exec(state_db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
            std::string state_error;
            const auto extracted_root = spec.package_root / "rehydrated-savestates" / spec.target_namespace;
            const auto zip_path = spec.package_root / "savestates.zip";

            if (artifact_file != spec.stream_files.end()) {
                std::ifstream in(spec.package_root / artifact_file->rel_path, std::ios::binary);
                std::string line;
                while (std::getline(in, line)) {
                    if (line.empty()) continue;
                    bool ok_artifact = false;
                    bool ok_sha = false;
                    bool ok_file_ext = false;
                    bool ok_artifact_kind = false;
                    const auto old_artifact_id = JsonExtractInt(state_db_, line, "$.artifact_id", &ok_artifact);
                    const auto sha = JsonExtractText(state_db_, line, "$.sha256", &ok_sha);
                    const auto file_ext = JsonExtractText(state_db_, line, "$.file_ext", &ok_file_ext);
                    const auto artifact_kind = JsonExtractText(state_db_, line, "$.artifact_kind", &ok_artifact_kind);
                    if (!ok_artifact || !ok_sha || sha.empty()) continue;

                    auto existing = FindArtifactBySha(state_db_, sha, &state_error);
                    std::int64_t new_artifact_id = existing.value_or(0);
                    if (!state_error.empty()) break;
                    if (new_artifact_id == 0) {
                        const auto extension = SafeArchiveExtension(
                            ok_artifact_kind ? artifact_kind : std::string_view{},
                            ok_file_ext ? file_ext : std::string_view{});
                        const auto entry_name = sha + extension;
                        const auto output_path = extracted_root / entry_name;
                        if (!ExtractStoredZipEntry(zip_path, entry_name, output_path, &state_error)) break;
                        try {
                            const auto actual_sha = hash::sha256_of_file(output_path.string());
                            if (actual_sha != sha) {
                                state_error = "extracted savestate sha256 mismatch for " + sha;
                                break;
                            }
                        } catch (const std::exception& ex) {
                            state_error = ex.what();
                            break;
                        }

                        Statement insert_artifact;
                        if (!Prepare(
                                state_db_,
                                "INSERT INTO state_artifact(sha256,size_bytes,compression_kind,filename,file_ext,artifact_kind,created_at_utc) "
                                "VALUES(?1,json_extract(?2,'$.size_bytes'),json_extract(?2,'$.compression_kind'),?3,json_extract(?2,'$.file_ext'),json_extract(?2,'$.artifact_kind'),json_extract(?2,'$.created_at_utc'));",
                                &insert_artifact,
                                &state_error)) break;
                        sqlite3_bind_text(insert_artifact.st, 1, sha.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(insert_artifact.st, 2, line.c_str(), -1, SQLITE_TRANSIENT);
                        const auto filename = output_path.string();
                        sqlite3_bind_text(insert_artifact.st, 3, filename.c_str(), -1, SQLITE_TRANSIENT);
                        if (!StepDone(state_db_, insert_artifact.st, &state_error)) break;
                        new_artifact_id = sqlite3_last_insert_rowid(state_db_);
                    }
                    if (!map_existing_id("state_artifact", old_artifact_id, new_artifact_id, &state_error)) break;
                }
            }

            if (state_error.empty() && savestate_file != spec.stream_files.end()) {
                std::ifstream in(spec.package_root / savestate_file->rel_path, std::ios::binary);
                std::string line;
                while (std::getline(in, line)) {
                    if (line.empty()) continue;
                    bool ok_savestate = false;
                    bool ok_artifact = false;
                    bool ok_type = false;
                    const auto old_savestate_id = JsonExtractInt(state_db_, line, "$.savestate_id", &ok_savestate);
                    const auto old_artifact_id = JsonExtractInt(state_db_, line, "$.artifact_id", &ok_artifact);
                    bool ok_dtm = false;
                    const auto old_dtm_artifact_id = JsonExtractInt(
                        state_db_, line, "$.dtm_artifact_id", &ok_dtm);
                    const auto savestate_type = JsonExtractText(state_db_, line, "$.savestate_type", &ok_type);
                    if (!ok_savestate || !ok_artifact || !ok_type) continue;
                    const auto artifact_it = id_map["state_artifact"].find(old_artifact_id);
                    if (artifact_it == id_map["state_artifact"].end()) {
                        state_error = "artifact mapping missing for archived savestate";
                        break;
                    }
                    std::optional<std::int64_t> mapped_dtm;
                    if (ok_dtm) {
                        const auto dtm_it = id_map["state_artifact"].find(
                            old_dtm_artifact_id);
                        if (dtm_it == id_map["state_artifact"].end()) {
                            state_error = "DTM artifact mapping missing for archived movie-paired savestate";
                            break;
                        }
                        mapped_dtm = dtm_it->second;
                    }

                    auto existing = FindSavestateForArtifact(state_db_, artifact_it->second, savestate_type, &state_error);
                    std::int64_t new_savestate_id = existing.value_or(0);
                    if (!state_error.empty()) break;
                    if (new_savestate_id == 0) {
                        Statement insert_savestate;
                        if (!Prepare(
                                state_db_,
                                "INSERT INTO state_savestate(artifact_id,savestate_type,note,is_complete,created_at_utc,playback_state,dtm_artifact_id) "
                                "VALUES(?1,json_extract(?2,'$.savestate_type'),json_extract(?2,'$.note'),json_extract(?2,'$.is_complete'),json_extract(?2,'$.created_at_utc'),json_extract(?2,'$.playback_state'),?3);",
                                &insert_savestate,
                                &state_error)) break;
                        sqlite3_bind_int64(insert_savestate.st, 1, artifact_it->second);
                        sqlite3_bind_text(insert_savestate.st, 2, line.c_str(), -1, SQLITE_TRANSIENT);
                        if (mapped_dtm)
                            sqlite3_bind_int64(insert_savestate.st, 3, *mapped_dtm);
                        else
                            sqlite3_bind_null(insert_savestate.st, 3);
                        if (!StepDone(state_db_, insert_savestate.st, &state_error)) break;
                        new_savestate_id = sqlite3_last_insert_rowid(state_db_);
                    }
                    if (!map_existing_id("state_savestate", old_savestate_id, new_savestate_id, &state_error)) break;
                }
            }

            if (state_error.empty() && tas_movie_root_file != spec.stream_files.end()) {
                std::ifstream in(spec.package_root / tas_movie_root_file->rel_path, std::ios::binary);
                std::string line;
                while (std::getline(in, line)) {
                    if (line.empty()) continue;
                    bool ok_id = false, ok_source = false, ok_dtm = false, ok_itinerary = false, ok_savestate = false;
                    const auto old_id = JsonExtractInt(state_db_, line, "$.tas_movie_root_id", &ok_id);
                    const auto old_source = JsonExtractInt(state_db_, line, "$.source_dtm_artifact_id", &ok_source);
                    const auto old_dtm = JsonExtractInt(state_db_, line, "$.dtm_artifact_id", &ok_dtm);
                    const auto old_itinerary = JsonExtractInt(state_db_, line, "$.itinerary_artifact_id", &ok_itinerary);
                    const auto old_savestate = JsonExtractInt(state_db_, line, "$.checkpoint_savestate_id", &ok_savestate);
                    if (!ok_id || !ok_source || !ok_dtm || !ok_itinerary || !ok_savestate) continue;
                    const auto source = id_map["state_artifact"].find(old_source);
                    const auto dtm = id_map["state_artifact"].find(old_dtm);
                    const auto itinerary = id_map["state_artifact"].find(old_itinerary);
                    const auto savestate = id_map["state_savestate"].find(old_savestate);
                    if (source == id_map["state_artifact"].end()
                        || dtm == id_map["state_artifact"].end()
                        || itinerary == id_map["state_artifact"].end()
                        || savestate == id_map["state_savestate"].end()) {
                        state_error = "TAS movie root artifact or checkpoint mapping is missing";
                        break;
                    }
                    Statement find;
                    if (!Prepare(state_db_,
                            "SELECT tas_movie_root_id,dtm_artifact_id,itinerary_artifact_id,checkpoint_savestate_id,required_final_breakpoint_pc "
                            "FROM state_tas_movie_root WHERE source_dtm_artifact_id=?1 AND rtc_value=json_extract(?2,'$.rtc_value');",
                            &find, &state_error)) break;
                    sqlite3_bind_int64(find.st, 1, source->second);
                    sqlite3_bind_text(find.st, 2, line.c_str(), -1, SQLITE_TRANSIENT);
                    std::int64_t new_id = 0;
                    if (sqlite3_step(find.st) == SQLITE_ROW) {
                        if (sqlite3_column_int64(find.st, 1) != dtm->second
                            || sqlite3_column_int64(find.st, 2) != itinerary->second
                            || sqlite3_column_int64(find.st, 3) != savestate->second
                            || sqlite3_column_int64(find.st, 4) != JsonExtractInt(state_db_, line, "$.required_final_breakpoint_pc", &ok_id)) {
                            state_error = "rehydrated TAS movie root conflicts with an existing immutable root";
                            break;
                        }
                        new_id = sqlite3_column_int64(find.st, 0);
                    } else {
                        Statement insert;
                        if (!Prepare(state_db_,
                                "INSERT INTO state_tas_movie_root(source_dtm_artifact_id,dtm_artifact_id,rtc_value,itinerary_artifact_id,"
                                "required_final_breakpoint_pc,checkpoint_savestate_id,source_context_kind,source_context_id,created_at_utc) "
                                "VALUES(?1,?2,json_extract(?5,'$.rtc_value'),?3,json_extract(?5,'$.required_final_breakpoint_pc'),?4,"
                                "json_extract(?5,'$.source_context_kind'),json_extract(?5,'$.source_context_id'),json_extract(?5,'$.created_at_utc'));",
                                &insert, &state_error)) break;
                        sqlite3_bind_int64(insert.st, 1, source->second);
                        sqlite3_bind_int64(insert.st, 2, dtm->second);
                        sqlite3_bind_int64(insert.st, 3, itinerary->second);
                        sqlite3_bind_int64(insert.st, 4, savestate->second);
                        sqlite3_bind_text(insert.st, 5, line.c_str(), -1, SQLITE_TRANSIENT);
                        if (!StepDone(state_db_, insert.st, &state_error)) break;
                        new_id = sqlite3_last_insert_rowid(state_db_);
                        inserted_tas_movie_root_ids.push_back(new_id);
                    }
                    if (!map_existing_id("state_tas_movie_root", old_id, new_id, &state_error)) break;
                }
            }

            if (state_error.empty() && tas_movie_tree_file != spec.stream_files.end()) {
                std::ifstream in(spec.package_root / tas_movie_tree_file->rel_path, std::ios::binary);
                std::string line;
                while (std::getline(in, line)) {
                    if (line.empty()) continue;
                    bool ok_id = false, ok_root = false, ok_parent = false, ok_dtm = false, ok_itinerary = false, ok_savestate = false;
                    const auto old_id = JsonExtractInt(state_db_, line, "$.tas_movie_tree_id", &ok_id);
                    const auto old_root = JsonExtractInt(state_db_, line, "$.tas_movie_root_id", &ok_root);
                    const auto old_parent = JsonExtractInt(state_db_, line, "$.parent_tas_movie_tree_id", &ok_parent);
                    const auto old_dtm = JsonExtractInt(state_db_, line, "$.dtm_artifact_id", &ok_dtm);
                    const auto old_itinerary = JsonExtractInt(state_db_, line, "$.itinerary_artifact_id", &ok_itinerary);
                    const auto old_savestate = JsonExtractInt(state_db_, line, "$.checkpoint_savestate_id", &ok_savestate);
                    if (!ok_id || !ok_root || !ok_dtm || !ok_itinerary || !ok_savestate) continue;
                    const auto root = id_map["state_tas_movie_root"].find(old_root);
                    const auto dtm = id_map["state_artifact"].find(old_dtm);
                    const auto itinerary = id_map["state_artifact"].find(old_itinerary);
                    const auto savestate = id_map["state_savestate"].find(old_savestate);
                    const auto parent = ok_parent ? id_map["state_tas_movie_tree"].find(old_parent) : id_map["state_tas_movie_tree"].end();
                    if (root == id_map["state_tas_movie_root"].end()
                        || dtm == id_map["state_artifact"].end()
                        || itinerary == id_map["state_artifact"].end()
                        || savestate == id_map["state_savestate"].end()
                        || (ok_parent && parent == id_map["state_tas_movie_tree"].end())) {
                        state_error = "TAS movie tree lineage, artifact, or checkpoint mapping is missing";
                        break;
                    }
                    Statement find;
                    if (!Prepare(state_db_, "SELECT tas_movie_tree_id,tas_movie_root_id,parent_tas_movie_tree_id,itinerary_artifact_id,checkpoint_savestate_id,required_final_breakpoint_pc FROM state_tas_movie_trees WHERE dtm_artifact_id=?1;", &find, &state_error)) break;
                    sqlite3_bind_int64(find.st, 1, dtm->second);
                    std::int64_t new_id = 0;
                    if (sqlite3_step(find.st) == SQLITE_ROW) {
                        const bool parent_matches = ok_parent
                            ? sqlite3_column_type(find.st, 2) != SQLITE_NULL && sqlite3_column_int64(find.st, 2) == parent->second
                            : sqlite3_column_type(find.st, 2) == SQLITE_NULL;
                        if (sqlite3_column_int64(find.st, 1) != root->second || !parent_matches
                            || sqlite3_column_int64(find.st, 3) != itinerary->second
                            || sqlite3_column_int64(find.st, 4) != savestate->second
                            || sqlite3_column_int64(find.st, 5) != JsonExtractInt(state_db_, line, "$.required_final_breakpoint_pc", &ok_id)) {
                            state_error = "rehydrated TAS movie tree conflicts with an existing immutable tree";
                            break;
                        }
                        new_id = sqlite3_column_int64(find.st, 0);
                    } else {
                        Statement insert;
                        if (!Prepare(state_db_,
                                "INSERT INTO state_tas_movie_trees(tas_movie_root_id,parent_tas_movie_tree_id,dtm_artifact_id,itinerary_artifact_id,"
                                "required_final_breakpoint_pc,checkpoint_savestate_id,source_context_kind,source_context_id,created_at_utc) "
                                "VALUES(?1,?2,?3,?4,json_extract(?6,'$.required_final_breakpoint_pc'),?5,json_extract(?6,'$.source_context_kind'),"
                                "json_extract(?6,'$.source_context_id'),json_extract(?6,'$.created_at_utc'));",
                                &insert, &state_error)) break;
                        sqlite3_bind_int64(insert.st, 1, root->second);
                        if (ok_parent) sqlite3_bind_int64(insert.st, 2, parent->second); else sqlite3_bind_null(insert.st, 2);
                        sqlite3_bind_int64(insert.st, 3, dtm->second);
                        sqlite3_bind_int64(insert.st, 4, itinerary->second);
                        sqlite3_bind_int64(insert.st, 5, savestate->second);
                        sqlite3_bind_text(insert.st, 6, line.c_str(), -1, SQLITE_TRANSIENT);
                        if (!StepDone(state_db_, insert.st, &state_error)) break;
                        new_id = sqlite3_last_insert_rowid(state_db_);
                        inserted_tas_movie_tree_ids.push_back(new_id);
                    }
                    if (!map_existing_id("state_tas_movie_tree", old_id, new_id, &state_error)) break;
                }
            }

            if (state_error.empty() && derivation_file != spec.stream_files.end()) {
                std::ifstream in(spec.package_root / derivation_file->rel_path, std::ios::binary);
                std::string line;
                while (std::getline(in, line)) {
                    if (!line.empty()) pending_state_derivation_lines.push_back(std::move(line));
                }
            }

            if (state_error.empty()) {
                sqlite3_exec(state_db_, "COMMIT;", nullptr, nullptr, nullptr);
            } else {
                sqlite3_exec(state_db_, "ROLLBACK;", nullptr, nullptr, nullptr);
                result.error = StructuredError{ "STATE_REHYDRATE_ERROR", "failed restoring archived State DB savestates", state_error }.ToJson();
            }
        }
    }

    if (!result.error.has_value()) {
        EmitArchiveProgress(
            request.progress_sink,
            ArchiveOperationPhase::ExportingRows,
            "Rehydrating execution rows",
            0,
            static_cast<std::int64_t>(spec.stream_files.size()),
            false);
        bool execution_transaction_started = false;
        if (sqlite3_exec(execution_db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            result.error = StructuredError{
                "EXECUTION_REHYDRATE_ERROR",
                "failed starting execution rehydrate transaction",
                sqlite3_errmsg(execution_db_)
            }.ToJson();
        } else {
            execution_transaction_started = true;
        }
        const bool has_analysis_streams = std::any_of(spec.stream_files.begin(), spec.stream_files.end(), [](const StreamFile& file) {
            return file.item_kind.rfind("analysis_", 0) == 0;
        });
        bool analysis_transaction_started = false;
        if (has_analysis_streams && analysis_db_ != nullptr) {
            if (analysis_db_ != execution_db_) {
                if (sqlite3_exec(analysis_db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
                    result.error = StructuredError{
                        "ANALYSIS_REHYDRATE_ERROR",
                        "failed starting analysis rehydrate transaction",
                        sqlite3_errmsg(analysis_db_)
                    }.ToJson();
                } else {
                    analysis_transaction_started = true;
                }
            }
        } else if (has_analysis_streams && analysis_db_ == nullptr) {
            result.error = StructuredError{ "ANALYSIS_DB_MISSING", "analysis streams require an analysis database", "analysis_db is null" }.ToJson();
        }
        if (!result.error.has_value()
            && has_analysis_streams
            && sqlite3_exec(analysis_db_, "PRAGMA defer_foreign_keys=ON;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            result.error = StructuredError{
                "ANALYSIS_REHYDRATE_ERROR",
                "failed deferring analysis foreign keys for rehydrate",
                sqlite3_errmsg(analysis_db_)
            }.ToJson();
        }
        if (!result.error.has_value()
            && sqlite3_exec(execution_db_, "PRAGMA defer_foreign_keys=ON;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            result.error = StructuredError{
                "EXECUTION_REHYDRATE_ERROR",
                "failed deferring execution foreign keys for rehydrate",
                sqlite3_errmsg(execution_db_)
            }.ToJson();
        }
        auto rollback = [&]() {
            if (execution_transaction_started) {
                sqlite3_exec(execution_db_, "ROLLBACK;", nullptr, nullptr, nullptr);
                execution_transaction_started = false;
            }
            if (analysis_transaction_started) {
                sqlite3_exec(analysis_db_, "ROLLBACK;", nullptr, nullptr, nullptr);
                analysis_transaction_started = false;
            }
        };

        std::string db_error;
        std::unordered_map<std::string, int> restored_execution_counts;
        std::vector<std::string> order = {
            "job_sets", "worksets", "workset_dispatch_attempts", "jobs", "job_events",
            "job_cancellation_requests", "workflow_instances", "workflow_unit_activations",
            "workflow_unit_activation_edges", "workflow_steps", "workflow_edges",
            "workflow_step_outputs", "workflow_instance_input_bindings",
            "workflow_instance_arguments", "workflow_events", "triggers"
        };
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

        for (const auto& kind : order) {
            if (result.error.has_value()) {
                break;
            }
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

                if (kind == "job_sets") {
                    bool ok_id = false;
                    bool ok_parent = false;
                    const auto old_id = JsonExtractInt(execution_db_, line, "$.job_set_id", &ok_id);
                    const auto old_parent = JsonExtractInt(execution_db_, line, "$.parent_job_set_id", &ok_parent);
                    if (!ok_id) continue;
                    const auto new_id = map_id("job_set", old_id);
                    const auto new_parent = ok_parent ? map_id("job_set", old_parent) : 0;
                    if (new_id == 0 || (ok_parent && new_parent == 0)) break;

                    bool ok_materialization_key = false;
                    const auto old_materialization_key = JsonExtractText(
                        execution_db_, line, "$.materialization_key", &ok_materialization_key);
                    const auto materialization_key = ok_materialization_key
                        ? Fnv1a64(
                            spec.target_namespace + ":job-set-materialization:"
                            + std::to_string(new_id) + ":" + old_materialization_key)
                        : std::string{};
                    Statement st;
                    if (!Prepare(execution_db_,
                            "INSERT INTO exec_job_set(job_set_id,parent_job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note,"
                            "materialization_key,materialization_state,population_sealed_at_utc,workset_publication_completed_at_utc) "
                            "VALUES(?1,?2,json_extract(?3,'$.program_kind'),json_extract(?3,'$.purpose'),?4,json_extract(?3,'$.created_at_utc'),json_extract(?3,'$.priority_boost'),json_extract(?3,'$.expected_total'),"
                            "json_extract(?3,'$.domain_ref_kind'),json_extract(?3,'$.domain_ref_id'),?5,?6,json_extract(?3,'$.materialization_state'),json_extract(?3,'$.population_sealed_at_utc'),"
                            "json_extract(?3,'$.workset_publication_completed_at_utc'));",
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
                    if (ok_materialization_key) {
                        sqlite3_bind_text(st.st, 6, materialization_key.c_str(), -1, SQLITE_TRANSIENT);
                    } else {
                        sqlite3_bind_null(st.st, 6);
                    }
                    if (!StepDone(execution_db_, st.st, &db_error)) break;
                    ++restored_execution_counts[kind];
                } else if (kind == "worksets") {
                    bool ok_id = false;
                    bool ok_set = false;
                    bool ok_step = false;
                    bool ok_root_set = false;
                    const auto old_id = JsonExtractInt(execution_db_, line, "$.workset_id", &ok_id);
                    const auto old_set = JsonExtractInt(execution_db_, line, "$.job_set_id", &ok_set);
                    const auto old_step = JsonExtractInt(execution_db_, line, "$.workflow_step_id", &ok_step);
                    const auto old_root_set = JsonExtractInt(execution_db_, line, "$.root_job_set_id", &ok_root_set);
                    if (!ok_id || !ok_set || !ok_step || !ok_root_set) {
                        db_error = "archived workset is missing its Full Phase invocation anchor";
                        break;
                    }
                    const auto new_id = map_id("workset", old_id);
                    const auto new_set = map_id("job_set", old_set);
                    const auto new_step = map_id("workflow_step", old_step);
                    const auto new_root_set = map_id("job_set", old_root_set);
                    if (new_id == 0 || new_set == 0 || new_step == 0 || new_root_set == 0) {
                        db_error = "archived workset Full Phase invocation anchor could not be remapped";
                        break;
                    }

                    Statement st;
                    if (!Prepare(execution_db_,
                            "INSERT INTO exec_workset(workset_id,job_set_id,workflow_step_id,root_job_set_id,workset_key,program_kind,program_version,compatibility_key,"
                            "module_canonical_id,module_version,module_sha256,entrypoint,verified_dependency_sha256,runtime_profile_sha256,"
                            "required_capability_mask,execution_affinity_key,estimated_payload_bytes,priority,item_count,published_at_utc) "
                            "VALUES(?1,?2,?3,?4,json_extract(?5,'$.workset_key'),json_extract(?5,'$.program_kind'),json_extract(?5,'$.program_version'),"
                            "json_extract(?5,'$.compatibility_key'),json_extract(?5,'$.module_canonical_id'),json_extract(?5,'$.module_version'),"
                            "json_extract(?5,'$.module_sha256'),json_extract(?5,'$.entrypoint'),json_extract(?5,'$.verified_dependency_sha256'),"
                            "json_extract(?5,'$.runtime_profile_sha256'),json_extract(?5,'$.required_capability_mask'),"
                            "json_extract(?5,'$.execution_affinity_key'),"
                            "json_extract(?5,'$.estimated_payload_bytes'),json_extract(?5,'$.priority'),json_extract(?5,'$.item_count'),"
                            "json_extract(?5,'$.published_at_utc'));",
                            &st,
                            &db_error)) break;
                    sqlite3_bind_int64(st.st, 1, new_id);
                    sqlite3_bind_int64(st.st, 2, new_set);
                    sqlite3_bind_int64(st.st, 3, new_step);
                    sqlite3_bind_int64(st.st, 4, new_root_set);
                    sqlite3_bind_text(st.st, 5, line.c_str(), -1, SQLITE_TRANSIENT);
                    if (!StepDone(execution_db_, st.st, &db_error)) break;
                    ++restored_execution_counts[kind];
                } else if (kind == "workset_dispatch_attempts") {
                    bool ok_id = false;
                    bool ok_workset = false;
                    const auto old_id = JsonExtractInt(execution_db_, line, "$.dispatch_attempt_id", &ok_id);
                    const auto old_workset = JsonExtractInt(execution_db_, line, "$.workset_id", &ok_workset);
                    if (!ok_id || !ok_workset) continue;
                    const auto new_id = map_id("workset_dispatch_attempt", old_id);
                    const auto new_workset = map_id("workset", old_workset);
                    if (new_id == 0 || new_workset == 0) break;

                    bool ok_claim_token = false;
                    const auto old_claim_token = JsonExtractText(
                        execution_db_, line, "$.claim_token", &ok_claim_token);
                    if (!ok_claim_token) continue;
                    const auto claim_token = Fnv1a64(
                        spec.target_namespace + ":workset-dispatch:"
                        + std::to_string(new_id) + ":" + old_claim_token);
                    Statement st;
                    if (!Prepare(execution_db_,
                            "INSERT INTO exec_workset_dispatch_attempt(dispatch_attempt_id,workset_id,dispatch_sequence,state,claim_token,"
                            "lease_expires_at_utc,claimed_at_utc,dispatched_at_utc,draining_at_utc,closed_at_utc,close_reason_code,close_reason_text) "
                            "VALUES(?1,?2,json_extract(?3,'$.dispatch_sequence'),json_extract(?3,'$.state'),?4,"
                            "json_extract(?3,'$.lease_expires_at_utc'),json_extract(?3,'$.claimed_at_utc'),"
                            "json_extract(?3,'$.dispatched_at_utc'),json_extract(?3,'$.draining_at_utc'),json_extract(?3,'$.closed_at_utc'),"
                            "json_extract(?3,'$.close_reason_code'),json_extract(?3,'$.close_reason_text'));",
                            &st,
                            &db_error)) break;
                    sqlite3_bind_int64(st.st, 1, new_id);
                    sqlite3_bind_int64(st.st, 2, new_workset);
                    sqlite3_bind_text(st.st, 3, line.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(st.st, 4, claim_token.c_str(), -1, SQLITE_TRANSIENT);
                    if (!StepDone(execution_db_, st.st, &db_error)) break;
                    ++restored_execution_counts[kind];
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
                    // A restored fingerprint is an opaque uniqueness token. It
                    // is never decoded for SeedProbe business intent, and
                    // namespacing it with the mapped job id prevents collisions
                    // even when the archived text contains old domain ids.
                    const auto safe_fingerprint = Fnv1a64(spec.target_namespace + ":job:" + std::to_string(new_id) + ":" + base_fingerprint);

                    bool ok_workset = false;
                    bool ok_dispatch = false;
                    bool ok_savestate = false;
                    const auto old_workset = JsonExtractInt(execution_db_, line, "$.workset_id", &ok_workset);
                    const auto old_dispatch = JsonExtractInt(execution_db_, line, "$.dispatch_attempt_id", &ok_dispatch);
                    const auto old_savestate = JsonExtractInt(execution_db_, line, "$.savestate_id", &ok_savestate);
                    const auto new_workset = ok_workset ? map_id("workset", old_workset) : 0;
                    const auto new_dispatch = ok_dispatch
                        ? map_id("workset_dispatch_attempt", old_dispatch)
                        : 0;
                    if ((ok_workset && new_workset == 0) || (ok_dispatch && new_dispatch == 0)) break;

                    auto new_savestate = old_savestate;
                    if (ok_savestate) {
                        const auto mapped_kind = id_map.find("state_savestate");
                        if (mapped_kind != id_map.end()) {
                            const auto mapped = mapped_kind->second.find(old_savestate);
                            if (mapped != mapped_kind->second.end()) {
                                new_savestate = mapped->second;
                            }
                        }
                    }

                    bool ok_input_ini = false;
                    bool ok_cancellation_group_key = false;
                    auto restored_input_ini = JsonExtractText(
                        execution_db_, line, "$.input_ini", &ok_input_ini);
                    auto restored_cancellation_group_key = JsonExtractText(
                        execution_db_,
                        line,
                        "$.cancellation_group_key",
                        &ok_cancellation_group_key);

                    bool ok_cancellation_key = false;
                    const auto old_cancellation_key = JsonExtractText(
                        execution_db_, line, "$.cancellation_request_key", &ok_cancellation_key);
                    const auto cancellation_key = ok_cancellation_key
                        ? Fnv1a64(
                            spec.target_namespace + ":job-cancellation:"
                            + std::to_string(new_id) + ":" + old_cancellation_key)
                        : std::string{};
                    Statement st;
                    if (!Prepare(execution_db_,
                            "INSERT INTO exec_job("
                            "job_id,job_set_id,parent_job_id,program_kind,program_version,program_ref_kind,program_ref_id,fingerprint,"
                            "priority,state,attempts,max_attempts,claimed_by_token,lease_expires_at_utc,queued_at_utc,started_at_utc,"
                            "ended_at_utc,error_code,error_text,savestate_id,input_ini,workset_id,workset_item_ordinal,"
                            "dispatch_attempt_id,dispatch_item_ordinal,reserved_attempt_id,execution_finished_at_utc,worker_terminal_status,"
                            "worker_terminal_fingerprint,worker_terminal_id,worker_terminal_error_code,worker_terminal_error_text,"
                            "worker_terminal_unstarted,worker_result_blob_id,result_processing_state,"
                            "result_processing_attempts,result_processing_failures,"
                            "result_processing_error_code,result_processing_error_text,result_processing_failed_at_utc,"
                            "result_processed_at_utc,cancellation_group_key,cancellation_state,"
                            "cancellation_request_key,cancellation_reason_code,"
                            "cancellation_reason_text,cancellation_requested_by,cancellation_caused_by_job_id,"
                            "cancellation_requested_at_utc,cancellation_delivery_attempts,"
                            "cancellation_last_delivery_error_code,cancellation_last_delivery_error_text,"
                            "cancellation_last_delivery_failed_at_utc,cancellation_delivered_at_utc,cancellation_resolved_at_utc,"
                            "cancellation_resolution_code) "
                            "VALUES(?1,?2,?3,json_extract(?4,'$.program_kind'),json_extract(?4,'$.program_version'),"
                            "json_extract(?4,'$.program_ref_kind'),json_extract(?4,'$.program_ref_id'),?5,"
                            "json_extract(?4,'$.priority'),json_extract(?4,'$.state'),json_extract(?4,'$.attempts'),"
                            "json_extract(?4,'$.max_attempts'),json_extract(?4,'$.claimed_by_token'),"
                            "json_extract(?4,'$.lease_expires_at_utc'),json_extract(?4,'$.queued_at_utc'),"
                            "json_extract(?4,'$.started_at_utc'),json_extract(?4,'$.ended_at_utc'),"
                            "json_extract(?4,'$.error_code'),json_extract(?4,'$.error_text'),?8,?10,"
                            "?6,json_extract(?4,'$.workset_item_ordinal'),?7,json_extract(?4,'$.dispatch_item_ordinal'),json_extract(?4,'$.reserved_attempt_id'),"
                            "json_extract(?4,'$.execution_finished_at_utc'),json_extract(?4,'$.worker_terminal_status'),"
                            "json_extract(?4,'$.worker_terminal_fingerprint'),json_extract(?4,'$.worker_terminal_id'),"
                            "json_extract(?4,'$.worker_terminal_error_code'),json_extract(?4,'$.worker_terminal_error_text'),"
                            "json_extract(?4,'$.worker_terminal_unstarted'),NULL,json_extract(?4,'$.result_processing_state'),"
                            "json_extract(?4,'$.result_processing_attempts'),json_extract(?4,'$.result_processing_failures'),"
                            "json_extract(?4,'$.result_processing_error_code'),json_extract(?4,'$.result_processing_error_text'),"
                            "json_extract(?4,'$.result_processing_failed_at_utc'),json_extract(?4,'$.result_processed_at_utc'),"
                            "?11,json_extract(?4,'$.cancellation_state'),"
                            "?9,json_extract(?4,'$.cancellation_reason_code'),"
                            "json_extract(?4,'$.cancellation_reason_text'),json_extract(?4,'$.cancellation_requested_by'),NULL,"
                            "json_extract(?4,'$.cancellation_requested_at_utc'),"
                            "json_extract(?4,'$.cancellation_delivery_attempts'),"
                            "json_extract(?4,'$.cancellation_last_delivery_error_code'),"
                            "json_extract(?4,'$.cancellation_last_delivery_error_text'),"
                            "json_extract(?4,'$.cancellation_last_delivery_failed_at_utc'),json_extract(?4,'$.cancellation_delivered_at_utc'),"
                            "json_extract(?4,'$.cancellation_resolved_at_utc'),json_extract(?4,'$.cancellation_resolution_code'));",
                            &st,
                            &db_error)) {
                        break;
                    }
                    sqlite3_bind_int64(st.st, 1, new_id);
                    sqlite3_bind_int64(st.st, 2, new_set);
                    if (ok_parent) sqlite3_bind_int64(st.st, 3, new_parent); else sqlite3_bind_null(st.st, 3);
                    sqlite3_bind_text(st.st, 4, line.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(st.st, 5, safe_fingerprint.c_str(), -1, SQLITE_TRANSIENT);
                    if (ok_workset) sqlite3_bind_int64(st.st, 6, new_workset); else sqlite3_bind_null(st.st, 6);
                    if (ok_dispatch) sqlite3_bind_int64(st.st, 7, new_dispatch); else sqlite3_bind_null(st.st, 7);
                    if (ok_savestate) sqlite3_bind_int64(st.st, 8, new_savestate); else sqlite3_bind_null(st.st, 8);
                    if (ok_cancellation_key) {
                        sqlite3_bind_text(st.st, 9, cancellation_key.c_str(), -1, SQLITE_TRANSIENT);
                    } else {
                        sqlite3_bind_null(st.st, 9);
                    }
                    if (ok_input_ini) {
                        sqlite3_bind_text(
                            st.st,
                            10,
                            restored_input_ini.c_str(),
                            -1,
                            SQLITE_TRANSIENT);
                    } else {
                        sqlite3_bind_null(st.st, 10);
                    }
                    if (ok_cancellation_group_key) {
                        sqlite3_bind_text(
                            st.st,
                            11,
                            restored_cancellation_group_key.c_str(),
                            -1,
                            SQLITE_TRANSIENT);
                    } else {
                        sqlite3_bind_null(st.st, 11);
                    }
                    if (!StepDone(execution_db_, st.st, &db_error)) break;
                    restored_jobs.push_back(new_id);
                    ++restored_execution_counts[kind];
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
                    ++restored_execution_counts[kind];
                } else if (kind == "job_cancellation_requests") {
                    bool ok_id = false;
                    bool ok_job = false;
                    bool ok_caused_by = false;
                    bool ok_request_key = false;
                    const auto old_id = JsonExtractInt(
                        execution_db_, line, "$.cancellation_request_id", &ok_id);
                    const auto old_job = JsonExtractInt(execution_db_, line, "$.job_id", &ok_job);
                    const auto old_caused_by = JsonExtractInt(
                        execution_db_, line, "$.caused_by_job_id", &ok_caused_by);
                    const auto old_request_key = JsonExtractText(
                        execution_db_, line, "$.request_key", &ok_request_key);
                    if (!ok_id || !ok_job || !ok_request_key) continue;
                    const auto new_id = map_id("job_cancellation_request", old_id);
                    const auto new_job = map_id("job", old_job);
                    const auto new_caused_by = ok_caused_by ? map_id("job", old_caused_by) : 0;
                    if (new_id == 0 || new_job == 0 || (ok_caused_by && new_caused_by == 0)) break;

                    const auto request_key = Fnv1a64(
                        spec.target_namespace + ":job-cancellation:"
                        + std::to_string(new_job) + ":" + old_request_key);
                    Statement st;
                    if (!Prepare(execution_db_,
                            "INSERT INTO exec_job_cancellation_request("
                            "cancellation_request_id,job_id,request_key,reason_code,reason_text,requested_by,caused_by_job_id,"
                            "requested_at_utc,state,delivery_attempts,last_delivery_error_code,"
                            "last_delivery_error_text,last_delivery_failed_at_utc,"
                            "delivered_at_utc,resolved_at_utc,resolution_code) "
                            "VALUES(?1,?2,?3,json_extract(?4,'$.reason_code'),json_extract(?4,'$.reason_text'),"
                            "json_extract(?4,'$.requested_by'),?5,json_extract(?4,'$.requested_at_utc'),"
                            "json_extract(?4,'$.state'),json_extract(?4,'$.delivery_attempts'),"
                            "json_extract(?4,'$.last_delivery_error_code'),"
                            "json_extract(?4,'$.last_delivery_error_text'),"
                            "json_extract(?4,'$.last_delivery_failed_at_utc'),json_extract(?4,'$.delivered_at_utc'),"
                            "json_extract(?4,'$.resolved_at_utc'),json_extract(?4,'$.resolution_code'));",
                            &st,
                            &db_error)) break;
                    sqlite3_bind_int64(st.st, 1, new_id);
                    sqlite3_bind_int64(st.st, 2, new_job);
                    sqlite3_bind_text(st.st, 3, request_key.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(st.st, 4, line.c_str(), -1, SQLITE_TRANSIENT);
                    if (ok_caused_by) sqlite3_bind_int64(st.st, 5, new_caused_by); else sqlite3_bind_null(st.st, 5);
                    if (!StepDone(execution_db_, st.st, &db_error)) break;
                    ++restored_execution_counts[kind];
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
                } else if (kind == "workflow_unit_activations") {
                    bool ok_id = false;
                    bool ok_instance = false;
                    bool ok_parent = false;
                    const auto old_id = JsonExtractInt(execution_db_, line, "$.workflow_unit_activation_id", &ok_id);
                    const auto old_instance = JsonExtractInt(execution_db_, line, "$.workflow_instance_id", &ok_instance);
                    const auto old_parent = JsonExtractInt(execution_db_, line, "$.parent_workflow_unit_activation_id", &ok_parent);
                    if (!ok_id || !ok_instance) continue;
                    const auto new_id = map_id("workflow_unit_activation", old_id);
                    const auto new_instance = map_id("workflow_instance", old_instance);
                    const auto new_parent = ok_parent ? map_id("workflow_unit_activation", old_parent) : 0;
                    if (new_id == 0 || new_instance == 0 || (ok_parent && new_parent == 0)) break;
                    Statement st;
                    if (!Prepare(execution_db_,
                            "INSERT INTO exec_workflow_unit_activation(workflow_unit_activation_id,workflow_instance_id,parent_workflow_unit_activation_id,activation_key,graph_node_key,unit_kind,display_name,state,activation_params_json,authored_ref_kind,authored_ref_id,failure_code,failure_text,created_at_utc,ready_at_utc,started_at_utc,completed_at_utc,failed_at_utc) "
                            "VALUES(?1,?2,?3,json_extract(?4,'$.activation_key'),json_extract(?4,'$.graph_node_key'),json_extract(?4,'$.unit_kind'),json_extract(?4,'$.display_name'),json_extract(?4,'$.state'),COALESCE(json_extract(?4,'$.activation_params_json'),''),json_extract(?4,'$.authored_ref_kind'),json_extract(?4,'$.authored_ref_id'),json_extract(?4,'$.failure_code'),json_extract(?4,'$.failure_text'),json_extract(?4,'$.created_at_utc'),json_extract(?4,'$.ready_at_utc'),json_extract(?4,'$.started_at_utc'),json_extract(?4,'$.completed_at_utc'),json_extract(?4,'$.failed_at_utc'));",
                            &st,
                            &db_error)) break;
                    sqlite3_bind_int64(st.st, 1, new_id);
                    sqlite3_bind_int64(st.st, 2, new_instance);
                    if (ok_parent) sqlite3_bind_int64(st.st, 3, new_parent); else sqlite3_bind_null(st.st, 3);
                    sqlite3_bind_text(st.st, 4, line.c_str(), -1, SQLITE_TRANSIENT);
                    if (!StepDone(execution_db_, st.st, &db_error)) break;
                } else if (kind == "workflow_unit_activation_edges") {
                    bool ok_id = false;
                    bool ok_instance = false;
                    bool ok_from = false;
                    bool ok_to = false;
                    const auto old_id = JsonExtractInt(execution_db_, line, "$.workflow_unit_activation_edge_id", &ok_id);
                    const auto old_instance = JsonExtractInt(execution_db_, line, "$.workflow_instance_id", &ok_instance);
                    const auto old_from = JsonExtractInt(execution_db_, line, "$.from_workflow_unit_activation_id", &ok_from);
                    const auto old_to = JsonExtractInt(execution_db_, line, "$.to_workflow_unit_activation_id", &ok_to);
                    if (!ok_id || !ok_instance || !ok_from || !ok_to) continue;
                    const auto new_id = map_id("workflow_unit_activation_edge", old_id);
                    const auto new_instance = map_id("workflow_instance", old_instance);
                    const auto new_from = map_id("workflow_unit_activation", old_from);
                    const auto new_to = map_id("workflow_unit_activation", old_to);
                    if (new_id == 0 || new_instance == 0 || new_from == 0 || new_to == 0) break;
                    Statement st;
                    if (!Prepare(execution_db_,
                            "INSERT INTO exec_workflow_unit_activation_edge(workflow_unit_activation_edge_id,workflow_instance_id,from_workflow_unit_activation_id,to_workflow_unit_activation_id,output_key,input_key,condition_kind,condition_value,created_at_utc) "
                            "VALUES(?1,?2,?3,?4,json_extract(?5,'$.output_key'),json_extract(?5,'$.input_key'),json_extract(?5,'$.condition_kind'),json_extract(?5,'$.condition_value'),json_extract(?5,'$.created_at_utc'));",
                            &st,
                            &db_error)) break;
                    sqlite3_bind_int64(st.st, 1, new_id);
                    sqlite3_bind_int64(st.st, 2, new_instance);
                    sqlite3_bind_int64(st.st, 3, new_from);
                    sqlite3_bind_int64(st.st, 4, new_to);
                    sqlite3_bind_text(st.st, 5, line.c_str(), -1, SQLITE_TRANSIENT);
                    if (!StepDone(execution_db_, st.st, &db_error)) break;
                } else if (kind == "workflow_steps") {
                    bool ok_id = false;
                    bool ok_instance = false;
                    bool ok_set = false;
                    bool ok_activation = false;
                    const auto old_id = JsonExtractInt(execution_db_, line, "$.workflow_step_id", &ok_id);
                    const auto old_instance = JsonExtractInt(execution_db_, line, "$.workflow_instance_id", &ok_instance);
                    const auto old_set = JsonExtractInt(execution_db_, line, "$.job_set_id", &ok_set);
                    const auto old_activation = JsonExtractInt(execution_db_, line, "$.workflow_unit_activation_id", &ok_activation);
                    if (!ok_id || !ok_instance) continue;
                    const auto new_id = map_id("workflow_step", old_id);
                    const auto new_instance = map_id("workflow_instance", old_instance);
                    const auto new_set = ok_set ? map_id("job_set", old_set) : 0;
                    const auto new_activation = ok_activation ? map_id("workflow_unit_activation", old_activation) : 0;
                    if (new_id == 0 || new_instance == 0 || (ok_set && new_set == 0) || (ok_activation && new_activation == 0)) break;

                    bool ok_input_ref = false;
                    bool ok_output_ref = false;
                    bool ok_input_kind = false;
                    bool ok_output_kind = false;
                    const auto old_input_ref = JsonExtractInt(execution_db_, line, "$.input_ref_id", &ok_input_ref);
                    const auto old_output_ref = JsonExtractInt(execution_db_, line, "$.output_ref_id", &ok_output_ref);
                    const auto input_ref_kind = JsonExtractText(execution_db_, line, "$.input_ref_kind", &ok_input_kind);
                    const auto output_ref_kind = JsonExtractText(execution_db_, line, "$.output_ref_kind", &ok_output_kind);
                    auto remap_savestate_ref = [&](const std::string& ref_kind, bool ok_ref, std::int64_t old_ref) -> std::optional<std::int64_t> {
                        if (!ok_ref) {
                            return std::nullopt;
                        }
                        if (ref_kind.find("savestate") == std::string::npos) {
                            return old_ref;
                        }
                        const auto mapped = id_map["state_savestate"].find(old_ref);
                        if (mapped == id_map["state_savestate"].end()) {
                            return old_ref;
                        }
                        return mapped->second;
                    };
                    const auto input_ref_id = remap_savestate_ref(input_ref_kind, ok_input_ref, old_input_ref);
                    const auto output_ref_id = remap_savestate_ref(output_ref_kind, ok_output_ref, old_output_ref);
                    Statement st;
                    if (!Prepare(execution_db_,
                            "INSERT INTO exec_workflow_step(workflow_step_id,workflow_instance_id,workflow_unit_activation_id,step_key,graph_node_key,step_kind,state,guard_kind,guard_value,priority,attempts,max_attempts,job_set_id,input_ref_kind,input_ref_id,output_ref_kind,output_ref_id,blocked_reason,ready_at_utc,started_at_utc,completed_at_utc,failed_at_utc,created_at_utc) "
                            "VALUES(?1,?2,?3,json_extract(?4,'$.step_key'),json_extract(?4,'$.graph_node_key'),json_extract(?4,'$.step_kind'),json_extract(?4,'$.state'),json_extract(?4,'$.guard_kind'),json_extract(?4,'$.guard_value'),json_extract(?4,'$.priority'),json_extract(?4,'$.attempts'),json_extract(?4,'$.max_attempts'),?5,json_extract(?4,'$.input_ref_kind'),?6,json_extract(?4,'$.output_ref_kind'),?7,json_extract(?4,'$.blocked_reason'),json_extract(?4,'$.ready_at_utc'),json_extract(?4,'$.started_at_utc'),json_extract(?4,'$.completed_at_utc'),json_extract(?4,'$.failed_at_utc'),json_extract(?4,'$.created_at_utc'));",
                            &st,
                            &db_error)) break;
                    sqlite3_bind_int64(st.st, 1, new_id);
                    sqlite3_bind_int64(st.st, 2, new_instance);
                    if (ok_activation) sqlite3_bind_int64(st.st, 3, new_activation); else sqlite3_bind_null(st.st, 3);
                    sqlite3_bind_text(st.st, 4, line.c_str(), -1, SQLITE_TRANSIENT);
                    if (ok_set) sqlite3_bind_int64(st.st, 5, new_set); else sqlite3_bind_null(st.st, 5);
                    if (input_ref_id.has_value()) sqlite3_bind_int64(st.st, 6, *input_ref_id); else sqlite3_bind_null(st.st, 6);
                    if (output_ref_id.has_value()) sqlite3_bind_int64(st.st, 7, *output_ref_id); else sqlite3_bind_null(st.st, 7);
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
                } else if (kind == "workflow_step_outputs") {
                    bool ok_id = false;
                    bool ok_instance = false;
                    bool ok_step = false;
                    const auto old_id = JsonExtractInt(execution_db_, line, "$.workflow_step_output_id", &ok_id);
                    const auto old_instance = JsonExtractInt(execution_db_, line, "$.workflow_instance_id", &ok_instance);
                    const auto old_step = JsonExtractInt(execution_db_, line, "$.workflow_step_id", &ok_step);
                    if (!ok_id || !ok_instance || !ok_step) continue;
                    const auto new_id = map_id("workflow_step_output", old_id);
                    const auto new_instance = map_id("workflow_instance", old_instance);
                    const auto new_step = map_id("workflow_step", old_step);
                    if (new_id == 0 || new_instance == 0 || new_step == 0) break;

                    bool ok_ref = false;
                    bool ok_kind = false;
                    const auto old_ref = JsonExtractInt(execution_db_, line, "$.ref_id", &ok_ref);
                    const auto ref_kind = JsonExtractText(execution_db_, line, "$.ref_kind", &ok_kind);
                    std::int64_t new_ref = old_ref;
                    if (ok_ref && ref_kind.find("savestate") != std::string::npos) {
                        const auto mapped = id_map["state_savestate"].find(old_ref);
                        if (mapped != id_map["state_savestate"].end()) {
                            new_ref = mapped->second;
                        }
                    }

                    Statement st;
                    if (!Prepare(execution_db_,
                            "INSERT INTO exec_workflow_step_output(workflow_step_output_id,workflow_instance_id,workflow_step_id,graph_node_key,output_key,data_kind,ref_kind,ref_id,created_at_utc) "
                            "VALUES(?1,?2,?3,json_extract(?4,'$.graph_node_key'),json_extract(?4,'$.output_key'),json_extract(?4,'$.data_kind'),json_extract(?4,'$.ref_kind'),?5,json_extract(?4,'$.created_at_utc'));",
                            &st,
                            &db_error)) break;
                    sqlite3_bind_int64(st.st, 1, new_id);
                    sqlite3_bind_int64(st.st, 2, new_instance);
                    sqlite3_bind_int64(st.st, 3, new_step);
                    sqlite3_bind_text(st.st, 4, line.c_str(), -1, SQLITE_TRANSIENT);
                    if (ok_ref) sqlite3_bind_int64(st.st, 5, new_ref); else sqlite3_bind_null(st.st, 5);
                    if (!StepDone(execution_db_, st.st, &db_error)) break;
                } else if (kind == "workflow_instance_input_bindings") {
                    bool ok_id = false;
                    bool ok_instance = false;
                    const auto old_id = JsonExtractInt(execution_db_, line, "$.workflow_instance_input_binding_id", &ok_id);
                    const auto old_instance = JsonExtractInt(execution_db_, line, "$.workflow_instance_id", &ok_instance);
                    if (!ok_id || !ok_instance) continue;
                    const auto new_id = map_id("workflow_instance_input_binding", old_id);
                    const auto new_instance = map_id("workflow_instance", old_instance);
                    if (new_id == 0 || new_instance == 0) break;

                    bool ok_ref = false;
                    bool ok_kind = false;
                    const auto old_ref = JsonExtractInt(execution_db_, line, "$.ref_id", &ok_ref);
                    const auto ref_kind = JsonExtractText(execution_db_, line, "$.ref_kind", &ok_kind);
                    std::int64_t new_ref = old_ref;
                    if (ok_ref && ref_kind.find("savestate") != std::string::npos) {
                        const auto mapped = id_map["state_savestate"].find(old_ref);
                        if (mapped != id_map["state_savestate"].end()) {
                            new_ref = mapped->second;
                        }
                    }

                    Statement st;
                    if (!Prepare(execution_db_,
                            "INSERT INTO exec_workflow_instance_input_binding(workflow_instance_input_binding_id,workflow_instance_id,workflow_graph_revision_id,node_key,input_key,data_kind,ref_kind,ref_id,source_kind,created_at_utc) "
                            "VALUES(?1,?2,json_extract(?3,'$.workflow_graph_revision_id'),json_extract(?3,'$.node_key'),json_extract(?3,'$.input_key'),json_extract(?3,'$.data_kind'),json_extract(?3,'$.ref_kind'),?4,json_extract(?3,'$.source_kind'),json_extract(?3,'$.created_at_utc'));",
                            &st,
                            &db_error)) break;
                    sqlite3_bind_int64(st.st, 1, new_id);
                    sqlite3_bind_int64(st.st, 2, new_instance);
                    sqlite3_bind_text(st.st, 3, line.c_str(), -1, SQLITE_TRANSIENT);
                    if (ok_ref) sqlite3_bind_int64(st.st, 4, new_ref); else sqlite3_bind_null(st.st, 4);
                    if (!StepDone(execution_db_, st.st, &db_error)) break;
                } else if (kind == "workflow_instance_arguments") {
                    bool ok_id = false;
                    bool ok_instance = false;
                    const auto old_id = JsonExtractInt(execution_db_, line, "$.workflow_instance_argument_id", &ok_id);
                    const auto old_instance = JsonExtractInt(execution_db_, line, "$.workflow_instance_id", &ok_instance);
                    if (!ok_id || !ok_instance) continue;
                    const auto new_id = map_id("workflow_instance_argument", old_id);
                    const auto new_instance = map_id("workflow_instance", old_instance);
                    if (new_id == 0 || new_instance == 0) break;
                    Statement st;
                    if (!Prepare(execution_db_,
                            "INSERT INTO exec_workflow_instance_argument(workflow_instance_argument_id,workflow_instance_id,node_key,argument_key,value_type,integer_value,text_value,source_kind,created_at_utc) "
                            "VALUES(?1,?2,json_extract(?3,'$.node_key'),json_extract(?3,'$.argument_key'),json_extract(?3,'$.value_type'),json_extract(?3,'$.integer_value'),json_extract(?3,'$.text_value'),json_extract(?3,'$.source_kind'),json_extract(?3,'$.created_at_utc'));",
                            &st,
                            &db_error)) break;
                    sqlite3_bind_int64(st.st, 1, new_id);
                    sqlite3_bind_int64(st.st, 2, new_instance);
                    sqlite3_bind_text(st.st, 3, line.c_str(), -1, SQLITE_TRANSIENT);
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

        if (!result.error.has_value() && db_error.empty()) {
            const std::array<std::string_view, 6> required_counts = {
                "job_sets",
                "worksets",
                "workset_dispatch_attempts",
                "jobs",
                "job_events",
                "job_cancellation_requests",
            };
            for (const auto kind : required_counts) {
                const auto* stream = FindStream(spec, kind);
                if (stream != nullptr
                    && restored_execution_counts[std::string(kind)] != stream->row_count) {
                    db_error = "restored row count mismatch for " + std::string(kind);
                    break;
                }
            }
        }

        if (!result.error.has_value() && db_error.empty()) {
            const auto* jobs_stream = FindStream(spec, "jobs");
            if (jobs_stream != nullptr) {
                for (const auto& line : ReadStreamLines(spec, *jobs_stream)) {
                    bool ok_job = false;
                    bool ok_caused_by = false;
                    const auto old_job = JsonExtractInt(
                        execution_db_, line, "$.job_id", &ok_job);
                    const auto old_caused_by = JsonExtractInt(
                        execution_db_, line, "$.cancellation_caused_by_job_id", &ok_caused_by);
                    if (!ok_job || !ok_caused_by) continue;
                    const auto job_it = id_map["job"].find(old_job);
                    const auto cause_it = id_map["job"].find(old_caused_by);
                    if (job_it == id_map["job"].end() || cause_it == id_map["job"].end()) {
                        db_error = "cancellation caused-by job mapping is missing";
                        break;
                    }
                    Statement update;
                    if (!Prepare(
                            execution_db_,
                            "UPDATE exec_job SET cancellation_caused_by_job_id=?1 WHERE job_id=?2;",
                            &update,
                            &db_error)) break;
                    sqlite3_bind_int64(update.st, 1, cause_it->second);
                    sqlite3_bind_int64(update.st, 2, job_it->second);
                    if (!StepDone(execution_db_, update.st, &db_error)) break;
                }
            }
        }

        if (!result.error.has_value() && !db_error.empty()) {
            result.error = StructuredError{
                "DB_VALIDATION_ERROR",
                "restored execution coordination validation failed",
                db_error
            }.ToJson();
        }

        if (!result.error.has_value() && has_analysis_streams) {
            auto lookup_map = [&](std::string_view map_kind, std::int64_t old_id) -> std::optional<std::int64_t> {
                const auto per_kind = id_map.find(std::string(map_kind));
                if (per_kind == id_map.end()) {
                    return std::nullopt;
                }
                const auto it = per_kind->second.find(old_id);
                return it == per_kind->second.end() ? std::nullopt : std::optional<std::int64_t>(it->second);
            };
            auto map_optional = [&](std::string_view map_kind, bool ok, std::int64_t old_id) -> std::optional<std::int64_t> {
                if (!ok) {
                    return std::nullopt;
                }
                const auto mapped = lookup_map(map_kind, old_id);
                if (mapped.has_value()) {
                    return mapped;
                }
                const auto next = map_id(map_kind, old_id);
                return next == 0 ? std::nullopt : std::optional<std::int64_t>(next);
            };
            auto map_savestate = [&](bool ok, std::int64_t old_id) -> std::optional<std::int64_t> {
                if (!ok) {
                    return std::nullopt;
                }
                const auto mapped = lookup_map("state_savestate", old_id);
                return mapped.has_value() ? mapped : std::optional<std::int64_t>(old_id);
            };
            auto restore_stream = [&](std::string_view item_kind, const auto& callback) {
                const auto* stream = FindStream(spec, item_kind);
                if (stream == nullptr || stream->rel_path.extension() != ".jsonl" || !db_error.empty()) {
                    return;
                }
                for (const auto& line : ReadStreamLines(spec, *stream)) {
                    callback(line);
                    if (!db_error.empty()) {
                        return;
                    }
                }
            };
            auto bind_optional_int64 = [](sqlite3_stmt* st, int index, std::optional<std::int64_t> value) {
                if (value.has_value()) {
                    sqlite3_bind_int64(st, index, *value);
                } else {
                    sqlite3_bind_null(st, index);
                }
            };

            restore_stream("analysis_tas_movie_validation_requests", [&](const std::string& line) {
                bool ok_id = false, ok_workflow = false, ok_step = false, ok_source_ref = false;
                bool ok_source_artifact = false, ok_itinerary = false, ok_source_kind = false;
                const auto old_id = JsonExtractInt(analysis_db_, line, "$.validation_request_id", &ok_id);
                const auto old_workflow = JsonExtractInt(analysis_db_, line, "$.workflow_instance_id", &ok_workflow);
                const auto old_step = JsonExtractInt(analysis_db_, line, "$.workflow_step_id", &ok_step);
                const auto old_source_ref = JsonExtractInt(analysis_db_, line, "$.source_ref_id", &ok_source_ref);
                const auto old_source_artifact = JsonExtractInt(analysis_db_, line, "$.source_dtm_artifact_id", &ok_source_artifact);
                const auto old_itinerary = JsonExtractInt(analysis_db_, line, "$.itinerary_artifact_id", &ok_itinerary);
                const auto source_kind = JsonExtractText(analysis_db_, line, "$.source_kind", &ok_source_kind);
                if (!ok_id || !ok_workflow || !ok_step || !ok_source_ref || !ok_source_artifact || !ok_source_kind) return;
                const auto new_id = map_id("analysis_tas_movie_validation_request", old_id);
                const auto workflow = lookup_map("workflow_instance", old_workflow).value_or(old_workflow);
                const auto step = lookup_map("workflow_step", old_step).value_or(old_step);
                const auto source_artifact = lookup_map("state_artifact", old_source_artifact);
                const auto itinerary = ok_itinerary ? lookup_map("state_artifact", old_itinerary) : std::optional<std::int64_t>{};
                std::optional<std::int64_t> source_ref;
                if (source_kind == "DTM_ARTIFACT") source_ref = lookup_map("state_artifact", old_source_ref);
                else if (source_kind == "TREE") source_ref = lookup_map("state_tas_movie_tree", old_source_ref);
                else source_ref = old_source_ref;
                if (new_id == 0 || !source_artifact || !source_ref || (ok_itinerary && !itinerary)) {
                    db_error = "TAS movie validation request State mapping is missing";
                    return;
                }
                const auto materialization_key = spec.target_namespace + ":rehydrate:tmv:" + std::to_string(old_id);
                Statement st;
                if (!Prepare(analysis_db_,
                        "INSERT INTO tmv_validation_request(validation_request_id,materialization_key,workflow_instance_id,workflow_step_id,step_kind,operation,source_kind,"
                        "source_ref_id,source_dtm_artifact_id,source_dtm_sha256,rtc_value,effective_dtm_sha256,itinerary_artifact_id,itinerary_sha256,"
                        "required_final_breakpoint_pc,capture_root_checkpoint,full_phase_program_kind,full_phase_program_version,full_phase_canonical_id,full_phase_contract_revision,"
                        "full_phase_sha256,module_canonical_id,module_revision,module_sha256,created_at_utc) "
                        "VALUES(?1,?2,?3,?4,json_extract(?5,'$.step_kind'),json_extract(?5,'$.operation'),json_extract(?5,'$.source_kind'),?6,?7,"
                        "json_extract(?5,'$.source_dtm_sha256'),json_extract(?5,'$.rtc_value'),json_extract(?5,'$.effective_dtm_sha256'),?8,json_extract(?5,'$.itinerary_sha256'),"
                        "json_extract(?5,'$.required_final_breakpoint_pc'),json_extract(?5,'$.capture_root_checkpoint'),json_extract(?5,'$.full_phase_program_kind'),"
                        "json_extract(?5,'$.full_phase_program_version'),json_extract(?5,'$.full_phase_canonical_id'),json_extract(?5,'$.full_phase_contract_revision'),"
                        "json_extract(?5,'$.full_phase_sha256'),json_extract(?5,'$.module_canonical_id'),json_extract(?5,'$.module_revision'),json_extract(?5,'$.module_sha256'),"
                        "json_extract(?5,'$.created_at_utc'));",
                        &st, &db_error)) return;
                sqlite3_bind_int64(st.st, 1, new_id);
                sqlite3_bind_text(st.st, 2, materialization_key.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(st.st, 3, workflow);
                sqlite3_bind_int64(st.st, 4, step);
                sqlite3_bind_text(st.st, 5, line.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(st.st, 6, *source_ref);
                sqlite3_bind_int64(st.st, 7, *source_artifact);
                bind_optional_int64(st.st, 8, itinerary);
                StepDone(analysis_db_, st.st, &db_error);
            });

            restore_stream("analysis_tas_movie_validation_attempts", [&](const std::string& line) {
                bool ok_id = false, ok_request = false, ok_job = false, ok_good = false;
                bool ok_candidate = false, ok_root = false;
                const auto old_id = JsonExtractInt(analysis_db_, line, "$.validation_attempt_id", &ok_id);
                const auto old_request = JsonExtractInt(analysis_db_, line, "$.validation_request_id", &ok_request);
                const auto old_job = JsonExtractInt(analysis_db_, line, "$.source_job_id", &ok_job);
                const auto old_good = JsonExtractInt(analysis_db_, line, "$.last_known_good_savestate_id", &ok_good);
                const auto old_candidate = JsonExtractInt(analysis_db_, line, "$.candidate_itinerary_artifact_id", &ok_candidate);
                const auto old_root = JsonExtractInt(analysis_db_, line, "$.produced_tas_movie_root_id", &ok_root);
                if (!ok_id || !ok_request || !ok_job) return;
                const auto new_id = map_id("analysis_tas_movie_validation_attempt", old_id);
                const auto request_id = lookup_map("analysis_tas_movie_validation_request", old_request);
                const auto job_id = lookup_map("job", old_job).value_or(old_job);
                const auto good = ok_good ? lookup_map("state_savestate", old_good) : std::optional<std::int64_t>{};
                const auto candidate = ok_candidate ? lookup_map("state_artifact", old_candidate) : std::optional<std::int64_t>{};
                const auto root = ok_root ? lookup_map("state_tas_movie_root", old_root) : std::optional<std::int64_t>{};
                if (new_id == 0 || !request_id || (ok_good && !good) || (ok_candidate && !candidate) || (ok_root && !root)) {
                    db_error = "TAS movie validation attempt mapping is missing";
                    return;
                }
                Statement st;
                if (!Prepare(analysis_db_,
                        "INSERT INTO tmv_validation_attempt(validation_attempt_id,validation_request_id,source_job_id,worker_terminal_sha256,outcome,failure_reason,expected_pc,"
                        "expected_input_count,actual_pc,actual_input_count,last_verified_itinerary_index,last_known_good_savestate_id,candidate_itinerary_artifact_id,"
                        "candidate_itinerary_sha256,produced_tas_movie_root_id,worker_id,worker_process_generation,workset_epoch,recorded_at_utc) "
                        "VALUES(?1,?2,?3,json_extract(?4,'$.worker_terminal_sha256'),json_extract(?4,'$.outcome'),json_extract(?4,'$.failure_reason'),"
                        "json_extract(?4,'$.expected_pc'),json_extract(?4,'$.expected_input_count'),json_extract(?4,'$.actual_pc'),json_extract(?4,'$.actual_input_count'),"
                        "json_extract(?4,'$.last_verified_itinerary_index'),?5,?6,json_extract(?4,'$.candidate_itinerary_sha256'),?7,json_extract(?4,'$.worker_id'),"
                        "json_extract(?4,'$.worker_process_generation'),json_extract(?4,'$.workset_epoch'),json_extract(?4,'$.recorded_at_utc'));",
                        &st, &db_error)) return;
                sqlite3_bind_int64(st.st, 1, new_id);
                sqlite3_bind_int64(st.st, 2, *request_id);
                sqlite3_bind_int64(st.st, 3, job_id);
                sqlite3_bind_text(st.st, 4, line.c_str(), -1, SQLITE_TRANSIENT);
                bind_optional_int64(st.st, 5, good);
                bind_optional_int64(st.st, 6, candidate);
                bind_optional_int64(st.st, 7, root);
                StepDone(analysis_db_, st.st, &db_error);
            });

            restore_stream("analysis_tas_movie_validation_statuses", [&](const std::string& line) {
                bool ok_attempt = false;
                const auto old_attempt = JsonExtractInt(analysis_db_, line, "$.validation_attempt_id", &ok_attempt);
                if (!ok_attempt) return;
                const auto attempt = lookup_map("analysis_tas_movie_validation_attempt", old_attempt);
                if (!attempt) {
                    db_error = "TAS movie validation status attempt mapping is missing";
                    return;
                }
                Statement st;
                if (!Prepare(analysis_db_,
                        "INSERT INTO tmv_dtm_validation_status(effective_dtm_sha256,status,validation_attempt_id,updated_at_utc) "
                        "VALUES(json_extract(?1,'$.effective_dtm_sha256'),json_extract(?1,'$.status'),?2,json_extract(?1,'$.updated_at_utc')) "
                        "ON CONFLICT(effective_dtm_sha256) DO UPDATE SET status=excluded.status,validation_attempt_id=excluded.validation_attempt_id,updated_at_utc=excluded.updated_at_utc;",
                        &st, &db_error)) return;
                sqlite3_bind_text(st.st, 1, line.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(st.st, 2, *attempt);
                StepDone(analysis_db_, st.st, &db_error);
            });

            restore_stream(
                "analysis_tas_movie_checkpoint_sterilization_requests",
                [&](const std::string& line) {
                    bool ok_id = false, ok_workflow = false, ok_step = false;
                    bool ok_source = false, ok_source_artifact = false;
                    bool ok_dtm_artifact = false, ok_reused = false;
                    const auto old_id = JsonExtractInt(
                        analysis_db_, line, "$.sterilization_request_id", &ok_id);
                    const auto old_workflow = JsonExtractInt(
                        analysis_db_, line, "$.workflow_instance_id", &ok_workflow);
                    const auto old_step = JsonExtractInt(
                        analysis_db_, line, "$.workflow_step_id", &ok_step);
                    const auto old_source = JsonExtractInt(
                        analysis_db_, line, "$.source_savestate_id", &ok_source);
                    const auto old_source_artifact = JsonExtractInt(
                        analysis_db_, line, "$.source_savestate_artifact_id", &ok_source_artifact);
                    const auto old_dtm_artifact = JsonExtractInt(
                        analysis_db_, line, "$.source_dtm_artifact_id", &ok_dtm_artifact);
                    const auto old_reused = JsonExtractInt(
                        analysis_db_, line, "$.reused_savestate_id", &ok_reused);
                    if (!ok_id || !ok_workflow || !ok_step || !ok_source
                        || !ok_source_artifact || !ok_dtm_artifact) return;
                    const auto new_id = map_id(
                        "analysis_tas_movie_checkpoint_sterilization_request", old_id);
                    const auto workflow = lookup_map("workflow_instance", old_workflow);
                    const auto step = lookup_map("workflow_step", old_step);
                    const auto source = lookup_map("state_savestate", old_source);
                    const auto source_artifact = lookup_map(
                        "state_artifact", old_source_artifact);
                    const auto dtm_artifact = lookup_map(
                        "state_artifact", old_dtm_artifact);
                    const auto reused = ok_reused
                        ? lookup_map("state_savestate", old_reused)
                        : std::optional<std::int64_t>{};
                    if (new_id == 0 || !workflow || !step || !source
                        || !source_artifact || !dtm_artifact
                        || (ok_reused && !reused)) {
                        db_error = "TAS movie checkpoint sterilization request mapping is missing";
                        return;
                    }
                    const auto materialization_key = spec.target_namespace
                        + ":rehydrate:tmv-sterilize:" + std::to_string(old_id);
                    Statement st;
                    if (!Prepare(analysis_db_,
                            "INSERT INTO tmv_checkpoint_sterilization_request(sterilization_request_id,materialization_key,workflow_instance_id,workflow_step_id,"
                            "source_savestate_id,source_savestate_artifact_id,source_savestate_sha256,source_dtm_artifact_id,source_dtm_sha256,reused_savestate_id,"
                            "full_phase_program_kind,full_phase_program_version,full_phase_canonical_id,full_phase_contract_revision,full_phase_sha256,module_canonical_id,"
                            "module_revision,module_sha256,created_at_utc) VALUES(?1,?2,?3,?4,?5,?6,json_extract(?7,'$.source_savestate_sha256'),?8,"
                            "json_extract(?7,'$.source_dtm_sha256'),?9,json_extract(?7,'$.full_phase_program_kind'),json_extract(?7,'$.full_phase_program_version'),"
                            "json_extract(?7,'$.full_phase_canonical_id'),json_extract(?7,'$.full_phase_contract_revision'),json_extract(?7,'$.full_phase_sha256'),"
                            "json_extract(?7,'$.module_canonical_id'),json_extract(?7,'$.module_revision'),json_extract(?7,'$.module_sha256'),json_extract(?7,'$.created_at_utc'));",
                            &st, &db_error)) return;
                    sqlite3_bind_int64(st.st, 1, new_id);
                    sqlite3_bind_text(st.st, 2, materialization_key.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(st.st, 3, *workflow);
                    sqlite3_bind_int64(st.st, 4, *step);
                    sqlite3_bind_int64(st.st, 5, *source);
                    sqlite3_bind_int64(st.st, 6, *source_artifact);
                    sqlite3_bind_text(st.st, 7, line.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(st.st, 8, *dtm_artifact);
                    bind_optional_int64(st.st, 9, reused);
                    StepDone(analysis_db_, st.st, &db_error);
                });

            restore_stream(
                "analysis_tas_movie_checkpoint_sterilization_attempts",
                [&](const std::string& line) {
                    bool ok_id = false, ok_request = false, ok_job = false;
                    bool ok_result = false;
                    const auto old_id = JsonExtractInt(
                        analysis_db_, line, "$.sterilization_attempt_id", &ok_id);
                    const auto old_request = JsonExtractInt(
                        analysis_db_, line, "$.sterilization_request_id", &ok_request);
                    const auto old_job = JsonExtractInt(
                        analysis_db_, line, "$.source_job_id", &ok_job);
                    const auto old_result = JsonExtractInt(
                        analysis_db_, line, "$.produced_savestate_id", &ok_result);
                    if (!ok_id || !ok_request || !ok_job || !ok_result) return;
                    const auto new_id = map_id(
                        "analysis_tas_movie_checkpoint_sterilization_attempt", old_id);
                    const auto request_id = lookup_map(
                        "analysis_tas_movie_checkpoint_sterilization_request", old_request);
                    const auto job_id = lookup_map("job", old_job).value_or(old_job);
                    const auto result_state = lookup_map("state_savestate", old_result);
                    if (new_id == 0 || !request_id || !result_state) {
                        db_error = "TAS movie checkpoint sterilization attempt mapping is missing";
                        return;
                    }
                    Statement st;
                    if (!Prepare(analysis_db_,
                            "INSERT INTO tmv_checkpoint_sterilization_attempt(sterilization_attempt_id,sterilization_request_id,source_job_id,worker_terminal_sha256,"
                            "candidate_savestate_sha256,produced_savestate_id,worker_id,worker_process_generation,workset_epoch,recorded_at_utc) "
                            "VALUES(?1,?2,?3,json_extract(?4,'$.worker_terminal_sha256'),json_extract(?4,'$.candidate_savestate_sha256'),?5,"
                            "json_extract(?4,'$.worker_id'),json_extract(?4,'$.worker_process_generation'),json_extract(?4,'$.workset_epoch'),json_extract(?4,'$.recorded_at_utc'));",
                            &st, &db_error)) return;
                    sqlite3_bind_int64(st.st, 1, new_id);
                    sqlite3_bind_int64(st.st, 2, *request_id);
                    sqlite3_bind_int64(st.st, 3, job_id);
                    sqlite3_bind_text(st.st, 4, line.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(st.st, 5, *result_state);
                    StepDone(analysis_db_, st.st, &db_error);
                });

            restore_stream("analysis_tas_movie_validation_requests", [&](const std::string& line) {
                bool ok_id = false, ok_source = false, ok_kind = false;
                const auto old_id = JsonExtractInt(analysis_db_, line, "$.validation_request_id", &ok_id);
                const auto old_source = JsonExtractInt(analysis_db_, line, "$.source_ref_id", &ok_source);
                const auto source_kind = JsonExtractText(analysis_db_, line, "$.source_kind", &ok_kind);
                if (!ok_id || !ok_source || !ok_kind || source_kind != "ROOT_ESTABLISHMENT") return;
                const auto request_id = lookup_map("analysis_tas_movie_validation_request", old_id);
                const auto source = lookup_map("analysis_tas_movie_validation_attempt", old_source);
                if (!request_id || !source) {
                    db_error = "root-validation establishment attempt mapping is missing";
                    return;
                }
                Statement st;
                if (!Prepare(analysis_db_, "UPDATE tmv_validation_request SET source_ref_id=?1 WHERE validation_request_id=?2;", &st, &db_error)) return;
                sqlite3_bind_int64(st.st, 1, *source);
                sqlite3_bind_int64(st.st, 2, *request_id);
                StepDone(analysis_db_, st.st, &db_error);
            });

            restore_stream("analysis_battle_completions", [&](const std::string& line) {
                bool ok_id = false, ok_workflow = false, ok_step = false, ok_job = false;
                bool ok_entry = false, ok_completion = false, ok_manifest_artifact = false, ok_trace_artifact = false;
                bool ok_manifest_hex = false;
                const auto old_id = JsonExtractInt(analysis_db_, line, "$.battle_completion_id", &ok_id);
                const auto old_workflow = JsonExtractInt(analysis_db_, line, "$.workflow_instance_id", &ok_workflow);
                const auto old_step = JsonExtractInt(analysis_db_, line, "$.workflow_step_id", &ok_step);
                const auto old_job = JsonExtractInt(analysis_db_, line, "$.exec_job_id", &ok_job);
                const auto old_entry = JsonExtractInt(analysis_db_, line, "$.entry_savestate_id", &ok_entry);
                const auto old_completion = JsonExtractInt(analysis_db_, line, "$.completion_savestate_id", &ok_completion);
                const auto old_manifest_artifact = JsonExtractInt(analysis_db_, line, "$.manifest_artifact_id", &ok_manifest_artifact);
                const auto old_trace_artifact = JsonExtractInt(analysis_db_, line, "$.input_trace_artifact_id", &ok_trace_artifact);
                const auto manifest_hex = JsonExtractText(analysis_db_, line, "$.manifest_blob_hex", &ok_manifest_hex);
                if (!ok_id || !ok_workflow || !ok_step || !ok_entry) return;
                const auto new_id = map_id("analysis_battle_completion", old_id);
                const auto workflow = map_optional("workflow_instance", true, old_workflow);
                const auto step = map_optional("workflow_step", true, old_step);
                const auto job = map_optional("job", ok_job, old_job);
                const auto entry = map_savestate(true, old_entry);
                const auto completion = map_savestate(ok_completion, old_completion);
                const auto manifest_blob = ok_manifest_hex
                    ? DecodeHex(manifest_hex)
                    : std::optional<std::string>{};
                if (ok_manifest_hex && !manifest_blob.has_value()) {
                    db_error = "battle completion manifest hex is invalid";
                    return;
                }
                const auto manifest_artifact = ok_manifest_artifact
                    ? lookup_map("state_artifact", old_manifest_artifact)
                    : std::optional<std::int64_t>{};
                const auto trace_artifact = ok_trace_artifact
                    ? lookup_map("state_artifact", old_trace_artifact)
                    : std::optional<std::int64_t>{};
                if ((ok_manifest_artifact && !manifest_artifact.has_value())
                    || (ok_trace_artifact && !trace_artifact.has_value())) {
                    db_error = "battle completion artifact mapping is missing";
                    return;
                }
                if (new_id == 0 || !workflow || !step || !entry) return;
                Statement st;
                if (!Prepare(analysis_db_,
                        "INSERT INTO ab_battle_completion(battle_completion_id,workflow_instance_id,workflow_step_id,exec_job_id,"
                        "entry_savestate_id,completion_savestate_id,entry_rng_seed,completion_rng_seed,manifest_version,manifest_blob,"
                        "manifest_artifact_id,input_trace_artifact_id,mismatch_count,invariant_failure_count,status,created_at_utc,completed_at_utc) "
                        "VALUES(?1,?2,?3,?4,?5,?6,json_extract(?7,'$.entry_rng_seed'),json_extract(?7,'$.completion_rng_seed'),"
                        "json_extract(?7,'$.manifest_version'),?8,?9,?10,json_extract(?7,'$.mismatch_count'),"
                        "json_extract(?7,'$.invariant_failure_count'),json_extract(?7,'$.status'),json_extract(?7,'$.created_at_utc'),json_extract(?7,'$.completed_at_utc'));",
                        &st, &db_error)) return;
                sqlite3_bind_int64(st.st, 1, new_id);
                sqlite3_bind_int64(st.st, 2, *workflow);
                sqlite3_bind_int64(st.st, 3, *step);
                bind_optional_int64(st.st, 4, job);
                sqlite3_bind_int64(st.st, 5, *entry);
                bind_optional_int64(st.st, 6, completion);
                sqlite3_bind_text(st.st, 7, line.c_str(), -1, SQLITE_TRANSIENT);
                if (manifest_blob.has_value()) {
                    sqlite3_bind_blob(st.st, 8, manifest_blob->data(), static_cast<int>(manifest_blob->size()), SQLITE_TRANSIENT);
                } else {
                    sqlite3_bind_null(st.st, 8);
                }
                bind_optional_int64(st.st, 9, manifest_artifact);
                bind_optional_int64(st.st, 10, trace_artifact);
                StepDone(analysis_db_, st.st, &db_error);
            });

            restore_stream("analysis_battle_sets", [&](const std::string& line) {
                bool ok_id = false;
                bool ok_entry = false;
                const auto old_id = JsonExtractInt(analysis_db_, line, "$.battle_set_id", &ok_id);
                const auto old_entry = JsonExtractInt(analysis_db_, line, "$.entry_savestate_id", &ok_entry);
                if (!ok_id) return;
                const auto new_id = map_id("analysis_battle_set", old_id);
                const auto entry = map_savestate(ok_entry, old_entry);
                if (new_id == 0 || !entry.has_value()) return;
                const auto name = spec.target_namespace + ":rehydrate:" + JsonExtractText(analysis_db_, line, "$.name", &ok_id);
                Statement st;
                if (!Prepare(analysis_db_,
                        "INSERT INTO ab_battle_set(battle_set_id,name,entry_savestate_id,battle_run_spec_id,explorer_settings_id,status,created_at_utc,completed_at_utc) "
                        "VALUES(?1,?2,?3,json_extract(?4,'$.battle_run_spec_id'),json_extract(?4,'$.explorer_settings_id'),json_extract(?4,'$.status'),json_extract(?4,'$.created_at_utc'),json_extract(?4,'$.completed_at_utc'));",
                        &st,
                        &db_error)) return;
                sqlite3_bind_int64(st.st, 1, new_id);
                sqlite3_bind_text(st.st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(st.st, 3, *entry);
                sqlite3_bind_text(st.st, 4, line.c_str(), -1, SQLITE_TRANSIENT);
                StepDone(analysis_db_, st.st, &db_error);
            });
            restore_stream("analysis_seed_candidates", [&](const std::string& line) {
                bool ok_id = false, ok_battle = false, ok_result = false, ok_frame = false;
                const auto old_id = JsonExtractInt(analysis_db_, line, "$.seed_candidate_id", &ok_id);
                const auto old_battle = JsonExtractInt(analysis_db_, line, "$.battle_set_id", &ok_battle);
                const auto old_result = JsonExtractInt(analysis_db_, line, "$.source_probe_result_id", &ok_result);
                const auto old_frame = JsonExtractInt(analysis_db_, line, "$.source_input_frame_id", &ok_frame);
                if (!ok_id || !ok_battle) return;
                const auto new_id = map_id("analysis_seed_candidate", old_id);
                const auto battle = map_optional("analysis_battle_set", true, old_battle);
                const auto probe_result = map_optional("analysis_seed_probe_result", ok_result, old_result);
                const auto frame = map_optional("analysis_seed_probe_input_frame", ok_frame, old_frame);
                if (new_id == 0 || !battle.has_value()) return;
                Statement st;
                if (!Prepare(analysis_db_,
                        "INSERT INTO ab_seed_candidate(seed_candidate_id,battle_set_id,source_probe_result_id,source_input_frame_id,seed_value,source_kind,candidate_status,created_at_utc) "
                        "VALUES(?1,?2,?3,?4,json_extract(?5,'$.seed_value'),json_extract(?5,'$.source_kind'),json_extract(?5,'$.candidate_status'),json_extract(?5,'$.created_at_utc'));",
                        &st,
                        &db_error)) return;
                sqlite3_bind_int64(st.st, 1, new_id);
                sqlite3_bind_int64(st.st, 2, *battle);
                bind_optional_int64(st.st, 3, probe_result);
                bind_optional_int64(st.st, 4, frame);
                sqlite3_bind_text(st.st, 5, line.c_str(), -1, SQLITE_TRANSIENT);
                StepDone(analysis_db_, st.st, &db_error);
            });
            restore_stream("analysis_battle_advancement_pools", [&](const std::string& line) {
                bool ok_id = false, ok_battle = false;
                const auto old_id = JsonExtractInt(analysis_db_, line, "$.battle_advancement_pool_id", &ok_id);
                const auto old_battle = JsonExtractInt(analysis_db_, line, "$.battle_set_id", &ok_battle);
                if (!ok_id || !ok_battle) return;
                const auto new_id = map_id("analysis_battle_advancement_pool", old_id);
                const auto battle = map_optional("analysis_battle_set", true, old_battle);
                if (new_id == 0 || !battle.has_value()) return;
                Statement st;
                if (!Prepare(analysis_db_,
                        "INSERT INTO ab_battle_advancement_pool(battle_advancement_pool_id,battle_set_id,turn_index,pool_name,criterion_kind,created_at_utc) "
                        "VALUES(?1,?2,json_extract(?3,'$.turn_index'),json_extract(?3,'$.pool_name'),json_extract(?3,'$.criterion_kind'),json_extract(?3,'$.created_at_utc'));",
                        &st,
                        &db_error)) return;
                sqlite3_bind_int64(st.st, 1, new_id);
                sqlite3_bind_int64(st.st, 2, *battle);
                sqlite3_bind_text(st.st, 3, line.c_str(), -1, SQLITE_TRANSIENT);
                StepDone(analysis_db_, st.st, &db_error);
            });
            restore_stream("analysis_turn_waves", [&](const std::string& line) {
                bool ok_id = false, ok_battle = false, ok_seed = false, ok_pool = false, ok_parent_wave = false;
                const auto old_id = JsonExtractInt(analysis_db_, line, "$.wave_id", &ok_id);
                const auto old_battle = JsonExtractInt(analysis_db_, line, "$.battle_set_id", &ok_battle);
                const auto old_seed = JsonExtractInt(analysis_db_, line, "$.seed_candidate_id", &ok_seed);
                const auto old_pool = JsonExtractInt(analysis_db_, line, "$.battle_advancement_pool_id", &ok_pool);
                const auto old_parent_wave = JsonExtractInt(analysis_db_, line, "$.parent_wave_id", &ok_parent_wave);
                if (!ok_id || !ok_battle || !ok_seed) return;
                const auto new_id = map_id("analysis_turn_wave", old_id);
                const auto battle = map_optional("analysis_battle_set", true, old_battle);
                const auto seed = map_optional("analysis_seed_candidate", true, old_seed);
                const auto pool = map_optional("analysis_battle_advancement_pool", ok_pool, old_pool);
                const auto parent_wave = map_optional("analysis_turn_wave", ok_parent_wave, old_parent_wave);
                if (new_id == 0 || !battle.has_value() || !seed.has_value()) return;
                Statement st;
                if (!Prepare(analysis_db_,
                        "INSERT INTO ab_turn_wave(wave_id,battle_set_id,turn_index,context_probe_id,parent_wave_id,parent_turn_job_id,seed_candidate_id,battle_advancement_pool_id,status,created_at_utc,completed_at_utc) "
                        "VALUES(?1,?2,json_extract(?3,'$.turn_index'),NULL,?4,NULL,?5,?6,json_extract(?3,'$.status'),json_extract(?3,'$.created_at_utc'),json_extract(?3,'$.completed_at_utc'));",
                        &st,
                        &db_error)) return;
                sqlite3_bind_int64(st.st, 1, new_id);
                sqlite3_bind_int64(st.st, 2, *battle);
                sqlite3_bind_text(st.st, 3, line.c_str(), -1, SQLITE_TRANSIENT);
                bind_optional_int64(st.st, 4, parent_wave);
                sqlite3_bind_int64(st.st, 5, *seed);
                bind_optional_int64(st.st, 6, pool);
                StepDone(analysis_db_, st.st, &db_error);
            });
            restore_stream("analysis_battle_context_probes", [&](const std::string& line) {
                bool ok_id = false, ok_wave = false, ok_sav = false, ok_job = false;
                const auto old_id = JsonExtractInt(analysis_db_, line, "$.context_probe_id", &ok_id);
                const auto old_wave = JsonExtractInt(analysis_db_, line, "$.wave_id", &ok_wave);
                const auto old_sav = JsonExtractInt(analysis_db_, line, "$.source_savestate_id", &ok_sav);
                const auto old_job = JsonExtractInt(analysis_db_, line, "$.exec_job_id", &ok_job);
                if (!ok_id || !ok_sav) return;
                const auto new_id = map_id("analysis_battle_context_probe", old_id);
                const auto wave = map_optional("analysis_turn_wave", ok_wave, old_wave);
                const auto sav = map_savestate(ok_sav, old_sav);
                const auto job = map_optional("job", ok_job, old_job);
                if (new_id == 0 || !sav.has_value()) return;
                Statement st;
                if (!Prepare(analysis_db_,
                        "INSERT INTO ab_battle_context_probe(context_probe_id,wave_id,source_savestate_id,exec_job_id,probe_status,context_blob,context_version,recorded_at_utc,created_at_utc) "
                        "VALUES(?1,?2,?3,?4,json_extract(?5,'$.probe_status'),json_extract(?5,'$.context_blob'),json_extract(?5,'$.context_version'),json_extract(?5,'$.recorded_at_utc'),json_extract(?5,'$.created_at_utc'));",
                        &st,
                        &db_error)) return;
                sqlite3_bind_int64(st.st, 1, new_id);
                bind_optional_int64(st.st, 2, wave);
                sqlite3_bind_int64(st.st, 3, *sav);
                bind_optional_int64(st.st, 4, job);
                sqlite3_bind_text(st.st, 5, line.c_str(), -1, SQLITE_TRANSIENT);
                StepDone(analysis_db_, st.st, &db_error);
            });
            restore_stream("analysis_battle_turn_jobs", [&](const std::string& line) {
                bool ok_id = false, ok_wave = false, ok_job = false, ok_source_sav = false, ok_output_sav = false, ok_seed = false;
                const auto old_id = JsonExtractInt(analysis_db_, line, "$.turn_job_id", &ok_id);
                const auto old_wave = JsonExtractInt(analysis_db_, line, "$.wave_id", &ok_wave);
                const auto old_job = JsonExtractInt(analysis_db_, line, "$.exec_job_id", &ok_job);
                const auto old_source_sav = JsonExtractInt(analysis_db_, line, "$.source_savestate_id", &ok_source_sav);
                const auto old_output_sav = JsonExtractInt(analysis_db_, line, "$.output_savestate_id", &ok_output_sav);
                const auto old_seed = JsonExtractInt(analysis_db_, line, "$.seed_candidate_id", &ok_seed);
                if (!ok_id || !ok_wave) return;
                const auto new_id = map_id("analysis_battle_turn_job", old_id);
                const auto wave = map_optional("analysis_turn_wave", true, old_wave);
                const auto job = map_optional("job", ok_job, old_job);
                const auto source_sav = map_savestate(ok_source_sav, old_source_sav);
                const auto output_sav = map_savestate(ok_output_sav, old_output_sav);
                const auto seed = map_optional("analysis_seed_candidate", ok_seed, old_seed);
                if (new_id == 0 || !wave.has_value()) return;
                Statement st;
                if (!Prepare(analysis_db_,
                        "INSERT INTO ab_turn_job(turn_job_id,wave_id,exec_job_id,plan_id,fake_attacks_this_turn,fake_attacks_used_before,job_state,started_at_utc,ended_at_utc,has_results,vi_start,vi_end,delta_vi,rng_seed,battle_outcome,plan_materialize_err,pred_passed,pred_total,pred_abort_run,output_savestate_id,applied_input_artifact_id,recorded_at_utc,result_context_blob_base64,result_context_version,source_savestate_id,seed_candidate_id,authored_plan_id,authored_turn_index,resolved_turn_commands_blob,resolved_turn_variant_key,input_trace_artifact_id) "
                        "VALUES(?1,?2,?3,json_extract(?4,'$.plan_id'),json_extract(?4,'$.fake_attacks_this_turn'),json_extract(?4,'$.fake_attacks_used_before'),json_extract(?4,'$.job_state'),json_extract(?4,'$.started_at_utc'),json_extract(?4,'$.ended_at_utc'),json_extract(?4,'$.has_results'),json_extract(?4,'$.vi_start'),json_extract(?4,'$.vi_end'),json_extract(?4,'$.delta_vi'),json_extract(?4,'$.rng_seed'),json_extract(?4,'$.battle_outcome'),json_extract(?4,'$.plan_materialize_err'),json_extract(?4,'$.pred_passed'),json_extract(?4,'$.pred_total'),json_extract(?4,'$.pred_abort_run'),?5,json_extract(?4,'$.applied_input_artifact_id'),json_extract(?4,'$.recorded_at_utc'),json_extract(?4,'$.result_context_blob_base64'),json_extract(?4,'$.result_context_version'),?6,?7,json_extract(?4,'$.authored_plan_id'),json_extract(?4,'$.authored_turn_index'),json_extract(?4,'$.resolved_turn_commands_blob'),json_extract(?4,'$.resolved_turn_variant_key'),json_extract(?4,'$.input_trace_artifact_id'));",
                        &st,
                        &db_error)) return;
                sqlite3_bind_int64(st.st, 1, new_id);
                sqlite3_bind_int64(st.st, 2, *wave);
                bind_optional_int64(st.st, 3, job);
                sqlite3_bind_text(st.st, 4, line.c_str(), -1, SQLITE_TRANSIENT);
                bind_optional_int64(st.st, 5, output_sav);
                bind_optional_int64(st.st, 6, source_sav);
                bind_optional_int64(st.st, 7, seed);
                StepDone(analysis_db_, st.st, &db_error);
            });
            restore_stream("analysis_battle_advancement_decisions", [&](const std::string& line) {
                bool ok_id = false, ok_pool = false, ok_turn = false;
                const auto old_id = JsonExtractInt(analysis_db_, line, "$.battle_advancement_decision_id", &ok_id);
                const auto old_pool = JsonExtractInt(analysis_db_, line, "$.battle_advancement_pool_id", &ok_pool);
                const auto old_turn = JsonExtractInt(analysis_db_, line, "$.turn_job_id", &ok_turn);
                if (!ok_id || !ok_pool || !ok_turn) return;
                const auto new_id = map_id("analysis_battle_advancement_decision", old_id);
                const auto pool = map_optional("analysis_battle_advancement_pool", true, old_pool);
                const auto turn = map_optional("analysis_battle_turn_job", true, old_turn);
                if (new_id == 0 || !pool.has_value() || !turn.has_value()) return;
                Statement st;
                if (!Prepare(analysis_db_,
                        "INSERT INTO ab_battle_advancement_decision(battle_advancement_decision_id,battle_advancement_pool_id,turn_job_id,decision_kind,decision_reason,created_at_utc) "
                        "VALUES(?1,?2,?3,json_extract(?4,'$.decision_kind'),json_extract(?4,'$.decision_reason'),json_extract(?4,'$.created_at_utc'));",
                        &st,
                        &db_error)) return;
                sqlite3_bind_int64(st.st, 1, new_id);
                sqlite3_bind_int64(st.st, 2, *pool);
                sqlite3_bind_int64(st.st, 3, *turn);
                sqlite3_bind_text(st.st, 4, line.c_str(), -1, SQLITE_TRANSIENT);
                StepDone(analysis_db_, st.st, &db_error);
            });
            restore_stream("analysis_manual_followups", [&](const std::string& line) {
                bool ok_id = false, ok_turn = false;
                const auto old_id = JsonExtractInt(analysis_db_, line, "$.manual_followup_id", &ok_id);
                const auto old_turn = JsonExtractInt(analysis_db_, line, "$.turn_job_id", &ok_turn);
                if (!ok_id || !ok_turn) return;
                const auto new_id = map_id("analysis_manual_followup", old_id);
                const auto turn = map_optional("analysis_battle_turn_job", true, old_turn);
                if (new_id == 0 || !turn.has_value()) return;
                Statement st;
                if (!Prepare(analysis_db_,
                        "INSERT INTO ab_manual_followup(manual_followup_id,turn_job_id,manual_followup_status,recorded_dtm_artifact_id,recorded_dtmini_artifact_id,recorded_sav_artifact_id,note,updated_at_utc) "
                        "VALUES(?1,?2,json_extract(?3,'$.manual_followup_status'),json_extract(?3,'$.recorded_dtm_artifact_id'),json_extract(?3,'$.recorded_dtmini_artifact_id'),json_extract(?3,'$.recorded_sav_artifact_id'),json_extract(?3,'$.note'),json_extract(?3,'$.updated_at_utc'));",
                        &st,
                        &db_error)) return;
                sqlite3_bind_int64(st.st, 1, new_id);
                sqlite3_bind_int64(st.st, 2, *turn);
                sqlite3_bind_text(st.st, 3, line.c_str(), -1, SQLITE_TRANSIENT);
                StepDone(analysis_db_, st.st, &db_error);
            });

            restore_stream("analysis_seed_probe_sets", [&](const std::string& line) {
                bool ok_id = false;
                const auto old_id = JsonExtractInt(analysis_db_, line, "$.probe_set_id", &ok_id);
                if (!ok_id) return;
                const auto new_id = map_id("analysis_seed_probe_set", old_id);
                const auto name = spec.target_namespace + ":rehydrate:" + JsonExtractText(analysis_db_, line, "$.name", &ok_id);
                Statement st;
                if (!Prepare(analysis_db_,
                        "INSERT INTO sp_probe_set(probe_set_id,name,probe_flavor,breakpoint_policy_name,dungeon_segment_file_num,dungeon_segment_file_letter,dungeon_segment_code,segment_source_kind,created_at_utc) "
                        "VALUES(?1,?2,json_extract(?3,'$.probe_flavor'),json_extract(?3,'$.breakpoint_policy_name'),json_extract(?3,'$.dungeon_segment_file_num'),json_extract(?3,'$.dungeon_segment_file_letter'),json_extract(?3,'$.dungeon_segment_code'),json_extract(?3,'$.segment_source_kind'),json_extract(?3,'$.created_at_utc'));",
                        &st,
                        &db_error)) return;
                sqlite3_bind_int64(st.st, 1, new_id);
                sqlite3_bind_text(st.st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st.st, 3, line.c_str(), -1, SQLITE_TRANSIENT);
                StepDone(analysis_db_, st.st, &db_error);
            });
            restore_stream("analysis_input_sets", [&](const std::string& line) {
                bool ok_id = false;
                const auto old_id = JsonExtractInt(analysis_db_, line, "$.input_set_id", &ok_id);
                if (!ok_id) return;
                const auto new_id = map_id("analysis_input_set", old_id);
                const auto content_hash = spec.target_namespace + ":rehydrate:" + std::to_string(old_id);
                Statement st;
                if (!Prepare(analysis_db_,
                        "INSERT INTO an_input_set(input_set_id,content_hash,source_ref_kind,source_ref_id,created_at_utc) "
                        "VALUES(?1,?2,NULL,NULL,json_extract(?3,'$.created_at_utc'));",
                        &st,
                        &db_error)) return;
                sqlite3_bind_int64(st.st, 1, new_id);
                sqlite3_bind_text(st.st, 2, content_hash.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st.st, 3, line.c_str(), -1, SQLITE_TRANSIENT);
                StepDone(analysis_db_, st.st, &db_error);
            });
            restore_stream("analysis_seed_probe_axis_xy", [&](const std::string& line) {
                bool ok_id = false, ok_x = false, ok_y = false;
                const auto old_id = JsonExtractInt(analysis_db_, line, "$.axis_xy_id", &ok_id);
                const auto x = JsonExtractInt(analysis_db_, line, "$.x", &ok_x);
                const auto y = JsonExtractInt(analysis_db_, line, "$.y", &ok_y);
                if (!ok_id) return;
                if (ok_x && ok_y) {
                    Statement find;
                    if (Prepare(analysis_db_, "SELECT axis_xy_id FROM sp_axis_xy WHERE x=?1 AND y=?2 LIMIT 1;", &find, &db_error)) {
                        sqlite3_bind_int64(find.st, 1, x);
                        sqlite3_bind_int64(find.st, 2, y);
                        if (sqlite3_step(find.st) == SQLITE_ROW) {
                            map_existing_id("analysis_seed_probe_axis_xy", old_id, sqlite3_column_int64(find.st, 0), &db_error);
                            return;
                        }
                    }
                    if (!db_error.empty()) return;
                }
                const auto new_id = map_id("analysis_seed_probe_axis_xy", old_id);
                Statement st;
                if (!Prepare(analysis_db_, "INSERT INTO sp_axis_xy(axis_xy_id,x,y) VALUES(?1,json_extract(?2,'$.x'),json_extract(?2,'$.y'));", &st, &db_error)) return;
                sqlite3_bind_int64(st.st, 1, new_id);
                sqlite3_bind_text(st.st, 2, line.c_str(), -1, SQLITE_TRANSIENT);
                StepDone(analysis_db_, st.st, &db_error);
            });
            restore_stream("analysis_seed_probe_input_frames", [&](const std::string& line) {
                bool ok_id = false, ok_main = false, ok_c = false, ok_t = false;
                const auto old_id = JsonExtractInt(analysis_db_, line, "$.input_frame_id", &ok_id);
                const auto old_main = JsonExtractInt(analysis_db_, line, "$.main_axis_xy_id", &ok_main);
                const auto old_c = JsonExtractInt(analysis_db_, line, "$.cstick_axis_xy_id", &ok_c);
                const auto old_t = JsonExtractInt(analysis_db_, line, "$.trigger_axis_xy_id", &ok_t);
                if (!ok_id || !ok_main || !ok_c || !ok_t) return;
                const auto main_axis = map_optional("analysis_seed_probe_axis_xy", true, old_main);
                const auto c_axis = map_optional("analysis_seed_probe_axis_xy", true, old_c);
                const auto t_axis = map_optional("analysis_seed_probe_axis_xy", true, old_t);
                if (!main_axis || !c_axis || !t_axis) return;
                Statement find;
                if (Prepare(
                        analysis_db_,
                        "SELECT input_frame_id FROM sp_input_frame WHERE main_axis_xy_id=?1 AND cstick_axis_xy_id=?2 AND trigger_axis_xy_id=?3 LIMIT 1;",
                        &find,
                        &db_error)) {
                    sqlite3_bind_int64(find.st, 1, *main_axis);
                    sqlite3_bind_int64(find.st, 2, *c_axis);
                    sqlite3_bind_int64(find.st, 3, *t_axis);
                    if (sqlite3_step(find.st) == SQLITE_ROW) {
                        map_existing_id("analysis_seed_probe_input_frame", old_id, sqlite3_column_int64(find.st, 0), &db_error);
                        return;
                    }
                }
                if (!db_error.empty()) return;
                const auto new_id = map_id("analysis_seed_probe_input_frame", old_id);
                if (new_id == 0) return;
                Statement st;
                if (!Prepare(analysis_db_,
                        "INSERT INTO sp_input_frame(input_frame_id,main_axis_xy_id,cstick_axis_xy_id,trigger_axis_xy_id) VALUES(?1,?2,?3,?4);",
                        &st,
                        &db_error)) return;
                sqlite3_bind_int64(st.st, 1, new_id);
                sqlite3_bind_int64(st.st, 2, *main_axis);
                sqlite3_bind_int64(st.st, 3, *c_axis);
                sqlite3_bind_int64(st.st, 4, *t_axis);
                StepDone(analysis_db_, st.st, &db_error);
            });
            restore_stream("analysis_input_set_frames", [&](const std::string& line) {
                bool ok_set = false, ok_frame = false;
                const auto old_set = JsonExtractInt(analysis_db_, line, "$.input_set_id", &ok_set);
                const auto old_frame = JsonExtractInt(analysis_db_, line, "$.input_frame_id", &ok_frame);
                const auto set_id = map_optional("analysis_input_set", ok_set, old_set);
                const auto frame_id = map_optional("analysis_seed_probe_input_frame", ok_frame, old_frame);
                if (!set_id || !frame_id) return;
                Statement st;
                if (!Prepare(analysis_db_, "INSERT OR IGNORE INTO an_input_set_frame(input_set_id,ordinal,input_frame_id,added_at_utc) VALUES(?1,json_extract(?2,'$.ordinal'),?3,json_extract(?2,'$.added_at_utc'));", &st, &db_error)) return;
                sqlite3_bind_int64(st.st, 1, *set_id);
                sqlite3_bind_text(st.st, 2, line.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(st.st, 3, *frame_id);
                StepDone(analysis_db_, st.st, &db_error);
            });
            restore_stream("analysis_seed_probe_runs", [&](const std::string& line) {
                bool ok_id = false, ok_set = false, ok_sav = false, ok_input_set = false;
                bool ok_established_source_job = false;
                bool ok_conflicting_source_job = false;
                const auto old_id = JsonExtractInt(analysis_db_, line, "$.probe_run_id", &ok_id);
                const auto old_set = JsonExtractInt(analysis_db_, line, "$.probe_set_id", &ok_set);
                const auto old_sav = JsonExtractInt(analysis_db_, line, "$.entry_savestate_id", &ok_sav);
                const auto old_input_set = JsonExtractInt(analysis_db_, line, "$.accepted_input_set_id", &ok_input_set);
                const auto old_established_source_job = JsonExtractInt(
                    analysis_db_, line, "$.established_endpoint_source_job_id",
                    &ok_established_source_job);
                const auto old_conflicting_source_job = JsonExtractInt(
                    analysis_db_, line, "$.conflicting_endpoint_source_job_id",
                    &ok_conflicting_source_job);
                if (!ok_id || !ok_set || !ok_sav || !ok_input_set) return;
                const auto new_id = map_id("analysis_seed_probe_run", old_id);
                const auto set_id = map_optional("analysis_seed_probe_set", true, old_set);
                const auto sav = map_savestate(true, old_sav);
                const auto input_set = map_optional("analysis_input_set", true, old_input_set);
                if (new_id == 0 || !set_id || !sav || !input_set) return;
                const auto established_source_job = map_optional(
                    "job", ok_established_source_job,
                    old_established_source_job);
                const auto conflicting_source_job = map_optional(
                    "job", ok_conflicting_source_job,
                    old_conflicting_source_job);
                if ((ok_established_source_job && !established_source_job)
                    || (ok_conflicting_source_job && !conflicting_source_job)) {
                    db_error = "SeedProbe endpoint source job mapping is missing";
                    return;
                }
                bool ok_old_materialization_key = false;
                const auto old_materialization_key = JsonExtractText(
                    analysis_db_,
                    line,
                    "$.materialization_key",
                    &ok_old_materialization_key);
                const auto materialization_key = Fnv1a64(
                    spec.target_namespace
                    + ":seedprobe-run-materialization:"
                    + std::to_string(new_id)
                    + ":"
                    + (ok_old_materialization_key
                           ? old_materialization_key
                           : std::to_string(old_id)));
                Statement st;
                if (!Prepare(analysis_db_,
                        "INSERT INTO sp_probe_run(probe_run_id,materialization_key,probe_set_id,entry_savestate_id,seed_probe_spec_id,codec_version,status,accepted_input_set_id,requested_at_utc,completed_at_utc,launch_samples_per_axis,established_endpoint,established_endpoint_source_job_id,conflicting_endpoint,conflicting_endpoint_source_job_id,invalidation_diagnostic,invalidated_at_utc) "
                        "VALUES(?1,?6,?2,?3,json_extract(?4,'$.seed_probe_spec_id'),json_extract(?4,'$.codec_version'),json_extract(?4,'$.status'),?5,json_extract(?4,'$.requested_at_utc'),json_extract(?4,'$.completed_at_utc'),json_extract(?4,'$.launch_samples_per_axis'),json_extract(?4,'$.established_endpoint'),?7,json_extract(?4,'$.conflicting_endpoint'),?8,json_extract(?4,'$.invalidation_diagnostic'),json_extract(?4,'$.invalidated_at_utc'));",
                        &st,
                        &db_error)) return;
                sqlite3_bind_int64(st.st, 1, new_id);
                sqlite3_bind_int64(st.st, 2, *set_id);
                sqlite3_bind_int64(st.st, 3, *sav);
                sqlite3_bind_text(st.st, 4, line.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(st.st, 5, *input_set);
                sqlite3_bind_text(
                    st.st,
                    6,
                    materialization_key.c_str(),
                    -1,
                    SQLITE_TRANSIENT);
                if (established_source_job) {
                    sqlite3_bind_int64(st.st, 7, *established_source_job);
                } else {
                    sqlite3_bind_null(st.st, 7);
                }
                if (conflicting_source_job) {
                    sqlite3_bind_int64(st.st, 8, *conflicting_source_job);
                } else {
                    sqlite3_bind_null(st.st, 8);
                }
                if (!StepDone(analysis_db_, st.st, &db_error)) return;
                Statement attach_input_set;
                if (!Prepare(
                        analysis_db_,
                        "UPDATE an_input_set SET source_ref_kind='sp_probe_run',source_ref_id=?2 "
                        "WHERE input_set_id=?1;",
                        &attach_input_set,
                        &db_error)) return;
                sqlite3_bind_int64(attach_input_set.st, 1, *input_set);
                sqlite3_bind_int64(attach_input_set.st, 2, new_id);
                StepDone(analysis_db_, attach_input_set.st, &db_error);
            });
            restore_stream("analysis_seed_probe_results", [&](const std::string& line) {
                bool ok_id = false, ok_run = false, ok_frame = false, ok_job = false, ok_confirmation = false;
                const auto old_id = JsonExtractInt(analysis_db_, line, "$.probe_result_id", &ok_id);
                const auto old_run = JsonExtractInt(analysis_db_, line, "$.probe_run_id", &ok_run);
                const auto old_frame = JsonExtractInt(analysis_db_, line, "$.input_frame_id", &ok_frame);
                const auto old_job = JsonExtractInt(analysis_db_, line, "$.source_job_id", &ok_job);
                const auto old_confirmation = JsonExtractInt(
                    analysis_db_, line, "$.confirmation_of_probe_result_id", &ok_confirmation);
                if (!ok_id || !ok_run || !ok_frame || !ok_job) return;
                const auto new_id = map_id("analysis_seed_probe_result", old_id);
                const auto run = map_optional("analysis_seed_probe_run", true, old_run);
                const auto frame = map_optional("analysis_seed_probe_input_frame", true, old_frame);
                const auto job = map_optional("job", true, old_job);
                const auto confirmation = map_optional(
                    "analysis_seed_probe_result", ok_confirmation, old_confirmation);
                if (new_id == 0 || !run || !frame || !job) return;
                Statement st;
                if (!Prepare(analysis_db_,
                        "INSERT INTO sp_probe_result(probe_result_id,probe_run_id,input_frame_id,source_job_id,seed_value,"
                        "origin_worker_id,origin_process_generation,origin_workset_epoch,terminal_sha256,"
                        "confirmation_of_probe_result_id,evidence_state,recorded_at_utc) "
                        "VALUES(?1,?2,?3,?4,json_extract(?5,'$.seed_value'),"
                        "json_extract(?5,'$.origin_worker_id'),json_extract(?5,'$.origin_process_generation'),"
                        "json_extract(?5,'$.origin_workset_epoch'),json_extract(?5,'$.terminal_sha256'),"
                        "?6,json_extract(?5,'$.evidence_state'),"
                        "json_extract(?5,'$.recorded_at_utc'));",
                        &st,
                        &db_error)) return;
                sqlite3_bind_int64(st.st, 1, new_id);
                sqlite3_bind_int64(st.st, 2, *run);
                sqlite3_bind_int64(st.st, 3, *frame);
                sqlite3_bind_int64(st.st, 4, *job);
                sqlite3_bind_text(st.st, 5, line.c_str(), -1, SQLITE_TRANSIENT);
                bind_optional_int64(st.st, 6, confirmation);
                StepDone(analysis_db_, st.st, &db_error);
            });
            restore_stream("analysis_battle_results", [&](const std::string& line) {
                bool ok_id = false, ok_completion = false, ok_workflow = false, ok_step = false, ok_job = false;
                bool ok_seed_ref = false, ok_seed_kind = false, ok_entry = false, ok_final = false;
                bool ok_result_artifact = false, ok_trace_artifact = false;
                const auto old_id = JsonExtractInt(analysis_db_, line, "$.battle_results_id", &ok_id);
                const auto old_completion = JsonExtractInt(analysis_db_, line, "$.battle_completion_id", &ok_completion);
                const auto old_workflow = JsonExtractInt(analysis_db_, line, "$.workflow_instance_id", &ok_workflow);
                const auto old_step = JsonExtractInt(analysis_db_, line, "$.workflow_step_id", &ok_step);
                const auto old_job = JsonExtractInt(analysis_db_, line, "$.exec_job_id", &ok_job);
                const auto seed_kind = JsonExtractText(analysis_db_, line, "$.selected_seed_ref_kind", &ok_seed_kind);
                const auto old_seed_ref = JsonExtractInt(analysis_db_, line, "$.selected_seed_ref_id", &ok_seed_ref);
                const auto old_entry = JsonExtractInt(analysis_db_, line, "$.entry_savestate_id", &ok_entry);
                const auto old_final = JsonExtractInt(analysis_db_, line, "$.final_savestate_id", &ok_final);
                const auto old_result_artifact = JsonExtractInt(analysis_db_, line, "$.result_artifact_id", &ok_result_artifact);
                const auto old_trace_artifact = JsonExtractInt(analysis_db_, line, "$.input_trace_artifact_id", &ok_trace_artifact);
                if (!ok_id || !ok_completion || !ok_workflow || !ok_step || !ok_seed_kind || !ok_seed_ref || !ok_entry) return;
                const auto new_id = map_id("analysis_battle_results", old_id);
                const auto completion = map_optional("analysis_battle_completion", true, old_completion);
                const auto workflow = map_optional("workflow_instance", true, old_workflow);
                const auto step = map_optional("workflow_step", true, old_step);
                const auto job = map_optional("job", ok_job, old_job);
                const auto entry = map_savestate(true, old_entry);
                const auto final_state = map_savestate(ok_final, old_final);
                const auto seed_map_kind = seed_kind == "analysisseedprobe.confirmed_result"
                    ? std::string_view("analysis_seed_probe_result")
                    : std::string_view{};
                const auto seed_ref = seed_map_kind.empty() ? std::nullopt : lookup_map(seed_map_kind, old_seed_ref);
                if (!seed_ref.has_value()) {
                    db_error = "direct selected seed row mapping is missing during battle results rehydrate";
                    return;
                }
                const auto result_artifact = ok_result_artifact
                    ? lookup_map("state_artifact", old_result_artifact)
                    : std::optional<std::int64_t>{};
                const auto trace_artifact = ok_trace_artifact
                    ? lookup_map("state_artifact", old_trace_artifact)
                    : std::optional<std::int64_t>{};
                if ((ok_result_artifact && !result_artifact.has_value())
                    || (ok_trace_artifact && !trace_artifact.has_value())) {
                    db_error = "battle results artifact mapping is missing";
                    return;
                }
                if (new_id == 0 || !completion || !workflow || !step || !entry) return;
                Statement st;
                if (!Prepare(analysis_db_,
                        "INSERT INTO ab_battle_results(battle_results_id,battle_completion_id,workflow_instance_id,workflow_step_id,exec_job_id,"
                        "selected_seed_ref_kind,selected_seed_ref_id,entry_savestate_id,final_savestate_id,selected_seed_value,entry_rng_seed,final_rng_seed,"
                        "rng_effect_kind,fixed_draw_count,result_artifact_id,input_trace_artifact_id,mismatch_count,invariant_failure_count,status,created_at_utc,completed_at_utc) "
                        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,json_extract(?10,'$.selected_seed_value'),json_extract(?10,'$.entry_rng_seed'),"
                        "json_extract(?10,'$.final_rng_seed'),json_extract(?10,'$.rng_effect_kind'),json_extract(?10,'$.fixed_draw_count'),?11,?12,"
                        "json_extract(?10,'$.mismatch_count'),json_extract(?10,'$.invariant_failure_count'),json_extract(?10,'$.status'),"
                        "json_extract(?10,'$.created_at_utc'),json_extract(?10,'$.completed_at_utc'));",
                        &st, &db_error)) return;
                sqlite3_bind_int64(st.st, 1, new_id);
                sqlite3_bind_int64(st.st, 2, *completion);
                sqlite3_bind_int64(st.st, 3, *workflow);
                sqlite3_bind_int64(st.st, 4, *step);
                bind_optional_int64(st.st, 5, job);
                sqlite3_bind_text(st.st, 6, seed_kind.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(st.st, 7, *seed_ref);
                sqlite3_bind_int64(st.st, 8, *entry);
                bind_optional_int64(st.st, 9, final_state);
                sqlite3_bind_text(st.st, 10, line.c_str(), -1, SQLITE_TRANSIENT);
                bind_optional_int64(st.st, 11, result_artifact);
                bind_optional_int64(st.st, 12, trace_artifact);
                StepDone(analysis_db_, st.st, &db_error);
            });
            restore_stream("analysis_seed_probe_encounter_projections", [&](const std::string& line) {
                bool ok_id = false, ok_run = false;
                const auto old_id = JsonExtractInt(analysis_db_, line, "$.encounter_projection_id", &ok_id);
                const auto old_run = JsonExtractInt(analysis_db_, line, "$.probe_run_id", &ok_run);
                if (!ok_id || !ok_run) return;
                const auto new_id = map_id("analysis_seed_probe_encounter_projection", old_id);
                const auto run = map_optional("analysis_seed_probe_run", true, old_run);
                if (new_id == 0 || !run) return;
                Statement st;
                if (!Prepare(analysis_db_, "INSERT INTO sp_encounter_projection(encounter_projection_id,probe_run_id,seed_value,option_ordinal,encounter_id,encounter_frame,stutter_step_at,movement_required,recorded_at_utc) VALUES(?1,?2,json_extract(?3,'$.seed_value'),json_extract(?3,'$.option_ordinal'),json_extract(?3,'$.encounter_id'),json_extract(?3,'$.encounter_frame'),json_extract(?3,'$.stutter_step_at'),json_extract(?3,'$.movement_required'),json_extract(?3,'$.recorded_at_utc'));", &st, &db_error)) return;
                sqlite3_bind_int64(st.st, 1, new_id);
                sqlite3_bind_int64(st.st, 2, *run);
                sqlite3_bind_text(st.st, 3, line.c_str(), -1, SQLITE_TRANSIENT);
                StepDone(analysis_db_, st.st, &db_error);
            });

            restore_stream("analysis_turn_waves", [&](const std::string& line) {
                bool ok_id = false, ok_context = false, ok_parent_turn = false;
                const auto old_id = JsonExtractInt(analysis_db_, line, "$.wave_id", &ok_id);
                const auto old_context = JsonExtractInt(analysis_db_, line, "$.context_probe_id", &ok_context);
                const auto old_parent_turn = JsonExtractInt(analysis_db_, line, "$.parent_turn_job_id", &ok_parent_turn);
                const auto wave = map_optional("analysis_turn_wave", ok_id, old_id);
                const auto context = map_optional("analysis_battle_context_probe", ok_context, old_context);
                const auto parent_turn = map_optional("analysis_battle_turn_job", ok_parent_turn, old_parent_turn);
                if (!wave || (!context && !parent_turn)) return;
                Statement st;
                if (!Prepare(analysis_db_, "UPDATE ab_turn_wave SET context_probe_id=?1,parent_turn_job_id=?2 WHERE wave_id=?3;", &st, &db_error)) return;
                bind_optional_int64(st.st, 1, context);
                bind_optional_int64(st.st, 2, parent_turn);
                sqlite3_bind_int64(st.st, 3, *wave);
                StepDone(analysis_db_, st.st, &db_error);
            });

            auto map_domain_ref = [&](std::string_view ref_kind, std::int64_t old_id) -> std::optional<std::int64_t> {
                if (ref_kind == "tmv_validation_request") {
                    return lookup_map("analysis_tas_movie_validation_request", old_id);
                }
                if (ref_kind == "tmv_checkpoint_sterilization_request") {
                    return lookup_map(
                        "analysis_tas_movie_checkpoint_sterilization_request",
                        old_id);
                }
                if (ref_kind == "analysis.tas_movie_validation_attempt_id"
                    || ref_kind == "tmv_validation_attempt") {
                    return lookup_map("analysis_tas_movie_validation_attempt", old_id);
                }
                if (ref_kind == "state.tas_movie_tree_id"
                    || ref_kind == "state_tas_movie_tree") {
                    return lookup_map("state_tas_movie_tree", old_id);
                }
                if (ref_kind == "state_artifact"
                    || ref_kind == "state_artifact.dtm_artifact_id") {
                    return lookup_map("state_artifact", old_id);
                }
                if (ref_kind == "sp_probe_run") return lookup_map("analysis_seed_probe_run", old_id);
                if (ref_kind == "analysisseedprobe.confirmed_result") {
                    return lookup_map("analysis_seed_probe_result", old_id);
                }
                if (ref_kind == "analysis_battle.battle_completion_id"
                    || ref_kind == "analysis_battle.battle_completion"
                    || ref_kind == "analysisbattle.battle_completion"
                    || ref_kind == "ab_battle_completion") {
                    return lookup_map("analysis_battle_completion", old_id);
                }
                if (ref_kind == "analysis_battle.battle_results_id"
                    || ref_kind == "analysis_battle.battle_results"
                    || ref_kind == "analysisbattle.battle_results"
                    || ref_kind == "ab_battle_results") {
                    return lookup_map("analysis_battle_results", old_id);
                }
                return std::nullopt;
            };

            restore_stream("job_sets", [&](const std::string& line) {
                bool ok_set = false, ok_ref = false, ok_kind = false;
                const auto old_set = JsonExtractInt(execution_db_, line, "$.job_set_id", &ok_set);
                const auto old_ref = JsonExtractInt(execution_db_, line, "$.domain_ref_id", &ok_ref);
                const auto ref_kind = JsonExtractText(execution_db_, line, "$.domain_ref_kind", &ok_kind);
                if (!ok_set || !ok_ref) return;
                const auto job_set = map_optional("job_set", true, old_set);
                const auto mapped = map_domain_ref(ref_kind, old_ref);
                if (!job_set || !mapped) return;
                Statement st;
                if (!Prepare(execution_db_, "UPDATE exec_job_set SET domain_ref_id=?1 WHERE job_set_id=?2;", &st, &db_error)) return;
                sqlite3_bind_int64(st.st, 1, *mapped);
                sqlite3_bind_int64(st.st, 2, *job_set);
                StepDone(execution_db_, st.st, &db_error);
            });
            restore_stream("jobs", [&](const std::string& line) {
                bool ok_job = false, ok_ref = false, ok_kind = false;
                const auto old_job = JsonExtractInt(execution_db_, line, "$.job_id", &ok_job);
                const auto old_ref = JsonExtractInt(execution_db_, line, "$.program_ref_id", &ok_ref);
                const auto ref_kind = JsonExtractText(execution_db_, line, "$.program_ref_kind", &ok_kind);
                if (!ok_job || !ok_ref) return;
                const auto job = map_optional("job", true, old_job);
                const auto mapped = map_domain_ref(ref_kind, old_ref);
                if (!job) return;
                if (!mapped) {
                    if (ref_kind == "sp_probe_run") {
                        db_error =
                            "failed mapping SeedProbe run during job "
                            "rehydrate";
                    }
                    return;
                }

                if (ref_kind == "sp_probe_run") {
                    bool ok_input_ini = false;
                    const auto old_input_ini = JsonExtractText(
                        execution_db_,
                        line,
                        "$.input_ini",
                        &ok_input_ini);
                    if (!ok_input_ini) {
                        db_error =
                            "archived SeedProbe job is missing its request";
                        return;
                    }
                    std::string decode_error;
                    auto seed_probe_spec =
                        execution::programdb::seedprobe::
                            DecodeSeedProbeJobSpec(
                                old_input_ini,
                                &decode_error);
                    if (!seed_probe_spec.has_value()) {
                        db_error =
                            "archived SeedProbe request is invalid: "
                            + decode_error;
                        return;
                    }
                    const auto mapped_frame = lookup_map(
                        "analysis_seed_probe_input_frame",
                        seed_probe_spec->input_frame_id);
                    if (!mapped_frame.has_value()) {
                        db_error =
                            "failed mapping SeedProbe request input frame "
                            "during rehydrate";
                        return;
                    }
                    seed_probe_spec->input_frame_id = *mapped_frame;
                    if (seed_probe_spec
                            ->confirmation_of_probe_result_id
                            .has_value()) {
                        const auto mapped_result = lookup_map(
                            "analysis_seed_probe_result",
                            *seed_probe_spec
                                 ->confirmation_of_probe_result_id);
                        if (!mapped_result.has_value()) {
                            db_error =
                                "failed mapping SeedProbe confirmation "
                                "result during rehydrate";
                            return;
                        }
                        seed_probe_spec
                            ->confirmation_of_probe_result_id =
                            *mapped_result;
                    }
                    const auto input_ini =
                        execution::programdb::seedprobe::
                            EncodeSeedProbeJobSpec(*seed_probe_spec);
                    const auto cancellation_group =
                        execution::programdb::seedprobe::
                            SeedProbeCancellationGroupKey(
                                *mapped,
                                *seed_probe_spec);

                    Statement st;
                    if (!Prepare(
                            execution_db_,
                            "UPDATE exec_job "
                            "SET program_ref_id=?1,input_ini=?2,"
                            "cancellation_group_key=?3 WHERE job_id=?4;",
                            &st,
                            &db_error)) {
                        return;
                    }
                    sqlite3_bind_int64(st.st, 1, *mapped);
                    sqlite3_bind_text(
                        st.st,
                        2,
                        input_ini.c_str(),
                        -1,
                        SQLITE_TRANSIENT);
                    if (cancellation_group.has_value()) {
                        sqlite3_bind_text(
                            st.st,
                            3,
                            cancellation_group->c_str(),
                            -1,
                            SQLITE_TRANSIENT);
                    } else {
                        sqlite3_bind_null(st.st, 3);
                    }
                    sqlite3_bind_int64(st.st, 4, *job);
                    StepDone(execution_db_, st.st, &db_error);
                    return;
                }

                Statement st;
                if (!Prepare(execution_db_, "UPDATE exec_job SET program_ref_id=?1 WHERE job_id=?2;", &st, &db_error)) return;
                sqlite3_bind_int64(st.st, 1, *mapped);
                sqlite3_bind_int64(st.st, 2, *job);
                StepDone(execution_db_, st.st, &db_error);
            });

            restore_stream("workflow_steps", [&](const std::string& line) {
                bool ok_step = false, ok_input = false, ok_output = false, ok_input_kind = false, ok_output_kind = false;
                const auto old_step = JsonExtractInt(execution_db_, line, "$.workflow_step_id", &ok_step);
                const auto old_input = JsonExtractInt(execution_db_, line, "$.input_ref_id", &ok_input);
                const auto old_output = JsonExtractInt(execution_db_, line, "$.output_ref_id", &ok_output);
                const auto input_kind = JsonExtractText(execution_db_, line, "$.input_ref_kind", &ok_input_kind);
                const auto output_kind = JsonExtractText(execution_db_, line, "$.output_ref_kind", &ok_output_kind);
                const auto step = map_optional("workflow_step", ok_step, old_step);
                if (!step) return;
                if (ok_input) {
                    const auto mapped = map_domain_ref(input_kind, old_input);
                    if (mapped) {
                        Statement st;
                        if (!Prepare(execution_db_, "UPDATE exec_workflow_step SET input_ref_id=?1 WHERE workflow_step_id=?2;", &st, &db_error)) return;
                        sqlite3_bind_int64(st.st, 1, *mapped);
                        sqlite3_bind_int64(st.st, 2, *step);
                        StepDone(execution_db_, st.st, &db_error);
                    }
                }
                if (!db_error.empty()) return;
                if (ok_output) {
                    const auto mapped = map_domain_ref(output_kind, old_output);
                    if (mapped) {
                        Statement st;
                        if (!Prepare(execution_db_, "UPDATE exec_workflow_step SET output_ref_id=?1 WHERE workflow_step_id=?2;", &st, &db_error)) return;
                        sqlite3_bind_int64(st.st, 1, *mapped);
                        sqlite3_bind_int64(st.st, 2, *step);
                        StepDone(execution_db_, st.st, &db_error);
                    }
                }
            });
            restore_stream("workflow_step_outputs", [&](const std::string& line) {
                bool ok_id = false, ok_ref = false, ok_kind = false;
                const auto old_id = JsonExtractInt(execution_db_, line, "$.workflow_step_output_id", &ok_id);
                const auto old_ref = JsonExtractInt(execution_db_, line, "$.ref_id", &ok_ref);
                const auto ref_kind = JsonExtractText(execution_db_, line, "$.ref_kind", &ok_kind);
                if (!ok_id || !ok_ref) return;
                const auto output = map_optional("workflow_step_output", true, old_id);
                const auto mapped = map_domain_ref(ref_kind, old_ref);
                if (!output || !mapped) return;
                Statement st;
                if (!Prepare(execution_db_, "UPDATE exec_workflow_step_output SET ref_id=?1 WHERE workflow_step_output_id=?2;", &st, &db_error)) return;
                sqlite3_bind_int64(st.st, 1, *mapped);
                sqlite3_bind_int64(st.st, 2, *output);
                StepDone(execution_db_, st.st, &db_error);
            });
            restore_stream("workflow_instance_input_bindings", [&](const std::string& line) {
                bool ok_id = false, ok_ref = false, ok_kind = false;
                const auto old_id = JsonExtractInt(execution_db_, line, "$.workflow_instance_input_binding_id", &ok_id);
                const auto old_ref = JsonExtractInt(execution_db_, line, "$.ref_id", &ok_ref);
                const auto ref_kind = JsonExtractText(execution_db_, line, "$.ref_kind", &ok_kind);
                if (!ok_id || !ok_ref) return;
                const auto binding = map_optional("workflow_instance_input_binding", true, old_id);
                const auto mapped = map_domain_ref(ref_kind, old_ref);
                if (!binding || !mapped) return;
                Statement st;
                if (!Prepare(execution_db_, "UPDATE exec_workflow_instance_input_binding SET ref_id=?1 WHERE workflow_instance_input_binding_id=?2;", &st, &db_error)) return;
                sqlite3_bind_int64(st.st, 1, *mapped);
                sqlite3_bind_int64(st.st, 2, *binding);
                StepDone(execution_db_, st.st, &db_error);
            });

            if (db_error.empty() && state_db_ != nullptr) {
                const auto remap_state_contexts =
                    [&](std::string_view table,
                        std::string_view id_column,
                        const std::vector<std::int64_t>& inserted_ids) {
                    for (const auto new_state_id : inserted_ids) {
                        Statement read;
                        const auto select_sql = "SELECT source_context_kind,source_context_id FROM "
                            + std::string(table) + " WHERE " + std::string(id_column) + "=?1;";
                        if (!Prepare(state_db_, select_sql.c_str(), &read, &db_error)) return;
                        sqlite3_bind_int64(read.st, 1, new_state_id);
                        if (sqlite3_step(read.st) != SQLITE_ROW) continue;
                        const auto* context_text = sqlite3_column_text(read.st, 0);
                        const auto context_kind = context_text == nullptr
                            ? std::string{}
                            : std::string(reinterpret_cast<const char*>(context_text));
                        const auto old_context_id = sqlite3_column_int64(read.st, 1);
                        std::optional<std::int64_t> mapped;
                        if (context_kind == "tmv_validation_request") {
                            mapped = lookup_map("analysis_tas_movie_validation_request", old_context_id);
                        } else if (context_kind == "tmv_validation_attempt") {
                            mapped = lookup_map("analysis_tas_movie_validation_attempt", old_context_id);
                        }
                        if (!mapped) continue;
                        Statement update;
                        const auto update_sql = "UPDATE " + std::string(table)
                            + " SET source_context_id=?1 WHERE " + std::string(id_column) + "=?2;";
                        if (!Prepare(state_db_, update_sql.c_str(), &update, &db_error)) return;
                        sqlite3_bind_int64(update.st, 1, *mapped);
                        sqlite3_bind_int64(update.st, 2, new_state_id);
                        if (!StepDone(state_db_, update.st, &db_error)) return;
                    }
                };
                remap_state_contexts(
                    "state_tas_movie_root",
                    "tas_movie_root_id",
                    inserted_tas_movie_root_ids);
                remap_state_contexts(
                    "state_tas_movie_trees",
                    "tas_movie_tree_id",
                    inserted_tas_movie_tree_ids);
            }

            if (!db_error.empty()) {
                result.error = StructuredError{ "ANALYSIS_REHYDRATE_ERROR", "failed restoring analysis rows", db_error }.ToJson();
            }
        }

        if (!result.error.has_value() && state_db_ != nullptr && !pending_state_derivation_lines.empty()) {
            std::string state_error;
            const bool state_reuses_outer_transaction = state_db_ == execution_db_
                || (analysis_transaction_started && state_db_ == analysis_db_);
            bool state_transaction_started = false;
            if (!state_reuses_outer_transaction
                && sqlite3_exec(state_db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
                state_error = sqlite3_errmsg(state_db_);
            } else if (!state_reuses_outer_transaction) {
                state_transaction_started = true;
            }
            for (const auto& line : pending_state_derivation_lines) {
                if (!state_error.empty()) break;
                bool ok_from = false, ok_to = false, ok_context_kind = false, ok_context_id = false;
                const auto old_from = JsonExtractInt(state_db_, line, "$.from_savestate_id", &ok_from);
                const auto old_to = JsonExtractInt(state_db_, line, "$.to_savestate_id", &ok_to);
                const auto context_kind = JsonExtractText(state_db_, line, "$.source_context_kind", &ok_context_kind);
                const auto old_context_id = JsonExtractInt(state_db_, line, "$.source_context_id", &ok_context_id);
                const auto from_it = id_map["state_savestate"].find(old_from);
                const auto to_it = id_map["state_savestate"].find(old_to);
                if (!ok_from || !ok_to || !ok_context_kind || !ok_context_id
                    || from_it == id_map["state_savestate"].end()
                    || to_it == id_map["state_savestate"].end()) {
                    state_error = "savestate derivation mapping is incomplete";
                    break;
                }

                std::string map_kind;
                if (context_kind == "analysisseedprobe.confirmed_result") map_kind = "analysis_seed_probe_result";
                else if (context_kind == "analysis_battle.battle_completion_id"
                    || context_kind == "analysis_battle.battle_completion"
                    || context_kind == "analysisbattle.battle_completion"
                    || context_kind == "ab_battle_completion") map_kind = "analysis_battle_completion";
                else if (context_kind == "analysis_battle.battle_results_id"
                    || context_kind == "analysis_battle.battle_results"
                    || context_kind == "analysisbattle.battle_results"
                    || context_kind == "ab_battle_results") map_kind = "analysis_battle_results";
                else if (context_kind == "tmv_checkpoint_sterilization_request")
                    map_kind = "analysis_tas_movie_checkpoint_sterilization_request";

                auto new_context_id = old_context_id;
                if (!map_kind.empty()) {
                    const auto per_kind = id_map.find(map_kind);
                    if (per_kind == id_map.end()) {
                        state_error = "direct savestate derivation context mapping is missing for " + context_kind;
                        break;
                    }
                    const auto mapped = per_kind->second.find(old_context_id);
                    if (mapped == per_kind->second.end()) {
                        state_error = "direct savestate derivation context id mapping is missing for " + context_kind;
                        break;
                    }
                    new_context_id = mapped->second;
                }

                Statement insert_derivation;
                if (!Prepare(
                        state_db_,
                        "INSERT INTO state_savestate_derivation(from_savestate_id,to_savestate_id,method_kind,source_context_kind,source_context_id,created_at_utc) "
                        "VALUES(?1,?2,json_extract(?3,'$.method_kind'),?4,?5,json_extract(?3,'$.created_at_utc'));",
                        &insert_derivation,
                        &state_error)) break;
                sqlite3_bind_int64(insert_derivation.st, 1, from_it->second);
                sqlite3_bind_int64(insert_derivation.st, 2, to_it->second);
                sqlite3_bind_text(insert_derivation.st, 3, line.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_derivation.st, 4, context_kind.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(insert_derivation.st, 5, new_context_id);
                if (!StepDone(state_db_, insert_derivation.st, &state_error)) break;
            }
            if (state_error.empty() && state_transaction_started) {
                if (sqlite3_exec(state_db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
                    state_error = sqlite3_errmsg(state_db_);
                } else {
                    state_transaction_started = false;
                }
            }
            if (!state_error.empty()) {
                if (state_transaction_started) {
                    sqlite3_exec(state_db_, "ROLLBACK;", nullptr, nullptr, nullptr);
                }
                result.error = StructuredError{
                    "STATE_DERIVATION_REHYDRATE_ERROR",
                    "failed restoring savestate derivation lineage",
                    state_error
                }.ToJson();
            }
        }

        if (!result.error.has_value()) {
            std::string db_error2;
            const auto now_epoch = request.now_utc.time_since_epoch().count();
            std::string correlation = request.correlation_id.empty()
                ? ("rehydrate-request-" + std::to_string(request.rehydrate_request_id))
                : request.correlation_id;
            std::string causation = request.causation_id.empty()
                ? correlation
                : request.causation_id;

            for (const auto job_id : restored_jobs) {
                if (!InsertExecutionOutbox(
                        execution_db_,
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
                execution_transaction_started = false;
                if (analysis_transaction_started) {
                    sqlite3_exec(analysis_db_, "COMMIT;", nullptr, nullptr, nullptr);
                    analysis_transaction_started = false;
                }

                CompleteRehydrateCommand done{};
                done.rehydrate_request_id = request.rehydrate_request_id;
                done.status = "COMPLETED";
                done.completed_at_utc = request.now_utc;
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
        fail.correlation_id = request.correlation_id.empty()
            ? ("rehydrate-request-" + std::to_string(request.rehydrate_request_id))
            : request.correlation_id;
        fail.causation_id = request.causation_id.empty() ? fail.correlation_id : request.causation_id;
        std::string archive_error;
        archive_service_->FailRehydrate(fail, &archive_error);
        EmitArchiveProgress(
            request.progress_sink,
            ArchiveOperationPhase::Failed,
            "Rehydrate failed",
            0,
            0,
            false);
    } else {
        EmitArchiveProgress(
            request.progress_sink,
            ArchiveOperationPhase::Complete,
            "Rehydrate complete",
            result.restored_job_count,
            result.restored_job_count,
            false);
    }

    return result;
}

} // namespace savor::db::archive
