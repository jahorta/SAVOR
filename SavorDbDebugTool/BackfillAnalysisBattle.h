#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>

#include "Common/DbConfigPaths.h"

namespace savor::debugtool {

struct BackfillAnalysisBattleOptions {
    std::filesystem::path db_root;
    std::filesystem::path migration_root;
    bool apply = false;
};

struct BackfillAnalysisBattlePlan {
    savor::db::DbConfigPaths db_paths;
    std::filesystem::path migration_root;
    bool apply = false;
};

struct BackfillAnalysisBattleResult {
    bool applied = false;
    std::filesystem::path backup_path;
    std::int64_t candidate_rows = 0;
    std::int64_t rows_updated = 0;
    std::int64_t rows_already_complete = 0;
    std::int64_t rows_missing_exec_job = 0;
    std::int64_t rows_missing_or_invalid_ini = 0;
    std::int64_t rows_invalid_command_blob = 0;
};

bool ParseBackfillAnalysisBattleOptions(int argc, char** argv, BackfillAnalysisBattleOptions* options, std::string* error_out);
bool BuildBackfillAnalysisBattlePlan(const BackfillAnalysisBattleOptions& options, BackfillAnalysisBattlePlan* plan_out, std::string* error_out);
bool ExecuteBackfillAnalysisBattlePlan(const BackfillAnalysisBattlePlan& plan, BackfillAnalysisBattleResult* result_out, std::string* error_out);
void PrintBackfillAnalysisBattlePlan(const BackfillAnalysisBattlePlan& plan, std::ostream& out);
void PrintBackfillAnalysisBattleResult(const BackfillAnalysisBattleResult& result, std::ostream& out);
void PrintBackfillAnalysisBattleUsage(std::ostream& out);

} // namespace savor::debugtool
