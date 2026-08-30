#pragma once

#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"
#include "GUI/Refresh/RefreshCoordinator.h"

class CoordinatorController;
enum class CoordinatorLifecycleState;
class QCheckBox;
class QPoint;
class QLabel;
class QPushButton;
class QSpinBox;
class QTreeView;
class WorkerTableModel;
class VisualReplayWindow;
class VisualWorkerDashboardWindow;

class CoordinatorPane : public QWidget
{
    Q_OBJECT

public:
    enum class SettingsFocusTarget {
        CoordinatorSection,
        IsoPath,
        DolphinBaseDir
    };

    explicit CoordinatorPane(QWidget* parent = nullptr);
    ~CoordinatorPane() override;
    CoordinatorController* controller() const;
    void startCoordinator();
    void stopCoordinator();
    void togglePaused();
    void setTargetWorkers(int targetWorkers);
    void setVisualWorkerPoolEnabled(bool enabled);
    void showVisualWorkers();
    void closeVisualWorkersForApplicationClose();

signals:
    void settingsNavigationRequested(SettingsFocusTarget target);
    void statusToastRequested(StatusToast toast);

public slots:
    void requestVisualReplay(qint64 jobId);

private slots:
    void refreshUi();
    void handleValidationLinkActivated(const QString& link);
    void handleStartRequested();
    void handleTargetWorkersChanged(int targetWorkers);
    void handleVisualWorkersToggled(bool enabled);
    void showVisualWorkerDashboard();
    void showWorkerContextMenu(const QPoint& position);

private:
    void createWidgets();
    void configureTable(QTreeView* tableView);
    QWidget* createControlsCard();
    QWidget* createTableCard();
    QWidget* createMetricCard(const QString& caption, QLabel** valueLabel, const QString& objectName = QString());
    void setControlsEnabledForLifecycleState(CoordinatorLifecycleState state);
    void syncActionButtonStates(CoordinatorLifecycleState state, bool valid);
    void syncVisualReplayWindow();
    void ensureVisualWorkerDashboardSurfaces(int workerCount);
    void syncVisualWorkerDashboard();

    CoordinatorController* controller_ = nullptr;
    WorkerTableModel* workerTableModel_ = nullptr;

    QPushButton* startButton_ = nullptr;
    QPushButton* pauseButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPushButton* visualDashboardButton_ = nullptr;
    QCheckBox* visualWorkersCheck_ = nullptr;
    QSpinBox* targetWorkersSpin_ = nullptr;
    QLabel* activeWorkersLabel_ = nullptr;
    QLabel* statusValueLabel_ = nullptr;
    QLabel* snapshotCountLabel_ = nullptr;
    QLabel* validationLabel_ = nullptr;
    QLabel* warningLabel_ = nullptr;
    QLabel* stoppedLabel_ = nullptr;
    QLabel* tableSummaryLabel_ = nullptr;
    QTreeView* workerTableView_ = nullptr;
    VisualReplayWindow* visualReplayWindow_ = nullptr;
    VisualWorkerDashboardWindow* visualWorkerDashboard_ = nullptr;
    bool visualReplayDoneShown_ = false;
    QString lastToastSignature_;
    QString lastCleanupToastSignature_;
};
