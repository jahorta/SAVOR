#pragma once

#include <cstdint>
#include <string>

#include <sqlite3.h>

namespace simcore::db::execution::workflow {

class WorkflowProjector {
public:
    explicit WorkflowProjector(sqlite3* db);

    bool ProjectInstance(std::int64_t workflow_instance_id, std::string* error_out);
    bool ProjectFromOutbox(const std::string& projector_name, int max_batch_size, std::string* error_out, int max_attempts = 5);
    std::int64_t GetCheckpoint(const std::string& projector_name, std::string* error_out) const;

private:
    sqlite3* db_ = nullptr;
};

} // namespace simcore::db::execution::workflow
