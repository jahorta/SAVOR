#pragma once

#include <QtWidgets/QWidget>

class CoordinatorController;
class QCheckBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableView;
class QTimer;
class WorkerTableModel;

class CoordinatorPane : public QWidget
{
    Q_OBJECT

public:
    explicit CoordinatorPane(CoordinatorController* controller, QWidget* parent = nullptr);

private slots:
    void refreshUi();

private:
    void createWidgets();
    void configureTable();
    QWidget* createControlsCard();
    QWidget* createSettingsCard();
    QWidget* createTableCard();
    QWidget* createMetricCard(const QString& caption, QLabel** valueLabel, const QString& objectName = QString());
    QLabel* createFieldCaption(const QString& text, QWidget* parent) const;
    void setControlsEnabledForRunningState(bool running);
    void syncActionButtonStates(bool running, bool valid);

    CoordinatorController* controller_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
    WorkerTableModel* workerTableModel_ = nullptr;

    QPushButton* startButton_ = nullptr;
    QPushButton* pauseButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QSpinBox* targetWorkersSpin_ = nullptr;
    QLabel* activeWorkersLabel_ = nullptr;
    QLabel* statusValueLabel_ = nullptr;
    QLabel* snapshotCountLabel_ = nullptr;
    QLineEdit* isoPathEdit_ = nullptr;
    QLineEdit* dolphinBaseDirEdit_ = nullptr;
    QSpinBox* eventBufferSpin_ = nullptr;
    QCheckBox* startPausedCheck_ = nullptr;
    QLabel* validationLabel_ = nullptr;
    QLabel* stoppedLabel_ = nullptr;
    QLabel* tableSummaryLabel_ = nullptr;
    QTableView* workerTableView_ = nullptr;
};
