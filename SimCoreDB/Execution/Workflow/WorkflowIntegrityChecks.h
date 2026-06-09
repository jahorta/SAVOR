#pragma once

#include <string>

#include <sqlite3.h>

namespace simcore::db::execution::workflow {

struct WorkflowIntegrityReport {
    int dangling_edge_count = 0;
    int missing_job_set_link_count = 0;
    int non_terminal_step_in_completed_instance_count = 0;
    int non_terminal_step_in_terminal_instance_count = 0;
    int completed_step_missing_completion_ts_count = 0;
    int missing_step_activation_count = 0;
    int dangling_activation_edge_count = 0;
    int non_terminal_root_activation_in_completed_instance_count = 0;

    bool IsClean() const {
        return dangling_edge_count == 0
            && missing_job_set_link_count == 0
            && non_terminal_step_in_completed_instance_count == 0
            && non_terminal_step_in_terminal_instance_count == 0
            && completed_step_missing_completion_ts_count == 0
            && missing_step_activation_count == 0
            && dangling_activation_edge_count == 0
            && non_terminal_root_activation_in_completed_instance_count == 0;
    }
};

bool RunWorkflowIntegrityChecks(sqlite3* db, WorkflowIntegrityReport* report_out, std::string* error_out);

} // namespace simcore::db::execution::workflow
