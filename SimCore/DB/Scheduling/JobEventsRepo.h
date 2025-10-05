// SimCore/DB/JobEventsRepo.h
#pragma once
#include "../DBCore/DbResult.h"
#include "../DBCore/DbRetryPolicy.h"
#include "../DBCore/DbService.h"
#include "../Querying/Paging.h"
#include "../Querying/JobEventListDTO.h"
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <future>

namespace simcore::db {

    struct JobEventRow {
        int64_t event_id{};
        int64_t job_id{};
        int64_t ts{};
        std::string event_kind; // 'ENQUEUED','PROGRESS','RESULTS',...
        std::optional<std::string> payload{};
    };

    class JobEventsRepo {
    public:
        // Bulk latest payload by jobs for a given event kind (e.g., "PROGRESS")
        struct JobIdPayload { int64_t job_id; std::optional<std::string> payload; };

        static std::future<DbResult<int64_t>> AppendAsync(int64_t job_id, std::string kind, std::optional<std::string> payload = std::nullopt, RetryPolicy rp = {});
        static std::future<DbResult<std::optional<std::string>>> GetFirstPayloadAsync(int64_t job_id, std::string kind, RetryPolicy rp = {});
        static std::future<DbResult<std::optional<std::string>>> GetLatestPayloadAsync(int64_t job_id, std::string kind, RetryPolicy rp = {});
        static std::future<DbResult<std::vector<JobEventRow>>>   ListByJobSetAndKindAsync(int64_t job_set_id, std::string kind, RetryPolicy rp = {});
        static std::future<DbResult<std::vector<JobEventRow>>>   ListByJobAndKindAsync(int64_t job_id, std::string kind, RetryPolicy rp = {});
        static std::future<DbResult<Page<JobEventLite>>> ListPagedByTimeAsync(
            const JobEventsListScope& scope,
            std::optional<KeysetCursor> before, // (ts,event_id) DESC
            int limit,
            RetryPolicy rp = {}
        );
        static std::future<DbResult<std::vector<JobIdPayload>>> GetLatestPayloadByJobsAsync(
            const std::vector<int64_t>& job_ids, std::string kind, RetryPolicy rp = {});

        static inline DbResult<int64_t> Append(int64_t job_id, std::string kind, std::optional<std::string> payload = std::nullopt) {
            return AppendAsync(job_id, std::move(kind), std::move(payload)).get();
        }
        static inline DbResult<std::optional<std::string>> GetFirstPayload(int64_t job_id, std::string kind) {
            return GetFirstPayloadAsync(job_id, std::move(kind)).get();
        }
        static inline DbResult<std::optional<std::string>> GetLatestPayload(int64_t job_id, std::string kind) {
            return GetLatestPayloadAsync(job_id, std::move(kind)).get();
        }
        static inline DbResult<std::vector<JobEventRow>> ListByJobSetAndKind(int64_t job_set_id, std::string kind) {
            return ListByJobSetAndKindAsync(job_set_id, std::move(kind)).get();
        }
        static inline DbResult<std::vector<JobEventRow>> ListByJobAndKind(int64_t job_id, std::string kind) {
            return ListByJobAndKindAsync(job_id, std::move(kind)).get();
        }
        static inline DbResult<std::vector<JobIdPayload>> GetLatestPayloadByJobs(
            const std::vector<int64_t>& job_ids, std::string kind) {
            return GetLatestPayloadByJobsAsync(job_ids, std::move(kind)).get();
        }
    };

} // namespace simcore::db
