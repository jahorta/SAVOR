#include "DataService.h"
#include "../ProgramKindsRepo.h"
#include <thread>

namespace simcore::db {

    static inline DbResult<void> invalid_arg(const char* msg) {
        return DbResult<void>::Err(DbError{ DbErrorKind::InvalidArgument, 0, msg });
    }

    static inline DbResult<Page<JobLite>> invalid_arg_jobs(const char* msg) {
        return DbResult<Page<JobLite>>::Err(DbError{ DbErrorKind::InvalidArgument, 0, msg });
    }
    static inline DbResult<Page<JobEventLite>> invalid_arg_events(const char* msg) {
        return DbResult<Page<JobEventLite>>::Err(DbError{ DbErrorKind::InvalidArgument, 0, msg });
    }
    static inline DbResult<Page<JobSetLite>> invalid_arg_sets(const char* msg) {
        return DbResult<Page<JobSetLite>>::Err(DbError{ DbErrorKind::InvalidArgument, 0, msg });
    }

    std::future<DbResult<Page<JobLite>>> DataService::FetchJobsPage(
        const JobsListScope& scope, const PagedQuery<>& q, RetryPolicy rp) {
        if (q.order != PageOrder::Desc) {
            std::promise<DbResult<Page<JobLite>>> p;
            p.set_value(DbResult<Page<JobLite>>::Err({ DbErrorKind::InvalidArgument, 0, "only DESC supported" }));
            return p.get_future();
        }
        if (q.after.has_value()) {
            return JobsRepo::ListRecentAfterAsync(scope, q.after, q.limit, rp);
        }
        return JobsRepo::ListRecentAsync(scope, q.before, q.limit, rp);
    }

    std::future<DbResult<Page<JobLite>>> DataService::FetchJobsPageAsync(
        const JobsListScope& scope,
        std::optional<KeysetCursor> before,
        std::optional<KeysetCursor> after,
        int limit,
        RetryPolicy rp)
    {
        PagedQuery<> q;
        q.order = PageOrder::Desc;
        q.before = std::move(before);
        q.after = std::move(after);
        q.limit = limit;
        return FetchJobsPage(scope, q, rp);
    }

    std::future<DbResult<Page<JobEventLite>>> DataService::FetchJobEventsPage(const JobEventsListScope& scope, const PagedQuery<>& q, RetryPolicy rp) {
        if (q.order != PageOrder::Desc) {
            std::promise<DbResult<Page<JobEventLite>>> p; p.set_value(invalid_arg_events("only DESC supported")); return p.get_future();
        }
        if (q.after.has_value()) {
            std::promise<DbResult<Page<JobEventLite>>> p; p.set_value(invalid_arg_events("after not supported by repos yet")); return p.get_future();
        }
        return JobEventsRepo::ListPagedByTimeAsync(scope, q.before, q.limit, rp);
    }

    std::future<DbResult<Page<JobSetLite>>> DataService::FetchJobSetsPage(const JobSetsListScope& scope, const PagedQuery<>& q, RetryPolicy rp) {
        if (q.order != PageOrder::Desc) {
            std::promise<DbResult<Page<JobSetLite>>> p; p.set_value(invalid_arg_sets("only DESC supported")); return p.get_future();
        }
        if (q.after.has_value()) {
            std::promise<DbResult<Page<JobSetLite>>> p; p.set_value(invalid_arg_sets("after not supported by repos yet")); return p.get_future();
        }
        return JobSetsRepo::ListRecentAsync(scope, q.before, q.limit, rp);
    }

    class DataService::JobsPoller : public DataService::PollHandle {
    public:
        JobsPoller(JobsListScope scope, std::chrono::milliseconds interval, int limit, SnapshotMailbox<Page<JobLite>>& out, RetryPolicy rp)
            : m_scope(std::move(scope)), m_interval(interval), m_limit(limit), m_out(out), m_rp(rp)
        {
            m_thread = std::thread([this]() { run(); });
        }

        ~JobsPoller() override {
            stop();
            if (m_thread.joinable()) m_thread.join();
        }

        void stop() override {
            bool expected = true;
            if (m_running.compare_exchange_strong(expected, false)) {
                m_cv.notify_all();
            }
        }

        void nudge() override {
            m_dirty.store(true, std::memory_order_relaxed);
            m_cv.notify_all();
        }

    private:
        void run() {
            std::unique_lock<std::mutex> lk(m_mtx);
            while (m_running.load()) {
                lk.unlock();
                auto fut = JobsRepo::ListRecentAsync(m_scope, std::nullopt, m_limit, m_rp);
                auto res = fut.get();
                if (res.ok) {
                    m_out.push(std::move(res.value));
                }
                lk.lock();

                if (!m_running.load()) break;

                if (m_dirty.exchange(false)) {
                    continue;
                }
                m_cv.wait_for(lk, m_interval);
            }
        }

        JobsListScope m_scope;
        std::chrono::milliseconds m_interval;
        int m_limit;
        SnapshotMailbox<Page<JobLite>>& m_out;
        RetryPolicy m_rp;

        std::atomic<bool> m_running{ true };
        std::atomic<bool> m_dirty{ false };
        std::thread m_thread;
        std::mutex m_mtx;
        std::condition_variable m_cv;
    };

    class DataService::JobEventsPoller : public DataService::PollHandle {
    public:
        JobEventsPoller(JobEventsListScope scope,
            std::chrono::milliseconds interval,
            int limit,
            SnapshotMailbox<Page<JobEventLite>>& out,
            RetryPolicy rp)
            : m_scope(std::move(scope))
            , m_interval(interval)
            , m_limit(limit)
            , m_out(out)
            , m_rp(rp)
        {
            m_thread = std::thread([this]() { run(); });
        }

        ~JobEventsPoller() override {
            stop();
            if (m_thread.joinable()) m_thread.join();
        }

        void stop() override {
            bool expected = true;
            if (m_running.compare_exchange_strong(expected, false)) {
                m_cv.notify_all();
            }
        }

        void nudge() override {
            m_dirty.store(true, std::memory_order_relaxed);
            m_cv.notify_all();
        }

    private:
        void run() {
            std::unique_lock<std::mutex> lk(m_mtx);
            while (m_running.load()) {
                lk.unlock();
                auto fut = JobEventsRepo::ListPagedByTimeAsync(m_scope, std::nullopt, m_limit, m_rp);
                auto res = fut.get();
                if (res.ok) {
                    m_out.push(std::move(res.value));
                }
                lk.lock();

                if (!m_running.load()) break;

                if (m_dirty.exchange(false)) {
                    continue; // coalesce rapid nudges
                }
                m_cv.wait_for(lk, m_interval);
            }
        }

        JobEventsListScope m_scope;
        std::chrono::milliseconds m_interval;
        int m_limit;
        SnapshotMailbox<Page<JobEventLite>>& m_out;
        RetryPolicy m_rp;

        std::atomic<bool> m_running{ true };
        std::atomic<bool> m_dirty{ false };
        std::thread m_thread;
        std::mutex m_mtx;
        std::condition_variable m_cv;
    };

    class DataService::JobSetsPoller : public DataService::PollHandle {
    public:
        JobSetsPoller(JobSetsListScope scope,
            std::chrono::milliseconds interval,
            int limit,
            SnapshotMailbox<Page<JobSetLite>>& out,
            RetryPolicy rp)
            : m_scope(std::move(scope))
            , m_interval(interval)
            , m_limit(limit)
            , m_out(out)
            , m_rp(rp)
        {
            m_thread = std::thread([this]() { run(); });
        }

        ~JobSetsPoller() override {
            stop();
            if (m_thread.joinable()) m_thread.join();
        }

        void stop() override {
            bool expected = true;
            if (m_running.compare_exchange_strong(expected, false)) {
                m_cv.notify_all();
            }
        }

        void nudge() override {
            m_dirty.store(true, std::memory_order_relaxed);
            m_cv.notify_all();
        }

    private:
        void run() {
            std::unique_lock<std::mutex> lk(m_mtx);
            while (m_running.load()) {
                lk.unlock();
                auto fut = JobSetsRepo::ListRecentAsync(m_scope, std::nullopt, m_limit, m_rp);
                auto res = fut.get();
                if (res.ok) {
                    m_out.push(std::move(res.value));
                }
                lk.lock();

                if (!m_running.load()) break;

                if (m_dirty.exchange(false)) {
                    continue; // coalesce rapid nudges
                }
                m_cv.wait_for(lk, m_interval);
            }
        }

        JobSetsListScope m_scope;
        std::chrono::milliseconds m_interval;
        int m_limit;
        SnapshotMailbox<Page<JobSetLite>>& m_out;
        RetryPolicy m_rp;

        std::atomic<bool> m_running{ true };
        std::atomic<bool> m_dirty{ false };
        std::thread m_thread;
        std::mutex m_mtx;
        std::condition_variable m_cv;
    };

    std::unique_ptr<DataService::PollHandle> DataService::StartJobsPolling(
        JobsListScope scope,
        std::chrono::milliseconds interval,
        int page_limit,
        SnapshotMailbox<Page<JobLite>>& out,
        RetryPolicy rp)
    {
        return std::make_unique<JobsPoller>(std::move(scope), interval, page_limit, out, rp);
    }

    std::unique_ptr<DataService::PollHandle> DataService::StartJobEventsPolling(
        JobEventsListScope scope,
        std::chrono::milliseconds interval,
        int page_limit,
        SnapshotMailbox<Page<JobEventLite>>& out,
        RetryPolicy rp)
    {
        return std::make_unique<JobEventsPoller>(std::move(scope), interval, page_limit, out, rp);
    }

    std::unique_ptr<DataService::PollHandle> DataService::StartJobSetsPolling(
        JobSetsListScope scope,
        std::chrono::milliseconds interval,
        int page_limit,
        SnapshotMailbox<Page<JobSetLite>>& out,
        RetryPolicy rp)
    {
        return std::make_unique<JobSetsPoller>(std::move(scope), interval, page_limit, out, rp);
    }

    std::future<DbResult<std::vector<JobEventsRepo::JobIdPayload>>> DataService::BulkLatestProgressByJobsAsync(
        const std::vector<int64_t>& job_ids, RetryPolicy rp) {
        return JobEventsRepo::GetLatestPayloadByJobsAsync(job_ids, "PROGRESS", rp);
    }

    std::future<DbResult<std::vector<ProgramKindKV>>> DataService::ListProgramKindsAsync(RetryPolicy rp) {
        // Fetch on DB thread pool first:
        auto fut_rows = ProgramKindsRepo::ListAllAsync(rp);

        // Bridge to a UI-friendly DTO on a tiny continuation thread (non-blocking for UI):
        std::promise<DbResult<std::vector<ProgramKindKV>>> p;
        auto fut = p.get_future();
        std::thread([f = std::move(fut_rows), p = std::move(p)]() mutable {
            auto r = f.get();
            if (!r.ok) {
                p.set_value(DbResult<std::vector<ProgramKindKV>>::Err(r.error));
                return;
            }
            std::vector<ProgramKindKV> v;
            v.reserve(r.value.size());
            for (auto& row : r.value) v.push_back({ row.id, row.name });
            p.set_value(DbResult<std::vector<ProgramKindKV>>::Ok(std::move(v)));
            }).detach();

        return fut;
    }

} // namespace simcore::db
