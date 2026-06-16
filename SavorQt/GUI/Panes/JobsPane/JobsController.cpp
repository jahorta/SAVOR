#include "JobsController.h"

#include "DB/SavorDbJobService.h"

#include <QtCore/QSettings>

#include <exception>
#include <algorithm>
#include <utility>

using savorqt::db::SavorDbJobService;

namespace {
constexpr auto kSettingsGroup = "JobsPane";
constexpr auto kProgramKindKey = "program_kind";
constexpr auto kStateFilterKey = "state_filter";
constexpr auto kJobSetIdKey = "job_set_id";
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

template <typename AsyncCall>
auto runDataServiceCall(AsyncCall&& asyncCall)
{
    return QtConcurrent::run([call = std::forward<AsyncCall>(asyncCall)]() mutable {
        return call();
    });
}
}

JobsController::JobsController(QObject* parent)
    : QObject(parent)
{
    loadSettings();
    syncFetchStateFromView();

    connect(&kindsWatcher_, &QFutureWatcher<ProgramKindsResult>::finished, this, [this]() {
        try {
            const auto result = kindsWatcher_.result();
            kindsInFlight_ = false;
            setBusy(Operation::FetchKinds, false);
            if (result.ok) {
                state_.programNames.clear();
                for (const savor::db::UiProgramKind& kind : result.value) {
                    state_.programNames.insert(kind.id, QString::fromStdString(kind.name));
                }
                state_.errorMessage.clear();
            } else {
                state_.errorMessage = QStringLiteral("Program kinds failed: %1").arg(QString::fromStdString(result.error.message));
            }
        } catch (...) {
            kindsInFlight_ = false;
            setBusy(Operation::FetchKinds, false);
            state_.errorMessage = describeException("Program kinds failed");
        }
        emitStateChanged();
    });

    pageRefreshPipeline_ = new savorqt::gui::AsyncRefreshPipeline<JobPageFetchRequest, JobPageResult>(this);
    pageRefreshPipeline_->setRefreshIntervalMs(state_.refreshSeconds * 1000);
    pageRefreshPipeline_->setAutoRefreshEnabled(state_.autoRefresh);
    pageRefreshPipeline_->setRequestBuilder([this](savorqt::gui::RefreshReason reason) -> std::optional<JobPageFetchRequest> {
        if (reason == savorqt::gui::RefreshReason::Auto && !canAutoRefresh()) {
            return std::nullopt;
        }
        pageInFlight_ = true;
        state_.errorMessage.clear();
        emitStateChanged();
        return JobPageFetchRequest{ fetchScope_, before_, after_, fetchPageLimit_ };
    });
    pageRefreshPipeline_->setLoadAndPrepare([](JobPageFetchRequest request) {
        return savorqt::gui::AsyncRefreshResult<JobPageResult>::Ok(
            SavorDbJobService::FetchJobsPage(request.scope, request.before, request.after, request.limit));
    });
    pageRefreshPipeline_->setApply([this](const JobPageResult& result, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        pageInFlight_ = false;
        if (result.ok) {
            state_.page = result.value;
            state_.lastRefresh = QDateTime::currentDateTime();
            state_.errorMessage.clear();
            state_.infoMessage.clear();

            bool foundSelection = false;
            for (const savor::db::UiJobSummary& item : state_.page.items) {
                if (item.job_id == state_.selectedJobId) {
                    foundSelection = true;
                    break;
                }
            }
            if (!foundSelection) {
                state_.selectedJobId = state_.page.items.empty() ? 0 : state_.page.items.front().job_id;
                state_.detail = {};
            }
            if (state_.selectedJobId > 0) {
                kickDetailFetch(state_.selectedJobId);
            } else {
                state_.detail = {};
            }
        } else {
            state_.page = {};
            state_.detail = {};
            state_.errorMessage = QStringLiteral("Jobs failed: %1").arg(QString::fromStdString(result.error.message));
        }
        emitStateChanged();
    });
    pageRefreshPipeline_->setApplyError([this](const QString& error, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        pageInFlight_ = false;
        state_.page = {};
        state_.detail = {};
        state_.errorMessage = error;
        emitStateChanged();
    });

    connect(&detailWatcher_, &QFutureWatcher<JobDetailResult>::finished, this, [this]() {
        try {
            const auto result = detailWatcher_.result();
            detailInFlight_ = false;
            state_.detail.loading = false;
            if (result.ok && state_.selectedJobId == detailRequestJobId_) {
                state_.detail.jobId = detailRequestJobId_;
                state_.detail.events = std::move(result.value.events);
                state_.detail.artifacts = std::move(result.value.artifacts);
                state_.detail.loaded = true;
                state_.errorMessage.clear();
            } else if (!result.ok) {
                state_.detail = {};
                state_.errorMessage = QStringLiteral("Job detail failed: %1").arg(QString::fromStdString(result.error.message));
            }
        } catch (...) {
            detailInFlight_ = false;
            state_.detail = {};
            state_.errorMessage = describeException("Job detail failed");
        }
        emitStateChanged();
    });

    connect(&inputIniWatcher_, &QFutureWatcher<InputIniResult>::finished, this, [this]() {
        try {
            const auto result = inputIniWatcher_.result();
            inputIniInFlight_ = false;
            state_.detail.inputIniLoading = false;
            if (result.ok && state_.selectedJobId == inputIniRequestJobId_) {
                state_.detail.jobId = inputIniRequestJobId_;
                state_.detail.inputIniText = result.value;
                state_.detail.inputIniLoaded = true;
                state_.errorMessage.clear();
            } else if (!result.ok) {
                state_.errorMessage = QStringLiteral("Input INI failed: %1").arg(QString::fromStdString(result.error.message));
            }
        } catch (...) {
            inputIniInFlight_ = false;
            state_.detail.inputIniLoading = false;
            state_.errorMessage = describeException("Input INI failed");
        }
        emitStateChanged();
    });

    auto finishAction = [this](QFutureWatcher<VoidResult>& watcher, bool& flag, Operation operation, const QString& successMessage, const char* failurePrefix) {
        try {
            const auto result = watcher.result();
            flag = false;
            setBusy(operation, false);
            if (result.ok) {
                state_.infoMessage = successMessage;
                before_.reset();
                after_.reset();
                kickPageFetch();
            } else {
                state_.errorMessage = QStringLiteral("%1: %2").arg(QString::fromUtf8(failurePrefix), QString::fromStdString(result.error.message));
                emitStateChanged();
            }
        } catch (...) {
            flag = false;
            setBusy(operation, false);
            state_.errorMessage = describeException(failurePrefix);
            emitStateChanged();
        }
    };

    connect(&requeueWatcher_, &QFutureWatcher<VoidResult>::finished, this, [this, finishAction]() mutable {
        finishAction(requeueWatcher_, requeueInFlight_, Operation::Requeue, QStringLiteral("Requeued job %1.").arg(actionJobId_), "Requeue failed");
    });
    connect(&cancelWatcher_, &QFutureWatcher<VoidResult>::finished, this, [this, finishAction]() mutable {
        finishAction(cancelWatcher_, cancelInFlight_, Operation::Cancel, QStringLiteral("Canceled job %1.").arg(actionJobId_), "Cancel failed");
    });
    connect(&restartWatcher_, &QFutureWatcher<VoidResult>::finished, this, [this, finishAction]() mutable {
        finishAction(restartWatcher_, restartInFlight_, Operation::Restart, QStringLiteral("Restarted job %1.").arg(actionJobId_), "Restart failed");
    });
    pageRefreshPipeline_->setActive(pageActive_);
}

const JobsController::ViewState& JobsController::viewState() const { return state_; }

void JobsController::loadInitial()
{
    if (initialLoadStarted_ || !pageActive_) return;
    initialLoadStarted_ = true;
    kickKindsFetch();
    kickPageFetch();
}

void JobsController::setPageActive(bool active)
{
    if (pageActive_ == active) {
        return;
    }

    pageActive_ = active;
    if (pageRefreshPipeline_ != nullptr) {
        pageRefreshPipeline_->setActive(pageActive_);
        pageRefreshPipeline_->setAutoRefreshEnabled(state_.autoRefresh);
    }
    loadInitial();
}

void JobsController::applyFilters(const std::optional<int>& programKind, const std::optional<QString>& stateFilter, const std::optional<qint64>& jobSetId, int pageLimit)
{
    state_.scope = {};
    state_.scope.program_kind = programKind;
    if (stateFilter.has_value() && !stateFilter->isEmpty()) {
        state_.scope.states = { stateFilter->toStdString() };
    }
    state_.scope.job_set_id = jobSetId;
    state_.pageLimit = pageLimit;
    before_.reset();
    after_.reset();
    state_.selectedJobId = 0;
    state_.detail = {};
    state_.errorMessage.clear();
    state_.infoMessage.clear();
    syncFetchStateFromView();
    persistSettings();
    kickPageFetch();
}

void JobsController::resetFilters()
{
    state_.scope = {};
    state_.pageLimit = 100;
    state_.autoRefresh = true;
    state_.refreshSeconds = 2;
    before_.reset();
    after_.reset();
    state_.selectedJobId = 0;
    state_.detail = {};
    state_.errorMessage.clear();
    state_.infoMessage.clear();
    syncFetchStateFromView();
    persistSettings();
    if (pageRefreshPipeline_ != nullptr) {
        pageRefreshPipeline_->setAutoRefreshEnabled(state_.autoRefresh);
        pageRefreshPipeline_->setRefreshIntervalMs(state_.refreshSeconds * 1000);
    }
    kickPageFetch();
    emitStateChanged();
}

void JobsController::setAutoRefreshEnabled(bool enabled)
{
    state_.autoRefresh = enabled;
    persistSettings();
    if (pageRefreshPipeline_ != nullptr) {
        pageRefreshPipeline_->setAutoRefreshEnabled(enabled);
    }
    emitStateChanged();
}

void JobsController::setRefreshSeconds(int seconds)
{
    state_.refreshSeconds = seconds;
    persistSettings();
    if (pageRefreshPipeline_ != nullptr) {
        pageRefreshPipeline_->setRefreshIntervalMs(seconds * 1000);
    }
    emitStateChanged();
}
void JobsController::requestRefresh() { before_.reset(); after_.reset(); kickPageFetch(); }
void JobsController::requestNextPage() { if (state_.page.next) { before_ = state_.page.next; after_.reset(); kickPageFetch(); } }
void JobsController::requestPreviousPage() { if (state_.page.prev) { after_ = state_.page.prev; before_.reset(); kickPageFetch(); } }

void JobsController::selectJob(qint64 jobId)
{
    if (jobId <= 0 || state_.selectedJobId == jobId) return;
    state_.selectedJobId = jobId;
    state_.detail = {};
    kickDetailFetch(jobId, true);
    emitStateChanged();
}

void JobsController::refreshSelectedJobDetail() { if (state_.selectedJobId > 0) kickDetailFetch(state_.selectedJobId, true); }

void JobsController::requeueSelectedJob()
{
    const savor::db::UiJobSummary* job = selectedJob();
    if (!job || requeueInFlight_ || state_.actionsBusy) return;
    actionJobId_ = job->job_id;
    requeueInFlight_ = true;
    setBusy(Operation::Requeue, true);
    requeueWatcher_.setFuture(runDataServiceCall([jobId = job->job_id]() { return SavorDbJobService::RequeueJob(jobId); }));
}

void JobsController::cancelSelectedJob()
{
    const savor::db::UiJobSummary* job = selectedJob();
    if (!job || cancelInFlight_ || state_.actionsBusy) return;
    actionJobId_ = job->job_id;
    cancelInFlight_ = true;
    setBusy(Operation::Cancel, true);
    cancelWatcher_.setFuture(runDataServiceCall([jobId = job->job_id]() { return SavorDbJobService::CancelJob(jobId); }));
}

void JobsController::restartSelectedFailedJob()
{
    const savor::db::UiJobSummary* job = selectedJob();
    if (!job || restartInFlight_ || state_.actionsBusy || job->state != "FAILED") return;
    actionJobId_ = job->job_id;
    restartInFlight_ = true;
    setBusy(Operation::Restart, true);
    restartWatcher_.setFuture(runDataServiceCall([jobId = job->job_id]() {
        return SavorDbJobService::RestartFailedJob(jobId);
    }));
}

void JobsController::kickKindsFetch()
{
    if (kindsInFlight_) return;
    kindsInFlight_ = true;
    setBusy(Operation::FetchKinds, true);
    kindsWatcher_.setFuture(runDataServiceCall([]() { return SavorDbJobService::ListProgramKinds(); }));
}

void JobsController::kickPageFetch()
{
    if (pageRefreshPipeline_ != nullptr) {
        pageRefreshPipeline_->requestRefresh(savorqt::gui::RefreshReason::Manual);
    }
}

void JobsController::kickDetailFetch(qint64 jobId, bool force)
{
    if (jobId <= 0) return;
    if (detailInFlight_) return;
    if (!force && state_.detail.loaded && state_.detail.jobId == jobId) return;
    detailInFlight_ = true;
    detailRequestJobId_ = jobId;
    state_.detail.loading = true;
    state_.detail.loaded = false;
    state_.detail.jobId = jobId;
    detailWatcher_.setFuture(runDataServiceCall([jobId]() -> JobDetailResult {
        JobDetailBundle bundle{};
        auto eventsResult = SavorDbJobService::FetchJobEvents(jobId, 128);
        if (!eventsResult.ok) return JobDetailResult::Err(eventsResult.error);
        bundle.events = std::move(eventsResult.value);

        auto artifactsResult = SavorDbJobService::FetchJobArtifactRefs(jobId);
        if (artifactsResult.ok) bundle.artifacts = std::move(artifactsResult.value);

        return JobDetailResult::Ok(std::move(bundle));
    }));
    emitStateChanged();
}

void JobsController::loadSelectedJobInputIni()
{
    if (state_.selectedJobId <= 0 || inputIniInFlight_) return;
    inputIniInFlight_ = true;
    inputIniRequestJobId_ = state_.selectedJobId;
    state_.detail.jobId = state_.selectedJobId;
    state_.detail.inputIniLoading = true;
    state_.errorMessage.clear();
    const qint64 jobId = state_.selectedJobId;
    inputIniWatcher_.setFuture(runDataServiceCall([jobId]() -> InputIniResult {
        auto result = SavorDbJobService::FetchJobInputIni(jobId);
        if (!result.ok) return InputIniResult::Err(result.error);
        return InputIniResult::Ok(QString::fromStdString(result.value));
    }));
    emitStateChanged();
}

void JobsController::setBusy(Operation, bool)
{
    state_.actionsBusy = requeueInFlight_ || cancelInFlight_ || restartInFlight_;
    emitStateChanged();
}

bool JobsController::canAutoRefresh() const
{
    return pageActive_ && state_.autoRefresh && !before_.has_value() && !after_.has_value() && !pageInFlight_ && !state_.actionsBusy;
}

bool JobsController::anyWorkInFlight() const
{
    return kindsInFlight_ || pageInFlight_ || detailInFlight_ || inputIniInFlight_ || requeueInFlight_ || cancelInFlight_ || restartInFlight_;
}

const savor::db::UiJobSummary* JobsController::selectedJob() const
{
    for (const savor::db::UiJobSummary& job : state_.page.items) if (job.job_id == state_.selectedJobId) return &job;
    return nullptr;
}

QString JobsController::programKindLabel(int id) const { return state_.programNames.value(id, QStringLiteral("kind %1").arg(id)); }

void JobsController::emitStateChanged()
{
    state_.actionsBusy = requeueInFlight_ || cancelInFlight_ || restartInFlight_;
    state_.loading = pageInFlight_ || kindsInFlight_ || detailInFlight_ || inputIniInFlight_ || anyWorkInFlight();
    emit stateChanged();
}

void JobsController::loadSettings()
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);

    const QVariant programKind = settings.value(kProgramKindKey);
    state_.scope.program_kind = programKind.isValid() ? std::optional<int>(programKind.toInt()) : std::nullopt;
    const QString stateFilter = settings.value(kStateFilterKey).toString().trimmed();
    if (!stateFilter.isEmpty()) {
        state_.scope.states = { stateFilter.toStdString() };
    }
    const QVariant jobSetId = settings.value(kJobSetIdKey);
    state_.scope.job_set_id = jobSetId.isValid() ? std::optional<qint64>(jobSetId.toLongLong()) : std::nullopt;
    state_.pageLimit = (std::max)(1, settings.value(kPageLimitKey, state_.pageLimit).toInt());
    state_.autoRefresh = settings.value(kAutoRefreshKey, state_.autoRefresh).toBool();
    state_.refreshSeconds = (std::max)(1, settings.value(kRefreshSecondsKey, state_.refreshSeconds).toInt());

    settings.endGroup();
}

void JobsController::persistSettings() const
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);

    if (state_.scope.program_kind.has_value()) {
        settings.setValue(kProgramKindKey, *state_.scope.program_kind);
    } else {
        settings.remove(kProgramKindKey);
    }
    if (!state_.scope.states.empty()) {
        settings.setValue(kStateFilterKey, QString::fromStdString(*state_.scope.states.begin()));
    } else {
        settings.remove(kStateFilterKey);
    }
    if (state_.scope.job_set_id.has_value()) {
        settings.setValue(kJobSetIdKey, *state_.scope.job_set_id);
    } else {
        settings.remove(kJobSetIdKey);
    }
    settings.setValue(kPageLimitKey, state_.pageLimit);
    settings.setValue(kAutoRefreshKey, state_.autoRefresh);
    settings.setValue(kRefreshSecondsKey, state_.refreshSeconds);

    settings.endGroup();
}

void JobsController::syncFetchStateFromView()
{
    fetchScope_ = state_.scope;
    fetchPageLimit_ = state_.pageLimit;
}
