#include "ExplorerRunsController.h"

#include <QtCore/QSettings>
#include <QtCore/QTimer>

#include <algorithm>
#include <exception>
#include <utility>

using savorqt::db::ExplorerRunGroupQuery;
using savorqt::db::SavorDbExplorerRunService;

namespace {
constexpr auto kSettingsGroup = "ExplorerRunsPane";
constexpr auto kPageLimitKey = "page_limit";
constexpr auto kAutoRefreshKey = "auto_refresh";
constexpr auto kRefreshSecondsKey = "refresh_seconds";

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
}

ExplorerRunsController::ExplorerRunsController(QObject* parent)
    : QObject(parent)
{
    loadSettings();

    connect(&groupsWatcher_, &QFutureWatcher<GroupPageResult>::finished, this, [this]() {
        const bool shouldRefetch = pendingGroupsFetch_;
        pendingGroupsFetch_ = false;
        try {
            const auto result = groupsWatcher_.result();
            groupsInFlight_ = false;
            state_.loadingGroups = false;
            if (result.ok) {
                state_.groupPage = result.value;
                state_.lastRefresh = QDateTime::currentDateTime();
                state_.errorMessage.clear();
                state_.infoMessage.clear();

                bool selectionStillVisible = false;
                for (const auto& group : state_.groupPage.groups) {
                    if (group.job_set_id == state_.selectedJobSetId) {
                        selectionStillVisible = true;
                        break;
                    }
                }
                if (!selectionStillVisible) {
                    state_.selectedJobSetId = state_.groupPage.groups.empty() ? 0 : state_.groupPage.groups.front().job_set_id;
                    state_.selectedGroup.reset();
                    state_.selectedJob.reset();
                    state_.selectedJobId = 0;
                }
                if (state_.selectedJobSetId > 0) {
                    kickGroupDetailFetch(state_.selectedJobSetId);
                }
            } else {
                state_.groupPage = {};
                state_.selectedGroup.reset();
                state_.selectedJob.reset();
                state_.selectedJobId = 0;
                state_.errorMessage = describeError(result.error, QStringLiteral("Explorer runs failed"));
            }
        } catch (...) {
            groupsInFlight_ = false;
            state_.loadingGroups = false;
            state_.groupPage = {};
            state_.selectedGroup.reset();
            state_.selectedJob.reset();
            state_.selectedJobId = 0;
            state_.errorMessage = describeException("Explorer runs failed");
        }
        emitStateChanged();
        if (shouldRefetch) {
            kickGroupsFetch();
        }
    });

    connect(&groupDetailWatcher_, &QFutureWatcher<GroupDetailResult>::finished, this, [this]() {
        try {
            const auto result = groupDetailWatcher_.result();
            groupDetailInFlight_ = false;
            state_.loadingGroupDetail = false;
            if (result.ok && state_.selectedJobSetId == groupDetailRequestId_) {
                state_.selectedGroup = result.value;
                state_.errorMessage.clear();

                bool selectedJobVisible = false;
                for (const auto& job : state_.selectedGroup->jobs) {
                    if (job.job_id == state_.selectedJobId) {
                        selectedJobVisible = true;
                        break;
                    }
                }
                if (!selectedJobVisible) {
                    state_.selectedJobId = state_.selectedGroup->jobs.empty() ? 0 : state_.selectedGroup->jobs.front().job_id;
                    state_.selectedJob.reset();
                }
                if (state_.selectedJobId > 0) {
                    kickJobDetailFetch(state_.selectedJobId);
                }
            } else if (!result.ok) {
                state_.selectedGroup.reset();
                state_.selectedJob.reset();
                state_.selectedJobId = 0;
                state_.errorMessage = describeError(result.error, QStringLiteral("Explorer run detail failed"));
            }
        } catch (...) {
            groupDetailInFlight_ = false;
            state_.loadingGroupDetail = false;
            state_.selectedGroup.reset();
            state_.selectedJob.reset();
            state_.selectedJobId = 0;
            state_.errorMessage = describeException("Explorer run detail failed");
        }
        emitStateChanged();
    });

    connect(&jobDetailWatcher_, &QFutureWatcher<JobDetailResult>::finished, this, [this]() {
        try {
            const auto result = jobDetailWatcher_.result();
            jobDetailInFlight_ = false;
            state_.loadingJobDetail = false;
            if (result.ok && state_.selectedJobId == jobDetailRequestId_) {
                state_.selectedJob = result.value;
                state_.errorMessage.clear();
            } else if (!result.ok) {
                state_.selectedJob.reset();
                state_.errorMessage = describeError(result.error, QStringLiteral("Explorer job detail failed"));
            }
        } catch (...) {
            jobDetailInFlight_ = false;
            state_.loadingJobDetail = false;
            state_.selectedJob.reset();
            state_.errorMessage = describeException("Explorer job detail failed");
        }
        emitStateChanged();
    });

    refreshTimer_ = new QTimer(this);
    connect(refreshTimer_, &QTimer::timeout, this, [this]() {
        if (canAutoRefresh()) {
            requestRefresh();
        }
    });
}

const ExplorerRunsController::ViewState& ExplorerRunsController::viewState() const
{
    return state_;
}

void ExplorerRunsController::setPageActive(bool active)
{
    if (pageActive_ == active) {
        return;
    }
    pageActive_ = active;
    if (!pageActive_) {
        refreshTimer_->stop();
        return;
    }
    refreshTimer_->start(state_.refreshSeconds * 1000);
    loadInitial();
}

void ExplorerRunsController::requestRefresh()
{
    before_.reset();
    after_.reset();
    kickGroupsFetch();
}

void ExplorerRunsController::requestNextPage()
{
    if (!state_.groupPage.next.has_value()) {
        return;
    }
    before_ = state_.groupPage.next;
    after_.reset();
    kickGroupsFetch();
}

void ExplorerRunsController::requestPreviousPage()
{
    if (!state_.groupPage.prev.has_value()) {
        return;
    }
    after_ = state_.groupPage.prev;
    before_.reset();
    kickGroupsFetch();
}

void ExplorerRunsController::setAutoRefreshEnabled(bool enabled)
{
    state_.autoRefresh = enabled;
    persistSettings();
    emitStateChanged();
}

void ExplorerRunsController::setRefreshSeconds(int seconds)
{
    state_.refreshSeconds = (std::max)(1, seconds);
    persistSettings();
    if (pageActive_) {
        refreshTimer_->start(state_.refreshSeconds * 1000);
    }
    emitStateChanged();
}

void ExplorerRunsController::setPageLimit(int limit)
{
    state_.pageLimit = (std::max)(1, limit);
    persistSettings();
    before_.reset();
    after_.reset();
    kickGroupsFetch();
}

void ExplorerRunsController::selectGroup(qint64 jobSetId)
{
    if (jobSetId <= 0 || state_.selectedJobSetId == jobSetId) {
        return;
    }
    state_.selectedJobSetId = jobSetId;
    state_.selectedGroup.reset();
    state_.selectedJob.reset();
    state_.selectedJobId = 0;
    kickGroupDetailFetch(jobSetId);
    emitStateChanged();
}

void ExplorerRunsController::selectJob(qint64 jobId)
{
    if (jobId <= 0 || state_.selectedJobId == jobId) {
        return;
    }
    state_.selectedJobId = jobId;
    state_.selectedJob.reset();
    kickJobDetailFetch(jobId, true);
    emitStateChanged();
}

void ExplorerRunsController::refreshSelectedJob()
{
    if (state_.selectedJobId > 0) {
        kickJobDetailFetch(state_.selectedJobId, true);
    }
}

void ExplorerRunsController::loadInitial()
{
    if (initialLoadStarted_ || !pageActive_) {
        return;
    }
    initialLoadStarted_ = true;
    kickGroupsFetch();
}

void ExplorerRunsController::kickGroupsFetch()
{
    if (groupsInFlight_) {
        pendingGroupsFetch_ = true;
        return;
    }
    groupsInFlight_ = true;
    pendingGroupsFetch_ = false;
    state_.loadingGroups = true;
    state_.errorMessage.clear();

    ExplorerRunGroupQuery query{};
    query.before = before_;
    query.after = after_;
    query.limit = state_.pageLimit;
    groupsWatcher_.setFuture(QtConcurrent::run([query]() {
        return SavorDbExplorerRunService::ListGroups(query);
    }));
    emitStateChanged();
}

void ExplorerRunsController::kickGroupDetailFetch(qint64 jobSetId)
{
    if (groupDetailInFlight_ || jobSetId <= 0) {
        return;
    }
    groupDetailInFlight_ = true;
    state_.loadingGroupDetail = true;
    groupDetailRequestId_ = jobSetId;
    groupDetailWatcher_.setFuture(QtConcurrent::run([jobSetId]() {
        return SavorDbExplorerRunService::GetGroupDetail(jobSetId, 1000);
    }));
    emitStateChanged();
}

void ExplorerRunsController::kickJobDetailFetch(qint64 jobId, bool force)
{
    if (jobId <= 0 || jobDetailInFlight_) {
        return;
    }
    if (!force && state_.selectedJob.has_value() && state_.selectedJob->job.summary.job_id == jobId) {
        return;
    }
    jobDetailInFlight_ = true;
    state_.loadingJobDetail = true;
    jobDetailRequestId_ = jobId;
    jobDetailWatcher_.setFuture(QtConcurrent::run([jobId]() {
        return SavorDbExplorerRunService::GetJobDetail(jobId);
    }));
    emitStateChanged();
}

bool ExplorerRunsController::canAutoRefresh() const
{
    return pageActive_
        && state_.autoRefresh
        && !groupsInFlight_
        && !groupDetailInFlight_
        && !jobDetailInFlight_
        && !before_.has_value()
        && !after_.has_value();
}

void ExplorerRunsController::emitStateChanged()
{
    emit stateChanged();
}

void ExplorerRunsController::loadSettings()
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    state_.pageLimit = (std::max)(1, settings.value(kPageLimitKey, state_.pageLimit).toInt());
    state_.autoRefresh = settings.value(kAutoRefreshKey, state_.autoRefresh).toBool();
    state_.refreshSeconds = (std::max)(1, settings.value(kRefreshSecondsKey, state_.refreshSeconds).toInt());
    settings.endGroup();
}

void ExplorerRunsController::persistSettings() const
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    settings.setValue(kPageLimitKey, state_.pageLimit);
    settings.setValue(kAutoRefreshKey, state_.autoRefresh);
    settings.setValue(kRefreshSecondsKey, state_.refreshSeconds);
    settings.endGroup();
}

QString ExplorerRunsController::describeError(const savorqt::db::ServiceError& error, const QString& prefix) const
{
    return QStringLiteral("%1: %2").arg(prefix, QString::fromStdString(error.message));
}
