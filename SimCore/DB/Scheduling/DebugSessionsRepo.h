#pragma once

#include "../DBCore/DbResult.h"
#include "../DBCore/DbRetryPolicy.h"
#include "../DBCore/DbService.h"
#include <future>
#include <optional>
#include <string>
#include <vector>

namespace simcore::db {

    struct DebugSessionRow {
        int64_t id{};
        int64_t job_id{};
        std::string state;
        int64_t created_at{};
        int64_t updated_at{};
        std::optional<std::string> started_by;
        std::optional<int64_t> worker_id;
        std::optional<int64_t> slot_id;
        std::optional<std::string> session_token;
        std::optional<std::string> vm_endpoint;
        std::optional<std::string> dolphin_endpoint;
        std::optional<int64_t> lock_acquired_at;
        std::optional<int64_t> lock_released_at;
        std::optional<std::string> failure_code;
        std::optional<std::string> failure_detail;
    };

    struct StartDebugAdmissionResult {
        int64_t request_id{};
        std::string initial_status; // QueuedStartup or rejection code
    };

    class DebugSessionsRepo {
    public:
        static bool IsActiveState(const std::string& state);

        static std::future<DbResult<StartDebugAdmissionResult>> StartDebugAsync(int64_t job_id, std::string started_by, int64_t slot_id = 0, RetryPolicy rp = {});
        static std::future<DbResult<void>> MarkLaunchingAsync(int64_t session_id, RetryPolicy rp = {});
        static std::future<DbResult<void>> MarkAttachReadyAsync(int64_t session_id, std::optional<int64_t> worker_id, std::string token, std::string vm_endpoint, std::string dolphin_endpoint, RetryPolicy rp = {});
        static std::future<DbResult<void>> MarkActiveAsync(int64_t session_id, RetryPolicy rp = {});
        static std::future<DbResult<void>> MarkStoppedAsync(int64_t session_id, RetryPolicy rp = {});
        static std::future<DbResult<void>> MarkFailedAsync(int64_t session_id, std::string code, std::string detail, RetryPolicy rp = {});
        static std::future<DbResult<int>> CleanupOrphanedActiveSessionsAsync(int64_t max_age_seconds, RetryPolicy rp = {});
        static std::future<DbResult<std::optional<DebugSessionRow>>> GetByIdAsync(int64_t session_id, RetryPolicy rp = {});
        static std::future<DbResult<std::optional<DebugSessionRow>>> GetActiveByJobIdAsync(int64_t job_id, RetryPolicy rp = {});
        static std::future<DbResult<std::vector<DebugSessionRow>>> ListRecentAsync(int limit = 50, RetryPolicy rp = {});

        static inline DbResult<StartDebugAdmissionResult> StartDebug(int64_t job_id, std::string started_by, int64_t slot_id = 0) { return StartDebugAsync(job_id, std::move(started_by), slot_id).get(); }
        static inline DbResult<void> MarkLaunching(int64_t session_id) { return MarkLaunchingAsync(session_id).get(); }
        static inline DbResult<void> MarkAttachReady(int64_t session_id, std::optional<int64_t> worker_id, std::string token, std::string vm_endpoint, std::string dolphin_endpoint) {
            return MarkAttachReadyAsync(session_id, worker_id, std::move(token), std::move(vm_endpoint), std::move(dolphin_endpoint)).get();
        }
        static inline DbResult<void> MarkActive(int64_t session_id) { return MarkActiveAsync(session_id).get(); }
        static inline DbResult<void> MarkStopped(int64_t session_id) { return MarkStoppedAsync(session_id).get(); }
        static inline DbResult<void> MarkFailed(int64_t session_id, std::string code, std::string detail) { return MarkFailedAsync(session_id, std::move(code), std::move(detail)).get(); }
        static inline DbResult<int> CleanupOrphanedActiveSessions(int64_t max_age_seconds) { return CleanupOrphanedActiveSessionsAsync(max_age_seconds).get(); }
        static inline DbResult<std::optional<DebugSessionRow>> GetById(int64_t session_id) { return GetByIdAsync(session_id).get(); }
        static inline DbResult<std::optional<DebugSessionRow>> GetActiveByJobId(int64_t job_id) { return GetActiveByJobIdAsync(job_id).get(); }
        static inline DbResult<std::vector<DebugSessionRow>> ListRecent(int limit = 50) { return ListRecentAsync(limit).get(); }
    };
}
