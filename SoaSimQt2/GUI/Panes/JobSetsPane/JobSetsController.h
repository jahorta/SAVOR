#pragma once

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDateTime>
#include <QtCore/QFuture>
#include <QtCore/QFutureWatcher>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QString>

#include "DB/Querying/DataService.h"
#include "DB/Querying/PagedQuery.h"

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
    void setPageActive(bool active);
    void applyFilters(const std::optional<int>& programKind, const std::optional<JobSetStateFilter>& stateFilter, const std::optional<QString>& tagKey, int pageLimit);
    void resetFilters();
    void setAutoRefreshEnabled(bool enabled);
    void setRefreshSeconds(int seconds);
    void requestRefresh();
    void requestNextPage();
    void requestPreviousPage();

signals:
    void stateChanged();
    void rowsChanged();

private:
    using ProgramKindsResult = simcore::db::DbResult<std::vector<simcore::db::ProgramKindKV>>;
    using JobSetPageResult = simcore::db::DbResult<simcore::db::JobSetPageWithFamilies>;

    enum class Operation {
        FetchKinds,
        FetchPage
    };

    void kickKindsFetch();
    void kickPageFetch();
    void setBusy(Operation operation, bool busy);
    bool canAutoRefresh() const;
    bool anyWorkInFlight() const;
    void emitStateChanged();
    void loadSettings();
    void persistSettings() const;
    void syncFetchStateFromView();

    ViewState state_;
    JobSetsListScope fetchScope_{};
    int fetchPageLimit_ = 100;
    std::optional<KeysetCursor> before_;
    std::optional<KeysetCursor> after_;
    bool initialLoadStarted_ = false;
    bool kindsInFlight_ = false;
    bool pageInFlight_ = false;
    bool pendingPageFetch_ = false;
    QFutureWatcher<ProgramKindsResult> kindsWatcher_;
    QFutureWatcher<JobSetPageResult> pageWatcher_;
    QTimer* refreshTimer_ = nullptr;
    bool pageActive_ = false;
};
