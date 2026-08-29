#pragma once

#include "WorkflowComposition.h"

#include <atomic>
#include <condition_variable>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct sqlite3;

namespace savor::db {
struct IAnalysisDb;
struct IAuthoringDb;
struct IExecutionDb;
struct IStateDb;
}

namespace savor::db::execution::workflow {

struct WorkflowExpansionTarget {
    std::int64_t neutral_epoch_count = 0;
    std::int64_t rtc_value = 0;

    auto operator<=>(const WorkflowExpansionTarget&) const = default;
};

std::vector<WorkflowExpansionTarget> NormalizeExactFirstBattleExpansionTargets(
    const std::vector<WorkflowExpansionTarget>& requested);

struct WorkflowExpansionCreateRequest {
    WorkflowExpansionKind kind = WorkflowExpansionKind::None;
    std::string source_ref_kind;
    std::int64_t source_ref_id = 0;
    std::optional<std::int64_t> rtc_min;
    std::optional<std::int64_t> rtc_max;
    std::int64_t max_neutral_epochs = 0;
    std::vector<WorkflowExpansionTarget> targets;
    std::string created_by;
};

struct WorkflowExpansionMemberSnapshot {
    std::int64_t workflow_expansion_member_id = 0;
    std::string role;
    std::int64_t neutral_epoch_count = 0;
    std::optional<std::int64_t> rtc_value;
    std::int64_t workflow_instance_id = 0;
    std::string state;
};

struct WorkflowExpansionSnapshot {
    std::int64_t workflow_expansion_id = 0;
    WorkflowExpansionKind kind = WorkflowExpansionKind::None;
    std::string state;
    std::int64_t source_ref_id = 0;
    std::optional<std::int64_t> rtc_min;
    std::optional<std::int64_t> rtc_max;
    std::int64_t max_neutral_epochs = 0;
    std::optional<std::string> failure_text;
    std::optional<std::int64_t> source_dtm_artifact_id;
    std::optional<std::int64_t> source_annotation_attempt_id;
    std::optional<std::int64_t> source_root_establishment_attempt_id;
    std::vector<WorkflowExpansionTarget> targets;
    std::vector<WorkflowExpansionMemberSnapshot> members;
};

struct PreparedTasRootSourceSnapshot {
    std::int64_t annotation_attempt_id = 0;
    std::int64_t root_establishment_attempt_id = 0;
    std::int64_t source_dtm_artifact_id = 0;
    std::string source_dtm_sha256;
    std::string display_name;
};

enum class FirstBattleCoverageStage : std::uint8_t {
    NotRun = 0,
    Validated,
    Sterilized,
    SeedProbed,
    BattleTested,
};

struct FirstBattleDelayPreparationSnapshot {
    std::int64_t neutral_epoch_count = 0;
    std::string state = "NOT_STARTED";
    std::vector<std::int64_t> workflow_instance_ids;
    std::vector<std::int64_t> retryable_workflow_instance_ids;
    std::string diagnostic;
};

struct FirstBattleCoverageCellSnapshot {
    std::int64_t rtc_value = 0;
    std::int64_t neutral_epoch_count = 0;
    FirstBattleCoverageStage stage = FirstBattleCoverageStage::NotRun;
    std::string lifecycle = "NOT_RUN";
    std::int64_t confirmed_seed_count = 0;
    bool active = false;
    bool retryable = false;
    bool invariant_violation = false;
    std::vector<std::int64_t> workflow_instance_ids;
    std::vector<std::int64_t> retryable_workflow_instance_ids;
    std::string diagnostic;
};

struct FirstBattleCoverageQuery {
    std::int64_t source_annotation_attempt_id = 0;
    std::optional<std::int64_t> workflow_expansion_id;
    std::int64_t rtc_min = 0;
    std::int64_t rtc_max = 0;
    std::int64_t max_neutral_epochs = 0;
};

struct FirstBattleCoverageSnapshot {
    std::int64_t source_dtm_artifact_id = 0;
    std::optional<std::int64_t> source_annotation_attempt_id;
    std::optional<std::int64_t> source_root_establishment_attempt_id;
    std::int64_t rtc_min = 0;
    std::int64_t rtc_max = 0;
    std::int64_t max_neutral_epochs = 0;
    std::vector<FirstBattleDelayPreparationSnapshot> delay_preparations;
    std::vector<FirstBattleCoverageCellSnapshot> cells;
};

struct LaunchMissingFirstBattleCoverageRequest {
    std::int64_t source_annotation_attempt_id = 0;
    std::vector<WorkflowExpansionTarget> targets;
    std::string created_by;
};

struct LaunchMissingFirstBattleCoverageReceipt {
    std::optional<std::int64_t> workflow_expansion_id;
    std::int64_t requested_count = 0;
    std::int64_t launched_count = 0;
    std::int64_t already_covered_count = 0;
    std::int64_t active_count = 0;
    std::int64_t retryable_count = 0;
};

class WorkflowExpansionService {
public:
    WorkflowExpansionService(std::filesystem::path execution_db_path,
        std::filesystem::path analysis_db_path, IAuthoringDb* authoring,
        IExecutionDb* execution, IStateDb* state, IAnalysisDb* analysis);
    ~WorkflowExpansionService();

    bool Start(std::string* error_out = nullptr);
    void Stop();
    bool Create(const WorkflowExpansionCreateRequest& request,
        std::int64_t* expansion_id_out, std::string* error_out = nullptr);
    std::vector<WorkflowExpansionSnapshot> List(bool include_final = false) const;
    std::vector<PreparedTasRootSourceSnapshot> ListPreparedTasRootSources(
        int limit = 1000) const;
    bool ReadFirstBattleCoverage(const FirstBattleCoverageQuery& query,
        FirstBattleCoverageSnapshot* snapshot_out,
        std::string* error_out = nullptr) const;
    bool LaunchMissingFirstBattleCoverage(
        const LaunchMissingFirstBattleCoverageRequest& request,
        LaunchMissingFirstBattleCoverageReceipt* receipt_out,
        std::string* error_out = nullptr);
    void Wake();

private:
    void Run();
    void AdvanceAll();
    bool Advance(const WorkflowExpansionSnapshot& expansion, std::string* error_out);

    std::filesystem::path execution_db_path_;
    std::filesystem::path analysis_db_path_;
    IAuthoringDb* authoring_ = nullptr;
    IExecutionDb* execution_ = nullptr;
    IStateDb* state_ = nullptr;
    IAnalysisDb* analysis_ = nullptr;
    sqlite3* db_ = nullptr;
    sqlite3* analysis_sqlite_ = nullptr;
    std::atomic<bool> stopping_{false};
    mutable std::recursive_mutex db_mutex_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
};

} // namespace savor::db::execution::workflow
