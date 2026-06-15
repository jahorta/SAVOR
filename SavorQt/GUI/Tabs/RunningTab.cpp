#include "GUI/Tabs/RunningTab.h"

#include "DB/ProgramKindNameResolver.h"
#include "DB/SavorDbJobService.h"
#include "DB/SavorDbWorkflowService.h"
#include "GUI/Panes/CoordinatorPane/CoordinatorController.h"
#include "GUI/Refresh/AsyncRefreshPipeline.h"
#include "GUI/Refresh/RowUpdate.h"
#include "Worker/WorkerTelemetry.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSignalBlocker>
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
#include <QtWidgets/QSpinBox>
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

struct RunningRefreshRequest {
    bool controllerAvailable = false;
    QString validation;
    bool isoReady = true;
    bool dolphinReady = true;
    bool isoMissing = false;
    bool dolphinMissing = false;
    bool coordinatorRunning = false;
    bool coordinatorPaused = false;
    int activeWorkers = 0;
    int targetWorkers = 1;
    std::vector<WorkerSnapshot> workers;
};

struct RunningWorkflowRow {
    qint64 workflowInstanceId = 0;
    QString workflow;
    QString kind;
    QString state;
    QString problems;
    QString created;
    QString completed;
};

struct RunningWorkerRow {
    qint64 workerId = 0;
    QString state;
    QString job;
    QString kind;
    QString status;
};

struct QueueRowData {
    qint64 jobId = 0;
    QString title;
    QString state;
    QString timing;
    QString problem;
    bool showProblem = false;
};

struct QueueBucketData {
    QString title;
    std::vector<QueueRowData> rows;
    int totalCount = 0;
};

enum class AttentionRoute {
    None,
    CoordinatorSettings,
    Workflows,
    Jobs,
    Workers,
    StartCoordinator,
};

struct AttentionItemData {
    QString title;
    QString detail;
    QString buttonText;
    AttentionRoute route = AttentionRoute::None;
};

struct RunningRefreshData {
    bool workflowsOk = false;
    bool jobsOk = false;
    QString workflowError;
    QString jobError;
    QString coordinatorText;
    QString workersText;
    QString queueText;
    QString runningText;
    QString failuresText;
    QString lastRefreshText;
    QString workflowSummary;
    QString queueSummary;
    QString workerSummary;
    QString attentionSummary;
    bool controllerAvailable = false;
    bool hasValidation = false;
    QString validation;
    bool coordinatorRunning = false;
    bool coordinatorPaused = false;
    bool isoReady = true;
    bool dolphinReady = true;
    bool isoMissing = false;
    bool dolphinMissing = false;
    int targetWorkers = 1;
    int failedWorkflows = 0;
    int failedJobs = 0;
    std::vector<RunningWorkflowRow> workflowRows;
    std::vector<RunningWorkerRow> workerRows;
    std::vector<QueueBucketData> queueBuckets;
    std::vector<AttentionItemData> attentionItems;
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

QFrame* createQueueRow(const QueueRowData& rowData, QWidget* parent)
{
    auto* row = new QFrame(parent);
    row->setObjectName("workspaceInfoPanel");
    auto* layout = new QGridLayout(row);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setHorizontalSpacing(8);
    layout->setVerticalSpacing(2);

    auto* title = new QLabel(rowData.title, row);
    title->setObjectName("panelTitle");
    auto* state = new QLabel(rowData.state, row);
    state->setObjectName("sectionDescription");
    auto* timing = new QLabel(rowData.timing, row);
    timing->setObjectName("sectionDescription");
    auto* problem = new QLabel(rowData.problem, row);
    problem->setObjectName("sectionDescription");
    problem->setWordWrap(true);

    layout->addWidget(title, 0, 0, 1, 2);
    layout->addWidget(state, 1, 0);
    layout->addWidget(timing, 1, 1);
    if (rowData.showProblem) {
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

void addQueueBucket(
    QVBoxLayout* layout,
    const QueueBucketData& bucket,
    int maxRows,
    QWidget* parent)
{
    auto* bucketTitle = new QLabel(QStringLiteral("%1  %2").arg(bucket.title).arg(bucket.totalCount), parent);
    bucketTitle->setObjectName("panelTitle");
    layout->addWidget(bucketTitle);

    if (bucket.rows.empty()) {
        auto* empty = new QLabel(QStringLiteral("None sampled."), parent);
        empty->setObjectName("sectionDescription");
        layout->addWidget(empty);
        return;
    }

    const int rowCount = (std::min)(maxRows, static_cast<int>(bucket.rows.size()));
    for (int i = 0; i < rowCount; ++i) {
        layout->addWidget(createQueueRow(bucket.rows[static_cast<std::size_t>(i)], parent));
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

bool runningWorkflowRowsEqual(const RunningWorkflowRow& lhs, const RunningWorkflowRow& rhs)
{
    return lhs.workflowInstanceId == rhs.workflowInstanceId
        && lhs.workflow == rhs.workflow
        && lhs.kind == rhs.kind
        && lhs.state == rhs.state
        && lhs.problems == rhs.problems
        && lhs.created == rhs.created
        && lhs.completed == rhs.completed;
}

bool runningWorkerRowsEqual(const RunningWorkerRow& lhs, const RunningWorkerRow& rhs)
{
    return lhs.workerId == rhs.workerId
        && lhs.state == rhs.state
        && lhs.job == rhs.job
        && lhs.kind == rhs.kind
        && lhs.status == rhs.status;
}

void populateRunningWorkflowRow(QTableWidget* table, int row, const RunningWorkflowRow& workflow)
{
    table->setItem(row, 0, createTableItem(workflow.workflow));
    table->setItem(row, 1, createTableItem(workflow.kind));
    table->setItem(row, 2, createTableItem(workflow.state));
    table->setItem(row, 3, createTableItem(workflow.problems));
    table->setItem(row, 4, createTableItem(workflow.created));
    table->setItem(row, 5, createTableItem(workflow.completed));
}

void populateRunningWorkerRow(QTableWidget* table, int row, const RunningWorkerRow& worker)
{
    table->setItem(row, 0, createTableItem(QStringLiteral("#%1").arg(worker.workerId)));
    table->setItem(row, 1, createTableItem(worker.state));
    table->setItem(row, 2, createTableItem(worker.job));
    table->setItem(row, 3, createTableItem(worker.kind));
    table->setItem(row, 4, createTableItem(worker.status));
}

QueueRowData prepareQueueRow(const savor::db::UiJobSummary& job)
{
    return QueueRowData{
        job.job_id,
        QStringLiteral("#%1  %2").arg(job.job_id).arg(programKindName(job.program_kind)),
        QStringLiteral("%1  attempt %2/%3").arg(qs(job.state)).arg(job.attempts).arg(job.max_attempts),
        QStringLiteral("Queued %1").arg(formatTime(job.queued_at_utc)),
        jobProblemText(job),
        job.state == "FAILED" || !job.error_text.empty() || !job.error_code.empty(),
    };
}

QueueBucketData prepareQueueBucket(const QString& title, const std::vector<savor::db::UiJobSummary>& jobs)
{
    QueueBucketData bucket;
    bucket.title = title;
    bucket.totalCount = static_cast<int>(jobs.size());
    bucket.rows.reserve(jobs.size());
    for (const auto& job : jobs) {
        bucket.rows.push_back(prepareQueueRow(job));
    }
    return bucket;
}

RunningRefreshData prepareRunningRefreshData(const RunningRefreshRequest& request)
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
    const bool hasValidation = !request.validation.trimmed().isEmpty();

    RunningRefreshData data;
    data.workflowsOk = workflows.ok;
    data.jobsOk = jobs.ok;
    data.workflowError = workflows.ok ? QString() : qs(workflows.error.message);
    data.jobError = jobs.ok ? QString() : qs(jobs.error.message);
    data.controllerAvailable = request.controllerAvailable;
    data.hasValidation = hasValidation;
    data.validation = request.validation;
    data.coordinatorRunning = request.coordinatorRunning;
    data.coordinatorPaused = request.coordinatorPaused;
    data.isoReady = request.isoReady;
    data.dolphinReady = request.dolphinReady;
    data.isoMissing = request.isoMissing;
    data.dolphinMissing = request.dolphinMissing;
    data.targetWorkers = request.targetWorkers;
    data.failedWorkflows = workflowBuckets.failed;
    data.failedJobs = static_cast<int>(jobBuckets.failed.size());

    data.coordinatorText = QStringLiteral("Stopped");
    if (hasValidation) {
        data.coordinatorText = QStringLiteral("Blocked");
    } else if (request.coordinatorRunning && request.coordinatorPaused) {
        data.coordinatorText = QStringLiteral("Paused");
    } else if (request.coordinatorRunning) {
        data.coordinatorText = QStringLiteral("Running");
    }
    data.workersText = request.controllerAvailable
        ? QStringLiteral("%1/%2").arg(request.activeWorkers).arg(request.targetWorkers)
        : QStringLiteral("--");
    data.queueText = jobs.ok ? QString::number(static_cast<int>(jobBuckets.queued.size())) : QStringLiteral("--");
    data.runningText = jobs.ok ? QString::number(static_cast<int>(jobBuckets.running.size())) : QStringLiteral("--");
    data.failuresText = workflows.ok && jobs.ok
        ? QStringLiteral("%1/%2").arg(workflowBuckets.failed).arg(static_cast<int>(jobBuckets.failed.size()))
        : QStringLiteral("--");
    data.lastRefreshText = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));

    data.workflowSummary = workflows.ok
        ? QStringLiteral("%1 sampled, %2 active, %3 blocked, %4 failed, %5 terminal.")
            .arg(static_cast<int>(workflowItems.size()))
            .arg(workflowBuckets.active)
            .arg(workflowBuckets.blocked)
            .arg(workflowBuckets.failed)
            .arg(workflowBuckets.terminal)
        : QStringLiteral("Workflow list unavailable: %1").arg(data.workflowError);
    data.queueSummary = jobs.ok
        ? QStringLiteral("%1 sampled: %2 queued, %3 claimed/running, %4 failed, %5 recently terminal.")
            .arg(static_cast<int>(jobItems.size()))
            .arg(static_cast<int>(jobBuckets.queued.size()))
            .arg(static_cast<int>(jobBuckets.running.size()))
            .arg(static_cast<int>(jobBuckets.failed.size()))
            .arg(static_cast<int>(jobBuckets.terminal.size()))
        : QStringLiteral("Job queue unavailable: %1").arg(data.jobError);

    data.workflowRows.reserve(workflowItems.size());
    for (const auto& workflow : workflowItems) {
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
        data.workflowRows.push_back(RunningWorkflowRow{
            workflow.workflow_instance_id,
            QStringLiteral("#%1").arg(workflow.workflow_instance_id),
            qs(workflow.workflow_kind),
            qs(workflow.state),
            problems,
            formatTime(workflow.created_at_utc),
            formatOptionalTime(workflow.completed_at_utc),
        });
    }

    data.queueBuckets.push_back(prepareQueueBucket(QStringLiteral("Queued"), jobBuckets.queued));
    data.queueBuckets.push_back(prepareQueueBucket(QStringLiteral("Claimed / running"), jobBuckets.running));
    data.queueBuckets.push_back(prepareQueueBucket(QStringLiteral("Failed"), jobBuckets.failed));
    data.queueBuckets.push_back(prepareQueueBucket(QStringLiteral("Recently terminal"), jobBuckets.terminal));

    const int workerAttentionCount = static_cast<int>(std::count_if(request.workers.begin(), request.workers.end(), workerNeedsAttention));
    data.workerSummary = request.controllerAvailable
        ? QStringLiteral("%1 active, %2 target, %3 rows, %4 attention.")
            .arg(request.activeWorkers)
            .arg(request.targetWorkers)
            .arg(static_cast<int>(request.workers.size()))
            .arg(workerAttentionCount)
        : QStringLiteral("Coordinator controller unavailable.");
    data.workerRows.reserve(request.workers.size());
    for (const auto& worker : request.workers) {
        data.workerRows.push_back(RunningWorkerRow{
            worker.worker_id,
            workerStateLabel(worker.state),
            worker.job_id.has_value() ? QStringLiteral("#%1").arg(*worker.job_id) : QStringLiteral("--"),
            qs(savorqt::db::ResolveProgramKindName(worker.program_kind)),
            workerStatusText(worker),
        });
    }

    if (hasValidation) {
        data.attentionItems.push_back(AttentionItemData{
            QStringLiteral("Runtime setup is blocking work"),
            request.validation,
            QStringLiteral("Fix runtime setup"),
            AttentionRoute::CoordinatorSettings,
        });
    }
    if (!request.coordinatorRunning && !hasValidation && !jobBuckets.queued.empty()) {
        data.attentionItems.push_back(AttentionItemData{
            QStringLiteral("Coordinator stopped with queued jobs"),
            QStringLiteral("%1 jobs are waiting for workers.").arg(static_cast<int>(jobBuckets.queued.size())),
            QStringLiteral("Start coordinator"),
            AttentionRoute::StartCoordinator,
        });
    }
    if (workflowBuckets.failed > 0) {
        data.attentionItems.push_back(AttentionItemData{
            QStringLiteral("Failed workflows"),
            QStringLiteral("%1 recent workflow instances need triage.").arg(workflowBuckets.failed),
            QStringLiteral("Open Workflows"),
            AttentionRoute::Workflows,
        });
    }
    if (!jobBuckets.failed.empty()) {
        data.attentionItems.push_back(AttentionItemData{
            QStringLiteral("Failed jobs"),
            QStringLiteral("%1 recent jobs failed.").arg(static_cast<int>(jobBuckets.failed.size())),
            QStringLiteral("Open Jobs"),
            AttentionRoute::Jobs,
        });
    }
    if (workerAttentionCount > 0) {
        data.attentionItems.push_back(AttentionItemData{
            QStringLiteral("Worker errors"),
            QStringLiteral("%1 worker rows report an error, dead state, or repeated failures.").arg(workerAttentionCount),
            QStringLiteral("Open Workers"),
            AttentionRoute::Workers,
        });
    }
    if (!workflows.ok) {
        data.attentionItems.push_back(AttentionItemData{
            QStringLiteral("Workflow read unavailable"),
            data.workflowError,
            QStringLiteral("Open Workflows"),
            AttentionRoute::Workflows,
        });
    }
    if (!jobs.ok) {
        data.attentionItems.push_back(AttentionItemData{
            QStringLiteral("Job read unavailable"),
            data.jobError,
            QStringLiteral("Open Jobs"),
            AttentionRoute::Jobs,
        });
    }
    if (data.attentionItems.empty()) {
        data.attentionItems.push_back(AttentionItemData{
            QStringLiteral("No current attention items"),
            QStringLiteral("Coordinator setup, queue pressure, failures, and workers are quiet in the recent sample."),
            QString(),
            AttentionRoute::None,
        });
        data.attentionSummary = QStringLiteral("Clear.");
    } else {
        data.attentionSummary = QStringLiteral("%1 prioritized item%2.")
            .arg(static_cast<int>(data.attentionItems.size()))
            .arg(data.attentionItems.size() == 1 ? QString() : QStringLiteral("s"));
    }

    return data;
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

    pauseCoordinatorButton_ = createActionButton(QStringLiteral("Pause"), statusStrip);
    QObject::connect(pauseCoordinatorButton_, &QPushButton::clicked, statusStrip, [this]() {
        if (coordinatorController_ != nullptr) {
            coordinatorController_->togglePaused();
        }
    });
    statusLayout->addWidget(pauseCoordinatorButton_);

    stopCoordinatorButton_ = createActionButton(QStringLiteral("Stop"), statusStrip);
    QObject::connect(stopCoordinatorButton_, &QPushButton::clicked, statusStrip, [this]() {
        if (coordinatorController_ != nullptr) {
            coordinatorController_->stopCoordinator();
        }
    });
    statusLayout->addWidget(stopCoordinatorButton_);

    targetWorkersSpin_ = new QSpinBox(statusStrip);
    targetWorkersSpin_->setObjectName("jobsRefreshSpin");
    targetWorkersSpin_->setMinimum(1);
    targetWorkersSpin_->setMaximum(9999);
    targetWorkersSpin_->setPrefix(QStringLiteral("Target: "));
    QObject::connect(targetWorkersSpin_, qOverload<int>(&QSpinBox::valueChanged), statusStrip, [this](int value) {
        if (coordinatorController_ != nullptr) {
            coordinatorController_->setTargetWorkers(value);
        }
    });
    statusLayout->addWidget(targetWorkersSpin_);

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

    auto* refreshPipeline = new savorqt::gui::AsyncRefreshPipeline<RunningRefreshRequest, RunningRefreshData>(this);
    auto workflowRows = std::make_shared<std::vector<RunningWorkflowRow>>();
    auto workerRows = std::make_shared<std::vector<RunningWorkerRow>>();
    refreshPipeline->setRefreshIntervalMs(1000);
    refreshPipeline->setRequestBuilder([this](savorqt::gui::RefreshReason) {
        RunningRefreshRequest request;
        request.controllerAvailable = coordinatorController_ != nullptr;
        if (coordinatorController_ == nullptr) {
            request.validation = QStringLiteral("Coordinator controller is unavailable.");
            return std::optional<RunningRefreshRequest>{ request };
        }

        request.validation = coordinatorController_->validationMessage();
        request.isoReady = coordinatorIsoReady(coordinatorController_);
        request.dolphinReady = coordinatorDolphinBaseReady(coordinatorController_);
        request.isoMissing = coordinatorController_->isoPath().trimmed().isEmpty();
        request.dolphinMissing = coordinatorController_->dolphinBaseDir().trimmed().isEmpty();
        request.coordinatorRunning = coordinatorController_->isRunning();
        request.coordinatorPaused = coordinatorController_->isPaused();
        request.activeWorkers = coordinatorController_->activeWorkers();
        request.targetWorkers = coordinatorController_->targetWorkers();
        request.workers = coordinatorController_->freshSnapshot();
        return std::optional<RunningRefreshRequest>{ request };
    });
    refreshPipeline->setLoadAndPrepare([](RunningRefreshRequest request) {
        return savorqt::gui::AsyncRefreshResult<RunningRefreshData>::Ok(prepareRunningRefreshData(request));
    });
    refreshPipeline->setApply([=](const RunningRefreshData& data, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        coordinatorValueLabel_->setText(data.coordinatorText);
        workersValueLabel_->setText(data.workersText);
        queueValueLabel_->setText(data.queueText);
        runningValueLabel_->setText(data.runningText);
        failuresValueLabel_->setText(data.failuresText);
        lastRefreshLabel_->setText(data.lastRefreshText);

        lastFailedWorkflows_ = data.failedWorkflows;
        lastFailedJobs_ = data.failedJobs;

        startCoordinatorButton_->setVisible(data.controllerAvailable && !data.coordinatorRunning && !data.hasValidation);
        pauseCoordinatorButton_->setText(data.coordinatorPaused ? QStringLiteral("Resume") : QStringLiteral("Pause"));
        pauseCoordinatorButton_->setVisible(data.controllerAvailable && data.coordinatorRunning);
        stopCoordinatorButton_->setVisible(data.controllerAvailable && data.coordinatorRunning);
        {
            const QSignalBlocker blocker(targetWorkersSpin_);
            targetWorkersSpin_->setValue(data.targetWorkers);
        }
        targetWorkersSpin_->setVisible(data.controllerAvailable && data.coordinatorRunning);
        isoSetupButton_->setText(data.isoMissing ? QStringLiteral("Set ISO") : QStringLiteral("Fix ISO"));
        isoSetupButton_->setVisible(!data.isoReady);
        dolphinSetupButton_->setText(data.dolphinMissing ? QStringLiteral("Set Dolphin base") : QStringLiteral("Fix Dolphin base"));
        dolphinSetupButton_->setVisible(!data.dolphinReady);
        fixRuntimeSetupButton_->setVisible(data.hasValidation);
        fixRuntimeSetupButton_->setToolTip(data.validation);
        openFailuresButton_->setVisible(data.failedWorkflows > 0 || data.failedJobs > 0);

        workflowSummaryLabel_->setText(data.workflowSummary);
        savorqt::gui::ApplyTableRowsByKey(
            workflowTable_,
            *workflowRows,
            data.workflowRows,
            [](const RunningWorkflowRow& row) { return row.workflowInstanceId; },
            runningWorkflowRowsEqual,
            populateRunningWorkflowRow);

        queueSummaryLabel_->setText(data.queueSummary);
        clearLayout(queueBucketsLayout_);
        if (!data.jobsOk) {
            auto* error = new QLabel(data.jobError, queueBucketsLayout_->parentWidget());
            error->setObjectName("sectionDescription");
            error->setWordWrap(true);
            queueBucketsLayout_->addWidget(error);
        } else {
            auto* parent = queueBucketsLayout_->parentWidget();
            for (const auto& bucket : data.queueBuckets) {
                addQueueBucket(queueBucketsLayout_, bucket, 5, parent);
            }
        }
        queueBucketsLayout_->addStretch();

        workerSummaryLabel_->setText(data.workerSummary);
        savorqt::gui::ApplyTableRowsByKey(
            workerTable_,
            *workerRows,
            data.workerRows,
            [](const RunningWorkerRow& row) { return row.workerId; },
            runningWorkerRowsEqual,
            populateRunningWorkerRow);

        auto actionForRoute = [this](AttentionRoute route) -> std::function<void()> {
            switch (route) {
            case AttentionRoute::CoordinatorSettings:
                return actions_.openCoordinatorSettings ? actions_.openCoordinatorSettings : actions_.openWorkers;
            case AttentionRoute::Workflows:
                return actions_.openWorkflows;
            case AttentionRoute::Jobs:
                return actions_.openJobs;
            case AttentionRoute::Workers:
                return actions_.openWorkers;
            case AttentionRoute::StartCoordinator:
                return [this]() {
                    if (coordinatorController_ != nullptr) {
                        coordinatorController_->startCoordinator();
                    }
                };
            case AttentionRoute::None:
            default:
                return {};
            }
        };

        clearLayout(attentionLayout_);
        auto* attentionParent = attentionLayout_->parentWidget();
        for (const auto& item : data.attentionItems) {
            attentionLayout_->addWidget(createAttentionItem(
                item.title,
                item.detail,
                item.buttonText,
                actionForRoute(item.route),
                attentionParent));
        }
        attentionLayout_->addStretch();
        attentionSummaryLabel_->setText(data.attentionSummary);
    });
    requestCockpitRefresh_ = [refreshPipeline]() {
        refreshPipeline->requestRefresh(savorqt::gui::RefreshReason::Manual);
    };

    if (coordinatorController_ != nullptr) {
        QObject::connect(coordinatorController_, &CoordinatorController::stateChanged, this, [this]() {
            refreshCockpit();
        });
        QObject::connect(coordinatorController_, &CoordinatorController::snapshotChanged, this, [this]() {
            refreshCockpit();
        });
    }

    refreshPipeline->setActive(true);
    refreshPipeline->requestRefresh(savorqt::gui::RefreshReason::Initial);
}

void RunningTab::refreshCockpit()
{
    if (requestCockpitRefresh_) {
        requestCockpitRefresh_();
    }
}
