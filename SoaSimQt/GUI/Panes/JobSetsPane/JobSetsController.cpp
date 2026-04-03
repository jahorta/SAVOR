#include "JobSetsController.h"

#include <QtCore/QSettings>
#include <QtCore/QTimer>

#include <exception>
#include <algorithm>
#include <utility>

using simcore::db::DataService;
using simcore::db::ProgramKindKV;

namespace {
constexpr auto kSettingsGroup = "JobSetsPane";
constexpr auto kProgramKindKey = "program_kind";
constexpr auto kStateFilterKey = "state_filter";
constexpr auto kTagKey = "tag_key";
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
        return call().get();
    });
}
}

JobSetsController::JobSetsController(QObject* parent)
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
                for (const ProgramKindKV& kind : result.value) {
                    state_.programNames.insert(kind.id, QString::fromStdString(kind.name));
                }
                emit rowsChanged();
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

    connect(&pageWatcher_, &QFutureWatcher<JobSetPageResult>::finished, this, [this]() {
        const bool shouldRefetch = pendingPageFetch_;
        pendingPageFetch_ = false;
        try {
            auto result = pageWatcher_.result();
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
        } catch (...) {
            pageInFlight_ = false;
            state_.loading = false;
            state_.page = {};
            state_.familyItems.clear();
            state_.errorMessage = describeException("Job sets failed");
        }
        emit rowsChanged();
        emitStateChanged();
        if (shouldRefetch) {
            kickPageFetch();
        }
    });

    connect(&boostWatcher_, &QFutureWatcher<BoostResult>::finished, this, [this]() {
        try {
            const auto result = boostWatcher_.result();
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
        } catch (...) {
            boostInFlight_ = false;
            setBusy(Operation::Boost, false);
            state_.errorMessage = describeException("Boost failed");
            emitStateChanged();
        }
    });

    connect(&cancelWatcher_, &QFutureWatcher<CancelResult>::finished, this, [this]() {
        try {
            const auto result = cancelWatcher_.result();
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
        } catch (...) {
            cancelInFlight_ = false;
            setBusy(Operation::CancelQueued, false);
            state_.errorMessage = describeException("Cancel queued failed");
            emitStateChanged();
        }
    });

    connect(&deleteWatcher_, &QFutureWatcher<DeleteResult>::finished, this, [this]() {
        try {
            const auto result = deleteWatcher_.result();
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
        } catch (...) {
            deleteInFlight_ = false;
            setBusy(Operation::Delete, false);
            state_.errorMessage = describeException("Delete failed");
            emitStateChanged();
        }
        pendingActionJobSetId_ = 0;
    });

    refreshTimer_ = new QTimer(this);
    connect(refreshTimer_, &QTimer::timeout, this, [this]() {
        if (canAutoRefresh()) {
            requestRefresh();
        }
    });
    if (pageActive_) {
        refreshTimer_->start(state_.refreshSeconds * 1000);
    }
}

const JobSetsController::ViewState& JobSetsController::viewState() const
{
    return state_;
}

void JobSetsController::loadInitial()
{
    if (initialLoadStarted_ || !pageActive_) {
        return;
    }

    initialLoadStarted_ = true;
    state_.errorMessage.clear();
    state_.infoMessage.clear();
    kickKindsFetch();
    kickPageFetch();
}

void JobSetsController::setPageActive(bool active)
{
    if (pageActive_ == active) {
        return;
    }

    pageActive_ = active;
    if (!pageActive_) {
        if (refreshTimer_) {
            refreshTimer_->stop();
        }
        return;
    }

    if (refreshTimer_) {
        refreshTimer_->start(state_.refreshSeconds * 1000);
    }
    loadInitial();
}

void JobSetsController::applyFilters(const std::optional<int>& programKind, const std::optional<JobSetStateFilter>& stateFilter, const std::optional<QString>& tagKey, int pageLimit)
{
    state_.scope = {};
    state_.scope.program_kind = programKind;
    state_.scope.state_filter = stateFilter;
    if (tagKey.has_value() && !tagKey->trimmed().isEmpty()) {
        state_.scope.tag_key = tagKey->trimmed().toStdString();
    }
    state_.pageLimit = pageLimit;
    before_.reset();
    after_.reset();
    state_.errorMessage.clear();
    state_.infoMessage.clear();
    syncFetchStateFromView();
    persistSettings();
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
    syncFetchStateFromView();
    persistSettings();
    if (pageActive_) {
        refreshTimer_->start(state_.refreshSeconds * 1000);
    }
    kickPageFetch();
    emitStateChanged();
}

void JobSetsController::setAutoRefreshEnabled(bool enabled)
{
    state_.autoRefresh = enabled;
    persistSettings();
    emitStateChanged();
}

void JobSetsController::setRefreshSeconds(int seconds)
{
    state_.refreshSeconds = seconds;
    persistSettings();
    if (pageActive_) {
        refreshTimer_->start(state_.refreshSeconds * 1000);
    }
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
    kickPageFetch();
}

void JobSetsController::requestPreviousPage()
{
    if (!state_.page.prev.has_value()) {
        return;
    }

    after_ = state_.page.prev;
    before_.reset();
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
    boostWatcher_.setFuture(runDataServiceCall([jobSetId]() {
        return DataService::BoostJobSetPriorityTreeAsync(jobSetId);
    }));
}

void JobSetsController::cancelQueuedForTree(qint64 jobSetId)
{
    if (boostInFlight_ || cancelInFlight_ || deleteInFlight_ || jobSetId <= 0) {
        return;
    }

    pendingActionJobSetId_ = jobSetId;
    cancelInFlight_ = true;
    setBusy(Operation::CancelQueued, true);
    cancelWatcher_.setFuture(runDataServiceCall([jobSetId]() {
        return DataService::CancelQueuedJobsForJobSetTreeAsync(jobSetId);
    }));
}

void JobSetsController::deleteJobSet(qint64 jobSetId)
{
    if (deleteInFlight_ || cancelInFlight_ || boostInFlight_ || jobSetId <= 0) {
        return;
    }

    pendingActionJobSetId_ = jobSetId;
    deleteInFlight_ = true;
    setBusy(Operation::Delete, true);
    deleteWatcher_.setFuture(runDataServiceCall([jobSetId]() {
        return DataService::DeleteJobSetAsync(jobSetId);
    }));
}

void JobSetsController::kickKindsFetch()
{
    if (kindsInFlight_) {
        return;
    }

    kindsInFlight_ = true;
    setBusy(Operation::FetchKinds, true);
    kindsWatcher_.setFuture(runDataServiceCall([]() {
        return DataService::ListProgramKindsAsync();
    }));
}

void JobSetsController::kickPageFetch()
{
    if (pageInFlight_) {
        pendingPageFetch_ = true;
        return;
    }

    pageInFlight_ = true;
    pendingPageFetch_ = false;
    state_.loading = true;
    state_.errorMessage.clear();

    PagedQuery<> query;
    query.before = before_;
    query.after = after_;
    query.limit = fetchPageLimit_;
    pageWatcher_.setFuture(runDataServiceCall([scope = fetchScope_, query]() {
        return DataService::FetchJobSetsPageWithFamilies(scope, query);
    }));
    emitStateChanged();
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
    return pageActive_ && state_.autoRefresh && !before_.has_value() && !after_.has_value() && !pageInFlight_ && !state_.actionsBusy;
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

void JobSetsController::loadSettings()
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);

    const QVariant programKind = settings.value(kProgramKindKey);
    state_.scope.program_kind = programKind.isValid() ? std::optional<int>(programKind.toInt()) : std::nullopt;

    const QVariant stateFilter = settings.value(kStateFilterKey);
    state_.scope.state_filter = stateFilter.isValid()
        ? std::optional<JobSetStateFilter>(static_cast<JobSetStateFilter>(stateFilter.toInt()))
        : std::nullopt;
    const QString tagKey = settings.value(kTagKey).toString().trimmed();
    if (!tagKey.isEmpty()) {
        state_.scope.tag_key = tagKey.toStdString();
    }

    state_.pageLimit = (std::max)(1, settings.value(kPageLimitKey, state_.pageLimit).toInt());
    state_.autoRefresh = settings.value(kAutoRefreshKey, state_.autoRefresh).toBool();
    state_.refreshSeconds = (std::max)(1, settings.value(kRefreshSecondsKey, state_.refreshSeconds).toInt());

    settings.endGroup();
}

void JobSetsController::persistSettings() const
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);

    if (state_.scope.program_kind.has_value()) {
        settings.setValue(kProgramKindKey, *state_.scope.program_kind);
    } else {
        settings.remove(kProgramKindKey);
    }

    if (state_.scope.state_filter.has_value()) {
        settings.setValue(kStateFilterKey, static_cast<int>(*state_.scope.state_filter));
    } else {
        settings.remove(kStateFilterKey);
    }
    if (state_.scope.tag_key.has_value() && !state_.scope.tag_key->empty()) {
        settings.setValue(kTagKey, QString::fromStdString(*state_.scope.tag_key));
    } else {
        settings.remove(kTagKey);
    }

    settings.setValue(kPageLimitKey, state_.pageLimit);
    settings.setValue(kAutoRefreshKey, state_.autoRefresh);
    settings.setValue(kRefreshSecondsKey, state_.refreshSeconds);

    settings.endGroup();
}

void JobSetsController::syncFetchStateFromView()
{
    fetchScope_ = state_.scope;
    fetchPageLimit_ = state_.pageLimit;
}
