#pragma once
#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include "DBCore/DbService.h"
#include <future>
#include <cstdint>
#include <vector>

namespace simcore {
    namespace db {

        struct BattleContextRow {
            int64_t context_id{};
            int64_t job_set_id{};
            int64_t job_id{};
            int64_t artifact_id{};
            int64_t created_at{};
        };

        struct BattleContextRepo {
            static std::future<DbResult<int64_t>> InsertAsync(int64_t job_set_id, int64_t job_id, int64_t artifact_id, RetryPolicy rp = {});
            static std::future<DbResult<std::optional<BattleContextRow>>> GetByJobAsync(int64_t job_id, RetryPolicy rp = {});
            static std::future<DbResult<std::vector<BattleContextRow>>> ListByJobSetAsync(int64_t job_set_id, RetryPolicy rp = {});

            static inline DbResult<int64_t> Insert(int64_t job_set_id, int64_t job_id, int64_t artifact_id) { return InsertAsync(job_set_id, job_id, artifact_id).get(); }
            static inline DbResult<std::optional<BattleContextRow>> GetByJob(int64_t job_id) { return GetByJobAsync(job_id).get(); }
            static inline DbResult<std::vector<BattleContextRow>> ListByJobSet(int64_t job_set_id) { return ListByJobSetAsync(job_set_id).get(); }
        };

    }
} // namespace
