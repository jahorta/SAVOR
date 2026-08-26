#pragma once

#include "WorkflowComposition.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct sqlite3;

namespace savor::db {
struct IAnalysisDb;
struct IAuthoringDb;
struct IExecutionDb;
struct IStateDb;
}

namespace savor::db::execution::workflow {

struct WorkflowExpansionCreateRequest {
    WorkflowExpansionKind kind = WorkflowExpansionKind::None;
    std::string source_ref_kind;
    std::int64_t source_ref_id = 0;
    std::optional<std::int64_t> rtc_min;
    std::optional<std::int64_t> rtc_max;
    std::int64_t max_neutral_epochs = 0;
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
    std::vector<WorkflowExpansionMemberSnapshot> members;
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
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
};

} // namespace savor::db::execution::workflow
