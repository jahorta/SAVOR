#include "CoordinatorPane.h"

#include "../../Coordinator/CoordinatorController.h"
#include "../Models/WorkerTableModel.h"

#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTableView>
#include <QtWidgets/QVBoxLayout>

namespace {
constexpr int kRefreshIntervalMs = 500;
}

CoordinatorPane::CoordinatorPane(CoordinatorController* controller, QWidget* parent)
    : QWidget(parent)
    , controller_(controller)
{
    createWidgets();

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(kRefreshIntervalMs);
    connect(refreshTimer_, &QTimer::timeout, controller_, &CoordinatorController::refreshSnapshot);
    refreshTimer_->start();

    connect(controller_, &CoordinatorController::stateChanged, this, &CoordinatorPane::refreshUi);
    connect(controller_, &CoordinatorController::snapshotChanged, this, &CoordinatorPane::refreshUi);

    refreshUi();
}

void CoordinatorPane::refreshUi()
{
    const bool running = controller_->isRunning();

    {
        const QSignalBlocker blocker(targetWorkersSpin_);
        targetWorkersSpin_->setValue(controller_->targetWorkers());
    }
    {
        const QSignalBlocker blocker(eventBufferSpin_);
        eventBufferSpin_->setValue(controller_->eventBufferCapacity());
    }
    {
        const QSignalBlocker blocker(isoPathEdit_);
        isoPathEdit_->setText(controller_->isoPath());
    }
    {
        const QSignalBlocker blocker(dolphinBaseDirEdit_);
        dolphinBaseDirEdit_->setText(controller_->dolphinBaseDir());
    }

    activeWorkersLabel_->setText(running
        ? QStringLiteral("Active: %1").arg(controller_->activeWorkers())
        : QStringLiteral("Active: --"));

    pauseButton_->setText(controller_->isPaused() ? QStringLiteral("Resume") : QStringLiteral("Pause"));
    pauseButton_->setProperty("coordinatorPaused", controller_->isPaused());
    pauseButton_->style()->unpolish(pauseButton_);
    pauseButton_->style()->polish(pauseButton_);

    validationLabel_->setText(controller_->validationMessage());
    validationLabel_->setVisible(!controller_->validationMessage().isEmpty());

    startButton_->setEnabled(!running && controller_->validationMessage().isEmpty());
    setControlsEnabledForRunningState(running);
    stoppedLabel_->setVisible(!running);
    workerTableView_->setVisible(running);

    workerTableModel_->setSnapshots(controller_->snapshot());
}

void CoordinatorPane::createWidgets()
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    workerTableModel_ = new WorkerTableModel(this);

    layout->addWidget(createControlsCard());
    layout->addWidget(createSettingsCard());
    layout->addWidget(createTableCard(), 1);
}

void CoordinatorPane::configureTable()
{
    workerTableView_->setModel(workerTableModel_);
    workerTableView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    workerTableView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    workerTableView_->setSelectionMode(QAbstractItemView::SingleSelection);
    workerTableView_->setAlternatingRowColors(true);
    workerTableView_->verticalHeader()->setVisible(false);
    workerTableView_->horizontalHeader()->setStretchLastSection(true);
    workerTableView_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
}

QWidget* CoordinatorPane::createControlsCard()
{
    QFrame* card = new QFrame(this);
    card->setObjectName("coordinatorCard");

    QHBoxLayout* layout = new QHBoxLayout(card);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    startButton_ = new QPushButton("Start", card);
    pauseButton_ = new QPushButton("Pause", card);
    stopButton_ = new QPushButton("Stop", card);
    targetWorkersSpin_ = new QSpinBox(card);
    activeWorkersLabel_ = new QLabel("Active: --", card);

    targetWorkersSpin_->setMinimum(1);
    targetWorkersSpin_->setMaximum(9999);
    targetWorkersSpin_->setPrefix("Target: ");

    layout->addWidget(startButton_);
    layout->addWidget(pauseButton_);
    layout->addWidget(stopButton_);
    layout->addWidget(targetWorkersSpin_);
    layout->addWidget(activeWorkersLabel_);
    layout->addStretch();

    connect(startButton_, &QPushButton::clicked, controller_, &CoordinatorController::startCoordinator);
    connect(pauseButton_, &QPushButton::clicked, controller_, &CoordinatorController::togglePaused);
    connect(stopButton_, &QPushButton::clicked, controller_, &CoordinatorController::stopCoordinator);
    connect(targetWorkersSpin_, qOverload<int>(&QSpinBox::valueChanged), controller_, &CoordinatorController::setTargetWorkers);

    return card;
}

QWidget* CoordinatorPane::createSettingsCard()
{
    QFrame* card = new QFrame(this);
    card->setObjectName("coordinatorCard");

    QVBoxLayout* rootLayout = new QVBoxLayout(card);
    rootLayout->setContentsMargins(16, 16, 16, 16);
    rootLayout->setSpacing(10);

    QLabel* heading = new QLabel("Coordinator Settings", card);
    heading->setObjectName("panelTitle");
    rootLayout->addWidget(heading);

    isoPathEdit_ = new QLineEdit(card);
    isoPathEdit_->setPlaceholderText("Path to SkiesOfArcadia iso");

    dolphinBaseDirEdit_ = new QLineEdit(card);
    dolphinBaseDirEdit_->setPlaceholderText("Path to DolphinQt base directory with portable.txt");

    eventBufferSpin_ = new QSpinBox(card);
    eventBufferSpin_->setMinimum(8);
    eventBufferSpin_->setMaximum(1000000);
    eventBufferSpin_->setPrefix("Event ring: ");

    validationLabel_ = new QLabel(card);
    validationLabel_->setObjectName("coordinatorValidation");
    validationLabel_->setWordWrap(true);

    rootLayout->addWidget(isoPathEdit_);
    rootLayout->addWidget(dolphinBaseDirEdit_);
    rootLayout->addWidget(eventBufferSpin_);
    rootLayout->addWidget(validationLabel_);

    connect(isoPathEdit_, &QLineEdit::textChanged, controller_, &CoordinatorController::setIsoPath);
    connect(dolphinBaseDirEdit_, &QLineEdit::textChanged, controller_, &CoordinatorController::setDolphinBaseDir);
    connect(eventBufferSpin_, qOverload<int>(&QSpinBox::valueChanged), controller_, &CoordinatorController::setEventBufferCapacity);

    return card;
}

QWidget* CoordinatorPane::createTableCard()
{
    QFrame* card = new QFrame(this);
    card->setObjectName("coordinatorCard");

    QVBoxLayout* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    QLabel* heading = new QLabel("Live Workers", card);
    heading->setObjectName("panelTitle");

    stoppedLabel_ = new QLabel("Coordinator is stopped.", card);
    stoppedLabel_->setObjectName("panelBody");

    workerTableView_ = new QTableView(card);
    workerTableView_->setObjectName("coordinatorTableView");
    configureTable();

    layout->addWidget(heading);
    layout->addWidget(stoppedLabel_);
    layout->addWidget(workerTableView_, 1);

    return card;
}

void CoordinatorPane::setControlsEnabledForRunningState(bool running)
{
    pauseButton_->setEnabled(running);
    stopButton_->setEnabled(running);
    isoPathEdit_->setEnabled(!running);
    dolphinBaseDirEdit_->setEnabled(!running);
    eventBufferSpin_->setEnabled(!running);
}
