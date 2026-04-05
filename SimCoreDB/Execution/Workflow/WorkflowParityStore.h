#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "WorkflowParityDiagnostics.h"

namespace simcore::db::execution::workflow {

struct WorkflowParitySummary {
    std::string run_ref;
    std::int64_t workflow_instance_id = 0;
    int compared_steps = 0;
    int matched_steps = 0;
    int mismatch_steps = 0;
};

class WorkflowParityStore {
public:
    explicit WorkflowParityStore(sqlite3* db);

    bool PersistReport(
        const std::string& run_ref,
        std::int64_t workflow_instance_id,
        const WorkflowParityReport& report,
        std::string* error_out);

    std::vector<WorkflowParitySummary> ListSummaries(std::string* error_out) const;

private:
    bool EnsureSchema(std::string* error_out) const;

    sqlite3* db_ = nullptr;
};

} // namespace simcore::db::execution::workflow
