#pragma once

#include <filesystem>
#include <iosfwd>
#include <cstdint>
#include <string>
#include <vector>

#include "Common/DbConfigPaths.h"

namespace savor::debugtool {

struct DirtySeedPlan {
    std::string entity_kind;
    std::string source_sql;
    std::int64_t count = 0;
};

struct StreamReprojectPlan {
    std::string stream_id;
    std::string source_context;
    std::string source_outbox_table;
    std::filesystem::path source_db_path;
    std::vector<std::string> ui_tables_to_clear;
    std::vector<DirtySeedPlan> dirty_seed_plans;
    std::int64_t source_high_water_outbox_id = 0;
    std::int64_t expected_dirty_rows = 0;
};

struct ReprojectUiReadOptions {
    std::filesystem::path db_root;
    std::filesystem::path migration_root;
    bool apply = false;
    int max_iterations = 100;
    std::vector<std::string> stream_ids;
};

struct ReprojectUiReadPlan {
    savor::db::DbConfigPaths db_paths;
    std::filesystem::path migration_root;
    bool apply = false;
    int max_iterations = 100;
    std::vector<StreamReprojectPlan> streams;
};

struct ReprojectUiReadResult {
    bool applied = false;
    std::filesystem::path backup_path;
    int iterations = 0;
    std::vector<StreamReprojectPlan> streams;
};

std::vector<std::string> AllUiReadProjectionStreamIds();

bool ParseOptions(int argc, char** argv, ReprojectUiReadOptions* options, std::string* error_out);
bool BuildPlan(const ReprojectUiReadOptions& options, ReprojectUiReadPlan* plan_out, std::string* error_out);
bool ExecutePlan(const ReprojectUiReadPlan& plan, ReprojectUiReadResult* result_out, std::string* error_out);
void PrintPlan(const ReprojectUiReadPlan& plan, std::ostream& out);
void PrintResult(const ReprojectUiReadResult& result, std::ostream& out);
void PrintUsage(std::ostream& out);

} // namespace savor::debugtool
