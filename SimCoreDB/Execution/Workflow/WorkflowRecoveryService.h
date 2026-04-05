#pragma once

#include <cstdint>
#include <string>

#include <sqlite3.h>

namespace simcore::db::execution::workflow {

struct WorkflowRecoveryResult {
    int completed_steps = 0;
    int failed_steps = 0;
};

class WorkflowRecoveryService {
public:
    explicit WorkflowRecoveryService(sqlite3* db);

    bool ReconcileInFlightInstances(WorkflowRecoveryResult* result_out, std::string* error_out);

private:
    sqlite3* db_ = nullptr;
};

} // namespace simcore::db::execution::workflow
