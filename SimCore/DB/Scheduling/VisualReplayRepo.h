#pragma once

#include "../DBCore/DbResult.h"
#include "../DBCore/DbRetryPolicy.h"
#include <cstdint>
#include <future>
#include <optional>
#include <string>

namespace simcore::db {

struct VisualReplayRow {
    int64_t visual_replay_id{};
    int64_t job_id{};
    int64_t requested_at{};
    std::string state;
    std::optional<int64_t> worker_id{};
    std::optional<int64_t> started_at{};
    std::optional<int64_t> ended_at{};
    std::optional<std::string> error_text{};
};

class VisualReplayRepo {
public:
    static std::future<DbResult<int64_t>> EnqueueAsync(int64_t job_id, RetryPolicy rp = {});
    static std::future<DbResult<std::optional<VisualReplayRow>>> ClaimNextQueuedAsync(int64_t worker_id, RetryPolicy rp = {});
    static std::future<DbResult<VisualReplayRow>> GetAsync(int64_t visual_replay_id, RetryPolicy rp = {});
    static std::future<DbResult<void>> MarkRunningAsync(int64_t visual_replay_id, int64_t worker_id, RetryPolicy rp = {});
    static std::future<DbResult<void>> MarkSucceededAsync(int64_t visual_replay_id, RetryPolicy rp = {});
    static std::future<DbResult<void>> MarkFailedAsync(int64_t visual_replay_id, const std::string& error_text, RetryPolicy rp = {});

    static inline DbResult<int64_t> Enqueue(int64_t job_id) { return EnqueueAsync(job_id).get(); }
    static inline DbResult<std::optional<VisualReplayRow>> ClaimNextQueued(int64_t worker_id) { return ClaimNextQueuedAsync(worker_id).get(); }
    static inline DbResult<VisualReplayRow> Get(int64_t visual_replay_id) { return GetAsync(visual_replay_id).get(); }
    static inline DbResult<void> MarkRunning(int64_t visual_replay_id, int64_t worker_id) { return MarkRunningAsync(visual_replay_id, worker_id).get(); }
    static inline DbResult<void> MarkSucceeded(int64_t visual_replay_id) { return MarkSucceededAsync(visual_replay_id).get(); }
    static inline DbResult<void> MarkFailed(int64_t visual_replay_id, const std::string& error_text) { return MarkFailedAsync(visual_replay_id, error_text).get(); }
};

} // namespace simcore::db
