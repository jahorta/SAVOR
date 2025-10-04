#pragma once
#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include "DBCore/DbService.h"
#include <future>
#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace simcore::db {

        struct TasMovieRow {
            int64_t id{};
            int64_t base_file_id{};
            std::optional<int64_t> new_rtc{};
            std::string status;              // planned|running|done
            std::string progress_log;        // may be empty
            int64_t created_at{};            // epoch seconds
            std::optional<int64_t> started_at{};
            std::optional<int64_t> completed_at{};
            int32_t attempt_count{};
            std::string last_error;          // may be empty
            int32_t priority{};
            std::optional<int64_t> output_savestate_id{};
        };

        struct TasMovieRepo {
            // Async API (primary)
            static std::future<DbResult<int64_t>> EnqueueAsync(int64_t base_file_id, int64_t new_rtc, int32_t priority, RetryPolicy rp = {});
            static std::future<DbResult<int64_t>> IdempotentEnqueueAsync(int64_t base_file_id, int64_t new_rtc, int32_t priority, RetryPolicy rp = {});
            static std::future<DbResult<void>>    MarkRunningAsync(int64_t id, RetryPolicy rp = {});
            static std::future<DbResult<void>>    AppendProgressAsync(int64_t id, std::string text, int max_bytes = 8192, RetryPolicy rp = {});
            static std::future<DbResult<void>>    MarkFailedAsync(int64_t id, std::string err, RetryPolicy rp = {});
            static std::future<DbResult<void>>    MarkDoneAsync(int64_t id, int64_t savestate_id, RetryPolicy rp = {});
            static std::future<DbResult<TasMovieRow>>               GetAsync(int64_t id, RetryPolicy rp = {});
            static std::future<DbResult<std::vector<TasMovieRow>>>  ListPlannedAsync(RetryPolicy rp = {});
            static std::future<DbResult<std::vector<TasMovieRow>>>  ListActiveAsync(RetryPolicy rp = {});

            // Optional blocking conveniences
            static inline DbResult<int64_t> Enqueue(int64_t base_file_id, int64_t new_rtc, int32_t priority) { return EnqueueAsync(base_file_id, new_rtc, priority).get(); }
            static inline DbResult<int64_t> IdempotentEnqueue(int64_t base_file_id, int64_t new_rtc, int32_t priority) { return IdempotentEnqueueAsync(base_file_id, new_rtc, priority).get(); }
            static inline DbResult<void> MarkRunning(int64_t id) { return MarkRunningAsync(id).get(); }
            static inline DbResult<void> AppendProgress(int64_t id, std::string text, int max_bytes = 8192) { return AppendProgressAsync(id, std::move(text), max_bytes).get(); }
            static inline DbResult<void> MarkFailed(int64_t id, std::string err) { return MarkFailedAsync(id, std::move(err)).get(); }
            static inline DbResult<void> MarkDone(int64_t id, int64_t savestate_id) { return MarkDoneAsync(id, savestate_id).get(); }
            static inline DbResult<TasMovieRow> Get(int64_t id) { return GetAsync(id).get(); }
            static inline DbResult<std::vector<TasMovieRow>> ListPlanned() { return ListPlannedAsync().get(); }
            static inline DbResult<std::vector<TasMovieRow>> ListActive() { return ListActiveAsync().get(); }
        };

} // namespace simcore::db
