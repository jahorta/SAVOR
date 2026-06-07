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
        QString inputIniText;
        QString resultsText;
        QString decodedProgressText;
        std::vector<JobEventLite> events;
        std::vector<simcore::db::ArtifactRefLite> artifacts;
        bool loading = false;
        bool loaded = false;
        bool inputIniLoading = false;
        bool inputIniLoaded = false;
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
    void applyFilters(const std::optional<int>& programKind, const std::optional<QString>& stateFilter, const std::optional<qint64>& jobSetId, int pageLimit);
    void resetFilters();
    void setAutoRefreshEnabled(bool enabled);
    void setRefreshSeconds(int seconds);
    void requestRefresh();
    void requestNextPage();
    void requestPreviousPage();
    void selectJob(qint64 jobId);
    void refreshSelectedJobDetail();
    void loadSelectedJobInputIni();
    void requeueSelectedJob();
    void cancelSelectedJob();
    void restartSelectedFailedJob();

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
        QString resultsText;
        QString decodedProgressText;
        std::vector<JobEventLite> events;
        std::vector<simcore::db::ArtifactRefLite> artifacts;
    };
    using JobDetailResult = simcore::db::DbResult<JobDetailBundle>;
    using InputIniResult = simcore::db::DbResult<QString>;
    using VoidResult = simcore::db::DbResult<void>;

    enum class Operation { FetchKinds, FetchPage, FetchDetail, FetchInputIni, Requeue, Cancel, Restart };

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
    bool inputIniInFlight_ = false;
    bool requeueInFlight_ = false;
    bool cancelInFlight_ = false;
    bool restartInFlight_ = false;
    qint64 detailRequestJobId_ = 0;
    qint64 inputIniRequestJobId_ = 0;
    qint64 actionJobId_ = 0;
    QFutureWatcher<ProgramKindsResult> kindsWatcher_;
    QFutureWatcher<JobPageResult> pageWatcher_;
    QFutureWatcher<JobDetailResult> detailWatcher_;
    QFutureWatcher<InputIniResult> inputIniWatcher_;
    QFutureWatcher<VoidResult> requeueWatcher_;
    QFutureWatcher<VoidResult> cancelWatcher_;
    QFutureWatcher<VoidResult> restartWatcher_;
    QTimer* refreshTimer_ = nullptr;
    bool pageActive_ = false;
};
