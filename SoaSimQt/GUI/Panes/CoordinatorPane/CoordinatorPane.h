#pragma once

#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"
class CoordinatorController;
class QCheckBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;
class QTreeView;
class WorkerTableModel;
class VisualReplayDialog;

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

private slots:
    void refreshUi();
    void handleValidationLinkActivated(const QString& link);
public slots:
    void requestVisualReplay(qint64 jobId);

private:
    void createWidgets();
    void configureTable(QTreeView* tableView);
    QWidget* createControlsCard();
    QWidget* createTableCard();
    QWidget* createMetricCard(const QString& caption, QLabel** valueLabel, const QString& objectName = QString());
    void setControlsEnabledForRunningState(bool running);
    void syncActionButtonStates(bool running, bool valid);

    CoordinatorController* controller_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
    WorkerTableModel* workerTableModel_ = nullptr;
    WorkerTableModel* visualWorkerTableModel_ = nullptr;

    QPushButton* startButton_ = nullptr;
    QPushButton* pauseButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QSpinBox* targetWorkersSpin_ = nullptr;
    QLabel* activeWorkersLabel_ = nullptr;
    QLabel* statusValueLabel_ = nullptr;
    QLabel* snapshotCountLabel_ = nullptr;
    QLabel* validationLabel_ = nullptr;
    QLabel* stoppedLabel_ = nullptr;
    QLabel* tableSummaryLabel_ = nullptr;
    QLabel* visualTableSummaryLabel_ = nullptr;
    QTreeView* workerTableView_ = nullptr;
    QTreeView* visualWorkerTableView_ = nullptr;
    VisualReplayDialog* visualReplayDialog_ = nullptr;
    bool visualReplayRequested_ = false;
    bool visualWorkerObservedRunning_ = false;
    bool visualReplayDoneShown_ = false;
    QString lastToastSignature_;
};
