#include "GUI/Tabs/RunningTab.h"

#include "DB/ProgramKindNameResolver.h"
#include "DB/SavorDbJobService.h"
#include "DB/SavorDbWorkflowService.h"
#include "GUI/Panes/CoordinatorPane/CoordinatorController.h"
#include "Worker/WorkerTelemetry.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtCore/QTimeZone>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace {

struct WorkflowBuckets {
    int active = 0;
    int blocked = 0;
    int failed = 0;
    int terminal = 0;
};

struct JobBuckets {
    std::vector<savor::db::UiJobSummary> queued;
    std::vector<savor::db::UiJobSummary> running;
    std::vector<savor::db::UiJobSummary> failed;
    std::vector<savor::db::UiJobSummary> terminal;
};

QString qs(const std::string& value)
{
    return QString::fromStdString(value);
}

QString formatTime(std::int64_t epochSeconds)
{
    if (epochSeconds <= 0) {
        return QStringLiteral("--");
    }
    return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(epochSeconds), QTimeZone::fromSecondsAheadOfUtc(0))
        .toLocalTime()
        .toString(QStringLiteral("MM-dd HH:mm"));
}

QString formatOptionalTime(const std::optional<std::int64_t>& epochSeconds)
{
    return epochSeconds.has_value() ? formatTime(*epochSeconds) : QStringLiteral("--");
}

QString compactText(QString text, int maxLength = 84)
{
    text = text.simplified();
    if (text.size() <= maxLength) {
        return text;
    }
    return text.left(maxLength - 1) + QChar(0x2026);
}

QTableWidgetItem* createTableItem(const QString& text)
{
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QFrame* createStatusPill(const QString& label, QWidget* parent, QLabel** valueLabel)
{
    auto* pill = new QFrame(parent);
    pill->setObjectName("workspaceMetricTile");
    auto* layout = new QHBoxLayout(pill);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(6);

    auto* labelText = new QLabel(label + QStringLiteral(":"), pill);
    labelText->setObjectName("sectionDescription");
    auto* valueText = new QLabel(QStringLiteral("--"), pill);
    valueText->setObjectName("panelTitle");

    layout->addWidget(labelText);
    layout->addWidget(valueText);
    if (valueLabel != nullptr) {
        *valueLabel = valueText;
    }
    return pill;
}

QPushButton* createActionButton(const QString& text, QWidget* parent, bool primary = false)
{
    auto* button = new QPushButton(text, parent);
    button->setObjectName(primary ? "jobsPrimaryButton" : "jobsSecondaryButton");
    return button;
}

QFrame* createSectionPanel(const QString& title, QWidget* parent)
{
    auto* panel = new QFrame(parent);
    panel->setObjectName("workspaceHeroPanel");
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    auto* titleLabel = new QLabel(title, panel);
    titleLabel->setObjectName("panelTitle");
    layout->addWidget(titleLabel);
    return panel;
}

void clearLayout(QLayout* layout)
{
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        if (QLayout* childLayout = item->layout()) {
            clearLayout(childLayout);
            delete childLayout;
        }
        delete item;
    }
}

QScrollArea* createPanelScrollArea(QWidget* parent, QVBoxLayout** contentLayout)
{
    auto* scrollArea = new QScrollArea(parent);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content = new QWidget(scrollArea);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    scrollArea->setWidget(content);
    if (contentLayout != nullptr) {
        *contentLayout = layout;
    }
    return scrollArea;
}

void configureTable(QTableWidget* table)
{
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(28);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setShowGrid(false);
}

bool isWorkflowFailed(const savor::db::UiWorkflowInstanceSummary& workflow)
{
    return workflow.state == "FAILED" || workflow.failed_step_count > 0;
}

bool isWorkflowTerminal(const savor::db::UiWorkflowInstanceSummary& workflow)
{
    return workflow.state == "COMPLETED" || workflow.state == "SKIPPED" || workflow.state == "CANCELED";
}

bool isJobRunning(const savor::db::UiJobSummary& job)
{
    return job.state == "RUNNING" || job.state == "CLAIMED";
}

bool isJobTerminal(const savor::db::UiJobSummary& job)
{
    return job.state == "SUCCEEDED"
        || job.state == "CANCELED"
        || job.state == "SUPERSEDED"
        || job.state == "SUCCEEDED_WINNER"
        || job.state == "SUCCEEDED_DUPLICATE";
}

WorkflowBuckets bucketWorkflows(const std::vector<savor::db::UiWorkflowInstanceSummary>& workflows)
{
    WorkflowBuckets buckets{};
    for (const auto& workflow : workflows) {
        if (workflow.blocked_step_count > 0) {
            ++buckets.blocked;
        }
        if (isWorkflowFailed(workflow)) {
            ++buckets.failed;
        } else if (isWorkflowTerminal(workflow)) {
            ++buckets.terminal;
        } else {
            ++buckets.active;
        }
    }
    return buckets;
}

JobBuckets bucketJobs(const std::vector<savor::db::UiJobSummary>& jobs)
{
    JobBuckets buckets{};
    for (const auto& job : jobs) {
        if (job.state == "QUEUED") {
            buckets.queued.push_back(job);
        } else if (isJobRunning(job)) {
            buckets.running.push_back(job);
        } else if (job.state == "FAILED") {
            buckets.failed.push_back(job);
        } else if (isJobTerminal(job)) {
            buckets.terminal.push_back(job);
        }
    }
    return buckets;
}

QString programKindName(int programKind)
{
    return qs(savorqt::db::ResolveProgramKindName(programKind, "kind " + std::to_string(programKind)));
}

QString jobProblemText(const savor::db::UiJobSummary& job)
{
    if (!job.error_text.empty()) {
        return compactText(qs(job.error_text));
    }
    if (!job.error_code.empty()) {
        return compactText(qs(job.error_code));
    }
    return QStringLiteral("--");
}

QString workerStateLabel(WorkerStateKind state)
{
    switch (state) {
    case WorkerStateKind::Spawning: return QStringLiteral("Spawning");
    case WorkerStateKind::Idle: return QStringLiteral("Idle");
    case WorkerStateKind::Leasing: return QStringLiteral("Leasing");
    case WorkerStateKind::Running: return QStringLiteral("Running");
    case WorkerStateKind::Renewing: return QStringLiteral("Renewing");
    case WorkerStateKind::Paused: return QStringLiteral("Paused");
    case WorkerStateKind::Draining: return QStringLiteral("Draining");
    case WorkerStateKind::Exiting: return QStringLiteral("Exiting");
    case WorkerStateKind::Stopping: return QStringLiteral("Stopping");
    case WorkerStateKind::Dead: return QStringLiteral("Dead");
    default: return QStringLiteral("Unknown");
    }
}

QString workerStatusText(const WorkerSnapshot& worker)
{
    const bool hasProgress = !worker.last_progress.empty();
    const bool hasError = !worker.last_error.empty();
    if (hasProgress && (!hasError || worker.last_progress_mono_ns >= worker.last_error_mono_ns)) {
        return compactText(qs(worker.last_progress));
    }
    if (hasError) {
        return compactText(qs(worker.last_error));
    }
    if (worker.consecutive_failures > 0) {
        return QStringLiteral("%1 consecutive failures").arg(worker.consecutive_failures);
    }
    return QStringLiteral("--");
}

bool workerNeedsAttention(const WorkerSnapshot& worker)
{
    return worker.state == WorkerStateKind::Dead
        || !worker.last_error.empty()
        || worker.consecutive_failures > 0;
}

QFrame* createQueueRow(const savor::db::UiJobSummary& job, QWidget* parent)
{
    auto* row = new QFrame(parent);
    row->setObjectName("workspaceInfoPanel");
    auto* layout = new QGridLayout(row);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setHorizontalSpacing(8);
    layout->setVerticalSpacing(2);

    auto* title = new QLabel(QStringLiteral("#%1  %2").arg(job.job_id).arg(programKindName(job.program_kind)), row);
    title->setObjectName("panelTitle");
    auto* state = new QLabel(QStringLiteral("%1  attempt %2/%3").arg(qs(job.state)).arg(job.attempts).arg(job.max_attempts), row);
    state->setObjectName("sectionDescription");
    auto* timing = new QLabel(QStringLiteral("Queued %1").arg(formatTime(job.queued_at_utc)), row);
    timing->setObjectName("sectionDescription");
    auto* problem = new QLabel(jobProblemText(job), row);
    problem->setObjectName("sectionDescription");
    problem->setWordWrap(true);

    layout->addWidget(title, 0, 0, 1, 2);
    layout->addWidget(state, 1, 0);
    layout->addWidget(timing, 1, 1);
    if (job.state == "FAILED" || !job.error_text.empty() || !job.error_code.empty()) {
        layout->addWidget(problem, 2, 0, 1, 2);
    }
    return row;
}

void addQueueBucket(
    QVBoxLayout* layout,
    const QString& title,
    const std::vector<savor::db::UiJobSummary>& jobs,
    int maxRows,
    QWidget* parent)
{
    auto* bucketTitle = new QLabel(QStringLiteral("%1  %2").arg(title).arg(static_cast<int>(jobs.size())), parent);
    bucketTitle->setObjectName("panelTitle");
    layout->addWidget(bucketTitle);

    if (jobs.empty()) {
        auto* empty = new QLabel(QStringLiteral("None sampled."), parent);
        empty->setObjectName("sectionDescription");
        layout->addWidget(empty);
        return;
    }

    const int rowCount = (std::min)(maxRows, static_cast<int>(jobs.size()));
    for (int i = 0; i < rowCount; ++i) {
        layout->addWidget(createQueueRow(jobs[static_cast<std::size_t>(i)], parent));
    }
}

QFrame* createAttentionItem(
    const QString& title,
    const QString& detail,
    const QString& buttonText,
    std::function<void()> action,
    QWidget* parent)
{
    auto* item = new QFrame(parent);
    item->setObjectName("workspaceInfoPanel");
    auto* layout = new QHBoxLayout(item);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(10);

    auto* textHost = new QFrame(item);
    auto* textLayout = new QVBoxLayout(textHost);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(3);
    auto* titleLabel = new QLabel(title, textHost);
    titleLabel->setObjectName("panelTitle");
    auto* detailLabel = new QLabel(detail, textHost);
    detailLabel->setObjectName("sectionDescription");
    detailLabel->setWordWrap(true);
    textLayout->addWidget(titleLabel);
    textLayout->addWidget(detailLabel);
    layout->addWidget(textHost, 1);

    if (action) {
        auto* button = createActionButton(buttonText, item, true);
        QObject::connect(button, &QPushButton::clicked, item, [action = std::move(action)]() {
            action();
        });
        layout->addWidget(button);
    }
    return item;
}

bool coordinatorIsoReady(const CoordinatorController* controller)
{
    if (controller == nullptr) {
        return true;
    }

    const QString isoPath = controller->isoPath().trimmed();
    return !isoPath.isEmpty() && QFileInfo(isoPath).isFile();
}

bool coordinatorDolphinBaseReady(const CoordinatorController* controller)
{
    if (controller == nullptr) {
        return true;
    }

    const QString dolphinBase = controller->dolphinBaseDir().trimmed();
    if (dolphinBase.isEmpty() || !QFileInfo(dolphinBase).isDir()) {
        return false;
    }

    const QDir dolphinDir(dolphinBase);
    return QFileInfo(dolphinDir.filePath(QStringLiteral("portable.txt"))).isFile()
        && QFileInfo(dolphinDir.filePath(QStringLiteral("Sys/GC/dsp_coef.bin"))).isFile();
}

} // namespace

RunningTab::RunningTab(CoordinatorController* coordinatorController, Actions actions, QWidget* parent)
    : WorkspacePageShell(
        QStringLiteral("running"),
        QStringLiteral("Running"),
        QStringLiteral("Supervise active workflow groups, queues, workers, failures, and operational interventions."),
        parent)
    , coordinatorController_(coordinatorController)
    , actions_(std::move(actions))
{
    build();
}

void RunningTab::build()
{
    setPageVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* statusStrip = new QFrame(this);
    statusStrip->setObjectName("workspaceHeroPanel");
    auto* statusLayout = new QHBoxLayout(statusStrip);
    statusLayout->setContentsMargins(10, 10, 10, 10);
    statusLayout->setSpacing(8);
    statusLayout->addWidget(createStatusPill(QStringLiteral("Coordinator"), statusStrip, &coordinatorValueLabel_));
    statusLayout->addWidget(createStatusPill(QStringLiteral("Workers"), statusStrip, &workersValueLabel_));
    statusLayout->addWidget(createStatusPill(QStringLiteral("Queue"), statusStrip, &queueValueLabel_));
    statusLayout->addWidget(createStatusPill(QStringLiteral("Running"), statusStrip, &runningValueLabel_));
    statusLayout->addWidget(createStatusPill(QStringLiteral("Failures"), statusStrip, &failuresValueLabel_));
    statusLayout->addStretch();

    lastRefreshLabel_ = new QLabel(QStringLiteral("--"), statusStrip);
    lastRefreshLabel_->setObjectName("sectionDescription");
    statusLayout->addWidget(lastRefreshLabel_);

    startCoordinatorButton_ = createActionButton(QStringLiteral("Start coordinator"), statusStrip, true);
    QObject::connect(startCoordinatorButton_, &QPushButton::clicked, statusStrip, [this]() {
        if (coordinatorController_ != nullptr) {
            coordinatorController_->startCoordinator();
        }
    });
    statusLayout->addWidget(startCoordinatorButton_);

    isoSetupButton_ = new QPushButton(QStringLiteral("Set ISO"), statusStrip);
    isoSetupButton_->setObjectName("setupWarningButton");
    QObject::connect(isoSetupButton_, &QPushButton::clicked, statusStrip, [this]() {
        if (actions_.openIsoSettings) {
            actions_.openIsoSettings();
        } else if (actions_.openCoordinatorSettings) {
            actions_.openCoordinatorSettings();
        } else if (actions_.openWorkers) {
            actions_.openWorkers();
        }
    });
    statusLayout->addWidget(isoSetupButton_);

    dolphinSetupButton_ = new QPushButton(QStringLiteral("Set Dolphin base"), statusStrip);
    dolphinSetupButton_->setObjectName("setupWarningButton");
    QObject::connect(dolphinSetupButton_, &QPushButton::clicked, statusStrip, [this]() {
        if (actions_.openDolphinSettings) {
            actions_.openDolphinSettings();
        } else if (actions_.openCoordinatorSettings) {
            actions_.openCoordinatorSettings();
        } else if (actions_.openWorkers) {
            actions_.openWorkers();
        }
    });
    statusLayout->addWidget(dolphinSetupButton_);

    fixRuntimeSetupButton_ = new QPushButton(QStringLiteral("Fix runtime setup"), statusStrip);
    fixRuntimeSetupButton_->setObjectName("setupWarningButton");
    QObject::connect(fixRuntimeSetupButton_, &QPushButton::clicked, statusStrip, [this]() {
        if (actions_.openCoordinatorSettings) {
            actions_.openCoordinatorSettings();
        } else if (actions_.openWorkers) {
            actions_.openWorkers();
        }
    });
    statusLayout->addWidget(fixRuntimeSetupButton_);

    openFailuresButton_ = createActionButton(QStringLiteral("Open failures"), statusStrip, true);
    QObject::connect(openFailuresButton_, &QPushButton::clicked, statusStrip, [this]() {
        if (lastFailedWorkflows_ > 0 && actions_.openWorkflows) {
            actions_.openWorkflows();
        } else if (lastFailedJobs_ > 0 && actions_.openJobs) {
            actions_.openJobs();
        }
    });
    statusLayout->addWidget(openFailuresButton_);
    canvasLayout()->addWidget(statusStrip);

    auto* cockpit = new QFrame(this);
    cockpit->setObjectName("workspaceCardGrid");
    cockpit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* grid = new QGridLayout(cockpit);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(10);
    grid->setColumnStretch(0, 3);
    grid->setColumnStretch(1, 2);
    grid->setRowStretch(0, 1);
    grid->setRowStretch(1, 1);

    auto* workflowPanel = createSectionPanel(QStringLiteral("Workflow Lanes"), cockpit);
    workflowPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* workflowLayout = qobject_cast<QVBoxLayout*>(workflowPanel->layout());
    auto* workflowHeader = new QHBoxLayout();
    workflowHeader->setContentsMargins(0, 0, 0, 0);
    workflowHeader->setSpacing(8);
    workflowSummaryLabel_ = new QLabel(QStringLiteral("--"), workflowPanel);
    workflowSummaryLabel_->setObjectName("sectionDescription");
    workflowSummaryLabel_->setWordWrap(true);
    workflowHeader->addWidget(workflowSummaryLabel_, 1);
    auto* openWorkflowsButton = createActionButton(QStringLiteral("Open Workflows"), workflowPanel);
    QObject::connect(openWorkflowsButton, &QPushButton::clicked, workflowPanel, [this]() {
        if (actions_.openWorkflows) {
            actions_.openWorkflows();
        }
    });
    workflowHeader->addWidget(openWorkflowsButton);
    workflowLayout->addLayout(workflowHeader);

    workflowTable_ = new QTableWidget(workflowPanel);
    configureTable(workflowTable_);
    workflowTable_->setColumnCount(6);
    workflowTable_->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("Workflow"),
        QStringLiteral("Kind"),
        QStringLiteral("State"),
        QStringLiteral("Problems"),
        QStringLiteral("Created"),
        QStringLiteral("Completed"),
    });
    workflowTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    workflowTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    workflowTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    workflowTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    workflowTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    workflowTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    QObject::connect(workflowTable_, &QTableWidget::cellDoubleClicked, workflowTable_, [this](int, int) {
        if (actions_.openWorkflows) {
            actions_.openWorkflows();
        }
    });
    workflowLayout->addWidget(workflowTable_, 1);
    grid->addWidget(workflowPanel, 0, 0);

    auto* queuePanel = createSectionPanel(QStringLiteral("Queue Pressure"), cockpit);
    queuePanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* queueLayout = qobject_cast<QVBoxLayout*>(queuePanel->layout());
    auto* queueHeader = new QHBoxLayout();
    queueHeader->setContentsMargins(0, 0, 0, 0);
    queueHeader->setSpacing(8);
    queueSummaryLabel_ = new QLabel(QStringLiteral("--"), queuePanel);
    queueSummaryLabel_->setObjectName("sectionDescription");
    queueSummaryLabel_->setWordWrap(true);
    queueHeader->addWidget(queueSummaryLabel_, 1);
    auto* openJobsButton = createActionButton(QStringLiteral("Open Jobs"), queuePanel);
    QObject::connect(openJobsButton, &QPushButton::clicked, queuePanel, [this]() {
        if (actions_.openJobs) {
            actions_.openJobs();
        }
    });
    queueHeader->addWidget(openJobsButton);
    queueLayout->addLayout(queueHeader);
    queueLayout->addWidget(createPanelScrollArea(queuePanel, &queueBucketsLayout_), 1);
    grid->addWidget(queuePanel, 0, 1);

    auto* workerPanel = createSectionPanel(QStringLiteral("Worker Rack"), cockpit);
    workerPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* workerLayout = qobject_cast<QVBoxLayout*>(workerPanel->layout());
    auto* workerHeader = new QHBoxLayout();
    workerHeader->setContentsMargins(0, 0, 0, 0);
    workerHeader->setSpacing(8);
    workerSummaryLabel_ = new QLabel(QStringLiteral("--"), workerPanel);
    workerSummaryLabel_->setObjectName("sectionDescription");
    workerSummaryLabel_->setWordWrap(true);
    workerHeader->addWidget(workerSummaryLabel_, 1);
    auto* openWorkersButton = createActionButton(QStringLiteral("Open Workers"), workerPanel);
    QObject::connect(openWorkersButton, &QPushButton::clicked, workerPanel, [this]() {
        if (actions_.openWorkers) {
            actions_.openWorkers();
        }
    });
    workerHeader->addWidget(openWorkersButton);
    workerLayout->addLayout(workerHeader);

    workerTable_ = new QTableWidget(workerPanel);
    configureTable(workerTable_);
    workerTable_->setColumnCount(5);
    workerTable_->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("Worker"),
        QStringLiteral("State"),
        QStringLiteral("Job"),
        QStringLiteral("Kind"),
        QStringLiteral("Heartbeat / progress / error"),
    });
    workerTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    workerTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    workerTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    workerTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    workerTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    QObject::connect(workerTable_, &QTableWidget::cellDoubleClicked, workerTable_, [this](int, int) {
        if (actions_.openWorkers) {
            actions_.openWorkers();
        }
    });
    workerLayout->addWidget(workerTable_, 1);
    grid->addWidget(workerPanel, 1, 0);

    auto* attentionPanel = createSectionPanel(QStringLiteral("Attention Stack"), cockpit);
    attentionPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* attentionPanelLayout = qobject_cast<QVBoxLayout*>(attentionPanel->layout());
    auto* attentionHeader = new QHBoxLayout();
    attentionHeader->setContentsMargins(0, 0, 0, 0);
    attentionHeader->setSpacing(8);
    attentionSummaryLabel_ = new QLabel(QStringLiteral("--"), attentionPanel);
    attentionSummaryLabel_->setObjectName("sectionDescription");
    attentionSummaryLabel_->setWordWrap(true);
    attentionHeader->addWidget(attentionSummaryLabel_, 1);
    attentionPanelLayout->addLayout(attentionHeader);
    attentionPanelLayout->addWidget(createPanelScrollArea(attentionPanel, &attentionLayout_), 1);
    grid->addWidget(attentionPanel, 1, 1);

    canvasLayout()->addWidget(cockpit, 1);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(1000);
    QObject::connect(refreshTimer_, &QTimer::timeout, this, [this]() {
        refreshCockpit();
    });
    refreshTimer_->start();

    if (coordinatorController_ != nullptr) {
        QObject::connect(coordinatorController_, &CoordinatorController::stateChanged, this, [this]() {
            refreshCockpit();
        });
        QObject::connect(coordinatorController_, &CoordinatorController::snapshotChanged, this, [this]() {
            refreshCockpit();
        });
    }

    refreshCockpit();
}

void RunningTab::refreshCockpit()
{
    savorqt::db::WorkflowListRequest workflowRequest{};
    workflowRequest.limit = 50;
    const auto workflows = savorqt::db::SavorDbWorkflowService::ListWorkflowInstances(workflowRequest);

    savor::db::UiReadJobListQuery jobQuery{};
    const auto jobs = savorqt::db::SavorDbJobService::FetchJobsPage(jobQuery, std::nullopt, std::nullopt, 100);

    const std::vector<savor::db::UiWorkflowInstanceSummary> workflowItems =
        workflows.ok ? workflows.value.items : std::vector<savor::db::UiWorkflowInstanceSummary>{};
    const std::vector<savor::db::UiJobSummary> jobItems =
        jobs.ok ? jobs.value.items : std::vector<savor::db::UiJobSummary>{};

    const WorkflowBuckets workflowBuckets = bucketWorkflows(workflowItems);
    const JobBuckets jobBuckets = bucketJobs(jobItems);
    lastFailedWorkflows_ = workflowBuckets.failed;
    lastFailedJobs_ = static_cast<int>(jobBuckets.failed.size());

    const bool controllerAvailable = coordinatorController_ != nullptr;
    const QString validation = controllerAvailable ? coordinatorController_->validationMessage() : QStringLiteral("Coordinator controller is unavailable.");
    const bool hasValidation = !validation.trimmed().isEmpty();
    const bool isoReady = coordinatorIsoReady(coordinatorController_);
    const bool dolphinReady = coordinatorDolphinBaseReady(coordinatorController_);
    const bool isoMissing = controllerAvailable && coordinatorController_->isoPath().trimmed().isEmpty();
    const bool dolphinMissing = controllerAvailable && coordinatorController_->dolphinBaseDir().trimmed().isEmpty();
    const bool coordinatorRunning = controllerAvailable && coordinatorController_->isRunning();
    const bool coordinatorPaused = controllerAvailable && coordinatorController_->isPaused();

    QString coordinatorText = QStringLiteral("Stopped");
    if (hasValidation) {
        coordinatorText = QStringLiteral("Blocked");
    } else if (coordinatorRunning && coordinatorPaused) {
        coordinatorText = QStringLiteral("Paused");
    } else if (coordinatorRunning) {
        coordinatorText = QStringLiteral("Running");
    }

    coordinatorValueLabel_->setText(coordinatorText);
    workersValueLabel_->setText(controllerAvailable
        ? QStringLiteral("%1/%2").arg(coordinatorController_->activeWorkers()).arg(coordinatorController_->targetWorkers())
        : QStringLiteral("--"));
    queueValueLabel_->setText(jobs.ok ? QString::number(static_cast<int>(jobBuckets.queued.size())) : QStringLiteral("--"));
    runningValueLabel_->setText(jobs.ok ? QString::number(static_cast<int>(jobBuckets.running.size())) : QStringLiteral("--"));
    failuresValueLabel_->setText(
        workflows.ok && jobs.ok
            ? QStringLiteral("%1/%2").arg(workflowBuckets.failed).arg(static_cast<int>(jobBuckets.failed.size()))
            : QStringLiteral("--"));
    lastRefreshLabel_->setText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")));

    startCoordinatorButton_->setVisible(controllerAvailable && !coordinatorRunning && !hasValidation);
    isoSetupButton_->setText(isoMissing ? QStringLiteral("Set ISO") : QStringLiteral("Fix ISO"));
    isoSetupButton_->setVisible(!isoReady);
    dolphinSetupButton_->setText(dolphinMissing ? QStringLiteral("Set Dolphin base") : QStringLiteral("Fix Dolphin base"));
    dolphinSetupButton_->setVisible(!dolphinReady);
    fixRuntimeSetupButton_->setVisible(hasValidation);
    fixRuntimeSetupButton_->setToolTip(validation);
    openFailuresButton_->setVisible(workflowBuckets.failed > 0 || !jobBuckets.failed.empty());

    workflowSummaryLabel_->setText(workflows.ok
        ? QStringLiteral("%1 sampled, %2 active, %3 blocked, %4 failed, %5 terminal.")
            .arg(static_cast<int>(workflowItems.size()))
            .arg(workflowBuckets.active)
            .arg(workflowBuckets.blocked)
            .arg(workflowBuckets.failed)
            .arg(workflowBuckets.terminal)
        : QStringLiteral("Workflow list unavailable: %1").arg(qs(workflows.error.message)));

    {
        QSignalBlocker blocker(workflowTable_);
        workflowTable_->setRowCount(static_cast<int>(workflowItems.size()));
        for (int row = 0; row < static_cast<int>(workflowItems.size()); ++row) {
            const auto& workflow = workflowItems[static_cast<std::size_t>(row)];
            QString problems;
            if (!workflow.failure_text.empty()) {
                problems = compactText(qs(workflow.failure_text));
            } else if (!workflow.failure_code.empty()) {
                problems = qs(workflow.failure_code);
            } else if (workflow.blocked_step_count > 0 || workflow.failed_step_count > 0) {
                problems = QStringLiteral("blocked %1 / failed %2")
                    .arg(workflow.blocked_step_count)
                    .arg(workflow.failed_step_count);
            } else {
                problems = QStringLiteral("--");
            }

            workflowTable_->setItem(row, 0, createTableItem(QStringLiteral("#%1").arg(workflow.workflow_instance_id)));
            workflowTable_->setItem(row, 1, createTableItem(qs(workflow.workflow_kind)));
            workflowTable_->setItem(row, 2, createTableItem(qs(workflow.state)));
            workflowTable_->setItem(row, 3, createTableItem(problems));
            workflowTable_->setItem(row, 4, createTableItem(formatTime(workflow.created_at_utc)));
            workflowTable_->setItem(row, 5, createTableItem(formatOptionalTime(workflow.completed_at_utc)));
        }
    }

    queueSummaryLabel_->setText(jobs.ok
        ? QStringLiteral("%1 sampled: %2 queued, %3 claimed/running, %4 failed, %5 recently terminal.")
            .arg(static_cast<int>(jobItems.size()))
            .arg(static_cast<int>(jobBuckets.queued.size()))
            .arg(static_cast<int>(jobBuckets.running.size()))
            .arg(static_cast<int>(jobBuckets.failed.size()))
            .arg(static_cast<int>(jobBuckets.terminal.size()))
        : QStringLiteral("Job queue unavailable: %1").arg(qs(jobs.error.message)));

    clearLayout(queueBucketsLayout_);
    if (!jobs.ok) {
        auto* error = new QLabel(qs(jobs.error.message), queueBucketsLayout_->parentWidget());
        error->setObjectName("sectionDescription");
        error->setWordWrap(true);
        queueBucketsLayout_->addWidget(error);
    } else {
        auto* parent = queueBucketsLayout_->parentWidget();
        addQueueBucket(queueBucketsLayout_, QStringLiteral("Queued"), jobBuckets.queued, 5, parent);
        addQueueBucket(queueBucketsLayout_, QStringLiteral("Claimed / running"), jobBuckets.running, 5, parent);
        addQueueBucket(queueBucketsLayout_, QStringLiteral("Failed"), jobBuckets.failed, 5, parent);
        addQueueBucket(queueBucketsLayout_, QStringLiteral("Recently terminal"), jobBuckets.terminal, 5, parent);
    }
    queueBucketsLayout_->addStretch();

    const auto workers = controllerAvailable
        ? coordinatorController_->snapshot()
        : std::vector<WorkerSnapshot>{};
    const int workerAttentionCount = static_cast<int>(std::count_if(workers.begin(), workers.end(), workerNeedsAttention));
    workerSummaryLabel_->setText(controllerAvailable
        ? QStringLiteral("%1 active, %2 target, %3 rows, %4 attention.")
            .arg(coordinatorController_->activeWorkers())
            .arg(coordinatorController_->targetWorkers())
            .arg(static_cast<int>(workers.size()))
            .arg(workerAttentionCount)
        : QStringLiteral("Coordinator controller unavailable."));

    {
        QSignalBlocker blocker(workerTable_);
        workerTable_->setRowCount(static_cast<int>(workers.size()));
        for (int row = 0; row < static_cast<int>(workers.size()); ++row) {
            const auto& worker = workers[static_cast<std::size_t>(row)];
            workerTable_->setItem(row, 0, createTableItem(QStringLiteral("#%1").arg(worker.worker_id)));
            workerTable_->setItem(row, 1, createTableItem(workerStateLabel(worker.state)));
            workerTable_->setItem(row, 2, createTableItem(worker.job_id.has_value() ? QStringLiteral("#%1").arg(*worker.job_id) : QStringLiteral("--")));
            workerTable_->setItem(row, 3, createTableItem(qs(savorqt::db::ResolveProgramKindName(worker.program_kind))));
            workerTable_->setItem(row, 4, createTableItem(workerStatusText(worker)));
        }
    }

    clearLayout(attentionLayout_);
    int attentionCount = 0;
    auto* attentionParent = attentionLayout_->parentWidget();

    if (hasValidation) {
        ++attentionCount;
        attentionLayout_->addWidget(createAttentionItem(
            QStringLiteral("Runtime setup is blocking work"),
            validation,
            QStringLiteral("Fix runtime setup"),
            actions_.openCoordinatorSettings ? actions_.openCoordinatorSettings : actions_.openWorkers,
            attentionParent));
    }
    if (!coordinatorRunning && !hasValidation && !jobBuckets.queued.empty()) {
        ++attentionCount;
        attentionLayout_->addWidget(createAttentionItem(
            QStringLiteral("Coordinator stopped with queued jobs"),
            QStringLiteral("%1 jobs are waiting for workers.").arg(static_cast<int>(jobBuckets.queued.size())),
            QStringLiteral("Start coordinator"),
            [this]() {
                if (coordinatorController_ != nullptr) {
                    coordinatorController_->startCoordinator();
                }
            },
            attentionParent));
    }
    if (workflowBuckets.failed > 0) {
        ++attentionCount;
        attentionLayout_->addWidget(createAttentionItem(
            QStringLiteral("Failed workflows"),
            QStringLiteral("%1 recent workflow instances need triage.").arg(workflowBuckets.failed),
            QStringLiteral("Open Workflows"),
            actions_.openWorkflows,
            attentionParent));
    }
    if (!jobBuckets.failed.empty()) {
        ++attentionCount;
        attentionLayout_->addWidget(createAttentionItem(
            QStringLiteral("Failed jobs"),
            QStringLiteral("%1 recent jobs failed.").arg(static_cast<int>(jobBuckets.failed.size())),
            QStringLiteral("Open Jobs"),
            actions_.openJobs,
            attentionParent));
    }
    if (workerAttentionCount > 0) {
        ++attentionCount;
        attentionLayout_->addWidget(createAttentionItem(
            QStringLiteral("Worker errors"),
            QStringLiteral("%1 worker rows report an error, dead state, or repeated failures.").arg(workerAttentionCount),
            QStringLiteral("Open Workers"),
            actions_.openWorkers,
            attentionParent));
    }
    if (!workflows.ok) {
        ++attentionCount;
        attentionLayout_->addWidget(createAttentionItem(
            QStringLiteral("Workflow read unavailable"),
            qs(workflows.error.message),
            QStringLiteral("Open Workflows"),
            actions_.openWorkflows,
            attentionParent));
    }
    if (!jobs.ok) {
        ++attentionCount;
        attentionLayout_->addWidget(createAttentionItem(
            QStringLiteral("Job read unavailable"),
            qs(jobs.error.message),
            QStringLiteral("Open Jobs"),
            actions_.openJobs,
            attentionParent));
    }

    if (attentionCount == 0) {
        attentionLayout_->addWidget(createAttentionItem(
            QStringLiteral("No current attention items"),
            QStringLiteral("Coordinator setup, queue pressure, failures, and workers are quiet in the recent sample."),
            QString(),
            {},
            attentionParent));
    }
    attentionLayout_->addStretch();
    attentionSummaryLabel_->setText(attentionCount == 0
        ? QStringLiteral("Clear.")
        : QStringLiteral("%1 prioritized item%2.")
            .arg(attentionCount)
            .arg(attentionCount == 1 ? QString() : QStringLiteral("s")));
}
