#pragma once

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDateTime>
#include <QtCore/QFutureWatcher>
#include <QtWidgets/QWidget>

#include "DB/SimCoreDbWorkflowService.h"
#include "GUI/Common/StatusToast.h"

#include <cstdint>
#include <optional>

class JobSetsPage;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
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

    void createWidgets();
    void wireSignals();
    void refreshWorkflows();
    void fetchWorkflowDetail(std::int64_t workflowInstanceId);
    void applyFilters();
    void requestNextPage();
    void requestPreviousPage();
    void handleWorkflowSelectionChanged();
    void handleRootTabChanged(int index);
    void syncJobSetsActiveState();
    void updateWorkflowTable();
    void updateWorkflowDetail();
    void clearWorkflowDetail(const QString& message);
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
    bool pendingWorkflowRefresh_ = false;
    QDateTime lastRefresh_;
    QString errorMessage_;
    QString infoMessage_;
    QString lastToastSignature_;

    QFutureWatcher<WorkflowPageResult> workflowWatcher_;
    QFutureWatcher<WorkflowDetailResult> detailWatcher_;
    QTimer* refreshTimer_ = nullptr;

    QTabWidget* rootTabs_ = nullptr;
    QWidget* overviewTab_ = nullptr;
    JobSetsPage* jobSetsPage_ = nullptr;

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
    QTreeWidget* alertsTree_ = nullptr;
};
