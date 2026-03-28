#pragma once

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDateTime>
#include <QtCore/QFutureWatcher>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QVector>

#include "DB/DeltaSeedRepo.h"
#include "DB/Querying/DataService.h"
#include "DB/SeedProbeRepo.h"

#include <optional>
#include <vector>

class QTimer;

class SeedProbeController final : public QObject
{
    Q_OBJECT

public:
    explicit SeedProbeController(QObject* parent = nullptr);

    struct ProbeSummary {
        qint64 probeId = 0;
        QString status;
        qint64 savestateId = 0;
        QString filename;
    };

    struct UniqueSeedRow {
        QString input;
        QString seedHex;
    };

    struct GridPoint {
        int x = 0;
        int y = 0;
        int xSpan = 1;
        int ySpan = 1;
        int delta = 0;
    };

    struct GridView {
        QVector<GridPoint> points;
        int minNeg = -2;
        int maxPos = 32;
        bool hasData = false;
    };

    struct ViewState {
        QVector<ProbeSummary> probeRows;
        Page<simcore::db::SeedProbeLite> page;
        QString search;
        bool onlyDone = false;
        bool autoRefresh = true;
        int refreshSeconds = 2;
        int pageLimit = 50;
        qint64 selectedProbeId = 0;
        bool loadingList = false;
        bool loadingDetail = false;
        QString errorMessage;
        QString infoMessage;
        QDateTime lastRefresh;

        QString neutralSeedText;
        QString probeIdText;
        QString statusText;
        QString codecVersionText;
        QString savestateText;
        GridView mainGrid;
        GridView cStickGrid;
        GridView triggerGrid;
        QVector<int> legendDeltas;
        QVector<UniqueSeedRow> uniqueRows;
    };

    const ViewState& viewState() const;

    void loadInitial();
    void setSearch(const QString& search);
    void setOnlyDone(bool onlyDone);
    void setAutoRefreshEnabled(bool enabled);
    void setRefreshSeconds(int seconds);
    void applyFilters(int pageLimit);
    void resetFilters();
    void requestRefresh();
    void requestNextPage();
    void requestPreviousPage();
    void selectProbe(qint64 probeId);

signals:
    void stateChanged();

private:
    struct ListBundle {
        Page<simcore::db::SeedProbeLite> page;
        QVector<ProbeSummary> rows;
    };
    using ListBundleResult = simcore::db::DbResult<ListBundle>;
    struct RunningProbeUpdate {
        qint64 probeId = 0;
        QString statusText;
    };
    using RunningProbeUpdateResult = simcore::db::DbResult<QVector<RunningProbeUpdate>>;

    struct DetailBundle {
        simcore::db::SeedProbeRow probe;
        QString savestateText;
        GridView mainGrid;
        GridView cStickGrid;
        GridView triggerGrid;
        QVector<int> legendDeltas;
        QVector<UniqueSeedRow> uniqueRows;
    };
    using DetailBundleResult = simcore::db::DbResult<DetailBundle>;

    void kickPageFetch();
    void kickDetailFetch(qint64 probeId, bool force = false);
    void kickRunningRefresh(const QVector<qint64>& probeIds);
    bool canAutoRefresh() const;
    void emitStateChanged();
    void loadSettings();
    void persistSettings() const;
    void syncFetchStateFromView();

    ViewState state_;
    QString fetchSearch_;
    bool fetchOnlyDone_ = false;
    int fetchPageLimit_ = 50;
    std::optional<KeysetCursor> before_;
    std::optional<KeysetCursor> after_;
    bool initialLoadStarted_ = false;
    bool pageInFlight_ = false;
    bool pendingPageFetch_ = false;
    bool detailInFlight_ = false;
    bool runningRefreshInFlight_ = false;
    qint64 detailRequestProbeId_ = 0;
    QFutureWatcher<ListBundleResult> pageWatcher_;
    QFutureWatcher<DetailBundleResult> detailWatcher_;
    QFutureWatcher<RunningProbeUpdateResult> runningRefreshWatcher_;
    QTimer* refreshTimer_ = nullptr;
};
