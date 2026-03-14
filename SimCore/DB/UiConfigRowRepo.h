#pragma once
#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include "DBCore/DbService.h"
#include <cstdint>
#include <vector>
#include <future>
#include <optional>

namespace simcore::db {

    struct UiConfigRow {
        int64_t  id{};
        int64_t  preset_id{};
        int32_t  turn_index{};
        int32_t  actor_slot{};
        int64_t  created_at{}; // unix seconds
    };

    struct UiConfigRowRepo {
        // Async
        static std::future<DbResult<std::vector<int64_t>>> InsertManyAsync(const std::vector<UiConfigRow>& rows, RetryPolicy rp = {});
        static std::future<DbResult<std::vector<UiConfigRow>>> GetByIdsAsync(const std::vector<int64_t>& ids, RetryPolicy rp = {});

        // Blocking conveniences
        static inline DbResult<std::vector<int64_t>> InsertMany(const std::vector<UiConfigRow>& rows) { return InsertManyAsync(rows).get(); }
        static inline DbResult<std::vector<UiConfigRow>> GetByIds(const std::vector<int64_t>& ids) { return GetByIdsAsync(ids).get(); }
    };

} // namespace simcore::db
