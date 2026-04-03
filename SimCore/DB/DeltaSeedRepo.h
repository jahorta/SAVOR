#pragma once
#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include "DBCore/DbService.h"
#include "../Core/Input/InputPlan.h"
#include <future>
#include <vector>
#include <cstdint>

namespace simcore {
    namespace db {

        struct DeltaSeedRow {
            int64_t id{};
            int64_t probe_id{};
            int32_t seed_delta{};   // result from the run
            GCInputFrame input{};   // stored as blob
            bool complete{};
        };

        struct DeltaSeedRepo {
            // Async
            static std::future<DbResult<int64_t>> BulkQueueAsync(
                int64_t probe_id,
                const std::vector<DeltaSeedRow>& rows,
                RetryPolicy rp = {});
            static std::future<DbResult<void>> MarkDoneAsync(int64_t id, RetryPolicy rp = {});
            static std::future<DbResult<void>> UpdateSeedDeltaAsync(int64_t id, int32_t seed_delta, RetryPolicy rp = {});
            static std::future<DbResult<void>> SetGridAsync(int64_t id, RetryPolicy rp = {});    // is_grid = 1
            static std::future<DbResult<void>> SetUniqueAsync(int64_t id, RetryPolicy rp = {});  // is_unique = 1
            static std::future<DbResult<std::vector<DeltaSeedRow>>> ListActiveAsync(int64_t probe_id, RetryPolicy rp = {});
            static std::future<DbResult<std::vector<DeltaSeedRow>>> ListForProbeAsync(int64_t probe_id, RetryPolicy rp = {});
            static std::future<DbResult<std::vector<DeltaSeedRow>>> ListGridForProbeAsync(int64_t probe_id, RetryPolicy rp = {});
            static std::future<DbResult<std::vector<DeltaSeedRow>>> ListUniqueForProbeAsync(int64_t probe_id, RetryPolicy rp = {});
            static std::future<DbResult<std::optional<DeltaSeedRow>>> GetAsync(int64_t id, RetryPolicy rp = {});
            static std::future<DbResult<bool>> ExistsForProbeSeedDeltaAsync(int64_t probe_id, int32_t seed_delta, RetryPolicy rp = {});

            // Blocking convenience
            static inline DbResult<int64_t> BulkQueue(int64_t probe_id, const std::vector<DeltaSeedRow>& rows) { return BulkQueueAsync(probe_id, rows).get(); }
            static inline DbResult<void> MarkDone(int64_t id) { return MarkDoneAsync(id).get(); }
            static inline DbResult<void> UpdateSeedDelta(int64_t id, int32_t seed_delta) { return UpdateSeedDeltaAsync(id, seed_delta).get(); }
            static inline DbResult<void> SetGrid(int64_t id) { return SetGridAsync(id).get(); }
            static inline DbResult<void> SetUnique(int64_t id) { return SetUniqueAsync(id).get(); }
            static inline DbResult<std::vector<DeltaSeedRow>> ListActive(int64_t probe_id) { return ListActiveAsync(probe_id).get(); }
            static inline DbResult<std::vector<DeltaSeedRow>> ListForProbe(int64_t probe_id) { return ListForProbeAsync(probe_id).get(); }
            static std::future<DbResult<int64_t>> InsertOneAsync(int64_t probe_id, const DeltaSeedRow& row, bool is_grid, bool is_unique, RetryPolicy rp = {});
            static inline DbResult<int64_t> InsertOne(int64_t probe_id, const DeltaSeedRow& row, bool is_grid, bool is_unique) { return InsertOneAsync(probe_id, row, is_grid, is_unique).get(); }
            static inline DbResult<std::vector<DeltaSeedRow>> ListGridForProbe(int64_t probe_id) { return ListGridForProbeAsync(probe_id).get(); }
            static inline DbResult<std::vector<DeltaSeedRow>> ListUniqueForProbe(int64_t probe_id) { return ListUniqueForProbeAsync(probe_id).get(); }
            static inline DbResult<std::optional<DeltaSeedRow>> Get(int64_t id) { return GetAsync(id).get(); }
            static inline DbResult<bool> ExistsForProbeSeedDelta(int64_t probe_id, int32_t seed_delta) { return ExistsForProbeSeedDeltaAsync(probe_id, seed_delta).get(); }
        };

    } // namespace db
} // namespace simcore
