#pragma once

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDateTime>
#include <QtCore/QFutureWatcher>
#include <QtCore/QPoint>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtWidgets/QWidget>

#include "DB/SavorDbJobSetService.h"
#include "DB/SavorDbWorkflowService.h"
#include "GUI/Common/StatusToast.h"
#include "GUI/Refresh/AsyncRefreshPipeline.h"

#include <cstdint>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;

class WorkflowsPage final : public QWidget
{
    Q_OBJECT

public:
    explicit WorkflowsPage(QWidget* parent = nullptr);
    void setPageActive(bool active);
    void showWorkflow(std::int64_t workflowInstanceId);
    void setWorkflowRetryInFlight(bool inFlight);
    void refreshAfterRetry(std::int64_t workflowInstanceId);

signals:
    void statusToastRequested(StatusToast toast);
    void retryFailedJobsRequested(qint64 workflowInstanceId);
    void openJobRequested(qint64 jobId);

public:
    struct WorkflowTableRow {
        std::int64_t workflowInstanceId = 0;
        QString kind;
        QString state;
        QString activeStep;
        QString steps;
        QString jobs;
        QString problems;
        QString created;
        QString completed;
        std::int64_t retryableJobs = 0;
    };

    struct TreeDisplayRow {
        QString key;
        QStringList columns;
        bool firstColumnSpanned = false;
        bool expanded = false;
        std::vector<TreeDisplayRow> children;
    };

private:
    using WorkflowPageResult = savorqt::db::ServiceResult<
        savor::db::UiReadPage<savor::db::UiWorkflowInstanceSummary>>;
    using WorkflowDetailResult = savorqt::db::ServiceResult<savor::db::UiWorkflowDetail>;
    using WorkflowJobSetsResult = savorqt::db::ServiceResult<
        std::vector<savorqt::db::WorkflowJobSetRow>>;
    using WorkflowCancelResult = savorqt::db::ServiceResult<void>;

    void createWidgets();
    void wireSignals();
    void refreshWorkflows();
    void fetchWorkflowDetail(std::int64_t workflowInstanceId);
    void fetchWorkflowJobSets(const savor::db::UiWorkflowDetail& detail);
    void applyFilters();
    void requestNextPage();
    void requestPreviousPage();
    void handleWorkflowSelectionChanged();
    void showWorkflowContextMenu(const QPoint& position);
    void showJobSetContextMenu(const QPoint& position);
    void cancelWorkflow(std::int64_t workflowInstanceId);
    void updateWorkflowTable();
    void updateWorkflowDetail();
    void updateWorkflowJobSets();
    void clearWorkflowDetail(const QString& message);
    void clearWorkflowJobSets(const QString& message);
    void updateStatusWidgets();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    std::optional<savor::db::UiReadListCursor> before_;
    std::optional<savor::db::UiReadListCursor> after_;
    savor::db::UiReadPage<savor::db::UiWorkflowInstanceSummary> workflowPage_{};
    std::optional<savor::db::UiWorkflowDetail> selectedWorkflowDetail_;
    std::int64_t selectedWorkflowInstanceId_ = 0;
    bool pageActive_ = false;
    bool workflowFetchInFlight_ = false;
    bool detailFetchInFlight_ = false;
    bool jobSetsFetchInFlight_ = false;
    bool retryFailedJobsInFlight_ = false;
    bool workflowCancelInFlight_ = false;
    std::int64_t jobSetsFetchWorkflowInstanceId_ = 0;
    std::int64_t focusedWorkflowInstanceId_ = 0;
    std::int64_t renderedWorkflowInstanceId_ = 0;
    std::int64_t renderedJobSetsWorkflowInstanceId_ = 0;
    QDateTime lastRefresh_;
    QString errorMessage_;
    QString infoMessage_;
    QString lastToastSignature_;
    std::vector<WorkflowTableRow> currentWorkflowRows_;
    std::vector<TreeDisplayRow> currentCurrentStepRows_;
    std::vector<TreeDisplayRow> currentFutureStepRows_;
    std::vector<TreeDisplayRow> currentPastStepRows_;
    std::vector<TreeDisplayRow> currentAlertRows_;
    std::vector<TreeDisplayRow> currentJobSetRows_;
    std::vector<savorqt::db::WorkflowJobSetRow> workflowJobSets_;

    savorqt::gui::AsyncRefreshPipeline<savorqt::db::WorkflowListRequest, WorkflowPageResult>* workflowRefreshPipeline_ = nullptr;
    QFutureWatcher<WorkflowDetailResult> detailWatcher_;
    QFutureWatcher<WorkflowJobSetsResult> jobSetsWatcher_;
    QFutureWatcher<WorkflowCancelResult> workflowCancelWatcher_;

    QComboBox* stateFilter_ = nullptr;
    QLineEdit* kindFilter_ = nullptr;
    QSpinBox* pageSizeSpin_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    QPushButton* prevButton_ = nullptr;
    QPushButton* nextButton_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QCheckBox* autoRefreshCheck_ = nullptr;
    QCheckBox* victoryOnlyCheck_ = nullptr;
    QSpinBox* refreshSecondsSpin_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QLabel* lastRefreshLabel_ = nullptr;
    QLabel* inlineMessageLabel_ = nullptr;
    QLabel* detailHeaderLabel_ = nullptr;
    QLabel* detailMetaLabel_ = nullptr;
    QLabel* pastCountLabel_ = nullptr;
    QLabel* currentCountLabel_ = nullptr;
    QLabel* futureCountLabel_ = nullptr;
    QTableWidget* workflowTable_ = nullptr;
    QTreeWidget* currentStepsTree_ = nullptr;
    QTreeWidget* futureStepsTree_ = nullptr;
    QTreeWidget* pastStepsTree_ = nullptr;
    QTreeWidget* jobSetsTree_ = nullptr;
    QTreeWidget* alertsTree_ = nullptr;
};
