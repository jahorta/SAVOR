#include "CoordinatorPane.h"

#include "CoordinatorController.h"
#include "WorkerTableModel.h"
#include "GUI/Widgets/ScrollBarStabilizer.h"

#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTreeView>
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
    const QString validationMessage = controller_->validationMessage();
    const bool valid = validationMessage.isEmpty();
    const auto& snapshot = controller_->snapshot();

    {
        const QSignalBlocker blocker(targetWorkersSpin_);
        targetWorkersSpin_->setValue(controller_->targetWorkers());
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

    QString validationText = QStringLiteral("Configuration looks good. You can start the coordinator when ready.");
    if (!valid) {
        QStringList issueLinks;
        if (validationMessage.contains(QStringLiteral("ISO path is required."))) {
            issueLinks.append(QStringLiteral("ISO path is required!"));
        }
        if (validationMessage.contains(QStringLiteral("Dolphin base directory is required."))) {
            issueLinks.append(QStringLiteral("Dolphin base directory is required!"));
        }
        if (!issueLinks.isEmpty()) {
            validationText = issueLinks.join(QStringLiteral(" "));
            validationText += QStringLiteral(" <a href=\"settings://coordinator\">Open coordinator settings.</a>");
        }
    }
    validationLabel_->setText(validationText);
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

    const ItemViewScrollSnapshot scrollSnapshot = captureItemViewScrollSnapshot(workerTableView_);
    workerTableModel_->setSnapshots(snapshot);
    restoreItemViewScrollSnapshot(workerTableView_, scrollSnapshot);
}

void CoordinatorPane::createWidgets()
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    workerTableModel_ = new WorkerTableModel(this);

    layout->addWidget(createControlsCard());
    layout->addWidget(createTableCard(), 1);
}

void CoordinatorPane::configureTable()
{
    workerTableView_->setModel(workerTableModel_);
    workerTableView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    workerTableView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    workerTableView_->setSelectionMode(QAbstractItemView::SingleSelection);
    workerTableView_->setAlternatingRowColors(true);
    workerTableView_->setSortingEnabled(false);
    workerTableView_->setRootIsDecorated(false);
    workerTableView_->setItemsExpandable(false);
    workerTableView_->setAllColumnsShowFocus(true);
    workerTableView_->setUniformRowHeights(true);
    workerTableView_->setIndentation(0);
    workerTableView_->header()->setStretchLastSection(true);
    workerTableView_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    workerTableView_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
}

QWidget* CoordinatorPane::createControlsCard()
{
    QFrame* card = new QFrame(this);
    card->setObjectName("coordinatorCard");

    QVBoxLayout* rootLayout = new QVBoxLayout(card);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(10);

    QHBoxLayout* topLayout = new QHBoxLayout();
    topLayout->setSpacing(10);

    QVBoxLayout* controlColumnLayout = new QVBoxLayout();
    controlColumnLayout->setSpacing(10);

    QLabel* heading = new QLabel("Coordinator Control", card);
    heading->setObjectName("panelTitle");
    controlColumnLayout->addWidget(heading);

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

    controlColumnLayout->addLayout(controlsLayout);
    topLayout->addLayout(controlColumnLayout, 1);

    QHBoxLayout* metricsLayout = new QHBoxLayout();
    metricsLayout->setSpacing(10);
    metricsLayout->addWidget(createMetricCard("Status", &statusValueLabel_, "coordinatorStateBadge"));
    metricsLayout->addWidget(createMetricCard("Active workers", &activeWorkersLabel_));
    metricsLayout->addWidget(createMetricCard("Snapshot", &snapshotCountLabel_));
    topLayout->addLayout(metricsLayout);

    rootLayout->addLayout(topLayout);

    validationLabel_ = new QLabel(card);
    validationLabel_->setObjectName("coordinatorValidation");
    validationLabel_->setWordWrap(true);
    validationLabel_->setTextFormat(Qt::RichText);
    validationLabel_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    validationLabel_->setOpenExternalLinks(false);
    rootLayout->addWidget(validationLabel_);
    connect(validationLabel_, &QLabel::linkActivated, this, &CoordinatorPane::handleValidationLinkActivated);

    connect(startButton_, &QPushButton::clicked, controller_, &CoordinatorController::startCoordinator);
    connect(pauseButton_, &QPushButton::clicked, controller_, &CoordinatorController::togglePaused);
    connect(stopButton_, &QPushButton::clicked, controller_, &CoordinatorController::stopCoordinator);
    connect(targetWorkersSpin_, qOverload<int>(&QSpinBox::valueChanged), controller_, &CoordinatorController::setTargetWorkers);

    return card;
}

QWidget* CoordinatorPane::createTableCard()
{
    QFrame* card = new QFrame(this);
    card->setObjectName("coordinatorCard");

    QVBoxLayout* layout = new QVBoxLayout(card);
    layout->setContentsMargins(12, 12, 12, 12);
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

    workerTableView_ = new QTreeView(card);
    workerTableView_->setObjectName("coordinatorTableView");
    configureTable();

    layout->addLayout(headerLayout);
    layout->addWidget(stoppedLabel_, 1);
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

void CoordinatorPane::setControlsEnabledForRunningState(bool running)
{
    pauseButton_->setEnabled(running);
    stopButton_->setEnabled(running);
}

void CoordinatorPane::syncActionButtonStates(bool running, bool valid)
{
    startButton_->setEnabled(!running && valid);
}

void CoordinatorPane::handleValidationLinkActivated(const QString& link)
{
    if (link == QStringLiteral("settings://coordinator")) {
        emit settingsNavigationRequested(SettingsFocusTarget::CoordinatorSection);
    }
}
