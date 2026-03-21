#include "JobsController.h"

#include <QtCore/QTimer>

#include <exception>
#include <utility>

using simcore::db::DataService;
using simcore::db::ProgramKindKV;

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
    connect(&kindsWatcher_, &QFutureWatcher<ProgramKindsResult>::finished, this, [this]() {
        try {
            const auto result = kindsWatcher_.result();
            kindsInFlight_ = false;
            setBusy(Operation::FetchKinds, false);
            if (result.ok) {
                state_.programNames.clear();
                for (const ProgramKindKV& kind : result.value) {
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

    connect(&pageWatcher_, &QFutureWatcher<JobPageResult>::finished, this, [this]() {
        try {
            const auto result = pageWatcher_.result();
            pageInFlight_ = false;
            if (result.ok) {
                state_.page = result.value.page;
                state_.progressSummary = result.value.progressSummary;
                state_.lastRefresh = QDateTime::currentDateTime();
                state_.errorMessage.clear();
                state_.infoMessage.clear();

                bool foundSelection = false;
                for (const JobLite& item : state_.page.items) {
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
                state_.progressSummary.clear();
                state_.detail = {};
                state_.errorMessage = QStringLiteral("Jobs failed: %1").arg(QString::fromStdString(result.error.message));
            }
        } catch (...) {
            pageInFlight_ = false;
            state_.page = {};
            state_.progressSummary.clear();
            state_.detail = {};
            state_.errorMessage = describeException("Jobs failed");
        }
        emitStateChanged();
    });

    connect(&detailWatcher_, &QFutureWatcher<JobDetailResult>::finished, this, [this]() {
        try {
            const auto result = detailWatcher_.result();
            detailInFlight_ = false;
            state_.detail.loading = false;
            if (result.ok && state_.selectedJobId == detailRequestJobId_) {
                state_.detail.jobId = detailRequestJobId_;
                state_.detail.payloadText = result.value.payloadText;
                state_.detail.resultsText = result.value.resultsText;
                state_.detail.decodedProgressText = result.value.decodedProgressText;
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

    auto finishAction = [this](QFutureWatcher<VoidResult>& watcher, bool& flag, const QString& successMessage, const char* failurePrefix) {
        try {
            const auto result = watcher.result();
            flag = false;
            setBusy(Operation::Requeue, false);
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
            setBusy(Operation::Requeue, false);
            state_.errorMessage = describeException(failurePrefix);
            emitStateChanged();
        }
    };

    connect(&requeueWatcher_, &QFutureWatcher<VoidResult>::finished, this, [this, finishAction]() mutable {
        finishAction(requeueWatcher_, requeueInFlight_, QStringLiteral("Requeued job %1.").arg(actionJobId_), "Requeue failed");
    });
    connect(&cancelWatcher_, &QFutureWatcher<VoidResult>::finished, this, [this, finishAction]() mutable {
        finishAction(cancelWatcher_, cancelInFlight_, QStringLiteral("Canceled job %1.").arg(actionJobId_), "Cancel failed");
    });
    connect(&bumpWatcher_, &QFutureWatcher<VoidResult>::finished, this, [this, finishAction]() mutable {
        finishAction(bumpWatcher_, bumpInFlight_, QStringLiteral("Updated priority for job %1.").arg(actionJobId_), "Priority bump failed");
    });
    connect(&restartWatcher_, &QFutureWatcher<VoidResult>::finished, this, [this, finishAction]() mutable {
        finishAction(restartWatcher_, restartInFlight_, QStringLiteral("Restarted job %1.").arg(actionJobId_), "Restart failed");
    });

    refreshTimer_ = new QTimer(this);
    connect(refreshTimer_, &QTimer::timeout, this, [this]() {
        if (canAutoRefresh()) {
            requestRefresh();
        }
    });
    refreshTimer_->start(state_.refreshSeconds * 1000);
}

const JobsController::ViewState& JobsController::viewState() const { return state_; }

void JobsController::loadInitial()
{
    if (initialLoadStarted_) return;
    initialLoadStarted_ = true;
    kickKindsFetch();
    kickPageFetch();
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
    refreshTimer_->start(state_.refreshSeconds * 1000);
    kickPageFetch();
    emitStateChanged();
}

void JobsController::setAutoRefreshEnabled(bool enabled) { state_.autoRefresh = enabled; emitStateChanged(); }
void JobsController::setRefreshSeconds(int seconds) { state_.refreshSeconds = seconds; refreshTimer_->start(seconds * 1000); emitStateChanged(); }
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
    const JobLite* job = selectedJob();
    if (!job || requeueInFlight_ || state_.actionsBusy) return;
    actionJobId_ = job->job_id;
    requeueInFlight_ = true;
    setBusy(Operation::Requeue, true);
    requeueWatcher_.setFuture(runDataServiceCall([jobId = job->job_id]() { return DataService::RequeueJobAsync(jobId).get(); }));
}

void JobsController::cancelSelectedJob()
{
    const JobLite* job = selectedJob();
    if (!job || cancelInFlight_ || state_.actionsBusy) return;
    actionJobId_ = job->job_id;
    cancelInFlight_ = true;
    setBusy(Operation::Cancel, true);
    cancelWatcher_.setFuture(runDataServiceCall([jobId = job->job_id]() { return DataService::CancelJobAsync(jobId).get(); }));
}

void JobsController::bumpSelectedJobPriority(int delta)
{
    const JobLite* job = selectedJob();
    if (!job || bumpInFlight_ || state_.actionsBusy) return;
    actionJobId_ = job->job_id;
    bumpInFlight_ = true;
    setBusy(Operation::Bump, true);
    bumpWatcher_.setFuture(runDataServiceCall([jobId = job->job_id, delta]() { return DataService::BumpPriorityAsync(jobId, delta).get(); }));
}

void JobsController::restartSelectedFailedJob(std::optional<QString> iniOverride)
{
    const JobLite* job = selectedJob();
    if (!job || restartInFlight_ || state_.actionsBusy || job->state != "FAILED") return;
    actionJobId_ = job->job_id;
    restartInFlight_ = true;
    setBusy(Operation::Restart, true);
    restartWatcher_.setFuture(runDataServiceCall([jobId = job->job_id, iniOverride]() {
        if (iniOverride.has_value()) {
            auto setResult = DataService::SetJobVmKvAsync(jobId, iniOverride->toStdString()).get();
            if (!setResult.ok) return setResult;
        }
        return DataService::RestartFailedJobAsync(jobId).get();
    }));
}

void JobsController::kickKindsFetch()
{
    if (kindsInFlight_) return;
    kindsInFlight_ = true;
    setBusy(Operation::FetchKinds, true);
    kindsWatcher_.setFuture(runDataServiceCall([]() { return DataService::ListProgramKindsAsync().get(); }));
}

void JobsController::kickPageFetch()
{
    if (pageInFlight_) return;
    pageInFlight_ = true;
    state_.errorMessage.clear();
    PagedQuery<> query; query.before = before_; query.after = after_; query.limit = state_.pageLimit;
    pageWatcher_.setFuture(runDataServiceCall([scope = state_.scope, query]() -> JobPageResult {
        auto pageResult = DataService::FetchJobsPageAsync(scope, query.before, query.after, query.limit).get();
        if (!pageResult.ok) return JobPageResult::Err(pageResult.error);
        JobPageBundle bundle{};
        bundle.page = pageResult.value;
        std::vector<int64_t> ids;
        ids.reserve(bundle.page.items.size());
        for (const JobLite& job : bundle.page.items) ids.push_back(job.job_id);
        auto progressResult = DataService::BulkLatestProgressByJobsAsync(ids).get();
        if (!progressResult.ok) return JobPageResult::Err(progressResult.error);
        for (const auto& item : progressResult.value) {
            if (item.payload.has_value()) {
                QString summary = QString::fromStdString(*item.payload);
                if (summary.size() > 120) summary = summary.left(120);
                bundle.progressSummary.insert(item.job_id, summary);
            }
        }
        return JobPageResult::Ok(std::move(bundle));
    }));
    emitStateChanged();
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
        JobEventsListScope scope{}; scope.job_id = jobId;
        PagedQuery<> query; query.limit = 128;
        auto eventsResult = DataService::FetchJobEventsPage(scope, query).get();
        if (!eventsResult.ok) return JobDetailResult::Err(eventsResult.error);
        bundle.events = eventsResult.value.items;

        auto payloadResult = DataService::FetchJobVmKvIniAsync(jobId).get();
        if (payloadResult.ok) bundle.payloadText = QString::fromStdString(payloadResult.value.to_string_preserve_order());

        auto resultsResult = DataService::FetchJobResultsIniAsync(jobId).get();
        if (resultsResult.ok) bundle.resultsText = QString::fromStdString(resultsResult.value.to_string_preserve_order());

        auto decodedResult = DataService::FetchDecodedProgressAsync(jobId).get();
        if (decodedResult.ok) bundle.decodedProgressText = QString::fromStdString(decodedResult.value);

        auto artifactsResult = DataService::FetchJobArtifactRefsAsync(jobId).get();
        if (artifactsResult.ok) bundle.artifacts = std::move(artifactsResult.value);

        return JobDetailResult::Ok(std::move(bundle));
    }));
    emitStateChanged();
}

void JobsController::setBusy(Operation, bool)
{
    state_.actionsBusy = requeueInFlight_ || cancelInFlight_ || bumpInFlight_ || restartInFlight_;
    emitStateChanged();
}

bool JobsController::canAutoRefresh() const
{
    return state_.autoRefresh && !before_.has_value() && !after_.has_value() && !pageInFlight_ && !state_.actionsBusy;
}

bool JobsController::anyWorkInFlight() const
{
    return kindsInFlight_ || pageInFlight_ || detailInFlight_ || requeueInFlight_ || cancelInFlight_ || bumpInFlight_ || restartInFlight_;
}

const JobLite* JobsController::selectedJob() const
{
    for (const JobLite& job : state_.page.items) if (job.job_id == state_.selectedJobId) return &job;
    return nullptr;
}

QString JobsController::programKindLabel(int id) const { return state_.programNames.value(id, QStringLiteral("kind %1").arg(id)); }

void JobsController::emitStateChanged()
{
    state_.actionsBusy = requeueInFlight_ || cancelInFlight_ || bumpInFlight_ || restartInFlight_;
    state_.loading = pageInFlight_ || kindsInFlight_ || detailInFlight_ || anyWorkInFlight();
    emit stateChanged();
}
