#include "DataService.h"
#include "../SavestateRepo.h"
#include "../SeedProbeRepo.h"
#include "../TasMovieRepo.h"
#include "../ExplorerSettingsRepo.h"
#include "../DBCore/ObjectStore.h"
#include "../ProgramKindsRepo.h"
#include "../../Runner/IPC/Wire.h"
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

    std::future<DbResult<IniDoc>> DataService::FetchJobVmKvIniAsync(int64_t job_id, RetryPolicy rp) {
        std::promise<DbResult<IniDoc>> p;
        auto fut = p.get_future();
        std::thread([job_id, rp, pr = std::move(p)]() mutable {
            auto jr = JobsRepo::GetAsync(job_id).get();
            if (!jr.ok) { pr.set_value(DbResult<IniDoc>::Err(jr.error)); return; }
            IniDoc doc;
            if (jr.value.vm_kv.has_value() && !jr.value.vm_kv->empty()) {
                try { doc = IniDoc::parse(*jr.value.vm_kv); }
                catch (...) { pr.set_value(DbResult<IniDoc>::Err({ DbErrorKind::InvalidData, 0, "vm_kv parse error" })); return; }
            }
            pr.set_value(DbResult<IniDoc>::Ok(std::move(doc)));
            }).detach();
        return fut;
    }

    

    std::future<DbResult<IniDoc>> DataService::FetchJobResultsIniAsync(int64_t job_id, RetryPolicy rp) {
        std::promise<DbResult<IniDoc>> p;
        auto fut = p.get_future();
        std::thread([job_id, rp, pr = std::move(p)]() mutable {
            auto jr = JobsRepo::GetAsync(job_id).get();
            if (!jr.ok) { pr.set_value(DbResult<IniDoc>::Err(jr.error)); return; }
            auto& codec = ProgramDBCodecRegistry::for_kind(jr.value.program_kind);
            auto r = codec.decode_results_from_db(job_id, std::optional<int64_t>{});
            if (!r.ok) { pr.set_value(DbResult<IniDoc>::Err(r.error)); return; }
            IniDoc doc;
            if (!r.value.empty()) {
                try { doc = IniDoc::parse(r.value); }
                catch (...) { pr.set_value(DbResult<IniDoc>::Err({ DbErrorKind::InvalidData, 0, "results ini parse error" })); return; }
            }
            pr.set_value(DbResult<IniDoc>::Ok(std::move(doc)));
            }).detach();
        return fut;
    }

    std::future<DbResult<std::string>> DataService::FetchDecodedProgressAsync(int64_t job_id, RetryPolicy rp) {
        std::promise<DbResult<std::string>> p;
        auto fut = p.get_future();
        std::thread([job_id, rp, pr = std::move(p)]() mutable {
            auto jr = JobsRepo::GetAsync(job_id).get();
            if (!jr.ok) { pr.set_value(DbResult<std::string>::Err(jr.error)); return; }
            auto& codec = ProgramDBCodecRegistry::for_kind(jr.value.program_kind);
            auto r = codec.decode_progress_from_db(job_id, std::optional<int64_t>{});
            if (!r.ok) { pr.set_value(DbResult<std::string>::Err(r.error)); return; }
            pr.set_value(DbResult<std::string>::Ok(r.value));
            }).detach();
        return fut;
    }

    std::future<DbResult<std::vector<ArtifactRefLite>>> DataService::FetchJobArtifactRefsAsync(int64_t job_id, RetryPolicy rp) {
        std::promise<DbResult<std::vector<ArtifactRefLite>>> p;
        auto fut = p.get_future();
        std::thread([job_id, rp, pr = std::move(p)]() mutable {
            auto jr = JobsRepo::GetAsync(job_id).get();
            if (!jr.ok) { pr.set_value(DbResult<std::vector<ArtifactRefLite>>::Err(jr.error)); return; }
            auto& codec = ProgramDBCodecRegistry::for_kind(jr.value.program_kind);
            auto r = codec.build_artifact_ini_from_db(job_id);
            if (!r.ok) { pr.set_value(DbResult<std::vector<ArtifactRefLite>>::Err(r.error)); return; }

            auto list = ArtifactIniBuilder::parse_from_ini(r.value);

            // Optional: enrich from ObjectStore metadata (best-effort)
            for (auto& a : list) {
                auto meta = ObjectStore::GetAsync(a.artifact_id).get();
                if (meta.ok) {
                    const auto& m = meta.value;
                    a.filename = m.filename;
                    a.size_bytes = static_cast<uint64_t>(m.size);
                    a.compression = static_cast<int>(m.compression);
                    //a.created_at = m.created_at;
                }
            }
            pr.set_value(DbResult<std::vector<ArtifactRefLite>>::Ok(std::move(list)));
            }).detach();
        return fut;
    }

    std::future<DbResult<void>> DataService::RequeueJobAsync(int64_t job_id, RetryPolicy rp) {
        std::promise<DbResult<void>> pr;
        auto fut = pr.get_future();
        std::thread([job_id, rp, p = std::move(pr)]() mutable {
            auto r = JobsRepo::RequeueAsync(job_id, rp).get();
            if (!r.ok) { p.set_value(DbResult<void>::Err(r.error)); return; }
            // Best-effort event; ignore errors (same behavior as before).
            (void)JobEventsRepo::AppendAsync(job_id, "REQUEUE", std::nullopt, rp).get();
            p.set_value(DbResult<void>::Ok());
            }).detach();
        return fut;
    }

    std::future<DbResult<void>> DataService::CancelJobAsync(int64_t job_id, RetryPolicy rp) {
        std::promise<DbResult<void>> pr;
        auto fut = pr.get_future();
        std::thread([job_id, rp, p = std::move(pr)]() mutable {
            auto r = JobsRepo::CancelIfNotRunningAsync(job_id, rp).get();
            if (!r.ok) { p.set_value(DbResult<void>::Err(r.error)); return; }
            (void)JobEventsRepo::AppendAsync(job_id, "CANCEL", std::nullopt, rp).get();
            p.set_value(DbResult<void>::Ok());
            }).detach();
        return fut;
    }

    std::future<DbResult<void>> DataService::BumpPriorityAsync(int64_t job_id, int delta, RetryPolicy rp) {
        std::promise<DbResult<void>> pr;
        auto fut = pr.get_future();
        std::thread([job_id, delta, rp, p = std::move(pr)]() mutable {
            auto r = JobsRepo::BumpPriorityAsync(job_id, delta, rp).get();
            if (!r.ok) { p.set_value(DbResult<void>::Err(r.error)); return; }
            // Include the +/-delta string as before.
            std::string payload = (delta > 0 ? "+" : "") + std::to_string(delta);
            (void)JobEventsRepo::AppendAsync(job_id, "PRIORITY_BUMP", payload, rp).get();
            p.set_value(DbResult<void>::Ok());
            }).detach();
        return fut;
    }

    std::future<DbResult<int64_t>> DataService::CreateJobSetAsync(
        std::optional<std::string> purpose,
        int program_kind,
        std::optional<std::string> created_by,
        std::optional<std::string> domain_ref_kind,
        std::optional<int64_t>    domain_ref_id,
        std::optional<std::string> meta_text,
        std::optional<int64_t>    expected_total,
        RetryPolicy rp)
    {
        return JobSetsRepo::CreateAsync(
            std::move(purpose), program_kind,
            std::move(created_by),
            std::move(domain_ref_kind),
            std::move(domain_ref_id),
            std::move(meta_text),
            std::move(expected_total),
            rp);
    }

    std::future<DbResult<int64_t>> DataService::EncodeJobSetWithCodecAsync(
        int program_kind,
        int64_t job_set_id,
        const std::string& ini_sorted,
        RetryPolicy /*rp*/)
    {
        return std::async(std::launch::async, [program_kind, job_set_id, ini = std::string(ini_sorted)]() mutable {
            simcore::db::codec::ensure_codecs_registered();
            auto& codec = ProgramDBCodecRegistry::for_kind(program_kind);
            return codec.encode_job_into_db(job_set_id, ini);
            });
    }

    std::future<DbResult<void>> DataService::SetJobSetExpectedTotalAsync(
        int64_t job_set_id,
        std::optional<int64_t> expected_total,
        RetryPolicy rp)
    {
        return JobSetsRepo::SetExpectedTotalAsync(job_set_id, std::move(expected_total), rp);
    }

    std::future<DbResult<Page<SavestateLite>>> DataService::FetchSavestatesPage(const PagedQuery<>& q, const std::string& search, RetryPolicy rp) {
        return SavestateRepo::ListPagedAsync(q, search, rp);
    }
    std::future<DbResult<Page<SeedProbeLite>>> DataService::FetchSeedProbesPage(const PagedQuery<>& q, const std::string& search, bool only_done, std::optional<int64_t> filter_savestate_id, RetryPolicy rp) {
        return SeedProbeRepo::ListPagedAsync(q, search, only_done, filter_savestate_id, rp);
    }
    std::future<DbResult<Page<TasMovieLite>>> DataService::FetchTasMoviesPage(const PagedQuery<>& q, const std::string& search, bool only_done, RetryPolicy rp) {
        return TasMovieRepo::ListPagedAsync(q, search, only_done, rp);
    }
    std::future<DbResult<Page<ExplorerSettingsLite>>> DataService::FetchExplorerSettingsPage(const PagedQuery<>& q, const std::string& search, RetryPolicy rp) {
        return ExplorerSettingsRepo::ListPagedAsync(q, search, rp);
    }
    std::future<DbResult<Page<ObjectRefLite>>> DataService::FetchObjectRefsPage(const PagedQuery<>& q, const std::string& search, const std::string& ext_filter, RetryPolicy rp) {
        return ObjectRefList::ListPagedAsync(q, search, ext_filter, rp);
    }

    std::future<DbResult<int64_t>> DataService::GetSavestateForSeedProbeAsync(int64_t seed_probe_id, RetryPolicy rp) {
        return std::async(std::launch::async, [seed_probe_id, rp]() -> DbResult<int64_t> {
            auto fr = SeedProbeRepo::GetAsync(seed_probe_id, rp).get();
            if (!fr.ok) return DbResult<int64_t>::Err(fr.error);
            if (fr.value.savestate_id <= 0)
                return DbResult<int64_t>::Err({ DbErrorKind::NotFound, 0, "seed_probe has no savestate_id" });
            return DbResult<int64_t>::Ok(fr.value.savestate_id);
            });
    }

    std::future<DbResult<std::optional<simcore::db::BattleContextRow>>> DataService::GetLatestBattleContextForSavestateAsync(int64_t savestate_id, RetryPolicy rp) {
        // Implement via a tiny repo helper or reuse List/ORDER BY created_at DESC LIMIT 1
        return BattleContextRepo::GetLatestBySavestateIdAsync(savestate_id);
        
    }

    std::future<DbResult<int64_t>> DataService::GetNewBattleContextAsync(const std::string& ini_string, RetryPolicy rp) {
        
        return std::async(std::launch::async, [ini_string, rp]() -> DbResult<int64_t> {
            auto js = JobSetsRepo::Create("New Context Probe", PK_BattleContextProbe, std::nullopt, std::nullopt, std::nullopt, ini_string, 1);
            if (!js.ok) return DbResult<int64_t>::Err(js.error);

            auto cpa = EncodeJobSetWithCodecAsync(PK_BattleContextProbe, js.value, ini_string);
            auto cp = cpa.get();
            if (!cp.ok) return DbResult<int64_t>::Err(cp.error);

            return DbResult<int64_t>::Ok(cp.value);
            });
    }

    std::future<DbResult<int64_t>> DataService::SaveAuthoringTemplateAsync(const simcore::db::AuthoringTemplateRow& r, bool is_update, RetryPolicy rp) {
        if (is_update) {
            return std::async(std::launch::async, [r, rp]() -> DbResult<int64_t> {
                auto res = AuthoringTemplatesRepo::UpdateAsync(r, rp).get();
                if (!res.ok) return DbResult<int64_t>::Err(res.error);
                return DbResult<int64_t>::Ok(r.id);
                });
        }
        return AuthoringTemplatesRepo::InsertAsync(r, rp);
    }

    std::future<DbResult<simcore::db::AuthoringTemplateRow>> DataService::LoadAuthoringTemplateAsync(int64_t template_id, RetryPolicy rp) {
        return AuthoringTemplatesRepo::GetAsync(template_id, rp);
    }

    std::future<DbResult<std::vector<simcore::db::TurnActionPresetLite>>> DataService::ListActionPresetsAsync(const std::string& search, int32_t limit, RetryPolicy rp) {
        return TurnActionPresetRepo::ListLiteAsync(search, limit, rp);
    }

    std::future<DbResult<int64_t>> DataService::SaveActionPresetAsync(const simcore::db::TurnActionPresetRow& r, bool is_update, RetryPolicy rp) {
        if (is_update) {
            return std::async(std::launch::async, [r, rp]() -> DbResult<int64_t> {
                auto res = TurnActionPresetRepo::UpdateAsync(r, rp).get();
                if (!res.ok) return DbResult<int64_t>::Err(res.error);
                return DbResult<int64_t>::Ok(r.id);
                });
        }
        return TurnActionPresetRepo::InsertAsync(r, rp);
    }

    std::future<DbResult<std::vector<int64_t>>> DataService::InsertUiConfigRowsAsync(const std::vector<UiConfigRow>& rows, RetryPolicy rp) {
        return UiConfigRowRepo::InsertManyAsync(rows, rp);
    }

    std::future<DbResult<std::vector<UiConfigRow>>> DataService::GetUiConfigRowsByIdsAsync(const std::vector<int64_t>& ids, RetryPolicy rp) {
        return UiConfigRowRepo::GetByIdsAsync(ids, rp);
    }

    std::future<DbResult<std::vector<simcore::db::PredicateSpecLite>>>
        DataService::ListPredicateSpecsAsync(const std::string& search, int32_t limit, RetryPolicy rp) {
        return simcore::db::PredicateSpecRepo::ListLiteAsync(search, limit, rp);
    }

    std::future<DbResult<void>> DataService::SetJobVmKvAsync(int64_t job_id, std::optional<std::string> vm_kv, RetryPolicy rp) {
        return JobsRepo::SetVmKvAsync(job_id, std::move(vm_kv), rp);
    }


} // namespace simcore::db
