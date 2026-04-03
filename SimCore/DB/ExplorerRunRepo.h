#pragma once
#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include "DBCore/DbService.h"
#include "Querying/Paging.h"
#include <future>
#include <string>
#include <cstdint>
#include <vector>

namespace simcore {
    namespace db {

        struct ExplorerRunRow {
            int64_t id{};
            int64_t root_job_set_id{};
            int64_t settings_id{};
            int64_t plan_id{};        
            int64_t delta_seed_id{};  
            bool has_victory{ false };
        };

        struct ExplorerRunRepo {
            // Async
            static std::future<DbResult<int64_t>> IdempotentCreateAsync(int64_t root_job_set_id, int64_t settings_id, int64_t plan_id, int64_t delta_seed_id, RetryPolicy rp = {});
            static std::future<DbResult<ExplorerRunRow>> GetAsync(int64_t run_id, RetryPolicy rp = {});
            static std::future<DbResult<void>> SetHasVictoryAsync(int64_t run_id, bool has_victory, RetryPolicy rp = {});
            static std::future<DbResult<std::vector<int64_t>>> ListRootJobSetIdsByVictoryAsync(bool has_victory, RetryPolicy rp = {});
            static std::future<DbResult<Page<int64_t>>> ListRootJobSetIdsByVictoryPagedAsync(
                bool has_victory,
                std::optional<KeysetCursor> before,
                int limit,
                RetryPolicy rp = {});

            // Blocking convenience
            static inline DbResult<int64_t> IdempotentCreate(int64_t root_job_set_id, int64_t settings_id, int64_t plan_id, int64_t delta_seed_id) { return IdempotentCreateAsync(root_job_set_id, settings_id, plan_id, delta_seed_id).get(); }
            static inline DbResult<ExplorerRunRow> Get(int64_t run_id) { return GetAsync(run_id).get(); }
            static inline DbResult<void> SetHasVictory(int64_t run_id, bool has_victory) {
                return SetHasVictoryAsync(run_id, has_victory).get();
            }
            static inline DbResult<std::vector<int64_t>> ListRootJobSetIdsByVictory(bool has_victory) {
                return ListRootJobSetIdsByVictoryAsync(has_victory).get();
            }
            static inline DbResult<Page<int64_t>> ListRootJobSetIdsByVictoryPaged(
                bool has_victory,
                std::optional<KeysetCursor> before,
                int limit) {
                return ListRootJobSetIdsByVictoryPagedAsync(has_victory, before, limit).get();
            }
        };

    }
}
