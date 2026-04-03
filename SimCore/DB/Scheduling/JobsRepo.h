// SimCore/DB/JobsRepo.h
#pragma once
#include "../DBCore/DbResult.h"
#include "../DBCore/DbRetryPolicy.h"
#include "../DBCore/DbService.h"
#include "../Querying/Paging.h"
#include "../Querying/JobListDTO.h"
#include <string>
#include <optional>
#include <cstdint>
#include <future>
#include <vector>

namespace simcore::db {

    struct JobRow {
        int64_t job_id{};
        int64_t job_set_id{};
        int program_kind{};
        int program_version{};
        int64_t program_ref_id{};
        std::optional<int64_t> savestate_id{};
        std::string fingerprint;
        int priority{};
        std::string state; // 'QUEUED', 'INTERRUPTED', 'CLAIMED', 'RUNNING', 'SUCCEEDED', 'FAILED', 'CANCELED', 'SUPERSEDED', 'SUCCEEDED_WINNER', 'SUCCEEDED_DUPLICATE'
        int attempts{};
        int max_attempts{};
        std::optional<std::string> claimed_by_token{};
        std::optional<int64_t> lease_expires_at{};
        int64_t queued_at{};
        std::optional<std::string> vm_kv{};
        std::optional<int64_t> parent_job_id{};
    };

    struct JobPriorityBoostResult {
        int new_priority{};
        std::vector<int64_t> changed_job_ids;
    };

    struct JobSetCancelQueuedResult {
        std::vector<int64_t> canceled_job_ids;
    };

    class JobsRepo {
    public:

        // Async methods
        static std::future<DbResult<int64_t>> CreateOrGetByFingerprintAsync(
            int64_t job_set_id, int program_kind, int program_version,
            int64_t program_ref_id, std::string fingerprint, int priority,
            std::optional<std::string> vm_kv = std::nullopt, 
            std::optional<int64_t> savestate_id = std::nullopt,
            std::optional<int64_t> parent_job_id = std::nullopt,
            RetryPolicy rp = {});
        static std::future<DbResult<JobRow>> GetAsync(int64_t job_id, RetryPolicy rp = {});
        static std::future<DbResult<void>> SetStateAsync(int64_t job_id, std::string new_state, RetryPolicy rp = {});
        static std::future<DbResult<void>> SetVmKvAsync(int64_t job_id, std::optional<std::string> vm_kv, RetryPolicy rp = {});
        static std::future<DbResult<void>> SetProgramRefIdAsync(int64_t job_id, int64_t program_ref_id, RetryPolicy rp = {});
        static std::future<DbResult<std::optional<JobRow>>> ClaimNextReadyAsync(
            std::string claim_token, int lease_seconds, double aging_factor,
            std::optional<int64_t> savestate_id = std::nullopt, RetryPolicy rp = {});
        static std::future<DbResult<void>> MarkRunningAsync(int64_t job_id, RetryPolicy rp = {});
        static std::future<DbResult<void>> RenewLeaseAsync(int64_t job_id, int lease_seconds, RetryPolicy rp = {});
        static std::future<DbResult<void>> RequeueExpiredLeasesAsync(RetryPolicy rp = {});
        static std::future<DbResult<std::vector<int64_t>>> InterruptInFlightAsync(RetryPolicy rp = {});
        static std::future<DbResult<std::vector<JobRow>>> GetByJobSetAsync(int64_t job_set_id, RetryPolicy rp = {});
        static std::future<DbResult<std::vector<JobRow>>> GetQueuedByJobSetAsync(int64_t job_set_id, RetryPolicy rp = {});
        static std::future<DbResult<Page<JobLite>>> ListRecentAsync(
            const JobsListScope& scope,
            std::optional<KeysetCursor> before, // keyset for DESC order
            int limit,
            RetryPolicy rp = {}
        );
        static std::future<DbResult<Page<JobLite>>> ListRecentAfterAsync(
            const JobsListScope& scope,
            std::optional<KeysetCursor> after,
            int limit,
            RetryPolicy rp = {});
        static std::future<DbResult<void>> RequeueAsync(int64_t job_id, RetryPolicy rp = {});
        static std::future<DbResult<void>> RestartFailedAsync(int64_t job_id, RetryPolicy rp = {});
        static std::future<DbResult<void>> CancelIfNotRunningAsync(int64_t job_id, RetryPolicy rp = {});
        static std::future<DbResult<void>> BumpPriorityAsync(int64_t job_id, int delta, RetryPolicy rp = {});
        static std::future<DbResult<JobPriorityBoostResult>> BoostPriorityForJobSetTreeAsync(int64_t root_job_set_id, RetryPolicy rp = {});
        static std::future<DbResult<JobSetCancelQueuedResult>> CancelQueuedForJobSetTreeAsync(int64_t root_job_set_id, RetryPolicy rp = {});


        // Blocking methods
        static inline DbResult<int64_t> CreateOrGetByFingerprint(
            int64_t job_set_id, int program_kind, int program_version,
            int64_t program_ref_id, std::string fingerprint, int priority,
            std::optional<std::string> vm_kv = std::nullopt,
            std::optional<int64_t> savestate_id = std::nullopt,
            std::optional<int64_t> parent_job_id = std::nullopt) {
            return CreateOrGetByFingerprintAsync(job_set_id, program_kind, program_version, program_ref_id, std::move(fingerprint), priority, std::move(vm_kv), std::move(savestate_id), std::move(parent_job_id)).get();
        }
        static inline DbResult<JobRow> Get(int64_t job_id) { return GetAsync(job_id).get(); }
        static inline DbResult<void> SetState(int64_t job_id, std::string new_state) { return SetStateAsync(job_id, std::move(new_state)).get(); }
        static inline DbResult<void> SetVmKv(int64_t job_id, std::optional<std::string> vm_kv) { return SetVmKvAsync(job_id, std::move(vm_kv)).get(); }
        static inline DbResult<void> SetProgramRefId(int64_t job_id, int64_t program_ref_id) { return SetProgramRefIdAsync(job_id, program_ref_id).get(); }

        static inline DbResult<std::optional<JobRow>> ClaimNextReady(
            std::string claim_token, int lease_seconds, double aging_factor,
            std::optional<int64_t> savestate_id = std::nullopt) {
            return ClaimNextReadyAsync(std::move(claim_token), lease_seconds, aging_factor, savestate_id).get();
        }
        static inline DbResult<void> MarkRunning(int64_t job_id) { return MarkRunningAsync(job_id).get(); }
        static inline DbResult<void> RenewLease(int64_t job_id, int lease_seconds) { return RenewLeaseAsync(job_id, lease_seconds).get(); }
        static inline DbResult<void> RequeueExpiredLeases() { return RequeueExpiredLeasesAsync().get(); }
        static inline DbResult<std::vector<int64_t>> InterruptInFlight() { return InterruptInFlightAsync().get(); }
        static inline DbResult<std::vector<JobRow>> GetByJobSet(int64_t job_set_id) {
            return GetByJobSetAsync(job_set_id).get();
        }
        static inline DbResult<std::vector<JobRow>> GetQueuedByJobSet(int64_t job_set_id) {
            return GetQueuedByJobSetAsync(job_set_id).get();
        }
        static inline DbResult<Page<JobLite>> ListRecentAfter(
            const JobsListScope& scope,
            std::optional<KeysetCursor> after,
            int limit,
            RetryPolicy rp = {}) {
            return ListRecentAfterAsync(scope, after, limit, rp).get();
        }
        static inline DbResult<void> Requeue(int64_t job_id) { return RequeueAsync(job_id).get(); }
        static inline DbResult<void> RestartFailed(int64_t job_id) { return RestartFailedAsync(job_id).get(); }
        static inline DbResult<void> CancelIfNotRunning(int64_t job_id) { return CancelIfNotRunningAsync(job_id).get(); }
        static inline DbResult<void> BumpPriority(int64_t job_id, int delta) { return BumpPriorityAsync(job_id, delta).get(); }
        static inline DbResult<JobPriorityBoostResult> BoostPriorityForJobSetTree(int64_t root_job_set_id) {
            return BoostPriorityForJobSetTreeAsync(root_job_set_id).get();
        }
        static inline DbResult<JobSetCancelQueuedResult> CancelQueuedForJobSetTree(int64_t root_job_set_id) {
            return CancelQueuedForJobSetTreeAsync(root_job_set_id).get();
        }
    };

    

} // namespace simcore::db
