#include "DB/DBCore/DbSnapshotService.h"

#include "SavorDbRuntime.h"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace savor::db {

DbSnapshotResult DbSnapshotResult::Ok() {
    DbSnapshotResult result{};
    result.ok = true;
    return result;
}

DbSnapshotResult DbSnapshotResult::Err(std::string message) {
    DbSnapshotResult result{};
    result.ok = false;
    result.error = std::move(message);
    return result;
}

namespace {

struct Statement {
    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }

    sqlite3_stmt* st = nullptr;
};

struct SnapshotFile {
    std::filesystem::path source_path;
    std::string relative_path;
};

std::string SqliteError(sqlite3* db, const char* fallback) {
    if (db != nullptr) {
        const char* message = sqlite3_errmsg(db);
        if (message != nullptr && *message != '\0') {
            return message;
        }
    }
    return fallback != nullptr ? fallback : "sqlite operation failed";
}

bool Exec(sqlite3* db, const char* sql, std::string* error_out) {
    char* sqlite_error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &sqlite_error);
    if (rc == SQLITE_OK) {
        return true;
    }

    if (error_out != nullptr) {
        *error_out = sqlite_error != nullptr ? sqlite_error : SqliteError(db, "sqlite3_exec failed");
    }
    sqlite3_free(sqlite_error);
    return false;
}

std::filesystem::path NormalPath(const std::filesystem::path& path) {
    std::error_code ec;
    auto absolute = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        absolute = std::filesystem::absolute(path, ec);
    }
    if (ec) {
        absolute = path;
    }
    return absolute.lexically_normal();
}

bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b) {
    return NormalPath(a) == NormalPath(b);
}

bool IsPathInside(const std::filesystem::path& path, const std::filesystem::path& root) {
    const auto normalized_path = NormalPath(path);
    const auto normalized_root = NormalPath(root);
    auto path_it = normalized_path.begin();
    auto root_it = normalized_root.begin();
    for (; root_it != normalized_root.end(); ++root_it, ++path_it) {
        if (path_it == normalized_path.end() || *path_it != *root_it) {
            return false;
        }
    }
    return true;
}

std::filesystem::path MakeTempSnapshotPath(const std::filesystem::path& snapshot_path) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path temp = snapshot_path;
    temp += "." + std::to_string(stamp) + ".tmp";
    return temp;
}

std::filesystem::path MakeRestoreCopyPath() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ("savorqt-restore-" + std::to_string(stamp) + ".soasnap");
}

bool ReadFile(const std::filesystem::path& path, std::vector<char>* bytes, std::string* error_out) {
    if (bytes == nullptr) {
        if (error_out != nullptr) {
            *error_out = "internal error: null file buffer";
        }
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        if (error_out != nullptr) {
            *error_out = "failed opening file for snapshot: " + path.string();
        }
        return false;
    }

    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) {
        if (error_out != nullptr) {
            *error_out = "failed reading file size for snapshot: " + path.string();
        }
        return false;
    }
    in.seekg(0, std::ios::beg);

    bytes->assign(static_cast<std::size_t>(size), '\0');
    if (size > 0) {
        in.read(bytes->data(), size);
    }
    if (!in.good() && !in.eof()) {
        if (error_out != nullptr) {
            *error_out = "failed reading file for snapshot: " + path.string();
        }
        return false;
    }
    return true;
}

bool WriteFile(const std::filesystem::path& path, const void* data, int size, std::string* error_out) {
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (error_out != nullptr) {
                *error_out = "failed creating snapshot restore directory: " + ec.message();
            }
            return false;
        }
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (error_out != nullptr) {
            *error_out = "failed opening restored file: " + path.string();
        }
        return false;
    }
    if (size > 0) {
        out.write(static_cast<const char*>(data), size);
    }
    if (!out.good()) {
        if (error_out != nullptr) {
            *error_out = "failed writing restored file: " + path.string();
        }
        return false;
    }
    return true;
}

bool CollectSnapshotFiles(
    const std::filesystem::path& root,
    const std::filesystem::path& snapshot_path,
    std::vector<SnapshotFile>* files,
    std::string* error_out) {
    if (files == nullptr) {
        if (error_out != nullptr) {
            *error_out = "internal error: null snapshot file list";
        }
        return false;
    }

    files->clear();
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        if (error_out != nullptr) {
            *error_out = "SavorDb root does not exist: " + root.string();
        }
        return false;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (ec) {
            if (error_out != nullptr) {
                *error_out = "failed scanning SavorDb root: " + ec.message();
            }
            return false;
        }
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        if (SamePath(entry.path(), snapshot_path)) {
            continue;
        }

        const auto relative = std::filesystem::relative(entry.path(), root, ec);
        if (ec) {
            if (error_out != nullptr) {
                *error_out = "failed computing snapshot relative path: " + ec.message();
            }
            return false;
        }

        files->push_back(SnapshotFile{
            .source_path = entry.path(),
            .relative_path = relative.generic_string(),
        });
    }
    return true;
}

bool OpenSnapshotForWrite(const std::filesystem::path& path, sqlite3** db, std::string* error_out) {
    if (db == nullptr) {
        if (error_out != nullptr) {
            *error_out = "internal error: null sqlite handle";
        }
        return false;
    }
    *db = nullptr;

    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (error_out != nullptr) {
                *error_out = "failed creating snapshot output directory: " + ec.message();
            }
            return false;
        }
    }

    const int rc = sqlite3_open_v2(
        path.string().c_str(),
        db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (rc != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = SqliteError(*db, "failed opening snapshot file");
        }
        if (*db != nullptr) {
            sqlite3_close(*db);
            *db = nullptr;
        }
        return false;
    }
    return true;
}

bool OpenSnapshotForRead(const std::filesystem::path& path, sqlite3** db, std::string* error_out) {
    if (db == nullptr) {
        if (error_out != nullptr) {
            *error_out = "internal error: null sqlite handle";
        }
        return false;
    }
    *db = nullptr;

    const int rc = sqlite3_open_v2(path.string().c_str(), db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = SqliteError(*db, "failed opening snapshot file");
        }
        if (*db != nullptr) {
            sqlite3_close(*db);
            *db = nullptr;
        }
        return false;
    }
    return true;
}

bool InitializeSnapshotSchema(sqlite3* db, std::string* error_out) {
    constexpr const char* kSql =
        "PRAGMA journal_mode=DELETE;"
        "CREATE TABLE snapshot_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "CREATE TABLE snapshot_files ("
        "  rel_path TEXT PRIMARY KEY NOT NULL,"
        "  size INTEGER NOT NULL,"
        "  content BLOB NOT NULL"
        ");";
    return Exec(db, kSql, error_out);
}

bool InsertMetadata(sqlite3* db, const char* key, const std::string& value, std::string* error_out) {
    Statement st;
    if (sqlite3_prepare_v2(db, "INSERT INTO snapshot_metadata(key, value) VALUES (?1, ?2);", -1, &st.st, nullptr)
        != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = SqliteError(db, "failed preparing snapshot metadata insert");
        }
        return false;
    }
    sqlite3_bind_text(st.st, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(st.st, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = SqliteError(db, "failed writing snapshot metadata");
        }
        return false;
    }
    return true;
}

bool InsertSnapshotFile(sqlite3* db, const SnapshotFile& file, std::string* error_out) {
    std::vector<char> bytes;
    if (!ReadFile(file.source_path, &bytes, error_out)) {
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "INSERT INTO snapshot_files(rel_path, size, content) VALUES (?1, ?2, ?3);",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = SqliteError(db, "failed preparing snapshot file insert");
        }
        return false;
    }

    sqlite3_bind_text(st.st, 1, file.relative_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 2, static_cast<sqlite3_int64>(bytes.size()));
    sqlite3_bind_blob(st.st, 3, bytes.empty() ? nullptr : bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = SqliteError(db, "failed writing snapshot file");
        }
        return false;
    }
    return true;
}

bool ValidateSnapshot(sqlite3* db, std::int64_t* file_count_out, std::string* error_out) {
    Statement st;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM snapshot_files;", -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = SqliteError(db, "snapshot file is missing snapshot_files table");
        }
        return false;
    }
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        if (error_out != nullptr) {
            *error_out = SqliteError(db, "failed reading snapshot file count");
        }
        return false;
    }
    if (file_count_out != nullptr) {
        *file_count_out = sqlite3_column_int64(st.st, 0);
    }
    return true;
}

bool IsSafeRelativePath(const std::filesystem::path& path) {
    if (path.is_absolute()) {
        return false;
    }
    for (const auto& part : path) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

bool RestoreSnapshotFiles(sqlite3* db, const std::filesystem::path& target_root, std::string* error_out) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT rel_path, content FROM snapshot_files ORDER BY rel_path;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = SqliteError(db, "failed preparing snapshot restore query");
        }
        return false;
    }

    while (true) {
        const int step = sqlite3_step(st.st);
        if (step == SQLITE_DONE) {
            return true;
        }
        if (step != SQLITE_ROW) {
            if (error_out != nullptr) {
                *error_out = SqliteError(db, "failed reading snapshot file row");
            }
            return false;
        }

        const auto* rel_text = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 0));
        if (rel_text == nullptr || *rel_text == '\0') {
            if (error_out != nullptr) {
                *error_out = "snapshot contains an empty relative path";
            }
            return false;
        }

        const std::filesystem::path relative_path = std::filesystem::path(rel_text).lexically_normal();
        if (!IsSafeRelativePath(relative_path)) {
            if (error_out != nullptr) {
                *error_out = "snapshot contains an unsafe relative path: " + relative_path.string();
            }
            return false;
        }

        const void* blob = sqlite3_column_blob(st.st, 1);
        const int blob_size = sqlite3_column_bytes(st.st, 1);
        if (!WriteFile(target_root / relative_path, blob, blob_size, error_out)) {
            return false;
        }
    }
}

DbSnapshotResult RestartRuntime(
    savorqt::SavorDbRuntime& runtime,
    const std::filesystem::path& root,
    const DbSnapshotResult& result) {
    std::string restart_error;
    if (runtime.start(root, &restart_error)) {
        return result;
    }

    if (result.ok) {
        return DbSnapshotResult::Err("snapshot operation completed, but failed restarting SavorDb: " + restart_error);
    }
    return DbSnapshotResult::Err(result.error + "; additionally failed restarting SavorDb: " + restart_error);
}

void PublishProgress(
    const std::function<void(const DbSnapshotProgress&)>& callback,
    DbSnapshotPhase phase,
    std::int64_t completed,
    std::int64_t total,
    std::string detail = {}) {
    if (!callback) {
        return;
    }

    DbSnapshotProgress progress{};
    progress.phase = phase;
    progress.completed = completed;
    progress.current = completed;
    progress.total = total;
    progress.detail = std::move(detail);
    callback(progress);
}

} // namespace

DbSnapshotResult DbSnapshotService::SaveSnapshot(
    const std::string& snapshot_path,
    std::function<void(const DbSnapshotProgress&)> progress_callback) {
    namespace fs = std::filesystem;

    if (snapshot_path.empty()) {
        return DbSnapshotResult::Err("snapshot path is empty");
    }

    auto& runtime = savorqt::SavorDbRuntime::instance();
    const fs::path root = runtime.root();
    const bool was_running = runtime.isRunning();
    if (root.empty()) {
        return DbSnapshotResult::Err("SavorDb root is empty");
    }

    PublishProgress(progress_callback, DbSnapshotPhase::Preparing, 0, 0);
    if (was_running) {
        runtime.stop();
    }

    DbSnapshotResult result = DbSnapshotResult::Ok();
    const fs::path snapshot = snapshot_path;
    const fs::path temp_snapshot = MakeTempSnapshotPath(snapshot);
    sqlite3* db = nullptr;

    do {
        std::vector<SnapshotFile> files;
        PublishProgress(progress_callback, DbSnapshotPhase::ScanningArtifacts, 0, 0);
        std::string error;
        if (!CollectSnapshotFiles(root, snapshot, &files, &error)) {
            result = DbSnapshotResult::Err(error);
            break;
        }

        std::error_code ec;
        fs::remove(temp_snapshot, ec);
        if (!OpenSnapshotForWrite(temp_snapshot, &db, &error)) {
            result = DbSnapshotResult::Err(error);
            break;
        }
        if (!InitializeSnapshotSchema(db, &error)) {
            result = DbSnapshotResult::Err(error);
            break;
        }
        if (!Exec(db, "BEGIN IMMEDIATE;", &error)) {
            result = DbSnapshotResult::Err(error);
            break;
        }
        if (!InsertMetadata(db, "format", "savorqt-savordb-snapshot-v1", &error)
            || !InsertMetadata(db, "source_root", root.string(), &error)
            || !InsertMetadata(db, "file_count", std::to_string(files.size()), &error)) {
            result = DbSnapshotResult::Err(error);
            break;
        }

        std::int64_t completed = 0;
        const std::int64_t total = static_cast<std::int64_t>(files.size());
        for (const SnapshotFile& file : files) {
            const bool artifact_file = file.relative_path.rfind("object_store/", 0) == 0
                || file.relative_path.rfind("archive_store/", 0) == 0;
            PublishProgress(
                progress_callback,
                artifact_file ? DbSnapshotPhase::WritingArtifacts : DbSnapshotPhase::WritingCoreEntries,
                completed,
                total,
                file.relative_path);
            if (!InsertSnapshotFile(db, file, &error)) {
                result = DbSnapshotResult::Err(error);
                break;
            }
            ++completed;
        }
        if (!result.ok) {
            break;
        }
        if (!Exec(db, "COMMIT;", &error)) {
            result = DbSnapshotResult::Err(error);
            break;
        }
        sqlite3_close(db);
        db = nullptr;

        PublishProgress(progress_callback, DbSnapshotPhase::Finalizing, total, total);
        fs::remove(snapshot, ec);
        ec.clear();
        fs::rename(temp_snapshot, snapshot, ec);
        if (ec) {
            result = DbSnapshotResult::Err("failed finalizing snapshot file: " + ec.message());
            break;
        }
        PublishProgress(progress_callback, DbSnapshotPhase::Complete, total, total);
    } while (false);

    if (db != nullptr) {
        std::string ignored;
        (void)Exec(db, "ROLLBACK;", &ignored);
        sqlite3_close(db);
    }

    if (!result.ok) {
        std::error_code ec;
        fs::remove(temp_snapshot, ec);
    }

    if (was_running) {
        return RestartRuntime(runtime, root, result);
    }
    return result;
}

DbSnapshotResult DbSnapshotService::LoadSnapshot(
    const std::string& snapshot_path,
    const std::string& target_root,
    bool switch_to_target) {
    namespace fs = std::filesystem;

    if (snapshot_path.empty()) {
        return DbSnapshotResult::Err("snapshot path is empty");
    }
    if (target_root.empty()) {
        return DbSnapshotResult::Err("target SavorDb root is empty");
    }

    auto& runtime = savorqt::SavorDbRuntime::instance();
    const fs::path previous_root = runtime.root();
    const bool was_running = runtime.isRunning();
    const fs::path target = target_root;
    fs::path snapshot = snapshot_path;

    std::error_code ec;
    if (!fs::exists(snapshot, ec) || !fs::is_regular_file(snapshot, ec)) {
        return DbSnapshotResult::Err("snapshot file does not exist: " + snapshot.string());
    }

    fs::path restore_copy;
    if (IsPathInside(snapshot, target)) {
        restore_copy = MakeRestoreCopyPath();
        fs::copy_file(snapshot, restore_copy, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return DbSnapshotResult::Err("failed copying snapshot out of target root before restore: " + ec.message());
        }
        snapshot = restore_copy;
    }

    if (was_running) {
        runtime.stop();
    }

    DbSnapshotResult result = DbSnapshotResult::Ok();
    sqlite3* db = nullptr;
    do {
        std::string error;
        if (!OpenSnapshotForRead(snapshot, &db, &error)) {
            result = DbSnapshotResult::Err(error);
            break;
        }
        std::int64_t file_count = 0;
        if (!ValidateSnapshot(db, &file_count, &error)) {
            result = DbSnapshotResult::Err(error);
            break;
        }

        fs::remove_all(target, ec);
        if (ec) {
            result = DbSnapshotResult::Err("failed clearing target SavorDb root: " + ec.message());
            break;
        }
        fs::create_directories(target, ec);
        if (ec) {
            result = DbSnapshotResult::Err("failed creating target SavorDb root: " + ec.message());
            break;
        }
        if (!RestoreSnapshotFiles(db, target, &error)) {
            result = DbSnapshotResult::Err(error);
            break;
        }
    } while (false);

    if (db != nullptr) {
        sqlite3_close(db);
    }
    if (!restore_copy.empty()) {
        fs::remove(restore_copy, ec);
    }

    if (switch_to_target) {
        return RestartRuntime(runtime, target, result);
    }
    if (was_running && !previous_root.empty()) {
        return RestartRuntime(runtime, previous_root, result);
    }
    return result;
}

} // namespace savor::db
