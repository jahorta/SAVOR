#pragma once

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDateTime>
#include <QtCore/QFutureWatcher>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QString>

#include "DB/SavorDbServiceResult.h"
#include "Execution/IExecutionDb.h"
#include "GUI/Refresh/DatabaseProjectionController.h"
#include "UIRead/IUiReadDb.h"

#include <optional>
#include <vector>

class JobsController final : public QObject
{
    Q_OBJECT

public:
    explicit JobsController(QObject* parent = nullptr);

    struct JobDetailState {
        qint64 jobId = 0;
        QString inputIniText;
        std::vector<savor::db::ExecutionJobEventRecord> events;
        std::vector<savor::db::UiJobArtifact> artifacts;
        bool loading = false;
        bool loaded = false;
        bool inputIniLoading = false;
        bool inputIniLoaded = false;
    };

    struct ViewState {
        QHash<int, QString> programNames;
        savor::db::UiReadPage<savor::db::UiJobSummary> page;
        JobDetailState detail;
        QString errorMessage;
        QString infoMessage;
        bool loading = false;
        bool actionsBusy = false;
        bool autoRefresh = true;
        int refreshSeconds = 2;
        int pageLimit = 100;
        QDateTime lastRefresh;
        savor::db::UiReadJobListQuery scope{};
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
    void showJob(qint64 jobId);
    void refreshSelectedJobDetail();
    void loadSelectedJobInputIni();
    void requeueSelectedJob();
    void restartSelectedFailedJob();

signals:
    void stateChanged();

private:
    using ProgramKindsResult = savorqt::db::ServiceResult<std::vector<savor::db::UiProgramKind>>;
    using JobPageResult = savorqt::db::ServiceResult<savor::db::UiReadPage<savor::db::UiJobSummary>>;
    struct JobDetailBundle {
        std::vector<savor::db::ExecutionJobEventRecord> events;
        std::vector<savor::db::UiJobArtifact> artifacts;
    };
    using JobDetailResult = savorqt::db::ServiceResult<JobDetailBundle>;
    using InputIniResult = savorqt::db::ServiceResult<QString>;
    using VoidResult = savorqt::db::ServiceResult<void>;
    struct JobPageFetchRequest {
        savor::db::UiReadJobListQuery scope{};
        std::optional<savor::db::UiReadListCursor> before;
        std::optional<savor::db::UiReadListCursor> after;
        int limit = 100;
    };

    enum class Operation { FetchKinds, FetchPage, FetchDetail, FetchInputIni, Requeue, Restart };

    void kickKindsFetch();
    void kickPageFetch();
    void kickDetailFetch(qint64 jobId, bool force = false);
    void setBusy(Operation operation, bool busy);
    bool canAutoRefresh() const;
    bool anyWorkInFlight() const;
    const savor::db::UiJobSummary* selectedJob() const;
    QString programKindLabel(int id) const;
    void emitStateChanged();
    void loadSettings();
    void persistSettings() const;
    void syncFetchStateFromView();

    ViewState state_;
    savor::db::UiReadJobListQuery fetchScope_{};
    int fetchPageLimit_ = 100;
    std::optional<savor::db::UiReadListCursor> before_;
    std::optional<savor::db::UiReadListCursor> after_;
    bool initialLoadStarted_ = false;
    bool kindsInFlight_ = false;
    bool pageInFlight_ = false;
    bool detailInFlight_ = false;
    bool inputIniInFlight_ = false;
    bool requeueInFlight_ = false;
    bool restartInFlight_ = false;
    qint64 detailRequestJobId_ = 0;
    qint64 inputIniRequestJobId_ = 0;
    qint64 actionJobId_ = 0;
    QFutureWatcher<ProgramKindsResult> kindsWatcher_;
    savorqt::gui::DatabaseProjectionController<JobPageFetchRequest, JobPageResult>* pageRefreshPipeline_ = nullptr;
    QFutureWatcher<JobDetailResult> detailWatcher_;
    QFutureWatcher<InputIniResult> inputIniWatcher_;
    QFutureWatcher<VoidResult> requeueWatcher_;
    QFutureWatcher<VoidResult> restartWatcher_;
    bool pageActive_ = false;
};
