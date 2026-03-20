#pragma once

#include <QtWidgets/QWidget>

class CoordinatorController;
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
    void setControlsEnabledForRunningState(bool running);

    CoordinatorController* controller_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
    WorkerTableModel* workerTableModel_ = nullptr;

    QPushButton* startButton_ = nullptr;
    QPushButton* pauseButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QSpinBox* targetWorkersSpin_ = nullptr;
    QLabel* activeWorkersLabel_ = nullptr;
    QLineEdit* isoPathEdit_ = nullptr;
    QLineEdit* dolphinBaseDirEdit_ = nullptr;
    QSpinBox* eventBufferSpin_ = nullptr;
    QLabel* validationLabel_ = nullptr;
    QLabel* stoppedLabel_ = nullptr;
    QTableView* workerTableView_ = nullptr;
};
