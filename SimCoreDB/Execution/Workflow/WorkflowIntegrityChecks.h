#pragma once

#include <string>

#include <sqlite3.h>

namespace simcore::db::execution::workflow {

struct WorkflowIntegrityReport {
    int dangling_edge_count = 0;
    int missing_job_set_link_count = 0;
    int non_terminal_step_in_completed_instance_count = 0;

    bool IsClean() const {
        return dangling_edge_count == 0
            && missing_job_set_link_count == 0
            && non_terminal_step_in_completed_instance_count == 0;
    }
};

bool RunWorkflowIntegrityChecks(sqlite3* db, WorkflowIntegrityReport* report_out, std::string* error_out);

} // namespace simcore::db::execution::workflow
