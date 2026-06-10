#include "MigrationRunner.h"

#if defined(__INTELLISENSE__) || !__has_include("GeneratedMigrations.h")
#include <array>
namespace savor::db::migrations {
struct EmbeddedMigrationEntry {
    MigrationContext context;
    const char* filename;
    const char* sql;
};
inline constexpr std::array<EmbeddedMigrationEntry, 0> kEmbeddedMigrations{};
} // namespace savor::db::migrations
#else
#include "GeneratedMigrations.h"
#endif

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace savor::db::migrations {

namespace {

bool Exec(sqlite3* db, const char* sql, std::string* error_out) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (error_out) {
            *error_out = err ? err : "sqlite3_exec failed";
        }
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool ScriptHasTransaction(const std::string& sql) {
    auto has_token = [&](const char* token) {
        std::string u(sql.size(), '\0');
        std::transform(sql.begin(), sql.end(), u.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return u.find(token) != std::string::npos;
    };

    return has_token("BEGIN") || has_token("COMMIT");
}

int ParseVersionPrefix(const std::string& migration_name) {
    int version = 0;
    for (char c : migration_name) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            break;
        }
        version = (version * 10) + (c - '0');
    }
    return version;
}

std::vector<MigrationEntry> LoadContextMigrationsFromFilesystem(
    const std::filesystem::path& migration_root,
    MigrationContext context) {
    std::vector<MigrationEntry> entries;
    const auto dir = migration_root / FolderName(context);
    if (!std::filesystem::exists(dir)) {
        return entries;
    }

    for (const auto& item : std::filesystem::directory_iterator(dir)) {
        if (!item.is_regular_file()) {
            continue;
        }
        if (item.path().extension() != ".sql") {
            continue;
        }

        std::ifstream ifs(item.path(), std::ios::binary);
        std::ostringstream os;
        os << ifs.rdbuf();

        entries.push_back(MigrationEntry{
            .context = context,
            .name = item.path().filename().string(),
            .path = item.path(),
            .sql = os.str(),
        });
    }

    std::sort(entries.begin(), entries.end(), [](const MigrationEntry& a, const MigrationEntry& b) {
        return a.name < b.name;
    });

    return entries;
}

std::vector<MigrationEntry> LoadContextMigrationsFromEmbedded(MigrationContext context) {
    std::vector<MigrationEntry> entries;
    entries.reserve(kEmbeddedMigrations.size());

    for (const auto& embedded_entry : kEmbeddedMigrations) {
        if (embedded_entry.context != context) {
            continue;
        }

        entries.push_back(MigrationEntry{
            .context = embedded_entry.context,
            .name = embedded_entry.filename,
            .path = std::filesystem::path(FolderName(embedded_entry.context)) / embedded_entry.filename,
            .sql = embedded_entry.sql,
        });
    }

    return entries;
}

} // namespace

std::vector<MigrationContext> ListAllMigrationContexts() {
    return {
        MigrationContext::Execution,
        MigrationContext::State,
        MigrationContext::AnalysisSpine,
        MigrationContext::AnalysisSeedProbe,
        MigrationContext::AnalysisBattle,
        MigrationContext::Authoring,
        MigrationContext::UIRead,
        MigrationContext::Archive,
    };
}

std::vector<MigrationEntry> LoadContextMigrations(MigrationContext context, const MigrationSourceOptions& options) {
    if (options.source_kind == MigrationSourceKind::Filesystem) {
        return LoadContextMigrationsFromFilesystem(options.filesystem_root, context);
    }

    return LoadContextMigrationsFromEmbedded(context);
}

bool EnsureMigrationTrackingTables(sqlite3* db, std::string* error_out) {
    static constexpr const char* kCreateHistory = R"SQL(
CREATE TABLE IF NOT EXISTS migration_history (
    context TEXT NOT NULL,
    migration_name TEXT NOT NULL,
    applied_at_utc INTEGER NOT NULL DEFAULT (unixepoch()),
    PRIMARY KEY (context, migration_name)
);
)SQL";

    static constexpr const char* kCreateVersion = R"SQL(
CREATE TABLE IF NOT EXISTS migration_schema_version (
    context TEXT PRIMARY KEY,
    version INTEGER NOT NULL
);
)SQL";

    return Exec(db, kCreateHistory, error_out) && Exec(db, kCreateVersion, error_out);
}

bool HasMigrationBeenApplied(
    sqlite3* db,
    MigrationContext context,
    const std::string& migration_name,
    bool* applied_out,
    std::string* error_out) {
    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql = "SELECT 1 FROM migration_history WHERE context=?1 AND migration_name=?2 LIMIT 1;";

    if (sqlite3_prepare_v2(db, kSql, -1, &st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }

    sqlite3_bind_text(st, 1, ToString(context), -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, migration_name.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }

    *applied_out = (rc == SQLITE_ROW);
    return true;
}

std::optional<int> GetCurrentContextSchemaVersion(sqlite3* db, MigrationContext context, std::string* error_out) {
    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql = "SELECT version FROM migration_schema_version WHERE context=?1 LIMIT 1;";

    if (sqlite3_prepare_v2(db, kSql, -1, &st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return std::nullopt;
    }

    sqlite3_bind_text(st, 1, ToString(context), -1, SQLITE_STATIC);
    const int rc = sqlite3_step(st);

    if (rc == SQLITE_DONE) {
        sqlite3_finalize(st);
        return 0;
    }
    if (rc != SQLITE_ROW) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        sqlite3_finalize(st);
        return std::nullopt;
    }

    const int v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
}

bool ApplyContextMigrations(
    sqlite3* db,
    MigrationContext context,
    const MigrationSourceOptions& options,
    std::string* error_out) {
    if (!EnsureMigrationTrackingTables(db, error_out)) {
        return false;
    }

    const auto entries = LoadContextMigrations(context, options);
    for (const auto& entry : entries) {
        bool already_applied = false;
        if (!HasMigrationBeenApplied(db, context, entry.name, &already_applied, error_out)) {
            return false;
        }
        if (already_applied) {
            continue;
        }

        const bool has_tx = ScriptHasTransaction(entry.sql);
        if (!has_tx && !Exec(db, "BEGIN IMMEDIATE;", error_out)) {
            return false;
        }

        if (!Exec(db, entry.sql.c_str(), error_out)) {
            Exec(db, "ROLLBACK;", nullptr);
            if (error_out) {
                *error_out = "Migration failed (" + entry.name + "): " + *error_out;
            }
            return false;
        }

        sqlite3_stmt* insert_st = nullptr;
        constexpr const char* kInsertHistory =
            "INSERT INTO migration_history(context, migration_name) VALUES(?1, ?2);";
        if (sqlite3_prepare_v2(db, kInsertHistory, -1, &insert_st, nullptr) != SQLITE_OK) {
            Exec(db, "ROLLBACK;", nullptr);
            if (error_out) *error_out = sqlite3_errmsg(db);
            return false;
        }
        sqlite3_bind_text(insert_st, 1, ToString(context), -1, SQLITE_STATIC);
        sqlite3_bind_text(insert_st, 2, entry.name.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(insert_st) != SQLITE_DONE) {
            sqlite3_finalize(insert_st);
            Exec(db, "ROLLBACK;", nullptr);
            if (error_out) *error_out = sqlite3_errmsg(db);
            return false;
        }
        sqlite3_finalize(insert_st);

        sqlite3_stmt* upsert_st = nullptr;
        constexpr const char* kUpsertVersion =
            "INSERT INTO migration_schema_version(context, version) VALUES(?1, ?2) "
            "ON CONFLICT(context) DO UPDATE SET version=MAX(version, excluded.version);";
        if (sqlite3_prepare_v2(db, kUpsertVersion, -1, &upsert_st, nullptr) != SQLITE_OK) {
            Exec(db, "ROLLBACK;", nullptr);
            if (error_out) *error_out = sqlite3_errmsg(db);
            return false;
        }
        sqlite3_bind_text(upsert_st, 1, ToString(context), -1, SQLITE_STATIC);
        sqlite3_bind_int(upsert_st, 2, ParseVersionPrefix(entry.name));
        if (sqlite3_step(upsert_st) != SQLITE_DONE) {
            sqlite3_finalize(upsert_st);
            Exec(db, "ROLLBACK;", nullptr);
            if (error_out) *error_out = sqlite3_errmsg(db);
            return false;
        }
        sqlite3_finalize(upsert_st);

        if (!has_tx && !Exec(db, "COMMIT;", error_out)) {
            Exec(db, "ROLLBACK;", nullptr);
            return false;
        }
    }

    return true;
}

} // namespace savor::db::migrations
