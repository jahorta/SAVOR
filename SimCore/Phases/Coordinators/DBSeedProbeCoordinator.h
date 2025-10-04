#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "../../DB/DBCore/DbResult.h"

namespace simcore {
    namespace phase {

        struct SeedProbeBatch {
            int64_t probe_id{};
            std::vector<std::string> inputs_hex;
            std::vector<std::string> expected_deltas_i32;   // optional
            std::vector<std::string> expected_tags;         // optional
            uint32_t run_ms{ 0 };
            uint32_t vi_stall_ms{ 0 };
            bool is_neutral{ false };
            bool is_grid{ false };
            bool is_unique{ false };
        };

        class SeedProbeCoordinator {
        public:
            static simcore::db::DbResult<int64_t> SetupJobSet();
            static simcore::db::DbResult<void> QueueBatches(int64_t job_set_id, const std::vector<SeedProbeBatch>& batches);
            static simcore::db::DbResult<std::string> PollProgress_JobSet(int64_t job_set_id);
            static simcore::db::DbResult<std::string> RetrieveResults_JobSet(int64_t job_set_id);
        };

    }
} // namespace
