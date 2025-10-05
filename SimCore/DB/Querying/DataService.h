#pragma once
#include "../DBCore/DbResult.h"
#include "../DBCore/DbRetryPolicy.h"
#include "Paging.h"
#include "PagedQuery.h"
#include "JobListDTO.h"
#include "JobEventListDTO.h"
#include "JobSetListDTO.h"
#include "../Scheduling/JobEventsRepo.h"
#include "../Scheduling/JobsRepo.h"
#include "../Scheduling/JobSetsRepo.h"
#include "SnapshotMailbox.h"
#include <future>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>

namespace simcore::db {

    struct ProgramKindKV { int id; std::string name; };

    class DataService {
    public:
        static std::future<DbResult<Page<JobLite>>>      FetchJobsPage(const JobsListScope& scope, const PagedQuery<>& q, RetryPolicy rp = {});
        static std::future<DbResult<Page<JobEventLite>>> FetchJobEventsPage(const JobEventsListScope& scope, const PagedQuery<>& q, RetryPolicy rp = {});
        static std::future<DbResult<Page<JobSetLite>>>   FetchJobSetsPage(const JobSetsListScope& scope, const PagedQuery<>& q, RetryPolicy rp = {});

        class PollHandle {
        public:
            virtual ~PollHandle() = default;
            virtual void stop() = 0;
            virtual void nudge() = 0;
        };

        static std::unique_ptr<PollHandle> StartJobsPolling(
            JobsListScope scope,
            std::chrono::milliseconds interval,
            int page_limit,
            SnapshotMailbox<Page<JobLite>>& out,
            RetryPolicy rp = {}
        );
        static std::unique_ptr<PollHandle> StartJobEventsPolling(
            JobEventsListScope scope,
            std::chrono::milliseconds interval,
            int page_limit,
            SnapshotMailbox<Page<JobEventLite>>& out,
            RetryPolicy rp = {}
        );

        static std::unique_ptr<PollHandle> StartJobSetsPolling(
            JobSetsListScope scope,
            std::chrono::milliseconds interval,
            int page_limit,
            SnapshotMailbox<Page<JobSetLite>>& out,
            RetryPolicy rp = {}
        );
        static std::future<DbResult<Page<JobLite>>> FetchJobsPageAsync(
            const JobsListScope& scope,
            std::optional<KeysetCursor> before,
            std::optional<KeysetCursor> after,
            int limit,
            RetryPolicy rp = {}
        );
        static std::future<DbResult<std::vector<JobEventsRepo::JobIdPayload>>> BulkLatestProgressByJobsAsync(
            const std::vector<int64_t>& job_ids, RetryPolicy rp = {});


        
        static std::future<DbResult<std::vector<ProgramKindKV>>> ListProgramKindsAsync(RetryPolicy rp = {});

    private:
        class JobsPoller;
        class JobSetsPoller;
        class JobEventsPoller;
    };

} // namespace simcore::db
