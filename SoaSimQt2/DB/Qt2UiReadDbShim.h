#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "SimCoreDbRuntime.h"
#include "UIRead/IUiReadDb.h"
#include "Utils/IniDoc.h"

namespace simcore {
enum ProgramKindShim {
    PK_None = 0,
    PK_SeedProbe = 1,
    PK_TasMovie = 2,
    PK_BattleTurnRunner = 3,
    PK_BattleContextProbe = 4,
    PK_BattleSingleTurnRunner = 5,
    PK_TasInputStreamDetector = 6,
};
}

struct KeysetCursor {
    std::int64_t primary{};
    std::int64_t secondary{};
};

template <typename T>
struct Page {
    std::vector<T> items;
    std::optional<KeysetCursor> next;
    std::optional<KeysetCursor> prev;
};

enum class PageOrder { Desc, Asc };

template <typename TCursor = KeysetCursor>
struct PagedQuery {
    PageOrder order{ PageOrder::Desc };
    std::optional<TCursor> before;
    std::optional<TCursor> after;
    int limit{ 50 };
};

struct JobLite {
    std::int64_t job_id{};
    std::int64_t job_set_id{};
    std::optional<std::int64_t> savestate_id{};
    int program_kind{};
    std::string state;
    int priority{};
    int attempts{};
    std::int64_t queued_at{};
};

struct JobsListScope {
    std::vector<std::string> states;
    std::optional<int> program_kind;
    std::optional<std::int64_t> job_set_id;
    std::optional<std::int64_t> since_queued_at;
    std::optional<std::string> tag_key;
};

struct JobEventLite {
    std::int64_t event_id{};
    std::int64_t job_id{};
    std::int64_t ts{};
    std::string event_kind;
    std::optional<std::string> payload_preview;
};

struct JobEventsListScope {
    std::optional<std::string> event_kind;
    std::optional<std::int64_t> job_set_id;
    std::optional<std::int64_t> job_id;
    std::optional<std::int64_t> since_ts;
};

struct JobSetLite {
    std::int64_t job_set_id{};
    std::optional<std::int64_t> parent_job_set_id{};
    int program_kind{};
    std::string purpose;
    std::int64_t created_at{};
    std::int64_t total_jobs{};
    std::int64_t completed_jobs{};
    std::int64_t succeeded_jobs{};
    std::int64_t failed_jobs{};
    std::int64_t canceled_jobs{};
    std::optional<std::int64_t> expected_total{};
};

enum class JobSetStateFilter {
    Completed,
    Incomplete,
    HasFailures,
};

struct JobSetsListScope {
    std::optional<int> program_kind;
    std::optional<std::int64_t> min_job_set_id;
    std::optional<JobSetStateFilter> state_filter;
    std::optional<std::string> tag_key;
};

namespace simcore::db {

enum class DbErrorKind {
    Ok,
    NotFound,
    InvalidState,
    Unavailable,
    Unknown,
};

struct DbError {
    DbErrorKind kind = DbErrorKind::Ok;
    int code = 0;
    std::string message;

    operator std::string() const { return message; }
};

template <typename T>
struct DbResult {
    bool ok = false;
    T value{};
    DbError error{};

    static DbResult Ok(T v) {
        DbResult r{};
        r.ok = true;
        r.value = std::move(v);
        return r;
    }

    static DbResult Err(DbError e) {
        DbResult r{};
        r.ok = false;
        r.error = std::move(e);
        return r;
    }
};

template <>
struct DbResult<void> {
    bool ok = false;
    DbError error{};

    static DbResult Ok() {
        DbResult r{};
        r.ok = true;
        return r;
    }

    static DbResult Err(DbError e) {
        DbResult r{};
        r.ok = false;
        r.error = std::move(e);
        return r;
    }
};

enum class Compression {
    None = 0,
    Zstd = 1,
    lz4 = 2,
};

struct ProgramKindKV {
    int id{};
    std::string name;
};

struct ArtifactRefLite {
    std::int64_t artifact_id{};
    std::string role;
    std::string filename;
    std::uint64_t size_bytes{};
    int compression{};
    std::int64_t created_at{};
};

struct ObjectRefLite {
    std::int64_t id{};
    std::string sha256;
    Compression compression{};
    std::int64_t size{};
    std::string filename;
    std::int64_t created_at{};
};

struct ObjectRefRow : ObjectRefLite {};

struct SavestateLite {
    std::int64_t id{};
    int savestate_type{};
    std::string note;
    std::optional<std::int64_t> object_ref_id;
    std::string filename;
    int complete{};
};

struct SeedProbeLite {
    std::int64_t id{};
    std::int64_t savestate_id{};
    std::optional<std::int64_t> neutral_seed;
    std::string status;
    std::string purpose;
    int complete{};
    bool has_battle_context = false;
};

struct TasMovieLite {
    std::int64_t id{};
    std::int64_t base_file_id{};
    std::optional<std::int64_t> new_rtc;
    std::string status;
    std::optional<std::int64_t> created_at;
    std::optional<std::int64_t> started_at;
    std::optional<std::int64_t> completed_at;
};

struct ExplorerSettingsLite {
    std::int64_t id{};
    std::string name;
    std::string description;
    std::string purpose;
};

struct JobSetPageWithFamilies {
    Page<JobSetLite> page;
    std::vector<JobSetLite> family_items;
};

struct JobSetPriorityBoostResult {
    int new_priority{};
    std::int64_t changed_jobs{};
};

struct JobSetCancelQueuedResult {
    std::vector<std::int64_t> canceled_job_ids;
};

struct ExplorerRunReconcileResult {
    std::int64_t roots_scanned{};
    std::int64_t roots_with_existing_runs{};
    std::int64_t roots_reconciled{};
    std::int64_t runs_created{};
    std::int64_t jobs_updated{};
};

struct BattleContextRow {
    std::int64_t id{};
    std::int64_t savestate_id{};
    std::string ini_text;
};

struct AuthoringTemplateRow {
    std::int64_t template_id{};
    std::string name;
    std::string description;
    std::string ini_text;
};

struct TurnActionPresetLite {
    std::int64_t id{};
    std::string name;
    std::string description;
};

struct TurnActionPresetRow {
    std::int64_t preset_id{};
    std::string name;
    std::string description;
};

struct PredicateSpecLite {
    std::int64_t id{};
    std::string name;
    std::string description;
};

struct PredicateSpecRow {
    std::int64_t predicate_id{};
    std::string name;
    std::string description;
    std::int32_t spec_version{};
    std::int32_t required_bp{};
    std::optional<std::string> required_bp_multi;
    std::int32_t kind{};
    std::int32_t width{ 4 };
    std::int32_t cmp_op{};
    std::int32_t flags{};
    std::int64_t lhs_addr{};
    std::uint64_t rhs_value{};
    std::int32_t turn_mask{};
    std::optional<std::int32_t> lhs_key;
    std::optional<std::int32_t> rhs_key;
    std::optional<std::int64_t> lhs_prog_id;
    std::optional<std::int64_t> rhs_prog_id;
    std::string fingerprint;
};

struct UiConfigRow {
    std::int64_t id{};
    std::int64_t preset_id{};
    std::int32_t turn_index{};
    std::int32_t actor_slot{};
};

struct JobRow {
    std::int64_t job_id{};
    std::int64_t job_set_id{};
    int program_kind{};
    std::string state;
    std::optional<std::string> vm_kv;
};

struct JobSetRow {
    std::int64_t job_set_id{};
    std::optional<std::int64_t> parent_job_set_id;
    int program_kind{};
    std::string purpose;
};

struct JobEventsRepo {
    struct JobIdPayload {
        std::int64_t job_id{};
        std::optional<std::string> payload;
    };
    static DbResult<std::optional<std::string>> GetLatestPayload(std::int64_t, const std::string&) { return DbResult<std::optional<std::string>>::Ok(std::nullopt); }
    static DbResult<std::vector<JobIdPayload>> GetLatestPayloadByJobs(const std::vector<std::int64_t>& ids, const std::string&) {
        std::vector<JobIdPayload> out;
        for (const auto id : ids) out.push_back({ id, std::nullopt });
        return DbResult<std::vector<JobIdPayload>>::Ok(std::move(out));
    }
    static DbResult<std::vector<JobEventLite>> ListByJobAndKind(std::int64_t, const std::string&) { return DbResult<std::vector<JobEventLite>>::Ok({}); }
};

namespace qt2shim {

inline IUiReadDb* UiRead() {
    return soasimqt2::SimCoreDbRuntime::instance().uiReadDb();
}

inline DbError NotMigrated(const char* operation) {
    return { DbErrorKind::Unavailable, 0, std::string(operation) + " is not implemented in this Qt2 migration slice yet" };
}

template <typename T>
inline std::future<T> ReadyFuture(T value) {
    return std::async(std::launch::deferred, [value = std::move(value)]() mutable {
        return std::move(value);
    });
}

inline KeysetCursor FromUiCursor(const UiReadListCursor& cursor) {
    return { cursor.primary, cursor.secondary };
}

inline UiReadListCursor ToUiCursor(const KeysetCursor& cursor) {
    return { cursor.primary, cursor.secondary };
}

inline JobLite ToJobLite(const UiJobSummary& row) {
    JobLite out{};
    out.job_id = row.job_id;
    out.job_set_id = row.job_set_id;
    out.program_kind = row.program_kind;
    out.state = row.state;
    out.priority = row.priority;
    out.attempts = row.attempts;
    out.queued_at = row.queued_at_utc;
    return out;
}

inline JobSetLite ToJobSetLite(const UiJobSetSummary& row) {
    JobSetLite out{};
    out.job_set_id = row.job_set_id;
    out.program_kind = row.program_kind;
    out.created_at = row.created_at_utc;
    out.total_jobs = row.total_jobs;
    out.completed_jobs = row.completed_jobs;
    out.succeeded_jobs = row.succeeded_jobs;
    out.failed_jobs = row.failed_jobs;
    out.canceled_jobs = row.canceled_jobs;
    return out;
}

inline IniDoc ToJobDetailIniDoc(const UiJobDetail& row) {
    IniDoc doc{};
    auto& summary = doc.ensure_section("ui_job_summary");
    summary.set("job_id", std::to_string(row.summary.job_id));
    summary.set("job_set_id", std::to_string(row.summary.job_set_id));
    summary.set("program_kind", std::to_string(row.summary.program_kind));
    summary.set("state", row.summary.state);
    summary.set("priority", std::to_string(row.summary.priority));
    summary.set("attempts", std::to_string(row.summary.attempts));
    summary.set("max_attempts", std::to_string(row.summary.max_attempts));
    summary.set("queued_at_utc", std::to_string(row.summary.queued_at_utc));
    if (row.summary.started_at_utc.has_value()) {
        summary.set("started_at_utc", std::to_string(*row.summary.started_at_utc));
    }
    if (row.summary.ended_at_utc.has_value()) {
        summary.set("ended_at_utc", std::to_string(*row.summary.ended_at_utc));
    }
    if (!row.summary.error_code.empty()) {
        summary.set("error_code", row.summary.error_code);
    }
    if (!row.summary.error_text.empty()) {
        summary.set("error_text", row.summary.error_text);
    }

    auto& detail = doc.ensure_section("ui_job_detail");
    detail.set("fingerprint", row.fingerprint);
    if (row.claimed_by_token.has_value()) {
        detail.set("claimed_by_token", *row.claimed_by_token);
    }
    if (row.lease_expires_at_utc.has_value()) {
        detail.set("lease_expires_at_utc", std::to_string(*row.lease_expires_at_utc));
    }
    return doc;
}

inline ObjectRefLite ToObjectRefLite(const UiArtifactSummary& row) {
    ObjectRefLite out{};
    out.id = row.artifact_id;
    out.sha256 = row.sha256;
    out.size = static_cast<std::int64_t>(row.size_bytes);
    out.filename = row.filename;
    out.created_at = row.created_at_utc;
    out.compression = Compression::None;
    return out;
}

} // namespace qt2shim

class DataService {
public:
    static std::future<DbResult<std::vector<ProgramKindKV>>> ListProgramKindsAsync() {
        auto* db = qt2shim::UiRead();
        if (db == nullptr) {
            return qt2shim::ReadyFuture(DbResult<std::vector<ProgramKindKV>>::Err(qt2shim::NotMigrated("UIRead startup")));
        }
        std::vector<ProgramKindKV> out;
        for (const auto& row : db->ListProgramKinds()) {
            out.push_back({ row.id, row.name });
        }
        return qt2shim::ReadyFuture(DbResult<std::vector<ProgramKindKV>>::Ok(std::move(out)));
    }

    static std::future<DbResult<Page<JobLite>>> FetchJobsPageAsync(
        const JobsListScope& scope,
        std::optional<KeysetCursor> before,
        std::optional<KeysetCursor> after,
        int limit) {
        auto* db = qt2shim::UiRead();
        if (db == nullptr) {
            return qt2shim::ReadyFuture(DbResult<Page<JobLite>>::Err(qt2shim::NotMigrated("UIRead startup")));
        }
        UiReadJobListQuery query{};
        query.limit = limit;
        query.states = scope.states;
        query.program_kind = scope.program_kind;
        query.job_set_id = scope.job_set_id;
        if (before.has_value()) query.before = qt2shim::ToUiCursor(*before);
        if (after.has_value()) query.after = qt2shim::ToUiCursor(*after);
        const auto page = db->ListJobs(query);
        Page<JobLite> out{};
        for (const auto& row : page.items) out.items.push_back(qt2shim::ToJobLite(row));
        if (page.next.has_value()) out.next = qt2shim::FromUiCursor(*page.next);
        if (page.prev.has_value()) out.prev = qt2shim::FromUiCursor(*page.prev);
        return qt2shim::ReadyFuture(DbResult<Page<JobLite>>::Ok(std::move(out)));
    }

    static std::future<DbResult<Page<JobEventLite>>> FetchJobEventsPage(const JobEventsListScope&, const PagedQuery<>&) {
        return qt2shim::ReadyFuture(DbResult<Page<JobEventLite>>::Ok({}));
    }

    static std::future<DbResult<std::vector<JobEventsRepo::JobIdPayload>>> BulkLatestProgressByJobsAsync(const std::vector<std::int64_t>& ids) {
        return qt2shim::ReadyFuture(JobEventsRepo::GetLatestPayloadByJobs(ids, "PROGRESS"));
    }

    static std::future<DbResult<IniDoc>> FetchJobVmKvIniAsync(std::int64_t job_id) {
        auto* db = qt2shim::UiRead();
        if (db == nullptr) {
            return qt2shim::ReadyFuture(DbResult<IniDoc>::Err(qt2shim::NotMigrated("UIRead startup")));
        }
        const auto detail = db->GetJobDetail(job_id);
        if (!detail.has_value()) {
            return qt2shim::ReadyFuture(DbResult<IniDoc>::Err({ DbErrorKind::NotFound, 0, "job not found" }));
        }
        return qt2shim::ReadyFuture(DbResult<IniDoc>::Ok(qt2shim::ToJobDetailIniDoc(*detail)));
    }

    static std::future<DbResult<IniDoc>> FetchJobResultsIniAsync(std::int64_t) {
        return qt2shim::ReadyFuture(DbResult<IniDoc>::Ok(IniDoc{}));
    }

    static std::future<DbResult<std::string>> FetchDecodedProgressAsync(std::int64_t) {
        return qt2shim::ReadyFuture(DbResult<std::string>::Ok({}));
    }

    static std::future<DbResult<std::vector<ArtifactRefLite>>> FetchJobArtifactRefsAsync(std::int64_t job_id) {
        auto* db = qt2shim::UiRead();
        if (db == nullptr) {
            return qt2shim::ReadyFuture(DbResult<std::vector<ArtifactRefLite>>::Err(qt2shim::NotMigrated("UIRead startup")));
        }
        std::vector<ArtifactRefLite> out;
        for (const auto& row : db->ListJobArtifacts(job_id)) {
            ArtifactRefLite item{};
            item.artifact_id = row.artifact_id;
            item.role = row.role_kind;
            item.filename = row.filename;
            item.size_bytes = row.size_bytes;
            item.created_at = row.created_at_utc;
            out.push_back(std::move(item));
        }
        return qt2shim::ReadyFuture(DbResult<std::vector<ArtifactRefLite>>::Ok(std::move(out)));
    }

    static std::future<DbResult<JobSetPageWithFamilies>> FetchJobSetsPageWithFamilies(const JobSetsListScope& scope, const PagedQuery<>& q) {
        auto* db = qt2shim::UiRead();
        if (db == nullptr) {
            return qt2shim::ReadyFuture(DbResult<JobSetPageWithFamilies>::Err(qt2shim::NotMigrated("UIRead startup")));
        }
        UiReadJobSetListQuery query{};
        query.limit = q.limit;
        query.program_kind = scope.program_kind;
        if (q.before.has_value()) query.before = qt2shim::ToUiCursor(*q.before);
        if (q.after.has_value()) query.after = qt2shim::ToUiCursor(*q.after);
        const auto page = db->ListJobSets(query);
        JobSetPageWithFamilies out{};
        for (const auto& row : page.items) out.page.items.push_back(qt2shim::ToJobSetLite(row));
        if (page.next.has_value()) out.page.next = qt2shim::FromUiCursor(*page.next);
        if (page.prev.has_value()) out.page.prev = qt2shim::FromUiCursor(*page.prev);
        out.family_items = out.page.items;
        return qt2shim::ReadyFuture(DbResult<JobSetPageWithFamilies>::Ok(std::move(out)));
    }

    static std::future<DbResult<Page<ObjectRefLite>>> FetchObjectRefsPage(const PagedQuery<>& q, const std::string& search, const std::string& ext_filter) {
        auto* db = qt2shim::UiRead();
        if (db == nullptr) {
            return qt2shim::ReadyFuture(DbResult<Page<ObjectRefLite>>::Err(qt2shim::NotMigrated("UIRead startup")));
        }
        UiReadArtifactListQuery query{};
        query.limit = q.limit;
        query.search = search;
        query.extension = ext_filter;
        if (q.before.has_value()) query.before = qt2shim::ToUiCursor(*q.before);
        if (q.after.has_value()) query.after = qt2shim::ToUiCursor(*q.after);
        const auto page = db->ListArtifacts(query);
        Page<ObjectRefLite> out{};
        for (const auto& row : page.items) out.items.push_back(qt2shim::ToObjectRefLite(row));
        if (page.next.has_value()) out.next = qt2shim::FromUiCursor(*page.next);
        if (page.prev.has_value()) out.prev = qt2shim::FromUiCursor(*page.prev);
        return qt2shim::ReadyFuture(DbResult<Page<ObjectRefLite>>::Ok(std::move(out)));
    }

    static std::future<DbResult<void>> RequeueJobAsync(std::int64_t) { return qt2shim::ReadyFuture(DbResult<void>::Err(qt2shim::NotMigrated("Requeue job"))); }
    static std::future<DbResult<void>> ReplayJobVisuallyAsync(std::int64_t) { return qt2shim::ReadyFuture(DbResult<void>::Err(qt2shim::NotMigrated("Replay job visually"))); }
    static std::future<DbResult<void>> RestartFailedJobAsync(std::int64_t) { return qt2shim::ReadyFuture(DbResult<void>::Err(qt2shim::NotMigrated("Restart job"))); }
    static std::future<DbResult<void>> CancelJobAsync(std::int64_t) { return qt2shim::ReadyFuture(DbResult<void>::Err(qt2shim::NotMigrated("Cancel job"))); }
    static std::future<DbResult<void>> SetJobVmKvAsync(std::int64_t, std::optional<std::string>) { return qt2shim::ReadyFuture(DbResult<void>::Err(qt2shim::NotMigrated("Set job VM KV"))); }
    static std::future<DbResult<JobSetPriorityBoostResult>> BoostJobSetPriorityTreeAsync(std::int64_t) { return qt2shim::ReadyFuture(DbResult<JobSetPriorityBoostResult>::Err(qt2shim::NotMigrated("Boost job set"))); }
    static std::future<DbResult<JobSetCancelQueuedResult>> CancelQueuedJobsForJobSetTreeAsync(std::int64_t) { return qt2shim::ReadyFuture(DbResult<JobSetCancelQueuedResult>::Err(qt2shim::NotMigrated("Cancel job set"))); }
    static std::future<DbResult<void>> DeleteJobSetAsync(std::int64_t) { return qt2shim::ReadyFuture(DbResult<void>::Err(qt2shim::NotMigrated("Delete job set"))); }
    static std::future<DbResult<std::int64_t>> CreateJobSetAsync(std::optional<std::string>, int, std::optional<std::string> = {}, std::optional<std::string> = {}, std::optional<std::int64_t> = {}, std::optional<std::string> = {}, std::optional<std::int64_t> = {}) { return qt2shim::ReadyFuture(DbResult<std::int64_t>::Err(qt2shim::NotMigrated("Create job set"))); }
    static std::future<DbResult<std::int64_t>> EncodeJobSetWithCodecAsync(int, std::int64_t, const std::string&) { return qt2shim::ReadyFuture(DbResult<std::int64_t>::Err(qt2shim::NotMigrated("Encode job set"))); }
    static std::future<DbResult<void>> SetJobSetExpectedTotalAsync(std::int64_t, std::optional<std::int64_t>) { return qt2shim::ReadyFuture(DbResult<void>::Err(qt2shim::NotMigrated("Set job set expected total"))); }
    static std::future<DbResult<Page<SavestateLite>>> FetchSavestatesPage(const PagedQuery<>&, const std::string&) { return qt2shim::ReadyFuture(DbResult<Page<SavestateLite>>::Ok({})); }
    static std::future<DbResult<Page<SeedProbeLite>>> FetchSeedProbesPage(const PagedQuery<>&, const std::string&, bool, std::optional<std::int64_t> = std::nullopt) { return qt2shim::ReadyFuture(DbResult<Page<SeedProbeLite>>::Ok({})); }
    static std::future<DbResult<Page<TasMovieLite>>> FetchTasMoviesPage(const PagedQuery<>&, const std::string&, bool) { return qt2shim::ReadyFuture(DbResult<Page<TasMovieLite>>::Ok({})); }
    static std::future<DbResult<Page<ExplorerSettingsLite>>> FetchExplorerSettingsPage(const PagedQuery<>&, const std::string&) { return qt2shim::ReadyFuture(DbResult<Page<ExplorerSettingsLite>>::Ok({})); }
    static std::future<DbResult<std::int64_t>> GetSavestateForSeedProbeAsync(std::int64_t) { return qt2shim::ReadyFuture(DbResult<std::int64_t>::Err(qt2shim::NotMigrated("SeedProbe savestate lookup"))); }
    static std::future<DbResult<std::optional<BattleContextRow>>> GetLatestBattleContextForSavestateAsync(std::int64_t) { return qt2shim::ReadyFuture(DbResult<std::optional<BattleContextRow>>::Ok(std::nullopt)); }
    static std::future<DbResult<std::int64_t>> GetNewBattleContextAsync(const std::string&) { return qt2shim::ReadyFuture(DbResult<std::int64_t>::Err(qt2shim::NotMigrated("Create battle context"))); }
    static std::future<DbResult<std::vector<TurnActionPresetLite>>> ListActionPresetsAsync(const std::string&, std::int32_t) { return qt2shim::ReadyFuture(DbResult<std::vector<TurnActionPresetLite>>::Ok({})); }
    static std::future<DbResult<std::vector<PredicateSpecLite>>> ListPredicateSpecsAsync(const std::string&, std::int32_t) { return qt2shim::ReadyFuture(DbResult<std::vector<PredicateSpecLite>>::Ok({})); }
    static std::future<DbResult<std::vector<UiConfigRow>>> GetUiConfigRowsByIdsAsync(const std::vector<std::int64_t>&) { return qt2shim::ReadyFuture(DbResult<std::vector<UiConfigRow>>::Ok({})); }
    static std::future<DbResult<std::vector<std::int64_t>>> InsertUiConfigRowsAsync(const std::vector<UiConfigRow>&) { return qt2shim::ReadyFuture(DbResult<std::vector<std::int64_t>>::Err(qt2shim::NotMigrated("Insert UI config rows"))); }
    static std::future<DbResult<ExplorerRunReconcileResult>> ReconcileMissingExplorerRunsAsync() { return qt2shim::ReadyFuture(DbResult<ExplorerRunReconcileResult>::Err(qt2shim::NotMigrated("Reconcile explorer runs"))); }
};

struct JobsRepo {
    static DbResult<JobRow> Get(std::int64_t job_id) {
        auto* db = qt2shim::UiRead();
        if (db == nullptr) return DbResult<JobRow>::Err(qt2shim::NotMigrated("UIRead startup"));
        const auto row = db->GetJobSummary(job_id);
        if (!row.has_value()) return DbResult<JobRow>::Err({ DbErrorKind::NotFound, 0, "job not found" });
        JobRow out{};
        out.job_id = row->job_id;
        out.job_set_id = row->job_set_id;
        out.program_kind = row->program_kind;
        out.state = row->state;
        return DbResult<JobRow>::Ok(std::move(out));
    }
    static std::future<DbResult<JobRow>> GetAsync(std::int64_t job_id) { return qt2shim::ReadyFuture(Get(job_id)); }
    static DbResult<std::vector<JobRow>> GetByJobSet(std::int64_t job_set_id) {
        auto* db = qt2shim::UiRead();
        if (db == nullptr) return DbResult<std::vector<JobRow>>::Err(qt2shim::NotMigrated("UIRead startup"));
        UiReadJobListQuery query{};
        query.limit = 1000;
        query.job_set_id = job_set_id;
        std::vector<JobRow> out;
        for (const auto& row : db->ListJobs(query).items) {
            out.push_back({ row.job_id, row.job_set_id, row.program_kind, row.state, std::nullopt });
        }
        return DbResult<std::vector<JobRow>>::Ok(std::move(out));
    }
};

struct JobSetsRepo {
    static DbResult<std::optional<std::int64_t>> GetParent(std::int64_t) { return DbResult<std::optional<std::int64_t>>::Ok(std::nullopt); }
    static DbResult<JobSetRow> Get(std::int64_t job_set_id) {
        auto* db = qt2shim::UiRead();
        if (db == nullptr) return DbResult<JobSetRow>::Err(qt2shim::NotMigrated("UIRead startup"));
        const auto detail = db->GetJobSetDetail(job_set_id, 0);
        if (!detail.has_value()) return DbResult<JobSetRow>::Err({ DbErrorKind::NotFound, 0, "job set not found" });
        return DbResult<JobSetRow>::Ok({ job_set_id, std::nullopt, detail->summary.program_kind, {} });
    }
    static std::future<DbResult<Page<JobSetLite>>> ListRecentAsync(const JobSetsListScope& scope, std::optional<KeysetCursor> before, int limit) {
        PagedQuery<> q{};
        q.before = before;
        q.limit = limit;
        return std::async(std::launch::deferred, [scope, q]() {
            auto result = DataService::FetchJobSetsPageWithFamilies(scope, q).get();
            if (!result.ok) return DbResult<Page<JobSetLite>>::Err(result.error);
            return DbResult<Page<JobSetLite>>::Ok(std::move(result.value.page));
        });
    }
    static DbResult<std::vector<JobSetLite>> ListFamiliesForSeeds(const std::vector<std::int64_t>&) { return DbResult<std::vector<JobSetLite>>::Ok({}); }
};

struct ObjectStore {
    static bool Ready() { return qt2shim::UiRead() != nullptr; }
    static std::future<DbResult<std::optional<ObjectRefRow>>> GetByShaAsync(const std::string&) { return qt2shim::ReadyFuture(DbResult<std::optional<ObjectRefRow>>::Ok(std::nullopt)); }
    static DbResult<ObjectRefRow> FinalizeFromFile(const std::string&, Compression, const std::string&) { return DbResult<ObjectRefRow>::Err(qt2shim::NotMigrated("Import artifact")); }
    static std::future<DbResult<ObjectRefRow>> FinalizeFromFileAsync(const std::string&, Compression, const std::string&) { return qt2shim::ReadyFuture(DbResult<ObjectRefRow>::Err(qt2shim::NotMigrated("Import artifact"))); }
    static DbResult<std::string> GetText(std::int64_t) { return DbResult<std::string>::Err(qt2shim::NotMigrated("Read artifact text")); }
    static std::future<DbResult<std::string>> GetTextAsync(std::int64_t) { return qt2shim::ReadyFuture(DbResult<std::string>::Err(qt2shim::NotMigrated("Read artifact text"))); }
    static std::future<DbResult<void>> MaterializeToPathAsync(std::int64_t, const std::string&) { return qt2shim::ReadyFuture(DbResult<void>::Err(qt2shim::NotMigrated("Export artifact"))); }
};

struct TagRecord {
    std::int64_t tag_id{};
    std::string tag_key;
    std::string description;
    std::string created_at;
};

struct TagRepo {
    static DbResult<std::int64_t> EnsureTag(const std::string&, std::optional<std::string>) { return DbResult<std::int64_t>::Err(qt2shim::NotMigrated("Create tag")); }
    static DbResult<std::vector<TagRecord>> ListTags(std::optional<std::string>) { return DbResult<std::vector<TagRecord>>::Ok({}); }
    static DbResult<std::vector<TagRecord>> ListEntityTags(const std::string&, std::int64_t) { return DbResult<std::vector<TagRecord>>::Ok({}); }
    static DbResult<void> AttachTagToEntity(const std::string&, std::int64_t, const std::string&, std::optional<std::string>) { return DbResult<void>::Err(qt2shim::NotMigrated("Attach tag")); }
    static DbResult<void> DetachTagFromEntity(const std::string&, std::int64_t, const std::string&) { return DbResult<void>::Err(qt2shim::NotMigrated("Detach tag")); }
    static DbResult<std::vector<std::int64_t>> FindEntityIdsByTag(const std::string&, const std::string&, bool) { return DbResult<std::vector<std::int64_t>>::Ok({}); }
    static DbResult<std::vector<TagRecord>> ListTagsForEntityKind(const std::string&) { return DbResult<std::vector<TagRecord>>::Ok({}); }
};

struct DeltaSeedRepo { static auto ListUniqueForProbeAsync(std::int64_t) { return qt2shim::ReadyFuture(DbResult<std::vector<std::int64_t>>::Ok({})); } static DbResult<std::int64_t> Get(std::int64_t id) { return DbResult<std::int64_t>::Ok(id); } };
struct ExplorerRunRepo { static DbResult<std::vector<std::int64_t>> ListRootJobSetIdsByVictoryPaged(bool, std::optional<std::int64_t>, int) { return DbResult<std::vector<std::int64_t>>::Ok({}); } static DbResult<std::int64_t> Get(std::int64_t id) { return DbResult<std::int64_t>::Ok(id); } };
struct BattlePlanAtomRepo { static DbResult<std::int64_t> Get(std::int64_t id) { return DbResult<std::int64_t>::Ok(id); } };
struct BattlePlanTurnRepo { struct Actor { std::int64_t atom_id{}; }; static DbResult<std::vector<Actor>> ListActorsByPlan(std::int64_t, std::int32_t) { return DbResult<std::vector<Actor>>::Ok({}); } };
struct SeedProbeRepo { struct Row { std::int64_t savestate_id{}; }; static DbResult<Row> Get(std::int64_t) { return DbResult<Row>::Err(qt2shim::NotMigrated("SeedProbe lookup")); } };
struct SavestateRepo { static DbResult<SavestateLite> Get(std::int64_t id) { SavestateLite row{}; row.id = id; return DbResult<SavestateLite>::Ok(row); } };
struct TasMovieRepo { static DbResult<TasMovieLite> Get(std::int64_t id) { TasMovieLite row{}; row.id = id; return DbResult<TasMovieLite>::Ok(row); } };
struct ExplorerSettingsPredicateRepo {};
struct BattleContextRepo {};
struct AddressProgramRepo { static DbResult<std::int64_t> Get(std::int64_t id) { return DbResult<std::int64_t>::Ok(id); } static DbResult<std::int64_t> Ensure(const std::string&) { return DbResult<std::int64_t>::Err(qt2shim::NotMigrated("Address program")); } };
struct AuthoringTemplatesRepo { static auto ListLiteAsync(const std::string&, std::int32_t) { return qt2shim::ReadyFuture(DbResult<std::vector<AuthoringTemplateRow>>::Ok({})); } static DbResult<AuthoringTemplateRow> Get(std::int64_t) { return DbResult<AuthoringTemplateRow>::Err(qt2shim::NotMigrated("Authoring template")); } static DbResult<std::int64_t> Insert(const AuthoringTemplateRow&) { return DbResult<std::int64_t>::Err(qt2shim::NotMigrated("Authoring template")); } };
struct PredicateSpecRepo { static DbResult<PredicateSpecRow> Get(std::int64_t) { return DbResult<PredicateSpecRow>::Err(qt2shim::NotMigrated("Predicate spec")); } static DbResult<std::int64_t> EnsureByFingerprint(const PredicateSpecRow&) { return DbResult<std::int64_t>::Err(qt2shim::NotMigrated("Predicate spec")); } };
struct TurnActionPresetRepo { static DbResult<TurnActionPresetRow> Get(std::int64_t) { return DbResult<TurnActionPresetRow>::Err(qt2shim::NotMigrated("Action preset")); } static DbResult<void> Update(const TurnActionPresetRow&) { return DbResult<void>::Err(qt2shim::NotMigrated("Action preset")); } static DbResult<std::int64_t> Insert(const TurnActionPresetRow&) { return DbResult<std::int64_t>::Err(qt2shim::NotMigrated("Action preset")); } };

struct BattleSingleTurnRunDBCodec {
    static DbResult<void> enqueue_next_wave_from_job(std::int64_t) { return DbResult<void>::Err(qt2shim::NotMigrated("Battle follow-up")); }
};

struct BattleContextDBCodec {};

enum class DbSnapshotPhase {
    Preparing,
    ScanningArtifacts,
    WritingCoreEntries,
    WritingArtifacts,
    Finalizing,
    Starting,
    Reading,
    Writing,
    Complete,
    Failed
};
struct DbSnapshotProgress {
    DbSnapshotPhase phase = DbSnapshotPhase::Preparing;
    std::int64_t completed{};
    std::int64_t current{};
    std::int64_t total{};
    std::string message;
    std::string detail;
};
struct DbSnapshotService {
    static DbResult<void> SaveSnapshot(const std::string&, std::function<void(const DbSnapshotProgress&)>) { return DbResult<void>::Err(qt2shim::NotMigrated("Save snapshot")); }
    static DbResult<void> LoadSnapshot(const std::string&, const std::string&, bool) { return DbResult<void>::Err(qt2shim::NotMigrated("Load snapshot")); }
};

} // namespace simcore::db
