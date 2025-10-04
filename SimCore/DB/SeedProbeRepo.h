#pragma once
#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include "DBCore/DbService.h"
#include <future>
#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace simcore {
    namespace db {

        struct SeedProbeRow {
            int64_t id{};
            int64_t savestate_id{};
            int64_t version_id{};
            int64_t neutral_seed{};
            std::string status; // planned|running|done
            int32_t complete{}; // if present in schema
        };

        struct SeedProbeRepo {
            // Async
            static std::future<DbResult<int64_t>> CreateAsync(int64_t savestate_id, int64_t version_id, RetryPolicy rp = {});
            static std::future<DbResult<void>>    MarkRunningAsync(int64_t probe_id, RetryPolicy rp = {});
            static std::future<DbResult<void>>    SetNeutralSeedAsync(int64_t probe_id, int64_t neutral_seed, RetryPolicy rp = {});
            static std::future<DbResult<void>>    MarkDoneAsync(int64_t probe_id, RetryPolicy rp = {});
            static std::future<DbResult<SeedProbeRow>> GetAsync(int64_t probe_id, RetryPolicy rp = {});
            static std::future<DbResult<std::vector<SeedProbeRow>>> ListActiveForSavestateAsync(int64_t savestate_id, RetryPolicy rp = {});
            static std::future<DbResult<std::vector<SeedProbeRow>>> ListPlannedAsync(RetryPolicy rp = {});

            // Blocking convenience
            static inline DbResult<int64_t> Create(int64_t savestate_id, int64_t version_id) { return CreateAsync(savestate_id, version_id).get(); }
            static inline DbResult<void>    MarkRunning(int64_t probe_id) { return MarkRunningAsync(probe_id).get(); }
            static inline DbResult<void>    SetNeutralSeed(int64_t probe_id, int64_t neutral_seed) { return SetNeutralSeedAsync(probe_id, neutral_seed).get(); }
            static inline DbResult<void>    MarkDone(int64_t probe_id) { return MarkDoneAsync(probe_id).get(); }
            static inline DbResult<SeedProbeRow> Get(int64_t probe_id) { return GetAsync(probe_id).get(); }
            static inline DbResult<std::vector<SeedProbeRow>> ListActiveForSavestate(int64_t savestate_id) { return ListActiveForSavestateAsync(savestate_id).get(); }
            static inline DbResult<std::vector<SeedProbeRow>> ListPlanned() { return ListPlannedAsync().get(); }
        };

    }
}
