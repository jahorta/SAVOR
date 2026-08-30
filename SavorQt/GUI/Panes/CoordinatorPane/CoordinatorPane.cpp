#include "CoordinatorPane.h"

#include "CoordinatorController.h"
#include "WorkerTableModel.h"
#include "GUI/Widgets/ScrollBarStabilizer.h"
#include "GUI/Widgets/VisualReplay/VisualReplayCoordinator.h"
#include "GUI/Widgets/VisualReplay/VisualReplayWindow.h"
#include "GUI/Widgets/VisualReplay/VisualWorkerDashboardWindow.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSignalBlocker>
#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTreeView>
#include <QtWidgets/QVBoxLayout>

CoordinatorPane::CoordinatorPane(QWidget* parent)
    : QWidget(parent, Qt::Window)
    , controller_(new CoordinatorController(this))
{
    setWindowTitle(QStringLiteral("Workers"));
    createWidgets();

    connect(controller_, &CoordinatorController::stateChanged, this, &CoordinatorPane::refreshUi);
    connect(controller_, &CoordinatorController::snapshotChanged, this, &CoordinatorPane::refreshUi);

    refreshUi();
}

CoordinatorPane::~CoordinatorPane()
{
    if (visualWorkerDashboard_ != nullptr) {
        visualWorkerDashboard_->close();
        delete visualWorkerDashboard_;
        visualWorkerDashboard_ = nullptr;
    }
}

void CoordinatorPane::refreshUi()
{
    if (controller_ == nullptr) {
        return;
    }

    const auto lifecycleState = controller_->lifecycleState();
    const bool running = lifecycleState == CoordinatorLifecycleState::Running;
    const bool paused = controller_->isPaused();
    const QString validationMessage = controller_->validationMessage();
    const bool valid = validationMessage.isEmpty();
    const auto& snapshot = controller_->snapshot();
    const auto& warnings = controller_->warningSnapshot();

    {
        const QSignalBlocker blocker(targetWorkersSpin_);
        targetWorkersSpin_->setValue(controller_->targetWorkers());
    }
    {
        const QSignalBlocker blocker(visualWorkersCheck_);
        visualWorkersCheck_->setChecked(controller_->visualWorkerPoolEnabled());
    }

    activeWorkersLabel_->setText(running
        ? QString::number(controller_->activeWorkers())
        : QStringLiteral("--"));
    QString lifecycleText = QStringLiteral("Stopped");
    QString lifecycleProperty = QStringLiteral("stopped");
    if (lifecycleState == CoordinatorLifecycleState::Starting) {
        lifecycleText = QStringLiteral("Starting");
        lifecycleProperty = QStringLiteral("starting");
    } else if (lifecycleState == CoordinatorLifecycleState::Stopping) {
        lifecycleText = QStringLiteral("Stopping");
        lifecycleProperty = QStringLiteral("stopping");
    } else if (running && paused) {
        lifecycleText = QStringLiteral("Paused");
        lifecycleProperty = QStringLiteral("paused");
    } else if (running) {
        lifecycleText = QStringLiteral("Running");
        lifecycleProperty = QStringLiteral("running");
    }
    statusValueLabel_->setText(lifecycleText);
    statusValueLabel_->setProperty("coordinatorState", lifecycleProperty);
    statusValueLabel_->style()->unpolish(statusValueLabel_);
    statusValueLabel_->style()->polish(statusValueLabel_);

    snapshotCountLabel_->setText(QString::number(snapshot.size()));

    pauseButton_->setText(paused ? QStringLiteral("Resume") : QStringLiteral("Pause"));
    pauseButton_->setProperty("coordinatorPaused", paused);
    pauseButton_->style()->unpolish(pauseButton_);
    pauseButton_->style()->polish(pauseButton_);

    QString validationText = QStringLiteral("Configuration looks good. You can start the coordinator when ready.");
    if (!valid) {
        QStringList issueLinks;
        if (validationMessage.contains(QStringLiteral("ISO path"))) {
            issueLinks.append(QStringLiteral("ISO path needs attention."));
        }
        if (validationMessage.contains(QStringLiteral("Dolphin base"))) {
            issueLinks.append(QStringLiteral("Dolphin base needs attention."));
        }
        validationText = issueLinks.isEmpty() ? validationMessage : issueLinks.join(QStringLiteral(" "));
        validationText += QStringLiteral(" <a href=\"settings://coordinator\">Open coordinator settings.</a>");
    }
    validationLabel_->setText(validationText);
    validationLabel_->setProperty("validationState", valid ? QStringLiteral("ok") : QStringLiteral("warn"));
    validationLabel_->style()->unpolish(validationLabel_);
    validationLabel_->style()->polish(validationLabel_);

    if (warnings.empty()) {
        warningLabel_->setVisible(false);
        warningLabel_->clear();
    } else {
        const auto& warning = warnings.back();
        warningLabel_->setVisible(true);
        warningLabel_->setText(QStringLiteral("Coordinator warning: %1 Job #%2, worker %3. %4")
            .arg(QString::fromStdString(warning.message))
            .arg(warning.job_id)
            .arg(warning.worker_id)
            .arg(QString::fromStdString(warning.detail)));
    }

    if (!validationMessage.isEmpty()) {
        const QString signature = QStringLiteral("validation|%1").arg(validationMessage);
        if (signature != lastToastSignature_) {
            lastToastSignature_ = signature;
            emit statusToastRequested(StatusToast{
                StatusToast::Severity::Warn,
                QStringLiteral("Coordinator configuration needs attention."),
                validationMessage,
                1,
                QDateTime::currentDateTimeUtc(),
                5000
            });
        }
    } else {
        lastToastSignature_.clear();
    }

    const QString cleanupError = controller_->resultStagingCleanupError();
    if (!cleanupError.isEmpty()) {
        const QString signature = QStringLiteral("result-staging|%1")
            .arg(cleanupError);
        if (signature != lastCleanupToastSignature_) {
            lastCleanupToastSignature_ = signature;
            emit statusToastRequested(StatusToast{
                StatusToast::Severity::Error,
                QStringLiteral("Result staging cleanup blocked."),
                cleanupError,
                1,
                QDateTime::currentDateTimeUtc(),
                6000,
            });
        }
    }

    tableSummaryLabel_->setText(running
        ? QStringLiteral("Worker telemetry refreshes continuously while the coordinator is running.")
        : QStringLiteral("Start the coordinator to populate the live worker table."));

    syncActionButtonStates(lifecycleState, valid);
    setControlsEnabledForLifecycleState(lifecycleState);
    stoppedLabel_->setVisible(!running);
    workerTableView_->setVisible(running);

    const ItemViewScrollSnapshot scrollSnapshot = captureItemViewScrollSnapshot(workerTableView_);
    workerTableModel_->setSnapshots(snapshot);
    restoreItemViewScrollSnapshot(workerTableView_, scrollSnapshot);

    syncVisualReplayWindow();
    syncVisualWorkerDashboard();
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

void CoordinatorPane::configureTable(QTreeView* tableView)
{
    tableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableView->setSelectionMode(QAbstractItemView::SingleSelection);
    tableView->setAlternatingRowColors(true);
    tableView->setSortingEnabled(false);
    tableView->setRootIsDecorated(false);
    tableView->setItemsExpandable(false);
    tableView->setAllColumnsShowFocus(true);
    tableView->setUniformRowHeights(true);
    tableView->setIndentation(0);
    tableView->header()->setStretchLastSection(true);
    tableView->header()->setSectionResizeMode(QHeaderView::Interactive);
    tableView->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    tableView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(
        tableView,
        &QTreeView::customContextMenuRequested,
        this,
        &CoordinatorPane::showWorkerContextMenu);
}

CoordinatorController* CoordinatorPane::controller() const
{
    return controller_;
}

void CoordinatorPane::startCoordinator()
{
    handleStartRequested();
}

void CoordinatorPane::stopCoordinator()
{
    if (controller_ != nullptr) {
        controller_->stopCoordinator();
    }
}

void CoordinatorPane::togglePaused()
{
    if (controller_ != nullptr) {
        controller_->togglePaused();
    }
}

void CoordinatorPane::setTargetWorkers(int targetWorkers)
{
    handleTargetWorkersChanged(targetWorkers);
}

void CoordinatorPane::setVisualWorkerPoolEnabled(bool enabled)
{
    handleVisualWorkersToggled(enabled);
}

void CoordinatorPane::showVisualWorkers()
{
    showVisualWorkerDashboard();
}

void CoordinatorPane::showWorkerContextMenu(const QPoint& position)
{
    if (!workerTableView_ || !workerTableModel_)
        return;
    const QModelIndex index = workerTableView_->indexAt(position);
    const auto snapshot = workerTableModel_->snapshotAt(index.row());
    if (!snapshot || snapshot->log_path.empty())
        return;

    const QFileInfo logInfo(QString::fromStdString(snapshot->log_path));
    QMenu menu(workerTableView_);
    QAction* openLog = menu.addAction(QStringLiteral("Open current log"));
    QAction* openFolder = menu.addAction(QStringLiteral("Open log folder"));
    openLog->setEnabled(logInfo.exists() && logInfo.isFile());
    openFolder->setEnabled(logInfo.dir().exists());
    QAction* selected = menu.exec(
        workerTableView_->viewport()->mapToGlobal(position));
    if (selected == openLog) {
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(logInfo.absoluteFilePath()));
    } else if (selected == openFolder) {
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(logInfo.absolutePath()));
    }
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

    QLabel* heading = new QLabel(QStringLiteral("Coordinator Control"), card);
    heading->setObjectName("panelTitle");
    controlColumnLayout->addWidget(heading);

    QHBoxLayout* controlsLayout = new QHBoxLayout();
    controlsLayout->setSpacing(10);

    startButton_ = new QPushButton(QStringLiteral("Start"), card);
    startButton_->setObjectName("jobsPrimaryButton");
    pauseButton_ = new QPushButton(QStringLiteral("Pause"), card);
    pauseButton_->setObjectName("jobsSecondaryButton");
    stopButton_ = new QPushButton(QStringLiteral("Stop"), card);
    stopButton_->setObjectName("jobsSecondaryButton");
    visualWorkersCheck_ = new QCheckBox(QStringLiteral("Visual workers"), card);
    visualWorkersCheck_->setObjectName("jobsSecondaryButton");
    visualDashboardButton_ = new QPushButton(QStringLiteral("Visual Dashboard"), card);
    visualDashboardButton_->setObjectName("jobsSecondaryButton");
    targetWorkersSpin_ = new QSpinBox(card);
    targetWorkersSpin_->setObjectName("jobsRefreshSpin");
    targetWorkersSpin_->setMinimum(1);
    targetWorkersSpin_->setMaximum(static_cast<int>(
        savor::runner::parallel::savordb::kMaximumWorkerCount));
    targetWorkersSpin_->setPrefix(QStringLiteral("Target: "));
    controlsLayout->addWidget(startButton_);
    controlsLayout->addWidget(pauseButton_);
    controlsLayout->addWidget(stopButton_);
    controlsLayout->addWidget(targetWorkersSpin_);
    controlsLayout->addWidget(visualWorkersCheck_);
    controlsLayout->addWidget(visualDashboardButton_);
    controlsLayout->addStretch();

    controlColumnLayout->addLayout(controlsLayout);
    topLayout->addLayout(controlColumnLayout, 1);

    QHBoxLayout* metricsLayout = new QHBoxLayout();
    metricsLayout->setSpacing(10);
    metricsLayout->addWidget(createMetricCard(QStringLiteral("Status"), &statusValueLabel_, QStringLiteral("coordinatorStateBadge")));
    metricsLayout->addWidget(createMetricCard(QStringLiteral("Active workers"), &activeWorkersLabel_));
    metricsLayout->addWidget(createMetricCard(QStringLiteral("Workers"), &snapshotCountLabel_));
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

    warningLabel_ = new QLabel(card);
    warningLabel_->setObjectName("coordinatorWarning");
    warningLabel_->setWordWrap(true);
    warningLabel_->setVisible(false);
    rootLayout->addWidget(warningLabel_);

    connect(startButton_, &QPushButton::clicked, this, &CoordinatorPane::handleStartRequested);
    connect(pauseButton_, &QPushButton::clicked, controller_, &CoordinatorController::togglePaused);
    connect(stopButton_, &QPushButton::clicked, controller_, &CoordinatorController::stopCoordinator);
    connect(targetWorkersSpin_, qOverload<int>(&QSpinBox::valueChanged), this, &CoordinatorPane::handleTargetWorkersChanged);
    connect(visualWorkersCheck_, &QCheckBox::toggled, this, &CoordinatorPane::handleVisualWorkersToggled);
    connect(visualDashboardButton_, &QPushButton::clicked, this, &CoordinatorPane::showVisualWorkerDashboard);

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
    QLabel* heading = new QLabel(QStringLiteral("Live Workers"), card);
    heading->setObjectName("panelTitle");
    tableSummaryLabel_ = new QLabel(card);
    tableSummaryLabel_->setObjectName("panelBody");
    headerLayout->addWidget(heading);
    headerLayout->addStretch();
    headerLayout->addWidget(tableSummaryLabel_);

    stoppedLabel_ = new QLabel(QStringLiteral("Coordinator is stopped."), card);
    stoppedLabel_->setObjectName("panelBody");

    workerTableView_ = new QTreeView(card);
    workerTableView_->setObjectName("coordinatorTableView");
    workerTableView_->setModel(workerTableModel_);
    configureTable(workerTableView_);

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

    QLabel* value = new QLabel(QStringLiteral("--"), frame);
    value->setObjectName(objectName.isEmpty() ? QStringLiteral("coordinatorMetricValue") : objectName);

    layout->addWidget(captionLabel);
    layout->addWidget(value);

    *valueLabel = value;
    return frame;
}

void CoordinatorPane::setControlsEnabledForLifecycleState(
    CoordinatorLifecycleState state)
{
    const bool running = state == CoordinatorLifecycleState::Running;
    const bool starting = state == CoordinatorLifecycleState::Starting;
    const bool stopped = state == CoordinatorLifecycleState::Stopped;
    pauseButton_->setEnabled(running);
    stopButton_->setText(starting ? QStringLiteral("Cancel startup") : QStringLiteral("Stop"));
    stopButton_->setEnabled(running || starting);
    targetWorkersSpin_->setEnabled(stopped || running);
    visualWorkersCheck_->setEnabled(stopped);
    visualDashboardButton_->setEnabled(
        (stopped || running)
        && controller_ != nullptr
        && controller_->visualWorkerPoolEnabled());
}

void CoordinatorPane::syncActionButtonStates(
    CoordinatorLifecycleState state,
    bool valid)
{
    startButton_->setEnabled(
        state == CoordinatorLifecycleState::Stopped && valid);
}

void CoordinatorPane::handleValidationLinkActivated(const QString& link)
{
    if (link == QStringLiteral("settings://coordinator")) {
        emit settingsNavigationRequested(SettingsFocusTarget::CoordinatorSection);
    }
}

void CoordinatorPane::handleStartRequested()
{
    if (controller_ == nullptr) {
        return;
    }

    if (controller_->visualWorkerPoolEnabled()) {
        ensureVisualWorkerDashboardSurfaces(controller_->targetWorkers());
        showVisualWorkerDashboard();
    }

    controller_->startCoordinator();
}

void CoordinatorPane::handleTargetWorkersChanged(int targetWorkers)
{
    if (controller_ == nullptr) {
        return;
    }

    const int previousTarget = controller_->targetWorkers();
    if (controller_->visualWorkerPoolEnabled() &&
        targetWorkers > previousTarget) {
        ensureVisualWorkerDashboardSurfaces(targetWorkers);
    }
    controller_->setTargetWorkers(targetWorkers);
    if (controller_->visualWorkerPoolEnabled() && visualWorkerDashboard_) {
        if (controller_->isStopped()) {
            visualWorkerDashboard_->releaseWorkersFrom(
                controller_->targetWorkers());
        } else {
            syncVisualWorkerDashboard();
        }
    }
}

void CoordinatorPane::handleVisualWorkersToggled(bool enabled)
{
    if (controller_ == nullptr) {
        return;
    }

    controller_->setVisualWorkerPoolEnabled(enabled);
    if (!controller_->visualWorkerPoolEnabled()) {
        controller_->clearVisualWorkerSurfaces();
        if (visualWorkerDashboard_) {
            visualWorkerDashboard_->hide();
        }
        return;
    }

    ensureVisualWorkerDashboardSurfaces(controller_->targetWorkers());
    showVisualWorkerDashboard();
}

void CoordinatorPane::showVisualWorkerDashboard()
{
    if (controller_ == nullptr || !controller_->visualWorkerPoolEnabled()) {
        return;
    }

    ensureVisualWorkerDashboardSurfaces(controller_->targetWorkers());
    visualWorkerDashboard_->show();
    visualWorkerDashboard_->raise();
    visualWorkerDashboard_->activateWindow();
}

void CoordinatorPane::closeVisualWorkersForApplicationClose()
{
    if (visualWorkerDashboard_ != nullptr) {
        visualWorkerDashboard_->close();
    }
}

void CoordinatorPane::syncVisualReplayWindow()
{
    if (!visualReplayWindow_ || controller_ == nullptr) {
        return;
    }

    const QString stateText = controller_->visualReplayRuntimeStateText();
    visualReplayWindow_->setReplayRuntimeStateText(stateText);
    visualReplayWindow_->setReplayControlsEnabled(controller_->visualReplayControlsEnabled());

    if (!visualReplayDoneShown_ && stateText.startsWith(QStringLiteral("Finished"))) {
        visualReplayDoneShown_ = true;
        visualReplayWindow_->showReplayDoneLabel();
    }
}

void CoordinatorPane::ensureVisualWorkerDashboardSurfaces(int workerCount)
{
    if (controller_ == nullptr) {
        return;
    }
    if (!visualWorkerDashboard_) {
        visualWorkerDashboard_ = new VisualWorkerDashboardWindow();
        connect(
            visualWorkerDashboard_,
            &VisualWorkerDashboardWindow::surfaceChanged,
            this,
            [this](const VisualWorkerSurfaceBinding& binding) {
                controller_->setVisualWorkerSurface(
                    binding.workerIndex,
                    binding.renderWidgetHandle,
                    binding.surfaceGeneration,
                    binding.ownerProcessId,
                    binding.hostEventsPipeName);
            });
        connect(
            visualWorkerDashboard_,
            &VisualWorkerDashboardWindow::surfaceInvalidated,
            controller_,
            &CoordinatorController::invalidateVisualWorkerSurface);
    }

    visualWorkerDashboard_->ensureWorkerCount(workerCount);
    for (const VisualWorkerSurfaceBinding& binding : visualWorkerDashboard_->surfaceBindings()) {
        controller_->setVisualWorkerSurface(
            binding.workerIndex,
            binding.renderWidgetHandle,
            binding.surfaceGeneration,
            binding.ownerProcessId,
            binding.hostEventsPipeName);
    }
    syncVisualWorkerDashboard();
}

void CoordinatorPane::syncVisualWorkerDashboard()
{
    if (!visualWorkerDashboard_ || controller_ == nullptr) {
        return;
    }

    QSet<int> activeWorkerIds;
    for (const WorkerSnapshot& snapshot : controller_->snapshot()) {
        activeWorkerIds.insert(static_cast<int>(snapshot.worker_id));
        visualWorkerDashboard_->updateWorkerStatus(snapshot);
    }
    visualWorkerDashboard_->clearWorkerStatusesExcept(activeWorkerIds);

    const int targetWorkers = controller_->targetWorkers();
    while (visualWorkerDashboard_->workerCount() > targetWorkers) {
        const int workerIndex = visualWorkerDashboard_->workerCount() - 1;
        const auto found = std::find_if(
            controller_->snapshot().begin(),
            controller_->snapshot().end(),
            [workerIndex](const WorkerSnapshot& snapshot) {
                return snapshot.worker_id ==
                    static_cast<std::size_t>(workerIndex);
            });
        if (found != controller_->snapshot().end() &&
            found->state != WorkerStateKind::Dead) {
            break;
        }
        visualWorkerDashboard_->releaseWorkersFrom(workerIndex);
    }
}

void CoordinatorPane::requestVisualReplay(qint64 jobId)
{
    if (controller_ == nullptr) {
        return;
    }
    if (!controller_->isRunning()) {
        emit statusToastRequested(StatusToast{
            StatusToast::Severity::Warn,
            QStringLiteral("Start the coordinator before visual debug replay."),
            QStringLiteral("The DB workflow visual debug session uses the active coordinator configuration and worker runtime."),
            1,
            QDateTime::currentDateTimeUtc(),
            6000
        });
        return;
    }
    if (!visualReplayWindow_) {
        visualReplayWindow_ = new VisualReplayWindow(this);
        connect(visualReplayWindow_, &VisualReplayWindow::aboutToClose, this, [this]() {
            visualReplayWindow_->stopLiveLogStreaming();
            visualReplayWindow_->stopHostEventsListener();
            controller_->stopVisualReplay();
            controller_->setVisualRenderWidgetHandle(0);
            controller_->setVisualHostEventsPipeName(QString());
        });
        connect(visualReplayWindow_, &VisualReplayWindow::pauseRequested, controller_, &CoordinatorController::pauseVisualReplayEmulation);
        connect(visualReplayWindow_, &VisualReplayWindow::vmStepRequested, controller_, &CoordinatorController::stepVisualReplayVm);
        connect(visualReplayWindow_, &VisualReplayWindow::resumeRequested, controller_, &CoordinatorController::resumeVisualReplayEmulation);
        connect(visualReplayWindow_, &VisualReplayWindow::visualLiveLogLinesRequested, controller_, &CoordinatorController::handleVisualLiveLogLinesRequested);
        connect(controller_, &CoordinatorController::visualLiveLogLinesReady, visualReplayWindow_->visualReplayCoordinator(), &VisualReplayCoordinator::setLiveLogLines);
    }

    visualReplayDoneShown_ = false;
    visualReplayWindow_->showRenderSurface();
    visualReplayWindow_->setReplayRuntimeStateText(QStringLiteral("Queued startup"));
    visualReplayWindow_->setReplayControlsEnabled(false);
    visualReplayWindow_->resetLiveLog();
    visualReplayWindow_->startLiveLogStreaming();
    visualReplayWindow_->startHostEventsListener();
    visualReplayWindow_->show();
    visualReplayWindow_->raise();
    visualReplayWindow_->activateWindow();

    controller_->setVisualRenderWidgetHandle(visualReplayWindow_->renderWidgetHandle());
    controller_->setVisualHostEventsPipeName(visualReplayWindow_->hostEventsPipeName());
    controller_->requestVisualReplay(jobId);
    syncVisualReplayWindow();

    emit statusToastRequested(StatusToast{
        StatusToast::Severity::Info,
        QStringLiteral("Starting visual debug replay for job %1.").arg(jobId),
        controller_->visualReplayRuntimeStateText(),
        1,
        QDateTime::currentDateTimeUtc(),
        4000
    });
}
