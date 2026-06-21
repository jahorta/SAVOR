#include "DbCopy.h"

#include <array>
#include <chrono>
#include <iostream>
#include <sqlite3.h>
#include <string>
#include <thread>

namespace savor::predict {

namespace fs = std::filesystem;

namespace {

class SqliteHandle {
public:
    SqliteHandle() = default;
    SqliteHandle(const SqliteHandle&) = delete;
    SqliteHandle& operator=(const SqliteHandle&) = delete;
    ~SqliteHandle() {
        if (db_ != nullptr) {
            sqlite3_close(db_);
        }
    }

    sqlite3** out() { return &db_; }
    sqlite3* get() const { return db_; }

private:
    sqlite3* db_ = nullptr;
};

std::string sqlite_error(sqlite3* db) {
    return db == nullptr ? "sqlite error" : sqlite3_errmsg(db);
}

bool is_delete_safe(const fs::path& dest, const fs::path& source) {
    std::error_code ec;
    const auto absolute_dest = fs::weakly_canonical(fs::absolute(dest), ec);
    if (ec || absolute_dest.empty() || absolute_dest == absolute_dest.root_path()) {
        return false;
    }

    const auto absolute_source = fs::weakly_canonical(fs::absolute(source), ec);
    if (!ec && absolute_dest == absolute_source) {
        return false;
    }

    return absolute_dest.has_filename() && absolute_dest.parent_path() != absolute_dest;
}

int copy_sqlite_db(const fs::path& source, const fs::path& dest, std::ostream& err) {
    SqliteHandle source_db;
    int rc = sqlite3_open_v2(source.string().c_str(), source_db.out(), SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        err << "Failed to open source sqlite database " << source.string() << ": " << sqlite_error(source_db.get()) << "\n";
        return 1;
    }

    SqliteHandle dest_db;
    rc = sqlite3_open_v2(dest.string().c_str(), dest_db.out(), SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        err << "Failed to create destination sqlite database " << dest.string() << ": " << sqlite_error(dest_db.get()) << "\n";
        return 1;
    }

    sqlite3_backup* backup = sqlite3_backup_init(dest_db.get(), "main", source_db.get(), "main");
    if (backup == nullptr) {
        err << "Failed to initialize sqlite backup for " << source.string() << ": " << sqlite_error(dest_db.get()) << "\n";
        return 1;
    }

    do {
        rc = sqlite3_backup_step(backup, 256);
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);

    const int finish_rc = sqlite3_backup_finish(backup);
    if (rc != SQLITE_DONE || finish_rc != SQLITE_OK) {
        err << "Sqlite backup failed for " << source.string() << ": step=" << rc << " finish=" << finish_rc << "\n";
        return 1;
    }

    return 0;
}

int copy_directory_if_present(const fs::path& source, const fs::path& dest, std::ostream& out, std::ostream& err) {
    std::error_code ec;
    if (!fs::exists(source, ec)) {
        return 0;
    }
    if (!fs::is_directory(source, ec)) {
        err << "Expected directory at " << source.string() << "\n";
        return 1;
    }

    out << "Copying " << source.filename().string() << "\n";
    fs::copy(source, dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        err << "Failed to copy " << source.string() << " to " << dest.string() << ": " << ec.message() << "\n";
        return 1;
    }
    return 0;
}

} // namespace

int run_prepare_db(const PrepareDbOptions& options, std::ostream& out, std::ostream& err) {
    std::error_code ec;
    if (!fs::is_directory(options.source, ec)) {
        err << "Source DB root does not exist: " << options.source.string() << "\n";
        return 1;
    }

    if (fs::exists(options.dest, ec)) {
        if (!options.overwrite) {
            err << "Destination already exists: " << options.dest.string() << "\n";
            err << "Pass --overwrite to replace it.\n";
            return 1;
        }
        if (!is_delete_safe(options.dest, options.source)) {
            err << "Refusing to overwrite unsafe destination path: " << options.dest.string() << "\n";
            return 1;
        }
        out << "Removing existing destination " << options.dest.string() << "\n";
        fs::remove_all(options.dest, ec);
        if (ec) {
            err << "Failed to remove destination: " << ec.message() << "\n";
            return 1;
        }
    }

    fs::create_directories(options.dest, ec);
    if (ec) {
        err << "Failed to create destination: " << ec.message() << "\n";
        return 1;
    }

    constexpr std::array<const char*, 6> db_names = {
        "analysis.db",
        "execution.db",
        "state.db",
        "ui_read.db",
        "authoring.db",
        "archive.db"};

    for (const auto* db_name : db_names) {
        const auto source_db = options.source / db_name;
        if (!fs::exists(source_db, ec)) {
            continue;
        }
        out << "Copying " << db_name << " via sqlite backup\n";
        const auto dest_db = options.dest / db_name;
        if (const int rc = copy_sqlite_db(source_db, dest_db, err); rc != 0) {
            return rc;
        }
    }

    if (const int rc = copy_directory_if_present(options.source / "object_store", options.dest / "object_store", out, err); rc != 0) {
        return rc;
    }
    if (const int rc = copy_directory_if_present(options.source / "archive_store", options.dest / "archive_store", out, err); rc != 0) {
        return rc;
    }

    out << "Prepared predictor DB root at " << options.dest.string() << "\n";
    return 0;
}

} // namespace savor::predict
