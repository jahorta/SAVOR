#pragma once
#include <string>
#include "../../DB/DBCore/DbResult.h"

namespace simcore {
    namespace phase {

        struct BCOptions { uint32_t run_ms{ 100000 }; uint32_t vi_stall_ms{ 2000 }; int priority{ 0 }; int64_t savestate_id{}; };

        class BattleContextCoordinator {
        public:
            static simcore::db::DbResult<int64_t> SetupJobSet();
            static simcore::db::DbResult<int64_t> QueueProbe(int64_t job_set_id, const BCOptions& opt);
            static simcore::db::DbResult<std::string> PollProgress_JobSet(int64_t job_set_id);
            static simcore::db::DbResult<std::string> RetrieveResults_JobSet(int64_t job_set_id);
        };

    }
} // namespace
