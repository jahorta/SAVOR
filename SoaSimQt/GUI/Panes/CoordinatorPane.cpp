#include "CoordinatorPane.h"

#include "../../Coordinator/CoordinatorController.h"
#include "../Models/WorkerTableModel.h"

#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
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
    const bool paused = controller_->isPaused();
    const bool valid = controller_->validationMessage().isEmpty();
    const auto& snapshot = controller_->snapshot();

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
    {
        const QSignalBlocker blocker(startPausedCheck_);
        startPausedCheck_->setChecked(controller_->startPaused());
    }

    activeWorkersLabel_->setText(running
        ? QString::number(controller_->activeWorkers())
        : QStringLiteral("--"));
    statusValueLabel_->setText(!running ? QStringLiteral("Stopped") : paused ? QStringLiteral("Paused") : QStringLiteral("Running"));
    statusValueLabel_->setProperty("coordinatorState", !running ? QStringLiteral("stopped") : paused ? QStringLiteral("paused") : QStringLiteral("running"));
    statusValueLabel_->style()->unpolish(statusValueLabel_);
    statusValueLabel_->style()->polish(statusValueLabel_);

    snapshotCountLabel_->setText(QStringLiteral("%1 rows").arg(snapshot.size()));

    pauseButton_->setText(paused ? QStringLiteral("Resume") : QStringLiteral("Pause"));
    pauseButton_->setProperty("coordinatorPaused", paused);
    pauseButton_->style()->unpolish(pauseButton_);
    pauseButton_->style()->polish(pauseButton_);

    validationLabel_->setText(valid
        ? QStringLiteral("Configuration looks good. You can start the coordinator when ready.")
        : controller_->validationMessage());
    validationLabel_->setProperty("validationState", valid ? QStringLiteral("ok") : QStringLiteral("warn"));
    validationLabel_->style()->unpolish(validationLabel_);
    validationLabel_->style()->polish(validationLabel_);

    tableSummaryLabel_->setText(running
        ? QStringLiteral("Live worker telemetry refreshes every %1 ms.").arg(kRefreshIntervalMs)
        : QStringLiteral("Start the coordinator to populate the live worker table."));

    syncActionButtonStates(running, valid);
    setControlsEnabledForRunningState(running);
    stoppedLabel_->setVisible(!running);
    workerTableView_->setVisible(running);

    workerTableModel_->setSnapshots(snapshot);
}

void CoordinatorPane::browseForIsoPath()
{
    const QString initialPath = isoPathEdit_->text().trimmed();
    const QString selectedPath = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Select Skies of Arcadia ISO"),
        initialPath);

    if (!selectedPath.isEmpty()) {
        isoPathEdit_->setText(selectedPath);
    }
}

void CoordinatorPane::browseForDolphinBaseDir()
{
    const QString initialPath = dolphinBaseDirEdit_->text().trimmed();
    const QString selectedDir = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Select Dolphin base directory"),
        initialPath);

    if (!selectedDir.isEmpty()) {
        dolphinBaseDirEdit_->setText(selectedDir);
    }
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
    workerTableView_->setShowGrid(true);
    workerTableView_->setSortingEnabled(false);
    workerTableView_->verticalHeader()->setVisible(false);
    workerTableView_->verticalHeader()->setDefaultSectionSize(28);
    workerTableView_->horizontalHeader()->setStretchLastSection(true);
    workerTableView_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    workerTableView_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
}

QWidget* CoordinatorPane::createControlsCard()
{
    QFrame* card = new QFrame(this);
    card->setObjectName("coordinatorCard");

    QVBoxLayout* rootLayout = new QVBoxLayout(card);
    rootLayout->setContentsMargins(16, 16, 16, 16);
    rootLayout->setSpacing(14);

    QHBoxLayout* headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(12);

    QLabel* heading = new QLabel("Coordinator Control", card);
    heading->setObjectName("panelTitle");

    QLabel* subheading = new QLabel("Match the original SoaSim flow: configure once, then manage start / pause / stop from a single command row.", card);
    subheading->setObjectName("panelBody");
    subheading->setWordWrap(true);

    QVBoxLayout* headingLayout = new QVBoxLayout();
    headingLayout->setSpacing(4);
    headingLayout->addWidget(heading);
    headingLayout->addWidget(subheading);

    headerLayout->addLayout(headingLayout, 1);
    headerLayout->addWidget(createMetricCard("Status", &statusValueLabel_, "coordinatorStateBadge"));
    headerLayout->addWidget(createMetricCard("Active workers", &activeWorkersLabel_));
    headerLayout->addWidget(createMetricCard("Snapshot", &snapshotCountLabel_));

    rootLayout->addLayout(headerLayout);

    QHBoxLayout* controlsLayout = new QHBoxLayout();
    controlsLayout->setSpacing(10);

    startButton_ = new QPushButton("Start", card);
    startButton_->setObjectName("jobsPrimaryButton");
    pauseButton_ = new QPushButton("Pause", card);
    pauseButton_->setObjectName("jobsSecondaryButton");
    stopButton_ = new QPushButton("Stop", card);
    stopButton_->setObjectName("jobsSecondaryButton");
    targetWorkersSpin_ = new QSpinBox(card);
    targetWorkersSpin_->setObjectName("jobsRefreshSpin");
    targetWorkersSpin_->setMinimum(1);
    targetWorkersSpin_->setMaximum(9999);
    targetWorkersSpin_->setPrefix("Target: ");

    controlsLayout->addWidget(startButton_);
    controlsLayout->addWidget(pauseButton_);
    controlsLayout->addWidget(stopButton_);
    controlsLayout->addWidget(targetWorkersSpin_);
    controlsLayout->addStretch();

    rootLayout->addLayout(controlsLayout);

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
    rootLayout->setSpacing(12);

    QLabel* heading = new QLabel("Coordinator Settings", card);
    heading->setObjectName("panelTitle");

    QLabel* body = new QLabel("These fields mirror the SoaSimGui pre-start inputs. They remain visible while running, but only editable while the coordinator is stopped.", card);
    body->setObjectName("panelBody");
    body->setWordWrap(true);

    rootLayout->addWidget(heading);
    rootLayout->addWidget(body);

    QGridLayout* formLayout = new QGridLayout();
    formLayout->setHorizontalSpacing(12);
    formLayout->setVerticalSpacing(10);

    isoPathEdit_ = new QLineEdit(card);
    isoPathEdit_->setPlaceholderText("Path to SkiesOfArcadia iso");
    isoBrowseButton_ = new QPushButton("Browse…", card);

    dolphinBaseDirEdit_ = new QLineEdit(card);
    dolphinBaseDirEdit_->setPlaceholderText("Path to DolphinQt base directory with portable.txt");
    dolphinBrowseButton_ = new QPushButton("Browse…", card);

    eventBufferSpin_ = new QSpinBox(card);
    eventBufferSpin_->setObjectName("jobsRefreshSpin");
    eventBufferSpin_->setMinimum(8);
    eventBufferSpin_->setMaximum(1000000);
    eventBufferSpin_->setPrefix("Event ring: ");

    startPausedCheck_ = new QCheckBox("Start paused", card);

    QHBoxLayout* isoLayout = new QHBoxLayout();
    isoLayout->setContentsMargins(0, 0, 0, 0);
    isoLayout->setSpacing(8);
    isoLayout->addWidget(isoPathEdit_, 1);
    isoLayout->addWidget(isoBrowseButton_);

    QHBoxLayout* dolphinLayout = new QHBoxLayout();
    dolphinLayout->setContentsMargins(0, 0, 0, 0);
    dolphinLayout->setSpacing(8);
    dolphinLayout->addWidget(dolphinBaseDirEdit_, 1);
    dolphinLayout->addWidget(dolphinBrowseButton_);

    formLayout->addWidget(createFieldCaption("ISO", card), 0, 0);
    formLayout->addLayout(isoLayout, 0, 1);
    formLayout->addWidget(createFieldCaption("Dolphin base", card), 1, 0);
    formLayout->addLayout(dolphinLayout, 1, 1);
    formLayout->addWidget(createFieldCaption("Buffer + startup", card), 2, 0);

    QHBoxLayout* compactControls = new QHBoxLayout();
    compactControls->setSpacing(10);
    compactControls->addWidget(eventBufferSpin_);
    compactControls->addWidget(startPausedCheck_);
    compactControls->addStretch();
    formLayout->addLayout(compactControls, 2, 1);

    validationLabel_ = new QLabel(card);
    validationLabel_->setObjectName("coordinatorValidation");
    validationLabel_->setWordWrap(true);

    rootLayout->addLayout(formLayout);
    rootLayout->addWidget(validationLabel_);

    connect(isoPathEdit_, &QLineEdit::textChanged, controller_, &CoordinatorController::setIsoPath);
    connect(dolphinBaseDirEdit_, &QLineEdit::textChanged, controller_, &CoordinatorController::setDolphinBaseDir);
    connect(isoBrowseButton_, &QPushButton::clicked, this, &CoordinatorPane::browseForIsoPath);
    connect(dolphinBrowseButton_, &QPushButton::clicked, this, &CoordinatorPane::browseForDolphinBaseDir);
    connect(eventBufferSpin_, qOverload<int>(&QSpinBox::valueChanged), controller_, &CoordinatorController::setEventBufferCapacity);
    connect(startPausedCheck_, &QCheckBox::toggled, controller_, &CoordinatorController::setStartPaused);

    return card;
}

QWidget* CoordinatorPane::createTableCard()
{
    QFrame* card = new QFrame(this);
    card->setObjectName("coordinatorCard");

    QVBoxLayout* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    QHBoxLayout* headerLayout = new QHBoxLayout();
    QLabel* heading = new QLabel("Live Workers", card);
    heading->setObjectName("panelTitle");
    tableSummaryLabel_ = new QLabel(card);
    tableSummaryLabel_->setObjectName("panelBody");
    headerLayout->addWidget(heading);
    headerLayout->addStretch();
    headerLayout->addWidget(tableSummaryLabel_);

    stoppedLabel_ = new QLabel("Coordinator is stopped.", card);
    stoppedLabel_->setObjectName("panelBody");

    workerTableView_ = new QTableView(card);
    workerTableView_->setObjectName("coordinatorTableView");
    configureTable();

    layout->addLayout(headerLayout);
    layout->addWidget(stoppedLabel_);
    layout->addWidget(workerTableView_, 1);

    return card;
}

QWidget* CoordinatorPane::createMetricCard(const QString& caption, QLabel** valueLabel, const QString& objectName)
{
    QFrame* frame = new QFrame(this);
    frame->setObjectName("coordinatorMetricCard");

    QVBoxLayout* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(2);

    QLabel* captionLabel = new QLabel(caption, frame);
    captionLabel->setObjectName("coordinatorMetricCaption");

    QLabel* value = new QLabel("--", frame);
    value->setObjectName(objectName.isEmpty() ? QStringLiteral("coordinatorMetricValue") : objectName);

    layout->addWidget(captionLabel);
    layout->addWidget(value);

    *valueLabel = value;
    return frame;
}

QLabel* CoordinatorPane::createFieldCaption(const QString& text, QWidget* parent) const
{
    QLabel* label = new QLabel(text, parent);
    label->setObjectName("coordinatorFieldCaption");
    return label;
}

void CoordinatorPane::setControlsEnabledForRunningState(bool running)
{
    pauseButton_->setEnabled(running);
    stopButton_->setEnabled(running);
    isoPathEdit_->setEnabled(!running);
    isoBrowseButton_->setEnabled(!running);
    dolphinBaseDirEdit_->setEnabled(!running);
    dolphinBrowseButton_->setEnabled(!running);
    eventBufferSpin_->setEnabled(!running);
    startPausedCheck_->setEnabled(!running);
}

void CoordinatorPane::syncActionButtonStates(bool running, bool valid)
{
    startButton_->setEnabled(!running && valid);
}
