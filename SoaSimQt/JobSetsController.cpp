#include "JobSetsController.h"

#include <QtCore/QTimer>

using simcore::db::DataService;
using simcore::db::ProgramKindKV;

JobSetsController::JobSetsController(QObject* parent)
    : QObject(parent)
{
    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(150);
    connect(pollTimer_, &QTimer::timeout, this, &JobSetsController::consumePending);
    pollTimer_->start();

    refreshTimer_ = new QTimer(this);
    connect(refreshTimer_, &QTimer::timeout, this, [this]() {
        if (canAutoRefresh()) {
            requestRefresh();
        }
    });
    refreshTimer_->start(state_.refreshSeconds * 1000);
}

const JobSetsController::ViewState& JobSetsController::viewState() const
{
    return state_;
}

void JobSetsController::loadInitial()
{
    if (initialLoadStarted_) {
        return;
    }

    initialLoadStarted_ = true;
    state_.errorMessage.clear();
    state_.infoMessage.clear();
    kickKindsFetch();
    kickPageFetch();
}

void JobSetsController::applyFilters(const std::optional<int>& programKind, const std::optional<JobSetStateFilter>& stateFilter, int pageLimit)
{
    state_.scope = {};
    state_.scope.program_kind = programKind;
    state_.scope.state_filter = stateFilter;
    state_.pageLimit = pageLimit;
    before_.reset();
    after_.reset();
    state_.errorMessage.clear();
    state_.infoMessage.clear();
    state_.familyItems.clear();
    kickPageFetch();
}

void JobSetsController::resetFilters()
{
    state_.scope = {};
    state_.pageLimit = 100;
    state_.autoRefresh = true;
    state_.refreshSeconds = 2;
    before_.reset();
    after_.reset();
    state_.errorMessage.clear();
    state_.infoMessage.clear();
    state_.familyItems.clear();
    refreshTimer_->start(state_.refreshSeconds * 1000);
    kickPageFetch();
    emitStateChanged();
}

void JobSetsController::setAutoRefreshEnabled(bool enabled)
{
    state_.autoRefresh = enabled;
    emitStateChanged();
}

void JobSetsController::setRefreshSeconds(int seconds)
{
    state_.refreshSeconds = seconds;
    refreshTimer_->start(state_.refreshSeconds * 1000);
    emitStateChanged();
}

void JobSetsController::requestRefresh()
{
    before_.reset();
    after_.reset();
    kickPageFetch();
}

void JobSetsController::requestNextPage()
{
    if (!state_.page.next.has_value()) {
        return;
    }

    before_ = state_.page.next;
    after_.reset();
    state_.familyItems.clear();
    kickPageFetch();
}

void JobSetsController::requestPreviousPage()
{
    if (!state_.page.prev.has_value()) {
        return;
    }

    after_ = state_.page.prev;
    before_.reset();
    state_.familyItems.clear();
    kickPageFetch();
}

void JobSetsController::boostJobSetTree(qint64 jobSetId)
{
    if (boostInFlight_ || cancelInFlight_ || deleteInFlight_ || jobSetId <= 0) {
        return;
    }

    pendingActionJobSetId_ = jobSetId;
    boostInFlight_ = true;
    setBusy(Operation::Boost, true);
    boostFuture_ = DataService::BoostJobSetPriorityTreeAsync(jobSetId);
}

void JobSetsController::cancelQueuedForTree(qint64 jobSetId)
{
    if (boostInFlight_ || cancelInFlight_ || deleteInFlight_ || jobSetId <= 0) {
        return;
    }

    pendingActionJobSetId_ = jobSetId;
    cancelInFlight_ = true;
    setBusy(Operation::CancelQueued, true);
    cancelFuture_ = DataService::CancelQueuedJobsForJobSetTreeAsync(jobSetId);
}

void JobSetsController::deleteJobSet(qint64 jobSetId)
{
    if (deleteInFlight_ || cancelInFlight_ || boostInFlight_ || jobSetId <= 0) {
        return;
    }

    pendingActionJobSetId_ = jobSetId;
    deleteInFlight_ = true;
    setBusy(Operation::Delete, true);
    deleteFuture_ = DataService::DeleteJobSetAsync(jobSetId);
}

void JobSetsController::kickKindsFetch()
{
    if (kindsInFlight_) {
        return;
    }

    kindsInFlight_ = true;
    setBusy(Operation::FetchKinds, true);
    kindsFuture_ = DataService::ListProgramKindsAsync();
}

void JobSetsController::kickPageFetch()
{
    if (pageInFlight_) {
        return;
    }

    pageInFlight_ = true;
    state_.loading = true;
    state_.errorMessage.clear();

    PagedQuery<> query;
    query.before = before_;
    query.after = after_;
    query.limit = state_.pageLimit;
    pageFuture_ = DataService::FetchJobSetsPageWithFamilies(state_.scope, query);
    emitStateChanged();
}

void JobSetsController::consumePending()
{
    using namespace std::chrono_literals;

    if (kindsInFlight_ && kindsFuture_.valid() && kindsFuture_.wait_for(0ms) == std::future_status::ready) {
        auto result = kindsFuture_.get();
        kindsInFlight_ = false;
        setBusy(Operation::FetchKinds, false);
        if (result.ok) {
            state_.programNames.clear();
            for (const ProgramKindKV& kind : result.value) {
                state_.programNames.insert(kind.id, QString::fromStdString(kind.name));
            }
        } else {
            state_.errorMessage = QStringLiteral("Program kinds failed: %1").arg(QString::fromStdString(result.error.message));
        }
        emitStateChanged();
    }

    if (pageInFlight_ && pageFuture_.valid() && pageFuture_.wait_for(0ms) == std::future_status::ready) {
        auto result = pageFuture_.get();
        pageInFlight_ = false;
        state_.loading = false;
        if (result.ok) {
            state_.page = std::move(result.value.page);
            state_.familyItems = std::move(result.value.family_items);
            state_.lastRefresh = QDateTime::currentDateTime();
            state_.errorMessage.clear();
            state_.infoMessage.clear();
        } else {
            state_.page = {};
            state_.familyItems.clear();
            state_.errorMessage = QStringLiteral("Job sets failed: %1").arg(QString::fromStdString(result.error.message));
        }
        emitStateChanged();
    }

    if (boostInFlight_ && boostFuture_.valid() && boostFuture_.wait_for(0ms) == std::future_status::ready) {
        auto result = boostFuture_.get();
        boostInFlight_ = false;
        setBusy(Operation::Boost, false);
        if (result.ok) {
            state_.infoMessage = QStringLiteral("Boosted %1 jobs to priority %2.").arg(result.value.changed_jobs).arg(result.value.new_priority);
            before_.reset();
            after_.reset();
            kickPageFetch();
        } else {
            state_.errorMessage = QStringLiteral("Boost failed: %1").arg(QString::fromStdString(result.error.message));
            emitStateChanged();
        }
    }

    if (cancelInFlight_ && cancelFuture_.valid() && cancelFuture_.wait_for(0ms) == std::future_status::ready) {
        auto result = cancelFuture_.get();
        cancelInFlight_ = false;
        setBusy(Operation::CancelQueued, false);
        if (result.ok) {
            state_.infoMessage = QStringLiteral("Canceled %1 queued jobs.").arg(result.value.canceled_job_ids.size());
            before_.reset();
            after_.reset();
            kickPageFetch();
        } else {
            state_.errorMessage = QStringLiteral("Cancel queued failed: %1").arg(QString::fromStdString(result.error.message));
            emitStateChanged();
        }
    }

    if (deleteInFlight_ && deleteFuture_.valid() && deleteFuture_.wait_for(0ms) == std::future_status::ready) {
        auto result = deleteFuture_.get();
        deleteInFlight_ = false;
        setBusy(Operation::Delete, false);
        if (result.ok) {
            state_.infoMessage = QStringLiteral("Deleted job set %1.").arg(pendingActionJobSetId_);
            before_.reset();
            after_.reset();
            kickPageFetch();
        } else {
            state_.errorMessage = QStringLiteral("Delete failed: %1").arg(QString::fromStdString(result.error.message));
            emitStateChanged();
        }
        pendingActionJobSetId_ = 0;
    }
}

void JobSetsController::setBusy(Operation operation, bool busy)
{
    Q_UNUSED(operation);
    Q_UNUSED(busy);
    state_.actionsBusy = boostInFlight_ || cancelInFlight_ || deleteInFlight_;
    emitStateChanged();
}

bool JobSetsController::canAutoRefresh() const
{
    return state_.autoRefresh && !before_.has_value() && !after_.has_value() && !pageInFlight_ && !state_.actionsBusy;
}

bool JobSetsController::anyWorkInFlight() const
{
    return kindsInFlight_ || pageInFlight_ || boostInFlight_ || cancelInFlight_ || deleteInFlight_;
}

void JobSetsController::emitStateChanged()
{
    state_.actionsBusy = boostInFlight_ || cancelInFlight_ || deleteInFlight_;
    state_.loading = pageInFlight_ || anyWorkInFlight();
    emit stateChanged();
}
