#pragma once

#include "GUI/Workspace/WorkspaceWidgets.h"

#include <functional>
#include <set>

class CoordinatorController;
class QEvent;
class QFrame;
class QLabel;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QVBoxLayout;
class QPoint;

class RunningTab final : public savorqt::gui::WorkspacePageShell
{
public:
    struct Actions {
        std::function<void()> openWorkflows;
        std::function<void(qint64)> openWorkflow;
        std::function<void(qint64)> retryWorkflowJobs;
        std::function<void()> openJobs;
        std::function<void()> openWorkers;
        std::function<void()> openCoordinatorSettings;
        std::function<void()> openIsoSettings;
        std::function<void()> openDolphinSettings;
    };

    explicit RunningTab(CoordinatorController* coordinatorController, Actions actions, QWidget* parent = nullptr);
    void requestRefresh();

private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void build();
    void refreshCockpit();
    void showOperationalDetails();
    void refreshDetailsButton();
    void openSelectedWorkflow();
    void showWorkflowContextMenu(const QPoint& position);

    CoordinatorController* coordinatorController_ = nullptr;
    Actions actions_;
    std::function<void()> requestCockpitRefresh_;

    QLabel* coordinatorValueLabel_ = nullptr;
    QLabel* workersValueLabel_ = nullptr;
    QLabel* queueValueLabel_ = nullptr;
    QLabel* runningValueLabel_ = nullptr;
    QLabel* failuresValueLabel_ = nullptr;
    QLabel* lastRefreshLabel_ = nullptr;

    QPushButton* startCoordinatorButton_ = nullptr;
    QPushButton* pauseCoordinatorButton_ = nullptr;
    QPushButton* stopCoordinatorButton_ = nullptr;
    QSpinBox* targetWorkersSpin_ = nullptr;
    QPushButton* isoSetupButton_ = nullptr;
    QPushButton* dolphinSetupButton_ = nullptr;
    QPushButton* fixRuntimeSetupButton_ = nullptr;
    QPushButton* openFailuresButton_ = nullptr;
    QPushButton* detailsDrawerButton_ = nullptr;

    QLabel* workflowSummaryLabel_ = nullptr;
    QLabel* workflowReadyValueLabel_ = nullptr;
    QLabel* workflowQueuedValueLabel_ = nullptr;
    QLabel* workflowWaitingValueLabel_ = nullptr;
    QLabel* workflowFinalValueLabel_ = nullptr;
    QLabel* queueSummaryLabel_ = nullptr;
    QLabel* workerSummaryLabel_ = nullptr;
    QLabel* attentionSummaryLabel_ = nullptr;

    QTableWidget* workflowTable_ = nullptr;
    QTableWidget* workerTable_ = nullptr;
    QTabWidget* detailTabs_ = nullptr;
    QVBoxLayout* queueBucketsLayout_ = nullptr;
    QVBoxLayout* attentionLayout_ = nullptr;

    int lastFailedWorkflows_ = 0;
    int lastFailedJobs_ = 0;
    int lastAttentionItems_ = 0;
    std::set<qint64> collapsedExpansionIds_;
};
