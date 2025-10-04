#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "../../DB/DBCore/DbResult.h"

namespace simcore {
    namespace phase {

        struct TasJobSpec {
            std::string dtm_path;
            std::string save_dir;
            uint32_t run_ms{ 0 };
            uint32_t vi_stall_ms{ 0 };
            int new_rtc{ 0 };
            int priority{ 0 };
            bool save_on_fail{ false };
            bool progress_enable{ true };
        };

        class TasMovieCoordinator {
        public:
            static simcore::db::DbResult<int64_t> SetupJobSet();
            static simcore::db::DbResult<void> QueueJobs(int64_t job_set_id, const std::vector<TasJobSpec>& jobs);
            static simcore::db::DbResult<std::string> PollProgress_JobSet(int64_t job_set_id);
            static simcore::db::DbResult<std::string> RetrieveResults_JobSet(int64_t job_set_id);
        };

    }
} // namespace
