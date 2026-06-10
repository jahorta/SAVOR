#pragma once

#include <cstdint>
#include <string>

#include <sqlite3.h>

namespace savor::db::execution::workflow {

class WorkflowProjector {
public:
    explicit WorkflowProjector(sqlite3* db);

    bool ProjectInstance(std::int64_t workflow_instance_id, std::string* error_out);
    bool ProjectFromOutbox(const std::string& projector_name, int max_batch_size, std::string* error_out, int max_attempts = 5);

private:
    sqlite3* db_ = nullptr;
};

} // namespace savor::db::execution::workflow
