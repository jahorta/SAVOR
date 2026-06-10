#pragma once

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDateTime>
#include <QtCore/QFutureWatcher>
#include <QtWidgets/QWidget>

#include "DB/SimCoreDbJobSetService.h"
#include "DB/SimCoreDbWorkflowService.h"
#include "GUI/Common/StatusToast.h"

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
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

class WorkflowsPage final : public QWidget
{
    Q_OBJECT

public:
    explicit WorkflowsPage(QWidget* parent = nullptr);
    void setPageActive(bool active);

signals:
    void statusToastRequested(StatusToast toast);

private:
    using WorkflowPageResult = soasimqt2::db::ServiceResult<
        simcore::db::UiReadPage<simcore::db::UiWorkflowInstanceSummary>>;
    using WorkflowDetailResult = soasimqt2::db::ServiceResult<simcore::db::UiWorkflowDetail>;
    using WorkflowJobSetsResult = soasimqt2::db::ServiceResult<
        std::vector<soasimqt2::db::WorkflowJobSetRow>>;

    void createWidgets();
    void wireSignals();
    void refreshWorkflows();
    void fetchWorkflowDetail(std::int64_t workflowInstanceId);
    void fetchWorkflowJobSets(const simcore::db::UiWorkflowDetail& detail);
    void applyFilters();
    void requestNextPage();
    void requestPreviousPage();
    void handleWorkflowSelectionChanged();
    void updateWorkflowTable();
    void updateWorkflowDetail();
    void updateWorkflowJobSets();
    void clearWorkflowDetail(const QString& message);
    void clearWorkflowJobSets(const QString& message);
    void updateStatusWidgets();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    std::optional<simcore::db::UiReadListCursor> before_;
    std::optional<simcore::db::UiReadListCursor> after_;
    simcore::db::UiReadPage<simcore::db::UiWorkflowInstanceSummary> workflowPage_{};
    std::optional<simcore::db::UiWorkflowDetail> selectedWorkflowDetail_;
    std::int64_t selectedWorkflowInstanceId_ = 0;
    bool pageActive_ = false;
    bool workflowFetchInFlight_ = false;
    bool detailFetchInFlight_ = false;
    bool jobSetsFetchInFlight_ = false;
    bool pendingWorkflowRefresh_ = false;
    std::int64_t jobSetsFetchWorkflowInstanceId_ = 0;
    QDateTime lastRefresh_;
    QString errorMessage_;
    QString infoMessage_;
    QString lastToastSignature_;
    QString lastWorkflowTableSignature_;
    QString lastWorkflowDetailSignature_;
    QString lastWorkflowJobSetsSignature_;
    std::vector<soasimqt2::db::WorkflowJobSetRow> workflowJobSets_;

    QFutureWatcher<WorkflowPageResult> workflowWatcher_;
    QFutureWatcher<WorkflowDetailResult> detailWatcher_;
    QFutureWatcher<WorkflowJobSetsResult> jobSetsWatcher_;
    QTimer* refreshTimer_ = nullptr;

    QComboBox* stateFilter_ = nullptr;
    QLineEdit* kindFilter_ = nullptr;
    QSpinBox* pageSizeSpin_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    QPushButton* prevButton_ = nullptr;
    QPushButton* nextButton_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QCheckBox* autoRefreshCheck_ = nullptr;
    QSpinBox* refreshSecondsSpin_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QLabel* lastRefreshLabel_ = nullptr;
    QLabel* inlineMessageLabel_ = nullptr;
    QLabel* detailHeaderLabel_ = nullptr;
    QLabel* detailMetaLabel_ = nullptr;
    QTableWidget* workflowTable_ = nullptr;
    QTreeWidget* currentStepsTree_ = nullptr;
    QTreeWidget* futureStepsTree_ = nullptr;
    QTreeWidget* pastStepsTree_ = nullptr;
    QTreeWidget* jobSetsTree_ = nullptr;
    QTreeWidget* alertsTree_ = nullptr;
};
