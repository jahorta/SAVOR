#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "../../DB/DBCore/DbResult.h"

namespace simcore {
    namespace phase {

        struct TasJobSpec {
            int64_t base_dtm_id;
            uint32_t run_ms{ 0 };
            uint32_t vi_stall_ms{ 0 };
            int new_rtc_min{ 0 };
            int new_rtc_max{ 0 };
            int priority{ 0 };
            bool progress_enable{ true };
        };

        class TasMovieCoordinator {
        public:
            static simcore::db::DbResult<int64_t> SetupJobSet();
            static simcore::db::DbResult<void> QueueJob(int64_t job_set_id, const TasJobSpec& job);
            static simcore::db::DbResult<std::string> PollProgress_JobSet(int64_t job_set_id);
            static simcore::db::DbResult<std::string> RetrieveResults_JobSet(int64_t job_set_id);
        };

    }
} // namespace
