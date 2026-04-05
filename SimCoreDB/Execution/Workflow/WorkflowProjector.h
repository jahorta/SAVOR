#pragma once

#include <cstdint>
#include <string>

#include <sqlite3.h>

namespace simcore::db::execution::workflow {

class WorkflowProjector {
public:
    explicit WorkflowProjector(sqlite3* db);

    bool ProjectInstance(std::int64_t workflow_instance_id, std::string* error_out);

private:
    sqlite3* db_ = nullptr;
};

} // namespace simcore::db::execution::workflow
