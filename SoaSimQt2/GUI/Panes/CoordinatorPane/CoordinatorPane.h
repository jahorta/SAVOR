#pragma once

#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"
#include "GUI/Refresh/RefreshCoordinator.h"

class CoordinatorController;
class QCheckBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTreeView;
class WorkerTableModel;
class VisualReplayDialog;
class VisualWorkerDashboardDialog;

class CoordinatorPane : public QWidget
{
    Q_OBJECT

public:
    enum class SettingsFocusTarget {
        CoordinatorSection,
        IsoPath,
        DolphinBaseDir
    };

    explicit CoordinatorPane(CoordinatorController* controller, QWidget* parent = nullptr);
    void setPageActive(bool active);

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

private:
    void createWidgets();
    void configureTable(QTreeView* tableView);
    QWidget* createControlsCard();
    QWidget* createTableCard();
    QWidget* createMetricCard(const QString& caption, QLabel** valueLabel, const QString& objectName = QString());
    void setControlsEnabledForRunningState(bool running);
    void syncActionButtonStates(bool running, bool valid);
    void syncVisualReplayDialog();
    void ensureVisualWorkerDashboardSurfaces(int workerCount, bool allowShrink);
    void syncVisualWorkerDashboard();

    CoordinatorController* controller_ = nullptr;
    soasimqt2::gui::RefreshCoordinator* refreshCoordinator_ = nullptr;
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
    QLabel* stoppedLabel_ = nullptr;
    QLabel* tableSummaryLabel_ = nullptr;
    QTreeView* workerTableView_ = nullptr;
    VisualReplayDialog* visualReplayDialog_ = nullptr;
    VisualWorkerDashboardDialog* visualWorkerDashboard_ = nullptr;
    bool visualReplayDoneShown_ = false;
    QString lastToastSignature_;
};
