#pragma once
#include "../../DB/DBCore/DbResult.h"
#include "../../DB/Scheduling/JobsRepo.h"

namespace simcore {

    struct TriggerCtx {
        int64_t prev_job_id{};      // Filled for job-scope
        int64_t prev_job_set_id{};  // Filled for both scopes
        int prev_program_kind{};    // PK_*
        bool prev_success{};
        const char* scope{};        // "job" or "job_set"
    };

    class TriggerEngine {
    public:
        static simcore::db::DbResult<void> after_terminal(int64_t job_id);
    };

} // namespace simcore
