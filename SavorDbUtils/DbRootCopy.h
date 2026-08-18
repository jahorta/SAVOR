#pragma once

#include "Common/DbConfigPaths.h"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace savor::dbutils {

enum class SandboxMode {
    MinimalBattleSingleTurn,
    FullCopy,
};

struct TableCopyCount {
    std::string db_name;
    std::string table_name;
    int rows_copied = 0;
};

struct CopiedArtifactFile {
    std::int64_t artifact_id = 0;
    std::int64_t savestate_id = 0;
    std::filesystem::path source_path;
    std::filesystem::path copied_path;
};

struct CopyDbRootOptions {
    std::filesystem::path source_root;
    std::filesystem::path dest_root;
    bool overwrite = false;
};

struct CreateEmptyMigratedDbRootOptions {
    std::filesystem::path db_root;
    bool overwrite = false;
};

struct BattleJobSelector {
    std::optional<std::int64_t> turn_job_id;
    std::optional<std::int64_t> exec_job_id;
};

struct HydrateBattleSingleTurnJobSubsetOptions {
    std::filesystem::path source_root;
    std::filesystem::path target_root;
    BattleJobSelector selector;
    bool overwrite_target = false;
};

struct HydrateBattleSingleTurnJobSubsetsOptions {
    std::filesystem::path source_root;
    std::filesystem::path target_root;
    std::vector<BattleJobSelector> selectors;
    bool overwrite_target = false;
};

struct BattleSingleTurnJobSubsetResult {
    SandboxMode sandbox_mode = SandboxMode::MinimalBattleSingleTurn;
    std::filesystem::path source_root;
    std::filesystem::path target_root;
    std::int64_t source_turn_job_id = 0;
    std::int64_t source_exec_job_id = 0;
    std::int64_t source_job_set_id = 0;
    std::int64_t source_battle_set_id = 0;
    std::int64_t source_wave_id = 0;
    std::vector<TableCopyCount> table_counts;
    std::vector<CopiedArtifactFile> copied_artifacts;
    std::vector<std::string> validation_errors;
};

struct BattleSingleTurnJobSubsetsResult {
    SandboxMode sandbox_mode = SandboxMode::MinimalBattleSingleTurn;
    std::filesystem::path source_root;
    std::filesystem::path target_root;
    std::vector<BattleSingleTurnJobSubsetResult> jobs;
    std::vector<TableCopyCount> table_counts;
    std::vector<CopiedArtifactFile> copied_artifacts;
    std::vector<std::string> validation_errors;
};

savor::db::DbConfigPaths MakeDbConfigPaths(const std::filesystem::path& root);

int CopyDbRootFull(
    const CopyDbRootOptions& options,
    std::ostream& out,
    std::ostream& err);

int CreateEmptyMigratedDbRoot(
    const CreateEmptyMigratedDbRootOptions& options,
    std::ostream& err);

int HydrateBattleSingleTurnJobSubset(
    const HydrateBattleSingleTurnJobSubsetOptions& options,
    BattleSingleTurnJobSubsetResult* result_out,
    std::ostream& out,
    std::ostream& err);

int HydrateBattleSingleTurnJobSubsets(
    const HydrateBattleSingleTurnJobSubsetsOptions& options,
    BattleSingleTurnJobSubsetsResult* result_out,
    std::ostream& out,
    std::ostream& err);

const char* ToString(SandboxMode mode);
std::optional<SandboxMode> ParseSandboxMode(std::string_view text);

} // namespace savor::dbutils
