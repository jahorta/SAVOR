#pragma once

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDateTime>
#include <QtCore/QFutureWatcher>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QString>

#include "DB/Querying/DataService.h"
#include "DB/Querying/PagedQuery.h"
#include "DB/Querying/JobEventListDTO.h"

#include <optional>
#include <vector>

class QTimer;

class JobsController final : public QObject
{
    Q_OBJECT

public:
    explicit JobsController(QObject* parent = nullptr);

    struct JobDetailState {
        qint64 jobId = 0;
        QString payloadText;
        QString resultsText;
        QString decodedProgressText;
        std::vector<JobEventLite> events;
        std::vector<simcore::db::ArtifactRefLite> artifacts;
        bool loading = false;
        bool loaded = false;
    };

    struct ViewState {
        QHash<int, QString> programNames;
        Page<JobLite> page;
        QHash<qint64, QString> progressSummary;
        JobDetailState detail;
        QString errorMessage;
        QString infoMessage;
        bool loading = false;
        bool actionsBusy = false;
        bool autoRefresh = true;
        int refreshSeconds = 2;
        int pageLimit = 100;
        QDateTime lastRefresh;
        JobsListScope scope{};
        qint64 selectedJobId = 0;
    };

    const ViewState& viewState() const;

    void loadInitial();
    void setPageActive(bool active);
    void applyFilters(const std::optional<int>& programKind, const std::optional<QString>& stateFilter, const std::optional<qint64>& jobSetId, const std::optional<QString>& tagKey, int pageLimit);
    void resetFilters();
    void setAutoRefreshEnabled(bool enabled);
    void setRefreshSeconds(int seconds);
    void requestRefresh();
    void requestNextPage();
    void requestPreviousPage();
    void selectJob(qint64 jobId);
    void refreshSelectedJobDetail();
    void requeueSelectedJob();
    void replaySelectedJobVisually();
    void cancelSelectedJob();
    void restartSelectedFailedJob(std::optional<QString> iniOverride = std::nullopt);

signals:
    void stateChanged();

private:
    using ProgramKindsResult = simcore::db::DbResult<std::vector<simcore::db::ProgramKindKV>>;
    struct JobPageBundle {
        Page<JobLite> page;
        QHash<qint64, QString> progressSummary;
    };
    using JobPageResult = simcore::db::DbResult<JobPageBundle>;
    struct JobDetailBundle {
        QString payloadText;
        QString resultsText;
        QString decodedProgressText;
        std::vector<JobEventLite> events;
        std::vector<simcore::db::ArtifactRefLite> artifacts;
    };
    using JobDetailResult = simcore::db::DbResult<JobDetailBundle>;
    using VoidResult = simcore::db::DbResult<void>;

    enum class Operation { FetchKinds, FetchPage, FetchDetail, Requeue, ReplayVisual, Cancel, Restart };

    void kickKindsFetch();
    void kickPageFetch();
    void kickDetailFetch(qint64 jobId, bool force = false);
    void setBusy(Operation operation, bool busy);
    bool canAutoRefresh() const;
    bool anyWorkInFlight() const;
    const JobLite* selectedJob() const;
    QString programKindLabel(int id) const;
    void emitStateChanged();
    void loadSettings();
    void persistSettings() const;
    void syncFetchStateFromView();

    ViewState state_;
    JobsListScope fetchScope_{};
    int fetchPageLimit_ = 100;
    std::optional<KeysetCursor> before_;
    std::optional<KeysetCursor> after_;
    bool initialLoadStarted_ = false;
    bool kindsInFlight_ = false;
    bool pageInFlight_ = false;
    bool pendingPageFetch_ = false;
    bool detailInFlight_ = false;
    bool requeueInFlight_ = false;
    bool replayVisualInFlight_ = false;
    bool cancelInFlight_ = false;
    bool restartInFlight_ = false;
    qint64 detailRequestJobId_ = 0;
    qint64 actionJobId_ = 0;
    QFutureWatcher<ProgramKindsResult> kindsWatcher_;
    QFutureWatcher<JobPageResult> pageWatcher_;
    QFutureWatcher<JobDetailResult> detailWatcher_;
    QFutureWatcher<VoidResult> requeueWatcher_;
    QFutureWatcher<VoidResult> replayVisualWatcher_;
    QFutureWatcher<VoidResult> cancelWatcher_;
    QFutureWatcher<VoidResult> restartWatcher_;
    QTimer* refreshTimer_ = nullptr;
    bool pageActive_ = false;
};
