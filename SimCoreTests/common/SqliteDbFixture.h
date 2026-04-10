#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include "Common/DbConfigPaths.h"
#include "Common/DbService.h"
#include "Common/Migrations/MigrationRunner.h"

class SqliteDbFixture : public ::testing::Test {
protected:
    void SetUp() override {
        temp_root_ = std::filesystem::temp_directory_path() / std::filesystem::path("simcoredb_tests_XXXXXX");
        const auto unique_suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        temp_root_.replace_filename(std::string("simcoredb_tests_") + unique_suffix);
        ASSERT_TRUE(std::filesystem::create_directories(temp_root_));

        const auto shared_db_path = temp_root_ / "simcoredb_test.sqlite";
        simcore::db::DbConfigPaths config_paths{};
        config_paths.execution_db_path = shared_db_path;
        config_paths.state_db_path = shared_db_path;
        config_paths.analysis_db_path = shared_db_path;
        config_paths.authoring_db_path = shared_db_path;
        config_paths.ui_read_db_path = shared_db_path;
        config_paths.archive_db_path = shared_db_path;

        db_service_ = std::make_unique<simcore::db::core::DBService>(
            config_paths,
            simcore::db::migrations::MigrationSourceOptions{ .source_kind = simcore::db::migrations::MigrationSourceKind::Embedded });

        std::string start_error;
        ASSERT_TRUE(db_service_->Start(&start_error)) << start_error;

        ASSERT_EQ(SQLITE_OK, sqlite3_open(shared_db_path.string().c_str(), &db_));
    }

    void TearDown() override {
        if (db_ != nullptr) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        if (db_service_) {
            db_service_->Stop();
            db_service_.reset();
        }

        if (!temp_root_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(temp_root_, ec);
        }
    }

    std::filesystem::path temp_root_;
    std::unique_ptr<simcore::db::core::DBService> db_service_;
    sqlite3* db_ = nullptr;
};
