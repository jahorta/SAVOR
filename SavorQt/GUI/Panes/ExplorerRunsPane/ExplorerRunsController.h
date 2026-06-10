#pragma once

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDateTime>
#include <QtCore/QFutureWatcher>
#include <QtCore/QObject>

#include <optional>

#include "DB/SavorDbExplorerRunService.h"

class QTimer;

class ExplorerRunsController final : public QObject
{
    Q_OBJECT

public:
    explicit ExplorerRunsController(QObject* parent = nullptr);

    struct ViewState {
        savorqt::db::ExplorerRunGroupPage groupPage;
        std::optional<savor::db::UiJobSetDetail> selectedGroup;
        std::optional<savorqt::db::ExplorerRunJobDetail> selectedJob;
        QString errorMessage;
        QString infoMessage;
        bool loadingGroups = false;
        bool loadingGroupDetail = false;
        bool loadingJobDetail = false;
        bool autoRefresh = true;
        int refreshSeconds = 2;
        int pageLimit = 50;
        QDateTime lastRefresh;
        qint64 selectedJobSetId = 0;
        qint64 selectedJobId = 0;
    };

    const ViewState& viewState() const;

    void setPageActive(bool active);
    void requestRefresh();
    void requestNextPage();
    void requestPreviousPage();
    void setAutoRefreshEnabled(bool enabled);
    void setRefreshSeconds(int seconds);
    void setPageLimit(int limit);
    void selectGroup(qint64 jobSetId);
    void selectJob(qint64 jobId);
    void refreshSelectedJob();

signals:
    void stateChanged();

private:
    using GroupPageResult = savorqt::db::ServiceResult<savorqt::db::ExplorerRunGroupPage>;
    using GroupDetailResult = savorqt::db::ServiceResult<savor::db::UiJobSetDetail>;
    using JobDetailResult = savorqt::db::ServiceResult<savorqt::db::ExplorerRunJobDetail>;

    void loadInitial();
    void kickGroupsFetch();
    void kickGroupDetailFetch(qint64 jobSetId);
    void kickJobDetailFetch(qint64 jobId, bool force = false);
    bool canAutoRefresh() const;
    void emitStateChanged();
    void loadSettings();
    void persistSettings() const;
    QString describeError(const savorqt::db::ServiceError& error, const QString& prefix) const;

    ViewState state_;
    std::optional<savorqt::db::ExplorerRunCursor> before_;
    std::optional<savorqt::db::ExplorerRunCursor> after_;
    bool pageActive_ = false;
    bool initialLoadStarted_ = false;
    bool groupsInFlight_ = false;
    bool pendingGroupsFetch_ = false;
    bool groupDetailInFlight_ = false;
    bool jobDetailInFlight_ = false;
    qint64 groupDetailRequestId_ = 0;
    qint64 jobDetailRequestId_ = 0;

    QFutureWatcher<GroupPageResult> groupsWatcher_;
    QFutureWatcher<GroupDetailResult> groupDetailWatcher_;
    QFutureWatcher<JobDetailResult> jobDetailWatcher_;
    QTimer* refreshTimer_ = nullptr;
};
