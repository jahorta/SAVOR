#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QString>

#include "DB/Querying/DataService.h"
#include "DB/Querying/PagedQuery.h"

#include <future>
#include <optional>
#include <vector>

class QTimer;

class JobSetsController final : public QObject
{
    Q_OBJECT

public:
    explicit JobSetsController(QObject* parent = nullptr);

    struct ViewState {
        QHash<int, QString> programNames;
        std::vector<JobSetLite> familyItems;
        Page<JobSetLite> page;
        QString errorMessage;
        QString infoMessage;
        bool loading = false;
        bool actionsBusy = false;
        bool autoRefresh = true;
        int refreshSeconds = 2;
        int pageLimit = 100;
        QDateTime lastRefresh;
        JobSetsListScope scope{};
    };

    const ViewState& viewState() const;

    void loadInitial();
    void applyFilters(const std::optional<int>& programKind, const std::optional<JobSetStateFilter>& stateFilter, int pageLimit);
    void resetFilters();
    void setAutoRefreshEnabled(bool enabled);
    void setRefreshSeconds(int seconds);
    void requestRefresh();
    void requestNextPage();
    void requestPreviousPage();
    void boostJobSetTree(qint64 jobSetId);
    void cancelQueuedForTree(qint64 jobSetId);
    void deleteJobSet(qint64 jobSetId);

signals:
    void stateChanged();

private:
    enum class Operation {
        FetchKinds,
        FetchPage,
        Boost,
        CancelQueued,
        Delete
    };

    void kickKindsFetch();
    void kickPageFetch();
    void consumePending();
    void setBusy(Operation operation, bool busy);
    bool canAutoRefresh() const;
    bool anyWorkInFlight() const;
    void emitStateChanged();

    ViewState state_;
    std::optional<KeysetCursor> before_;
    std::optional<KeysetCursor> after_;
    qint64 pendingActionJobSetId_ = 0;
    bool initialLoadStarted_ = false;
    bool kindsInFlight_ = false;
    bool pageInFlight_ = false;
    bool boostInFlight_ = false;
    bool cancelInFlight_ = false;
    bool deleteInFlight_ = false;
    std::future<simcore::db::DbResult<std::vector<simcore::db::ProgramKindKV>>> kindsFuture_;
    std::future<simcore::db::DbResult<simcore::db::JobSetPageWithFamilies>> pageFuture_;
    std::future<simcore::db::DbResult<simcore::db::JobSetPriorityBoostResult>> boostFuture_;
    std::future<simcore::db::DbResult<simcore::db::JobSetCancelQueuedResult>> cancelFuture_;
    std::future<simcore::db::DbResult<void>> deleteFuture_;
    QTimer* pollTimer_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
};
