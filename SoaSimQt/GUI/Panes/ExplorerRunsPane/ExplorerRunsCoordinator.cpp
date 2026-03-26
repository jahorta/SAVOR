#include "ExplorerRunsCoordinator.h"

#include "DB/BattlePlanAtomRepo.h"
#include "DB/BattlePlanTurnRepo.h"
#include "DB/DeltaSeedRepo.h"
#include "DB/ProgramDB/BattleSingleTurnRunDBCodec.h"
#include "DB/Querying/DataService.h"
#include "DB/Scheduling/JobEventsRepo.h"
#include "DB/Scheduling/JobsRepo.h"
#include "DB/Scheduling/JobSetsRepo.h"
#include "Core/Input/InputPlanFmt.h"
#include "Core/Input/SoaBattle/PlanWriter.h"
#include "Phases/Programs/BattleRunner/BattleOutcome.h"
#include "Utils/IniDoc.h"

#include <QtCore/QStringList>

#include <algorithm>
#include <exception>
#include <utility>

using namespace simcore::db;
using namespace simcore::db::codec::battle::singleturn;

namespace {
QString describeException(const char* prefix)
{
    try {
        throw;
    } catch (const std::exception& ex) {
        return QStringLiteral("%1: %2").arg(QString::fromUtf8(prefix), QString::fromUtf8(ex.what()));
    } catch (...) {
        return QStringLiteral("%1: unknown exception").arg(QString::fromUtf8(prefix));
    }
}

template <typename AsyncCall>
auto runAsync(AsyncCall&& asyncCall)
{
    return QtConcurrent::run([call = std::forward<AsyncCall>(asyncCall)]() mutable {
        return call();
    });
}
}

ExplorerRunsCoordinator::ExplorerRunsCoordinator(QObject* parent)
    : QObject(parent)
    , lastGroupsRefresh_(QDateTime::currentDateTimeUtc())
{
    connect(&groupsWatcher_, &QFutureWatcher<std::vector<GroupRow>>::finished, this, [this]() {
        try {
            groups_ = groupsWatcher_.result();
            lastGroupsRefresh_ = QDateTime::currentDateTimeUtc();
        } catch (...) {
            groups_.clear();
        }
        groupsInFlight_ = false;
        emitStateChanged();
    });

    connect(&jobsWatcher_, &QFutureWatcher<std::vector<JobViewRow>>::finished, this, [this]() {
        try {
            jobs_ = jobsWatcher_.result();
        } catch (...) {
            jobs_.clear();
        }
        jobsInFlight_ = false;
        emitStateChanged();
    });

    connect(&detailsWatcher_, &QFutureWatcher<DetailBundle>::finished, this, [this]() {
        try {
            details_ = detailsWatcher_.result();
        } catch (...) {
            details_ = {};
        }
        detailsInFlight_ = false;
        emitStateChanged();
    });

    connect(&autoRefreshTimer_, &QTimer::timeout, this, &ExplorerRunsCoordinator::handleAutoRefreshTick);
    startAutoRefreshTimer();
}

const std::vector<ExplorerRunsCoordinator::GroupRow>& ExplorerRunsCoordinator::groups() const { return groups_; }
const std::vector<ExplorerRunsCoordinator::JobViewRow>& ExplorerRunsCoordinator::jobs() const { return jobs_; }
const ExplorerRunsCoordinator::DetailBundle& ExplorerRunsCoordinator::details() const { return details_; }

bool ExplorerRunsCoordinator::groupsInFlight() const { return groupsInFlight_; }
bool ExplorerRunsCoordinator::jobsInFlight() const { return jobsInFlight_; }
bool ExplorerRunsCoordinator::detailsInFlight() const { return detailsInFlight_; }
bool ExplorerRunsCoordinator::autoRefreshEnabled() const { return autoRefreshEnabled_; }
int ExplorerRunsCoordinator::refreshSeconds() const { return refreshSeconds_; }

void ExplorerRunsCoordinator::requestGroupsRefresh()
{
    if (groupsInFlight_) {
        return;
    }

    groupsInFlight_ = true;
    groupsWatcher_.setFuture(runAsync([this]() { return buildGroups(); }));
    emitStateChanged();
}

void ExplorerRunsCoordinator::requestJobsRefresh(const std::vector<qint64>& waveJobSetIds)
{
    if (jobsInFlight_) {
        return;
    }
    if (waveJobSetIds.empty()) {
        clearJobs();
        return;
    }

    jobsInFlight_ = true;
    jobsWatcher_.setFuture(runAsync([this, waveJobSetIds]() { return buildJobsForWaves(waveJobSetIds); }));
    emitStateChanged();
}

void ExplorerRunsCoordinator::requestDetailsRefresh(qint64 jobId)
{
    if (jobId <= 0) {
        clearDetails();
        return;
    }

    detailsRequestJobId_ = jobId;
    detailsInFlight_ = true;
    detailsWatcher_.setFuture(runAsync([this, jobId]() {
        DetailBundle bundle;
        bundle.progressLog = buildProgressLog(jobId);
        bundle.blueprintInfo = buildBlueprintInfo(jobId);
        const std::optional<QString> results = fetchResultsIniText(jobId);
        bundle.resultsLog = results.has_value() ? *results : QStringLiteral("(no results)");
        return bundle;
    }));
    emitStateChanged();
}

void ExplorerRunsCoordinator::setAutoRefreshEnabled(bool enabled)
{
    if (autoRefreshEnabled_ == enabled) {
        return;
    }
    autoRefreshEnabled_ = enabled;
    if (enabled) {
        lastGroupsRefresh_ = QDateTime::currentDateTimeUtc();
    }
    emitStateChanged();
}

void ExplorerRunsCoordinator::setRefreshSeconds(int seconds)
{
    const int clampedSeconds = (std::max)(1, seconds);
    if (refreshSeconds_ == clampedSeconds) {
        return;
    }
    refreshSeconds_ = clampedSeconds;
    startAutoRefreshTimer();
    emitStateChanged();
}

void ExplorerRunsCoordinator::clearJobs()
{
    const bool changed = !jobs_.empty() || jobsInFlight_;
    jobs_.clear();
    jobsInFlight_ = false;
    if (changed) {
        emitStateChanged();
    }
}

void ExplorerRunsCoordinator::clearDetails()
{
    const bool changed = detailsRequestJobId_ != 0 || detailsInFlight_
        || !details_.resultsLog.isEmpty() || !details_.progressLog.isEmpty() || !details_.blueprintInfo.isEmpty();
    detailsRequestJobId_ = 0;
    details_ = {};
    detailsInFlight_ = false;
    if (changed) {
        emitStateChanged();
    }
}

void ExplorerRunsCoordinator::emitStateChanged()
{
    emit stateChanged();
}

void ExplorerRunsCoordinator::startAutoRefreshTimer()
{
    autoRefreshTimer_.start(1000);
}

void ExplorerRunsCoordinator::handleAutoRefreshTick()
{
    if (!autoRefreshEnabled_ || groupsInFlight_) {
        return;
    }
    if (lastGroupsRefresh_.secsTo(QDateTime::currentDateTimeUtc()) < refreshSeconds_) {
        return;
    }
    requestGroupsRefresh();
}

std::vector<ExplorerRunsCoordinator::GroupRow> ExplorerRunsCoordinator::buildGroups() const
{
    JobSetsListScope scope{};
    scope.program_kind = simcore::PK_BattleSingleTurnRunner;
    auto page = JobSetsRepo::ListRecentAsync(scope, std::nullopt, 400).get();
    if (!page.ok) {
        return {};
    }

    std::unordered_map<qint64, GroupRow> groupMap;
    for (const JobSetLite& lite : page.value.items) {
        auto js = JobSetsRepo::Get(lite.job_set_id);
        if (!js.ok) {
            continue;
        }

        const WaveMeta meta = parseWaveMeta(js.value.meta_text);
        const qint64 rootId = resolveRoot(js.value);

        GroupRow& group = groupMap[rootId];
        group.rootGroupId = rootId;
        if (group.createdAt == 0 || js.value.created_at < group.createdAt) {
            group.createdAt = js.value.created_at;
        }

        QString settingsLabel = QStringLiteral("settings_id=%1").arg(meta.settingsId);
        if (!meta.settingsName.empty()) {
            settingsLabel = QString::fromStdString(meta.settingsName);
        }
        group.settingsLabel = settingsLabel;

        auto jobs = JobsRepo::GetByJobSet(js.value.job_set_id);
        QString status = jobs.ok ? summarizeStates(jobs.value) : QStringLiteral("(error)");
        bool hasSuccess = false;
        bool hasWinner = false;
        if (jobs.ok && !jobs.value.empty()) {
            std::vector<qint64> ids;
            ids.reserve(jobs.value.size());
            for (const JobRow& job : jobs.value) {
                ids.push_back(job.job_id);
            }
            const auto resultMap = loadJobResultsMap(ids);
            for (const JobRow& job : jobs.value) {
                hasWinner = hasWinner || isWinnerState(QString::fromStdString(job.state));
                const auto it = resultMap.find(job.job_id);
                if (it != resultMap.end() && it->second.successOutcome) {
                    hasSuccess = true;
                }
            }
        }

        group.waves.push_back(WaveRow{ js.value.job_set_id, js.value.created_at, meta.waveTurn, status, hasWinner, hasSuccess });
        group.hasSuccessOutcome = group.hasSuccessOutcome || hasSuccess;
    }

    std::vector<GroupRow> groups;
    groups.reserve(groupMap.size());
    for (auto& entry : groupMap) {
        GroupRow& group = entry.second;
        std::sort(group.waves.begin(), group.waves.end(), [](const WaveRow& a, const WaveRow& b) {
            if (a.waveTurn != b.waveTurn) {
                return a.waveTurn < b.waveTurn;
            }
            return a.createdAt < b.createdAt;
        });
        group.totalWaves = static_cast<int>(group.waves.size());
        group.statusSummary = group.waves.empty() ? QString() : group.waves.back().statusSummary;

        std::unordered_map<quint32, bool> turnHasSuccess;
        for (const WaveRow& wave : group.waves) {
            turnHasSuccess[wave.waveTurn] = turnHasSuccess[wave.waveTurn] || wave.hasSuccessOutcome;
        }
        std::vector<quint32> turns;
        turns.reserve(turnHasSuccess.size());
        for (const auto& summary : turnHasSuccess) {
            turns.push_back(summary.first);
        }
        std::sort(turns.begin(), turns.end());
        QStringList icons;
        for (quint32 turn : turns) {
            icons.append(turnHasSuccess[turn] ? QStringLiteral("●") : QStringLiteral("○"));
        }
        group.resultsSummary = icons.join(QStringLiteral(" "));
        groups.push_back(group);
    }

    std::sort(groups.begin(), groups.end(), [](const GroupRow& a, const GroupRow& b) {
        return a.createdAt > b.createdAt;
    });
    return groups;
}

std::vector<ExplorerRunsCoordinator::JobViewRow> ExplorerRunsCoordinator::buildJobsForWaves(const std::vector<qint64>& waveJobSetIds) const
{
    std::vector<JobViewRow> out;
    for (qint64 waveJobSetId : waveJobSetIds) {
        auto jobs = JobsRepo::GetByJobSet(waveJobSetId);
        if (!jobs.ok) {
            continue;
        }

        std::vector<qint64> ids;
        ids.reserve(jobs.value.size());
        for (const JobRow& job : jobs.value) {
            ids.push_back(job.job_id);
        }
        const auto resultMap = loadJobResultsMap(ids);

        for (const JobRow& job : jobs.value) {
            JobResultSummary summary{};
            const auto it = resultMap.find(job.job_id);
            if (it != resultMap.end()) {
                summary = it->second;
            }
            out.push_back(JobViewRow{
                job.job_id,
                QString::fromStdString(job.state),
                summary.fakeUsed,
                summary.deltaVi,
                summary.viStart,
                summary.viEnd,
                summary.rngSeed,
                summary.battleOutcome,
                summary.planMaterializeErr,
                summary.predPassed,
                summary.predTotal,
                summary.predAbortRun,
                summary.hasResults
            });
        }
    }
    return out;
}

std::unordered_map<qint64, ExplorerRunsCoordinator::JobResultSummary> ExplorerRunsCoordinator::loadJobResultsMap(const std::vector<qint64>& ids) const
{
    std::unordered_map<qint64, JobResultSummary> out;
    if (ids.empty()) {
        return out;
    }

    auto res = JobEventsRepo::GetLatestPayloadByJobs(ids, "RESULTS");
    if (!res.ok) {
        return out;
    }

    for (const JobEventsRepo::JobIdPayload& row : res.value) {
        if (!row.payload.has_value()) {
            continue;
        }
        IniDoc ini = IniDoc::parse(*row.payload);
        if (!ini.has_section(ResultsIni::SECTION_NAME)) {
            continue;
        }
        const ResultsIni results = ResultsIni::from_section(ini);
        JobResultSummary summary{};
        summary.hasResults = true;
        summary.fakeUsed = results.fake_attacks_used;
        summary.viStart = results.vi_start;
        summary.viEnd = results.vi_end;
        summary.deltaVi = results.vi_end >= results.vi_start ? (results.vi_end - results.vi_start) : 0;
        summary.rngSeed = results.rng_seed;
        summary.battleOutcome = results.battle_outcome;
        summary.planMaterializeErr = results.plan_materialize_err;
        summary.predPassed = results.pred_passed;
        summary.predTotal = results.pred_total;
        summary.predAbortRun = results.pred_abort_run;
        summary.successOutcome = isSuccessOutcome(summary.battleOutcome);
        out[row.job_id] = summary;
    }

    return out;
}

QString ExplorerRunsCoordinator::buildProgressLog(qint64 jobId) const
{
    auto progress = JobEventsRepo::ListByJobAndKind(jobId, "PROGRESS");
    if (!progress.ok) {
        return QStringLiteral("(no progress)");
    }

    QStringList lines;
    for (const JobEventRow& row : progress.value) {
        if (row.payload.has_value()) {
            lines.append(QString::fromStdString(*row.payload));
        }
    }
    return lines.isEmpty() ? QStringLiteral("(no progress)") : lines.join(QStringLiteral("\n"));
}

QString ExplorerRunsCoordinator::buildBlueprintInfo(qint64 jobId) const
{
    auto job = JobsRepo::Get(jobId);
    if (!job.ok) {
        return QStringLiteral("(no blueprint info)");
    }
    if (!job.value.vm_kv.has_value()) {
        return QStringLiteral("(job has no vm_kv)");
    }

    IniDoc jobIni = IniDoc::parse(*job.value.vm_kv);
    const JobIni singleTurnJob = JobIni::from_section(jobIni);

    QString initialFrame = QStringLiteral("(none)");
    if (singleTurnJob.delta_seed_id > 0) {
        auto deltaSeed = DeltaSeedRepo::Get(singleTurnJob.delta_seed_id);
        if (deltaSeed.ok && deltaSeed.value.has_value()) {
            initialFrame = QString::fromStdString(DescribeFrameCompact(deltaSeed.value->input));
        }
    }

    QString planSummary = QStringLiteral("(unknown)");
    if (singleTurnJob.plan_id > 0 && singleTurnJob.turn_index > 0) {
        soa::battle::actions::TurnPlan turnPlan{ .fake_attack_count = singleTurnJob.fake_attacks_this_turn };
        auto actors = BattlePlanTurnRepo::ListActorsByPlan(singleTurnJob.plan_id, static_cast<int32_t>(singleTurnJob.turn_index - 1));
        if (actors.ok) {
            for (const auto& actor : actors.value) {
                auto atom = BattlePlanAtomRepo::Get(actor.atom_id);
                if (!atom.ok) {
                    continue;
                }
                soa::battle::actions::ActionPlan actionPlan{
                    .actor_slot = static_cast<uint8_t>(atom.value.actor_slot),
                    .macro = static_cast<soa::battle::actions::BattleAction>(atom.value.action_type)
                };
                if (atom.value.target_slot >= -1 && atom.value.target_slot < 12) {
                    actionPlan.params.target_slot = static_cast<uint8_t>(atom.value.target_slot);
                }
                if (atom.value.param_item_id >= 0) {
                    actionPlan.params.item_id = static_cast<uint16_t>(atom.value.param_item_id);
                }
                turnPlan.spec.push_back(std::move(actionPlan));
            }
        }
        planSummary = QString::fromStdString(soa::battle::actions::get_turn_plan_summary(turnPlan));
    }

    return QStringLiteral("delta_seed_id: %1\ninitial_frame_input: %2\nbattle_action_plan: %3")
        .arg(singleTurnJob.delta_seed_id)
        .arg(initialFrame)
        .arg(planSummary);
}

std::optional<QString> ExplorerRunsCoordinator::fetchResultsIniText(qint64 jobId) const
{
    auto results = DataService::FetchJobResultsIniAsync(jobId).get();
    if (!results.ok) {
        return std::nullopt;
    }
    return QString::fromStdString(results.value.to_string_sorted());
}

ExplorerRunsCoordinator::WaveMeta ExplorerRunsCoordinator::parseWaveMeta(const std::optional<std::string>& text) const
{
    WaveMeta meta{};
    if (!text.has_value() || text->empty()) {
        return meta;
    }

    IniDoc ini = IniDoc::parse(*text);
    constexpr const char* section = "BattleSingleTurn.WaveMeta";
    if (!ini.has_section(section)) {
        return meta;
    }

    IniKV kv = ini.section_kv(section);
    meta.rootGroupId = kv.get_i64("root_group_id", -1);
    meta.waveTurn = kv.get_u32("wave_turn", 1);
    meta.settingsId = kv.get_i64("settings_id", -1);
    meta.seedProbeId = kv.get_i64("seed_probe_id", -1);
    meta.settingsName = kv.get("settings_name", "");
    meta.tasMovieId = kv.get_i64("tas_movie_id", -1);
    return meta;
}

qint64 ExplorerRunsCoordinator::resolveRoot(const JobSetRow& js) const
{
    const WaveMeta meta = parseWaveMeta(js.meta_text);
    if (meta.rootGroupId > 0) {
        return meta.rootGroupId;
    }

    qint64 current = js.job_set_id;
    while (true) {
        auto parent = JobSetsRepo::GetParent(current);
        if (!parent.ok || !parent.value.has_value()) {
            break;
        }
        current = *parent.value;
    }
    return current;
}

QString ExplorerRunsCoordinator::summarizeStates(const std::vector<JobRow>& jobs) const
{
    int queued = 0;
    int running = 0;
    int failures = 0;
    int winners = 0;
    int duplicates = 0;
    for (const JobRow& job : jobs) {
        if (job.state == "QUEUED" || job.state == "CLAIMED") {
            ++queued;
        } else if (job.state == "RUNNING") {
            ++running;
        } else if (job.state == "FAILED" || job.state == "CANCELED") {
            ++failures;
        } else if (job.state == "SUCCEEDED_WINNER") {
            ++winners;
        } else if (job.state == "SUCCEEDED_DUPLICATE") {
            ++duplicates;
        }
    }

    return QStringLiteral("Q:%1 R:%2 F:%3 W:%4 D:%5")
        .arg(queued)
        .arg(running)
        .arg(failures)
        .arg(winners)
        .arg(duplicates);
}

bool ExplorerRunsCoordinator::isSuccessOutcome(quint32 battleOutcome) const
{
    return battleOutcome == static_cast<quint32>(simcore::battle::Outcome::ReachedNextTurn);
}

bool ExplorerRunsCoordinator::isWinnerState(const QString& state) const
{
    return state == QStringLiteral("SUCCEEDED_WINNER");
}
