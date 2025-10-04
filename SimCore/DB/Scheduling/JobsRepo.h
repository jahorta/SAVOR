// SimCore/DB/JobsRepo.h
#pragma once
#include "../DBCore/DbResult.h"
#include "../DBCore/DbRetryPolicy.h"
#include "../DBCore/DbService.h"
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
        std::string fingerprint;
        int priority{};
        std::string state; // 'QUEUED'...
        int attempts{};
        int max_attempts{};
        std::optional<std::string> claimed_by_token{};
        std::optional<int64_t> lease_expires_at{};
        int64_t queued_at{};
        std::optional<std::string> vm_kv{};
    };

    class JobsRepo {
    public:

        // Async methods
        static std::future<DbResult<int64_t>> CreateOrGetByFingerprintAsync(
            int64_t job_set_id, int program_kind, int program_version,
            int64_t program_ref_id, std::string fingerprint, int priority,
            std::optional<std::string> vm_kv = std::nullopt, RetryPolicy rp = {});
        static std::future<DbResult<JobRow>> GetAsync(int64_t job_id, RetryPolicy rp = {});
        static std::future<DbResult<void>> SetStateAsync(int64_t job_id, std::string new_state, RetryPolicy rp = {});
        static std::future<DbResult<void>> SetVmKvAsync(int64_t job_id, std::optional<std::string> vm_kv, RetryPolicy rp = {});
        static std::future<DbResult<std::optional<JobRow>>> ClaimNextReadyAsync(std::string claim_token, int lease_seconds, double aging_factor, RetryPolicy rp = {});
        static std::future<DbResult<void>> MarkRunningAsync(int64_t job_id, RetryPolicy rp = {});
        static std::future<DbResult<void>> RenewLeaseAsync(int64_t job_id, int lease_seconds, RetryPolicy rp = {});
        static std::future<DbResult<void>> RequeueExpiredLeasesAsync(RetryPolicy rp = {});
        static std::future<DbResult<std::vector<JobRow>>> GetByJobSetAsync(int64_t job_set_id, RetryPolicy rp = {});
        static std::future<DbResult<std::vector<JobRow>>> GetQueuedByJobSetAsync(int64_t job_set_id, RetryPolicy rp = {});


        // Blocking methods
        static inline DbResult<int64_t> CreateOrGetByFingerprint(
            int64_t job_set_id, int program_kind, int program_version,
            int64_t program_ref_id, std::string fingerprint, int priority,
            std::optional<std::string> vm_kv = std::nullopt) {
            return CreateOrGetByFingerprintAsync(job_set_id, program_kind, program_version, program_ref_id, std::move(fingerprint), priority, std::move(vm_kv)).get();
        }
        static inline DbResult<JobRow> Get(int64_t job_id) { return GetAsync(job_id).get(); }
        static inline DbResult<void> SetState(int64_t job_id, std::string new_state) { return SetStateAsync(job_id, std::move(new_state)).get(); }
        static inline DbResult<void> SetVmKv(int64_t job_id, std::optional<std::string> vm_kv) { return SetVmKvAsync(job_id, std::move(vm_kv)).get(); }

        static inline DbResult<std::optional<JobRow>> ClaimNextReady(std::string claim_token, int lease_seconds, double aging_factor) {
            return ClaimNextReadyAsync(std::move(claim_token), lease_seconds, aging_factor).get();
        }
        static inline DbResult<void> MarkRunning(int64_t job_id) { return MarkRunningAsync(job_id).get(); }
        static inline DbResult<void> RenewLease(int64_t job_id, int lease_seconds) { return RenewLeaseAsync(job_id, lease_seconds).get(); }
        static inline DbResult<void> RequeueExpiredLeases() { return RequeueExpiredLeasesAsync().get(); }
        static inline DbResult<std::vector<JobRow>> GetByJobSet(int64_t job_set_id) {
            return GetByJobSetAsync(job_set_id).get();
        }
        static inline DbResult<std::vector<JobRow>> GetQueuedByJobSet(int64_t job_set_id) {
            return GetQueuedByJobSetAsync(job_set_id).get();
        }
    };

    

} // namespace simcore::db
