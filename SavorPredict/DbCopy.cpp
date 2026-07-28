#include "DbCopy.h"

#include "DbRootCopy.h"

#include <mbedtls/sha256.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace savor::predict {
namespace {

struct MigrationContextInfo {
    std::string context;
    std::int64_t version = 0;
    std::int64_t migration_count = 0;
    std::string latest_migration;
};

struct PreparedDatabaseInfo {
    std::string name;
    std::uintmax_t size_bytes = 0;
    int schema_version = 0;
    int user_version = 0;
    std::string journal_mode;
    std::string quick_check;
    std::string sha256;
    std::vector<MigrationContextInfo> migration_contexts;
};

class SqliteHandle {
public:
    ~SqliteHandle() {
        if (db_ != nullptr) {
            sqlite3_close(db_);
        }
    }

    sqlite3** out() { return &db_; }
    sqlite3* get() const { return db_; }
    int close() {
        if (db_ == nullptr) {
            return SQLITE_OK;
        }
        const int rc = sqlite3_close(db_);
        if (rc == SQLITE_OK) {
            db_ = nullptr;
        }
        return rc;
    }

private:
    sqlite3* db_ = nullptr;
};

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default: out << ch; break;
        }
    }
    return out.str();
}

std::string normalized_path_string(const std::filesystem::path& path) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec);
    return (ec ? path : absolute).lexically_normal().generic_string();
}

std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

bool hash_file_sha256_impl(
    const std::filesystem::path& path,
    std::string* sha256,
    std::ostream& err) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        err << "Failed opening prepared database for hashing "
            << path.string() << "\n";
        return false;
    }

    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    if (mbedtls_sha256_starts_ret(&context, 0) != 0) {
        mbedtls_sha256_free(&context);
        err << "Failed initializing SHA-256 for " << path.string() << "\n";
        return false;
    }

    std::vector<unsigned char> buffer(1024 * 1024);
    while (file) {
        file.read(
            reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
        const auto count = file.gcount();
        if (count > 0
            && mbedtls_sha256_update_ret(
                &context,
                buffer.data(),
                static_cast<std::size_t>(count))
                != 0) {
            mbedtls_sha256_free(&context);
            err << "Failed hashing prepared database "
                << path.string() << "\n";
            return false;
        }
    }
    if (!file.eof()) {
        mbedtls_sha256_free(&context);
        err << "Failed reading prepared database for hashing "
            << path.string() << "\n";
        return false;
    }

    std::array<unsigned char, 32> digest{};
    if (mbedtls_sha256_finish_ret(&context, digest.data()) != 0) {
        mbedtls_sha256_free(&context);
        err << "Failed finalizing SHA-256 for " << path.string() << "\n";
        return false;
    }
    mbedtls_sha256_free(&context);

    constexpr char kHex[] = "0123456789abcdef";
    sha256->clear();
    sha256->reserve(digest.size() * 2);
    for (const auto byte : digest) {
        sha256->push_back(kHex[byte >> 4]);
        sha256->push_back(kHex[byte & 0x0F]);
    }
    return true;
}

bool read_pragma_int(
    sqlite3* db,
    const char* sql,
    int* value,
    std::ostream& err,
    const std::filesystem::path& path) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK) {
        err << "Failed preparing " << sql << " for " << path.string()
            << ": " << sqlite3_errmsg(db) << "\n";
        return false;
    }
    const int step_rc = sqlite3_step(statement);
    if (step_rc != SQLITE_ROW) {
        err << "Failed reading " << sql << " for " << path.string()
            << ": " << sqlite3_errmsg(db) << "\n";
        sqlite3_finalize(statement);
        return false;
    }
    *value = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    return true;
}

bool read_migration_contexts(
    sqlite3* db,
    std::vector<MigrationContextInfo>* contexts,
    std::ostream& err,
    const std::filesystem::path& path) {
    constexpr const char* kSql =
        "SELECT v.context,v.version,COUNT(h.migration_name),"
        "COALESCE(MAX(h.migration_name),'') "
        "FROM migration_schema_version v "
        "LEFT JOIN migration_history h ON h.context=v.context "
        "GROUP BY v.context,v.version "
        "ORDER BY v.context;";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, kSql, -1, &statement, nullptr) != SQLITE_OK) {
        err << "Failed reading migration provenance for " << path.string()
            << ": " << sqlite3_errmsg(db) << "\n";
        return false;
    }
    int step_rc = SQLITE_ROW;
    while ((step_rc = sqlite3_step(statement)) == SQLITE_ROW) {
        const auto* context_text = sqlite3_column_text(statement, 0);
        const auto* latest_text = sqlite3_column_text(statement, 3);
        contexts->push_back({
            .context = context_text == nullptr
                ? std::string{}
                : reinterpret_cast<const char*>(context_text),
            .version = sqlite3_column_int64(statement, 1),
            .migration_count = sqlite3_column_int64(statement, 2),
            .latest_migration = latest_text == nullptr
                ? std::string{}
                : reinterpret_cast<const char*>(latest_text),
        });
    }
    if (step_rc != SQLITE_DONE) {
        err << "Failed stepping migration provenance for " << path.string()
            << ": " << sqlite3_errmsg(db) << "\n";
        sqlite3_finalize(statement);
        return false;
    }
    sqlite3_finalize(statement);
    if (contexts->empty()) {
        err << "Prepared database has no migration provenance: "
            << path.string() << "\n";
        return false;
    }
    return true;
}

bool seal_database(
    const std::filesystem::path& path,
    PreparedDatabaseInfo* info,
    std::ostream& err) {
    SqliteHandle handle;
    const int open_rc = sqlite3_open_v2(
        path.string().c_str(),
        handle.out(),
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (open_rc != SQLITE_OK) {
        err << "Failed opening prepared database " << path.string() << ": "
            << (handle.get() == nullptr
                    ? "sqlite error"
                    : sqlite3_errmsg(handle.get()))
            << "\n";
        return false;
    }
    sqlite3_busy_timeout(handle.get(), 5000);

    char* sqlite_error = nullptr;
    const int checkpoint_rc = sqlite3_exec(
        handle.get(),
        "PRAGMA wal_checkpoint(TRUNCATE);",
        nullptr,
        nullptr,
        &sqlite_error);
    if (checkpoint_rc != SQLITE_OK) {
        err << "Failed checkpointing prepared database " << path.string()
            << ": "
            << (sqlite_error == nullptr
                    ? sqlite3_errmsg(handle.get())
                    : sqlite_error)
            << "\n";
        sqlite3_free(sqlite_error);
        return false;
    }

    sqlite3_stmt* journal_statement = nullptr;
    if (sqlite3_prepare_v2(
            handle.get(),
            "PRAGMA journal_mode=DELETE;",
            -1,
            &journal_statement,
            nullptr)
        != SQLITE_OK) {
        err << "Failed preparing journal-mode seal for " << path.string()
            << ": " << sqlite3_errmsg(handle.get()) << "\n";
        return false;
    }
    const int journal_step_rc = sqlite3_step(journal_statement);
    if (journal_step_rc != SQLITE_ROW) {
        err << "Failed sealing journal mode for " << path.string() << ": "
            << sqlite3_errmsg(handle.get()) << "\n";
        sqlite3_finalize(journal_statement);
        return false;
    }
    const auto* journal_text = sqlite3_column_text(journal_statement, 0);
    info->journal_mode = journal_text == nullptr
        ? std::string{}
        : reinterpret_cast<const char*>(journal_text);
    sqlite3_finalize(journal_statement);
    std::transform(
        info->journal_mode.begin(),
        info->journal_mode.end(),
        info->journal_mode.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    if (info->journal_mode != "delete") {
        err << "Prepared database " << path.string()
            << " did not enter DELETE journal mode; observed "
            << info->journal_mode << "\n";
        return false;
    }

    sqlite3_stmt* check_statement = nullptr;
    if (sqlite3_prepare_v2(
            handle.get(),
            "PRAGMA quick_check;",
            -1,
            &check_statement,
            nullptr)
        != SQLITE_OK) {
        err << "Failed preparing quick_check for " << path.string() << ": "
            << sqlite3_errmsg(handle.get()) << "\n";
        return false;
    }
    const int check_step_rc = sqlite3_step(check_statement);
    if (check_step_rc != SQLITE_ROW) {
        err << "Failed running quick_check for " << path.string() << ": "
            << sqlite3_errmsg(handle.get()) << "\n";
        sqlite3_finalize(check_statement);
        return false;
    }
    const auto* check_text = sqlite3_column_text(check_statement, 0);
    info->quick_check = check_text == nullptr
        ? std::string{}
        : reinterpret_cast<const char*>(check_text);
    const int check_done_rc = sqlite3_step(check_statement);
    sqlite3_finalize(check_statement);
    if (info->quick_check != "ok" || check_done_rc != SQLITE_DONE) {
        err << "Prepared database " << path.string()
            << " failed quick_check: " << info->quick_check;
        if (check_done_rc == SQLITE_ROW) {
            err << " (additional result rows)";
        } else if (check_done_rc != SQLITE_DONE) {
            err << " (" << sqlite3_errmsg(handle.get()) << ")";
        }
        err << "\n";
        return false;
    }

    if (!read_pragma_int(
            handle.get(),
            "PRAGMA schema_version;",
            &info->schema_version,
            err,
            path)
        || !read_pragma_int(
            handle.get(),
            "PRAGMA user_version;",
            &info->user_version,
            err,
            path)) {
        return false;
    }
    if (!read_migration_contexts(
            handle.get(),
            &info->migration_contexts,
            err,
            path)) {
        return false;
    }
    if (handle.close() != SQLITE_OK) {
        err << "Failed closing prepared database before hashing "
            << path.string() << "\n";
        return false;
    }

    std::error_code size_error;
    info->size_bytes = std::filesystem::file_size(path, size_error);
    if (size_error) {
        err << "Failed reading prepared database size " << path.string()
            << ": " << size_error.message() << "\n";
        return false;
    }
    if (!hash_file_sha256_impl(path, &info->sha256, err)) {
        return false;
    }
    return true;
}

bool hash_database_fingerprint(
    const std::vector<PreparedDatabaseInfo>& databases,
    std::string* fingerprint,
    std::ostream& err) {
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    if (mbedtls_sha256_starts_ret(&context, 0) != 0) {
        mbedtls_sha256_free(&context);
        err << "Failed initializing prediction DB fingerprint.\n";
        return false;
    }
    for (const auto& database : databases) {
        const std::string canonical =
            database.name + "\n"
            + std::to_string(database.size_bytes) + "\n"
            + database.sha256 + "\n";
        if (mbedtls_sha256_update_ret(
                &context,
                reinterpret_cast<const unsigned char*>(canonical.data()),
                canonical.size())
            != 0) {
            mbedtls_sha256_free(&context);
            err << "Failed computing prediction DB fingerprint.\n";
            return false;
        }
    }
    std::array<unsigned char, 32> digest{};
    if (mbedtls_sha256_finish_ret(&context, digest.data()) != 0) {
        mbedtls_sha256_free(&context);
        err << "Failed finalizing prediction DB fingerprint.\n";
        return false;
    }
    mbedtls_sha256_free(&context);

    constexpr char kHex[] = "0123456789abcdef";
    fingerprint->clear();
    fingerprint->reserve(digest.size() * 2);
    for (const auto byte : digest) {
        fingerprint->push_back(kHex[byte >> 4]);
        fingerprint->push_back(kHex[byte & 0x0F]);
    }
    return true;
}

bool write_db_manifest(
    const PrepareDbOptions& options,
    const std::vector<PreparedDatabaseInfo>& databases,
    std::ostream& err) {
    const auto manifest_path = options.dest / "db_manifest.json";
    const auto temp_path = options.dest / "db_manifest.json.tmp";
    std::string database_fingerprint;
    if (!hash_database_fingerprint(
            databases,
            &database_fingerprint,
            err)) {
        return false;
    }
    std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
    if (!file) {
        err << "Failed creating prediction DB manifest "
            << temp_path.string() << "\n";
        return false;
    }
    file << "{\n";
    file << "  \"schema_version\": 1,\n";
    file << "  \"format\": \"savor-predict-db-manifest-v1\",\n";
    file << "  \"kind\": \"prediction_snapshot\",\n";
    file << "  \"created_at_utc\": \"" << utc_timestamp() << "\",\n";
    file << "  \"source_root\": \""
        << json_escape(normalized_path_string(options.source)) << "\",\n";
    file << "  \"destination_root\": \""
        << json_escape(normalized_path_string(options.dest)) << "\",\n";
    file << "  \"fingerprint_algorithm\": "
         << "\"sha256-of-canonical-file-digests-v1\",\n";
    file << "  \"database_fingerprint\": \""
         << database_fingerprint << "\",\n";
    file << "  \"databases\": [\n";
    for (std::size_t i = 0; i < databases.size(); ++i) {
        const auto& database = databases[i];
        file << "    {\"name\":\"" << json_escape(database.name)
            << "\",\"size_bytes\":" << database.size_bytes
            << ",\"schema_version\":" << database.schema_version
            << ",\"user_version\":" << database.user_version
            << ",\"sha256\":\"" << database.sha256 << "\""
            << ",\"journal_mode\":\""
            << json_escape(database.journal_mode)
            << "\",\"quick_check\":\""
            << json_escape(database.quick_check)
            << "\",\"migration_contexts\":[";
        for (std::size_t context_index = 0;
             context_index < database.migration_contexts.size();
             ++context_index) {
            const auto& context =
                database.migration_contexts[context_index];
            if (context_index != 0) {
                file << ",";
            }
            file << "{\"context\":\""
                << json_escape(context.context)
                << "\",\"version\":" << context.version
                << ",\"migration_count\":" << context.migration_count
                << ",\"latest_migration\":\""
                << json_escape(context.latest_migration) << "\"}";
        }
        file << "]}";
        file << (i + 1 == databases.size() ? "\n" : ",\n");
    }
    file << "  ]\n";
    file << "}\n";
    file.flush();
    if (!file) {
        err << "Failed writing prediction DB manifest "
            << temp_path.string() << "\n";
        return false;
    }
    file.close();

    std::error_code rename_error;
    std::filesystem::rename(temp_path, manifest_path, rename_error);
    if (rename_error) {
        err << "Failed finalizing prediction DB manifest "
            << manifest_path.string() << ": " << rename_error.message()
            << "\n";
        return false;
    }
    return true;
}

} // namespace

bool compute_file_sha256_streaming(
    const std::filesystem::path& path,
    std::string* sha256,
    std::ostream& err) {
    if (sha256 == nullptr) {
        err << "SHA-256 output pointer is null.\n";
        return false;
    }
    return hash_file_sha256_impl(path, sha256, err);
}

int run_prepare_db(const PrepareDbOptions& options, std::ostream& out, std::ostream& err) {
    const int copy_rc = savor::dbutils::CopyDbRootFull(
        {
            .source_root = options.source,
            .dest_root = options.dest,
            .overwrite = options.overwrite,
        },
        out,
        err);
    if (copy_rc != 0) {
        return copy_rc;
    }

    constexpr std::array<const char*, 6> kDatabaseNames{
        "analysis.db",
        "execution.db",
        "state.db",
        "ui_read.db",
        "authoring.db",
        "archive.db",
    };
    for (const auto* name : kDatabaseNames) {
        const auto path = options.dest / name;
        std::error_code type_error;
        const bool exists = std::filesystem::exists(path, type_error);
        if (type_error) {
            err << "Failed inspecting required prepared database "
                << path.string() << ": " << type_error.message() << "\n";
            return 1;
        }
        if (!exists) {
            err << "Prepared DB root is incomplete; required database is "
                "missing: " << path.string() << "\n";
            return 1;
        }
        const bool is_database =
            std::filesystem::is_regular_file(path, type_error);
        if (type_error) {
            err << "Failed inspecting required prepared database "
                << path.string() << ": " << type_error.message() << "\n";
            return 1;
        }
        if (!is_database) {
            err << "Prepared DB root is incomplete; required database is "
                "missing: " << path.string() << "\n";
            return 1;
        }
    }

    std::vector<PreparedDatabaseInfo> databases;
    for (const auto* name : kDatabaseNames) {
        const auto path = options.dest / name;
        PreparedDatabaseInfo info;
        info.name = name;
        out << "Sealing " << name
            << " for read-only prediction access\n";
        if (!seal_database(path, &info, err)) {
            return 1;
        }
        databases.push_back(std::move(info));
    }
    if (!write_db_manifest(options, databases, err)) {
        return 1;
    }
    out << "Wrote prediction DB manifest "
        << (options.dest / "db_manifest.json").string() << "\n";
    return 0;
}

} // namespace savor::predict
