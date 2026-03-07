#pragma once
#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include "DBCore/DbService.h"
#include <future>
#include <string>
#include <cstdint>
#include <vector>

namespace simcore {
    namespace db {

        struct ExplorerRunRow {
            int64_t id{};
            int64_t probe_id{};
            int64_t settings_id{};
            int64_t plan_id{};        
            int64_t delta_seed_id{};  
            int64_t group_id{};       
            std::string status;   // planned|running|done
            std::string progress_log;
            int32_t complete{};
        };

        struct ExplorerRunRepo {
            // Async
            static std::future<DbResult<int64_t>> IdempotentCreateAsync(int64_t settings_id, int64_t plan_id, int64_t delta_seed_id, RetryPolicy rp = {});
            static std::future<DbResult<void>>    AppendProgressAsync(int64_t run_id, std::string text, int max_bytes = 8192, RetryPolicy rp = {});
            static std::future<DbResult<void>>    MarkRunningAsync(int64_t run_id, RetryPolicy rp = {});
            static std::future<DbResult<void>>    MarkDoneAsync(int64_t run_id, RetryPolicy rp = {});
            static std::future<DbResult<ExplorerRunRow>> GetAsync(int64_t run_id, RetryPolicy rp = {});
            static std::future<DbResult<std::vector<ExplorerRunRow>>> ListActiveForProbeAsync(int64_t probe_id, RetryPolicy rp = {}); static std::future<DbResult<void>> SetResultsIniAsync(int64_t run_id, std::string ini_text, RetryPolicy rp = {});
            static std::future<DbResult<void>> SetProgressLogArtifactIdAsync(int64_t run_id, int64_t artifact_id, RetryPolicy rp = {});

            // Blocking convenience
            static inline DbResult<int64_t> IdempotentCreate(int64_t settings_id, int64_t plan_id, int64_t delta_seed_id) { return IdempotentCreateAsync(settings_id, plan_id, delta_seed_id).get(); }
            static inline DbResult<void>    AppendProgress(int64_t run_id, std::string text, int max_bytes = 8192) { return AppendProgressAsync(run_id, std::move(text), max_bytes).get(); }
            static inline DbResult<void>    MarkRunning(int64_t run_id) { return MarkRunningAsync(run_id).get(); }
            static inline DbResult<void>    MarkDone(int64_t run_id) { return MarkDoneAsync(run_id).get(); }
            static inline DbResult<ExplorerRunRow> Get(int64_t run_id) { return GetAsync(run_id).get(); }
            static inline DbResult<std::vector<ExplorerRunRow>> ListActiveForProbe(int64_t probe_id) { return ListActiveForProbeAsync(probe_id).get(); }
            static inline DbResult<void> SetResultsIni(int64_t run_id, std::string ini_text) {
                return SetResultsIniAsync(run_id, std::move(ini_text)).get();
            }
            static inline DbResult<void> SetProgressLogArtifactId(int64_t run_id, int64_t artifact_id) {
                return SetProgressLogArtifactIdAsync(run_id, artifact_id).get();
            }
        };

    }
}
