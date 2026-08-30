#include "GUI/Tabs/RunningTab.h"

#include "DB/ProgramKindNameResolver.h"
#include "DB/SavorDbJobService.h"
#include "DB/SavorDbWorkflowService.h"
#include "GUI/Panes/CoordinatorPane/CoordinatorController.h"
#include "GUI/Refresh/AsyncRefreshPipeline.h"
#include "GUI/Refresh/RowUpdate.h"
#include "GUI/Widgets/SegmentedProgressDelegate.h"
#include "Worker/WorkerTelemetry.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimeZone>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
constexpr int kWorkflowIdRole = Qt::UserRole + 1;
constexpr int kRetryableJobsRole = Qt::UserRole + 2;
constexpr int kWorkflowExpansionIdRole = Qt::UserRole + 3;

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
    CoordinatorLifecycleState coordinatorState =
        CoordinatorLifecycleState::Stopped;
    bool coordinatorPaused = false;
    int activeWorkers = 0;
    int targetWorkers = 1;
    bool visualWorkerPoolEnabled = false;
    std::vector<WorkerSnapshot> workers;
};

struct RunningWorkflowRow {
    qint64 rowKey = 0;
    qint64 workflowInstanceId = 0;
    qint64 expansionId = 0;
    bool family = false;
    QString workflow;
    QString kind;
    QString state;
    qint64 retryable = 0;
    QString progress;
    QString current;
    qint64 done = 0;
    qint64 remaining = 0;
    qint64 failed = 0;
    qint64 canceled = 0;
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
    QString workflowReadyText;
    QString workflowQueuedText;
    QString workflowWaitingText;
    QString workflowFinalizedText;
    QString queueSummary;
    QString workerSummary;
    QString attentionSummary;
    bool controllerAvailable = false;
    bool hasValidation = false;
    QString validation;
    CoordinatorLifecycleState coordinatorState =
        CoordinatorLifecycleState::Stopped;
    bool coordinatorPaused = false;
    bool isoReady = true;
    bool dolphinReady = true;
    bool isoMissing = false;
    bool dolphinMissing = false;
    int targetWorkers = 1;
    bool visualWorkerPoolEnabled = false;
    int failedWorkflows = 0;
    int failedJobs = 0;
    int workerAttentionCount = 0;
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

bool isJobRunning(const savor::db::UiJobSummary& job)
{
    return job.state == "RUNNING" || job.state == "CLAIMED";
}

bool isJobTerminal(const savor::db::UiJobSummary& job)
{
    return job.state == "SUCCEEDED"
        || job.state == "INTERRUPTED"
        || job.state == "CANCELED"
        || job.state == "SUPERSEDED"
        || job.state == "SUCCEEDED_WINNER"
        || job.state == "SUCCEEDED_DUPLICATE";
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
    if (!job.last_progress_text.empty()) {
        return compactText(qs(job.last_progress_text), 120);
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
    return lhs.rowKey == rhs.rowKey
        && lhs.workflowInstanceId == rhs.workflowInstanceId
        && lhs.expansionId == rhs.expansionId
        && lhs.family == rhs.family
        && lhs.workflow == rhs.workflow
        && lhs.kind == rhs.kind
        && lhs.state == rhs.state
        && lhs.retryable == rhs.retryable
        && lhs.progress == rhs.progress
        && lhs.current == rhs.current
        && lhs.done == rhs.done
        && lhs.remaining == rhs.remaining
        && lhs.failed == rhs.failed
        && lhs.canceled == rhs.canceled;
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
    table->item(row, 0)->setData(kWorkflowIdRole, workflow.workflowInstanceId);
    table->item(row, 0)->setData(kWorkflowExpansionIdRole, workflow.expansionId);
    table->setItem(row, 1, createTableItem(workflow.kind));
    auto* stateItem = createTableItem(workflow.state);
    auto* retryableItem = createTableItem(QString::number(workflow.retryable));
    retryableItem->setData(kRetryableJobsRole, workflow.retryable);
    if (workflow.state == QStringLiteral("FAILED") || workflow.state == QStringLiteral("INTERRUPTED")) {
        QFont attentionFont = stateItem->font();
        attentionFont.setBold(true);
        stateItem->setFont(attentionFont);
        retryableItem->setFont(attentionFont);
        const QString tooltip = QStringLiteral("This workflow needs attention and has %1 retryable job(s).")
            .arg(workflow.retryable);
        stateItem->setToolTip(tooltip);
        retryableItem->setToolTip(tooltip);
    }
    table->setItem(row, 2, stateItem);
    table->setItem(row, 3, retryableItem);
    auto* progressItem = createTableItem(workflow.progress);
    progressItem->setData(savorqt::gui::SegmentedProgressRoles::Text, workflow.progress);
    progressItem->setData(savorqt::gui::SegmentedProgressRoles::Done, workflow.done);
    progressItem->setData(savorqt::gui::SegmentedProgressRoles::Remaining, workflow.remaining);
    progressItem->setData(savorqt::gui::SegmentedProgressRoles::Failed, workflow.failed);
    progressItem->setData(savorqt::gui::SegmentedProgressRoles::Canceled, workflow.canceled);
    table->setItem(row, 4, progressItem);
    table->setItem(row, 5, createTableItem(workflow.current));
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
        job.state == "FAILED" || !job.error_text.empty()
            || !job.error_code.empty() || !job.last_progress_text.empty(),
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

std::int64_t terminalJobCount(const savor::db::UiJobStateCounts& counts)
{
    return counts.succeeded + counts.canceled + counts.superseded;
}

QString formatCount(std::int64_t value)
{
    return QString::number(static_cast<qlonglong>(value));
}

QString workflowCurrentText(
    const savor::db::UiWorkflowInstanceSummary& workflow,
    const std::optional<savor::db::UiWorkflowDetail>& detail)
{
    if (!workflow.failure_text.empty()) {
        return compactText(qs(workflow.failure_text), 96);
    }
    if (!workflow.failure_code.empty()) {
        return qs(workflow.failure_code);
    }
    if (detail.has_value()) {
        for (const auto& alert : detail->alerts) {
            if (alert.is_active) {
                return compactText(qs(alert.message.empty() ? alert.alert_code : alert.message), 96);
            }
        }
        for (const auto& step : detail->steps) {
            if (step.state == "RUNNING" || step.state == "CLAIMED" || step.state == "READY") {
                return compactText(QStringLiteral("%1  %2").arg(qs(step.step_key), qs(step.state)), 96);
            }
        }
        for (const auto& activation : detail->unit_activations) {
            if (activation.state == "RUNNING" || activation.state == "READY" || activation.state == "BLOCKED") {
                const QString name = activation.display_name.empty()
                    ? qs(activation.unit_kind)
                    : qs(activation.display_name);
                return compactText(QStringLiteral("%1  %2").arg(name, qs(activation.state)), 96);
            }
        }
    }
    if (workflow.blocked_step_count > 0 || workflow.failed_step_count > 0) {
        return QStringLiteral("blocked %1 / failed %2")
            .arg(workflow.blocked_step_count)
            .arg(workflow.failed_step_count);
    }
    return QStringLiteral("--");
}

RunningWorkflowRow prepareWorkflowRow(
    const savor::db::UiWorkflowInstanceSummary& workflow,
    const savor::db::UiWorkflowDetail* workflowDetail)
{
    const std::optional<savor::db::UiWorkflowDetail> detail =
        workflowDetail != nullptr
            ? std::optional<savor::db::UiWorkflowDetail>{ *workflowDetail }
            : std::nullopt;

    qint64 total = 0;
    qint64 done = 0;
    qint64 failed = 0;
    if (detail.has_value()) {
        for (const auto& step : detail->steps) {
            total += static_cast<qint64>(step.job_count);
            qint64 stepFailed = static_cast<qint64>(step.job_failed_count);
            qint64 stepSettled = static_cast<qint64>(step.job_settled_count);
            if (stepSettled == 0
                && stepFailed == 0
                && step.job_count > 0
                && step.state == "COMPLETED") {
                stepSettled = static_cast<qint64>(step.job_count);
            }
            failed += stepFailed;
            done += (std::max<qint64>)(0, stepSettled - stepFailed);
        }
    }
    const qint64 canceled = 0;
    const qint64 remaining = (std::max<qint64>)(0, total - done - failed - canceled);
    QString progress = QStringLiteral("ok:%1 rem:%2 fail:%3 can:%4 done:%5/%6")
        .arg(done)
        .arg(remaining)
        .arg(failed)
        .arg(canceled)
        .arg(done + failed + canceled)
        .arg(total);
    if (total <= 0) {
        progress = QStringLiteral("No jobs yet");
    }

    return RunningWorkflowRow{
        workflow.workflow_instance_id,
        workflow.workflow_instance_id,
        0,
        false,
        QStringLiteral("#%1").arg(workflow.workflow_instance_id),
        qs(workflow.workflow_kind),
        qs(workflow.display_state.empty() ? workflow.state : workflow.display_state),
        workflow.retryable_job_count,
        progress,
        workflowCurrentText(workflow, detail),
        done,
        remaining,
        failed,
        canceled,
    };
}

RunningRefreshData prepareRunningRefreshData(const RunningRefreshRequest& request)
{
    savorqt::db::WorkflowListRequest workflowRequest{};
    workflowRequest.exclude_final = true;
    workflowRequest.limit = 100;
    const auto workflows = savorqt::db::SavorDbWorkflowService::ListWorkflowInstances(workflowRequest);
    const auto workflowCountsResult = savorqt::db::SavorDbWorkflowService::CountWorkflowDisplayStates();
    const auto expansions = savorqt::db::SavorDbWorkflowService::ListWorkflowExpansions(false);

    std::set<std::int64_t> requestedWorkflowDetailIds;
    if (workflows.ok) {
        for (const auto& workflow : workflows.value.items) {
            requestedWorkflowDetailIds.insert(workflow.workflow_instance_id);
        }
    }
    if (expansions.ok) {
        for (const auto& expansion : expansions.value) {
            for (const auto& member : expansion.members) {
                requestedWorkflowDetailIds.insert(member.workflow_instance_id);
            }
        }
    }
    const std::vector<std::int64_t> requestedWorkflowDetails(
        requestedWorkflowDetailIds.begin(), requestedWorkflowDetailIds.end());
    const auto workflowDetails =
        savorqt::db::SavorDbWorkflowService::GetWorkflowDetails(requestedWorkflowDetails);

    savor::db::UiReadJobListQuery jobQuery{};
    const auto jobs = savorqt::db::SavorDbJobService::FetchJobsPage(jobQuery, std::nullopt, std::nullopt, 100);
    const auto jobCounts = savorqt::db::SavorDbJobService::CountJobsByState(jobQuery);

    std::vector<savor::db::UiWorkflowInstanceSummary> workflowItems =
        workflows.ok ? workflows.value.items : std::vector<savor::db::UiWorkflowInstanceSummary>{};
    const std::vector<savor::db::UiJobSummary> jobItems =
        jobs.ok ? jobs.value.items : std::vector<savor::db::UiJobSummary>{};
    const savor::db::UiJobStateCounts counts = jobCounts.ok ? jobCounts.value : savor::db::UiJobStateCounts{};

    std::stable_sort(workflowItems.begin(), workflowItems.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.created_at_utc > rhs.created_at_utc;
    });
    const savor::db::UiWorkflowDisplayStateCounts workflowCounts =
        workflowCountsResult.ok ? workflowCountsResult.value : savor::db::UiWorkflowDisplayStateCounts{};
    const JobBuckets jobBuckets = bucketJobs(jobItems);
    const bool hasValidation = !request.validation.trimmed().isEmpty();

    RunningRefreshData data;
    data.workflowsOk = workflows.ok && workflowCountsResult.ok && expansions.ok && workflowDetails.ok;
    data.jobsOk = jobs.ok && jobCounts.ok;
    data.workflowError = !workflows.ok
        ? qs(workflows.error.message)
        : (!workflowCountsResult.ok
            ? qs(workflowCountsResult.error.message)
            : (!expansions.ok
                ? qs(expansions.error.message)
                : (workflowDetails.ok ? QString() : qs(workflowDetails.error.message))));
    data.jobError = !jobs.ok ? qs(jobs.error.message) : (jobCounts.ok ? QString() : qs(jobCounts.error.message));
    data.controllerAvailable = request.controllerAvailable;
    data.hasValidation = hasValidation;
    data.validation = request.validation;
    data.coordinatorState = request.coordinatorState;
    data.coordinatorPaused = request.coordinatorPaused;
    data.isoReady = request.isoReady;
    data.dolphinReady = request.dolphinReady;
    data.isoMissing = request.isoMissing;
    data.dolphinMissing = request.dolphinMissing;
    data.targetWorkers = request.targetWorkers;
    data.visualWorkerPoolEnabled = request.visualWorkerPoolEnabled;
    data.failedWorkflows = static_cast<int>((std::min<std::int64_t>)(workflowCounts.failed, std::numeric_limits<int>::max()));
    data.failedJobs = static_cast<int>(counts.failed);

    data.coordinatorText = QStringLiteral("Stopped");
    if (request.coordinatorState == CoordinatorLifecycleState::Starting) {
        data.coordinatorText = QStringLiteral("Starting");
    } else if (request.coordinatorState == CoordinatorLifecycleState::Stopping) {
        data.coordinatorText = QStringLiteral("Stopping");
    } else if (hasValidation) {
        data.coordinatorText = QStringLiteral("Blocked");
    } else if (request.coordinatorState == CoordinatorLifecycleState::Running
        && request.coordinatorPaused) {
        data.coordinatorText = QStringLiteral("Paused");
    } else if (request.coordinatorState == CoordinatorLifecycleState::Running) {
        data.coordinatorText = QStringLiteral("Running");
    }
    data.workersText = request.controllerAvailable
        ? QStringLiteral("%1/%2").arg(request.activeWorkers).arg(request.targetWorkers)
        : QStringLiteral("--");
    data.queueText = data.jobsOk ? formatCount(counts.queued) : QStringLiteral("--");
    data.runningText = data.jobsOk ? formatCount(counts.claimed + counts.running) : QStringLiteral("--");
    data.failuresText = data.workflowsOk && data.jobsOk
        ? QStringLiteral("%1/%2").arg(formatCount(workflowCounts.failed)).arg(formatCount(counts.failed))
        : QStringLiteral("--");
    data.lastRefreshText = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));

    data.workflowReadyText = data.workflowsOk ? formatCount(workflowCounts.running) : QStringLiteral("--");
    data.workflowQueuedText = data.workflowsOk ? formatCount(workflowCounts.queued) : QStringLiteral("--");
    data.workflowWaitingText = data.workflowsOk ? formatCount(workflowCounts.waiting) : QStringLiteral("--");
    data.workflowFinalizedText = data.workflowsOk ? formatCount(workflowCounts.finalized) : QStringLiteral("--");
    data.workflowSummary = data.workflowsOk
        ? QStringLiteral("%1 open workflow%2 shown in the main table.")
            .arg(static_cast<int>(workflowItems.size()))
            .arg(workflowItems.size() == 1 ? QString() : QStringLiteral("s"))
        : QStringLiteral("Workflow list unavailable: %1").arg(data.workflowError);
    data.queueSummary = data.jobsOk
        ? QStringLiteral("%1 total: %2 queued, %3 claimed/running, %4 failed, %5 terminal. Showing recent samples.")
            .arg(formatCount(counts.total))
            .arg(formatCount(counts.queued))
            .arg(formatCount(counts.claimed + counts.running))
            .arg(formatCount(counts.failed))
            .arg(formatCount(terminalJobCount(counts)))
        : QStringLiteral("Job queue unavailable: %1").arg(data.jobError);

    std::set<std::int64_t> familyWorkflowIds;
    if (expansions.ok) for (const auto& expansion : expansions.value)
        for (const auto& member : expansion.members)
            familyWorkflowIds.insert(member.workflow_instance_id);
    data.workflowRows.reserve(workflowItems.size() +
        (expansions.ok ? expansions.value.size() * 4u : 0u));
    const auto findWorkflowDetail = [&](std::int64_t workflowInstanceId) -> const savor::db::UiWorkflowDetail* {
        if (!workflowDetails.ok) return nullptr;
        const auto found = std::find_if(workflowDetails.value.begin(), workflowDetails.value.end(), [&](const auto& detail) {
            return detail.instance.workflow_instance_id == workflowInstanceId;
        });
        return found != workflowDetails.value.end() ? &*found : nullptr;
    };
    if (expansions.ok) for (const auto& expansion : expansions.value) {
        qint64 completed = 0;
        qint64 retryable = 0;
        for (const auto& member : expansion.members) {
            if (member.state == "COMPLETED") ++completed;
            if (const auto* detail = findWorkflowDetail(member.workflow_instance_id); detail != nullptr) {
                retryable += detail->instance.retryable_job_count;
            }
        }
        const QString kind = expansion.kind == savor::db::execution::workflow::WorkflowExpansionKind::TasMovieFirstBattleExploration
            ? QStringLiteral("First Battle Exploration") : QStringLiteral("Delay Exploration");
        data.workflowRows.push_back({
            -expansion.workflow_expansion_id, 0, expansion.workflow_expansion_id, true,
            QStringLiteral("▾ Family #%1").arg(expansion.workflow_expansion_id), kind,
            qs(expansion.state), retryable,
            QStringLiteral("%1/%2 child workflows complete").arg(completed).arg(expansion.members.size()),
            expansion.failure_text ? qs(*expansion.failure_text) : QStringLiteral("delays 0..%1").arg(expansion.max_neutral_epochs),
            completed, static_cast<qint64>(expansion.members.size())-completed,
            expansion.state=="ATTENTION" ? 1 : 0, 0});
        for (const auto& member : expansion.members) {
            const QString memberKind = QStringLiteral("%1 · delay %2%3")
                .arg(qs(member.role)).arg(member.neutral_epoch_count)
                .arg(member.rtc_value ? QStringLiteral(" · RTC %1").arg(*member.rtc_value) : QString());
            if (const auto* detail = findWorkflowDetail(member.workflow_instance_id); detail != nullptr) {
                RunningWorkflowRow row = prepareWorkflowRow(detail->instance, detail);
                row.expansionId = expansion.workflow_expansion_id;
                row.workflow = QStringLiteral("    ↳ #%1").arg(member.workflow_instance_id);
                row.kind = memberKind;
                data.workflowRows.push_back(std::move(row));
            } else {
                data.workflowRows.push_back({
                    member.workflow_instance_id, member.workflow_instance_id,
                    expansion.workflow_expansion_id, false,
                    QStringLiteral("    ↳ #%1").arg(member.workflow_instance_id),
                    memberKind, qs(member.state), 0,
                    QStringLiteral("Workflow details unavailable"), QStringLiteral("--"),
                    0, 0, 0, 0});
            }
        }
    }
    for (const auto& workflow : workflowItems) {
        if (familyWorkflowIds.contains(workflow.workflow_instance_id)) continue;
        data.workflowRows.push_back(prepareWorkflowRow(
            workflow, findWorkflowDetail(workflow.workflow_instance_id)));
    }

    data.queueBuckets.push_back(prepareQueueBucket(QStringLiteral("Queued"), jobBuckets.queued));
    data.queueBuckets.back().totalCount = static_cast<int>((std::min<std::int64_t>)(counts.queued, std::numeric_limits<int>::max()));
    data.queueBuckets.push_back(prepareQueueBucket(QStringLiteral("Claimed / running"), jobBuckets.running));
    data.queueBuckets.back().totalCount = static_cast<int>((std::min<std::int64_t>)(counts.claimed + counts.running, std::numeric_limits<int>::max()));
    data.queueBuckets.push_back(prepareQueueBucket(QStringLiteral("Failed"), jobBuckets.failed));
    data.queueBuckets.back().totalCount = static_cast<int>((std::min<std::int64_t>)(counts.failed, std::numeric_limits<int>::max()));
    data.queueBuckets.push_back(prepareQueueBucket(QStringLiteral("Recently settled"), jobBuckets.terminal));
    data.queueBuckets.back().totalCount = static_cast<int>((std::min<std::int64_t>)(terminalJobCount(counts), std::numeric_limits<int>::max()));

    const int workerAttentionCount = static_cast<int>(std::count_if(request.workers.begin(), request.workers.end(), workerNeedsAttention));
    data.workerAttentionCount = workerAttentionCount;
    data.workerSummary = request.controllerAvailable
        ? (workerAttentionCount > 0
            ? QStringLiteral("%1/%2 workers, %3 need attention.")
                .arg(request.activeWorkers)
                .arg(request.targetWorkers)
                .arg(workerAttentionCount)
            : QStringLiteral("%1/%2 workers, no worker attention.")
                .arg(request.activeWorkers)
                .arg(request.targetWorkers))
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
    if (request.coordinatorState == CoordinatorLifecycleState::Stopped
        && !hasValidation && counts.queued > 0) {
        data.attentionItems.push_back(AttentionItemData{
            QStringLiteral("Coordinator stopped with queued jobs"),
            QStringLiteral("%1 jobs are waiting for workers.").arg(formatCount(counts.queued)),
            QStringLiteral("Start coordinator"),
            AttentionRoute::StartCoordinator,
        });
    }
    if (workflowCounts.failed > 0) {
        data.attentionItems.push_back(AttentionItemData{
            QStringLiteral("Failed workflows"),
            QStringLiteral("%1 workflow instances need triage.").arg(formatCount(workflowCounts.failed)),
            QStringLiteral("Open Workflows"),
            AttentionRoute::Workflows,
        });
    }
    if (counts.failed > 0) {
        data.attentionItems.push_back(AttentionItemData{
            QStringLiteral("Failed jobs"),
            QStringLiteral("%1 jobs failed.").arg(formatCount(counts.failed)),
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
    if (!data.workflowsOk) {
        data.attentionItems.push_back(AttentionItemData{
            QStringLiteral("Workflow read unavailable"),
            data.workflowError,
            QStringLiteral("Open Workflows"),
            AttentionRoute::Workflows,
        });
    }
    if (!data.jobsOk) {
        data.attentionItems.push_back(AttentionItemData{
            QStringLiteral("Job read unavailable"),
            data.jobError,
            QStringLiteral("Open Jobs"),
            AttentionRoute::Jobs,
        });
    }
    data.attentionSummary = data.attentionItems.empty()
        ? QStringLiteral("No attention items.")
        : QStringLiteral("%1 prioritized item%2.")
            .arg(static_cast<int>(data.attentionItems.size()))
            .arg(data.attentionItems.size() == 1 ? QString() : QStringLiteral("s"));

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

void RunningTab::requestRefresh()
{
    refreshCockpit();
}

bool RunningTab::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == workflowTable_ && event != nullptr && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            openSelectedWorkflow();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Menu
            || (keyEvent->key() == Qt::Key_F10 && keyEvent->modifiers().testFlag(Qt::ShiftModifier))) {
            const QModelIndex index = workflowTable_->currentIndex();
            if (index.isValid()) {
                showWorkflowContextMenu(workflowTable_->visualRect(index).center());
            }
            return true;
        }
    }
    return WorkspacePageShell::eventFilter(watched, event);
}

void RunningTab::openSelectedWorkflow()
{
    if (workflowTable_ == nullptr || workflowTable_->currentRow() < 0 || !actions_.openWorkflow) {
        return;
    }
    const auto* item = workflowTable_->item(workflowTable_->currentRow(), 0);
    const qint64 workflowId = item != nullptr ? item->data(kWorkflowIdRole).toLongLong() : 0;
    if (workflowId > 0) {
        actions_.openWorkflow(workflowId);
    }
}

void RunningTab::showWorkflowContextMenu(const QPoint& position)
{
    if (workflowTable_ == nullptr) {
        return;
    }
    const QModelIndex index = workflowTable_->indexAt(position);
    if (!index.isValid()) {
        return;
    }
    workflowTable_->selectRow(index.row());
    const auto* idItem = workflowTable_->item(index.row(), 0);
    const auto* kindItem = workflowTable_->item(index.row(), 1);
    const auto* retryableItem = workflowTable_->item(index.row(), 3);
    const qint64 workflowId = idItem != nullptr ? idItem->data(kWorkflowIdRole).toLongLong() : 0;
    const qint64 expansionId = idItem != nullptr ? idItem->data(kWorkflowExpansionIdRole).toLongLong() : 0;
    const qint64 retryable = retryableItem != nullptr ? retryableItem->data(kRetryableJobsRole).toLongLong() : 0;
    if (workflowId <= 0 && expansionId <= 0) {
        return;
    }

    QMenu menu(workflowTable_);
    if (workflowId > 0) {
        auto* openAction = menu.addAction(QStringLiteral("Open workflow"));
        QObject::connect(openAction, &QAction::triggered, workflowTable_, [this]() { openSelectedWorkflow(); });
    }
    if (expansionId > 0 && kindItem != nullptr &&
        kindItem->text().contains(QStringLiteral("first battle"), Qt::CaseInsensitive) &&
        actions_.openFirstBattleCoverage) {
        auto* coverageAction = menu.addAction(QStringLiteral("Open First Battle Coverage"));
        QObject::connect(coverageAction, &QAction::triggered, workflowTable_,
            [this, expansionId]() { actions_.openFirstBattleCoverage(expansionId); });
    }
    if (retryable > 0 && actions_.retryWorkflowJobs) {
        auto* retryAction = menu.addAction(
            QStringLiteral("Retry failed or interrupted jobs (%1)").arg(retryable));
        QObject::connect(retryAction, &QAction::triggered, workflowTable_, [this, workflowId]() {
            actions_.retryWorkflowJobs(workflowId);
        });
    }
    menu.exec(workflowTable_->viewport()->mapToGlobal(position));
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
        if (actions_.startCoordinator) {
            actions_.startCoordinator();
        }
    });
    statusLayout->addWidget(startCoordinatorButton_);

    pauseCoordinatorButton_ = createActionButton(QStringLiteral("Pause"), statusStrip);
    QObject::connect(pauseCoordinatorButton_, &QPushButton::clicked, statusStrip, [this]() {
        if (actions_.toggleCoordinatorPaused) {
            actions_.toggleCoordinatorPaused();
        }
    });
    statusLayout->addWidget(pauseCoordinatorButton_);

    stopCoordinatorButton_ = createActionButton(QStringLiteral("Stop"), statusStrip);
    QObject::connect(stopCoordinatorButton_, &QPushButton::clicked, statusStrip, [this]() {
        if (actions_.stopCoordinator) {
            actions_.stopCoordinator();
        }
    });
    statusLayout->addWidget(stopCoordinatorButton_);

    targetWorkersSpin_ = new QSpinBox(statusStrip);
    targetWorkersSpin_->setObjectName("jobsRefreshSpin");
    targetWorkersSpin_->setMinimum(1);
    targetWorkersSpin_->setMaximum(static_cast<int>(
        savor::runner::parallel::savordb::kMaximumWorkerCount));
    targetWorkersSpin_->setPrefix(QStringLiteral("Target: "));
    QObject::connect(targetWorkersSpin_, qOverload<int>(&QSpinBox::valueChanged), statusStrip, [this](int value) {
        if (actions_.setTargetWorkers) {
            actions_.setTargetWorkers(value);
        }
    });
    statusLayout->addWidget(targetWorkersSpin_);

    visualWorkersCheck_ = new QCheckBox(QStringLiteral("Visual workers"), statusStrip);
    visualWorkersCheck_->setToolTip(QStringLiteral(
        "Start workers with embedded Dolphin render surfaces. "
        "Requires stopping the coordinator to change."));
    QObject::connect(
        visualWorkersCheck_,
        &QCheckBox::toggled,
        statusStrip,
        [this](bool enabled) {
            if (actions_.setVisualWorkerPoolEnabled) {
                actions_.setVisualWorkerPoolEnabled(enabled);
            }
        });
    statusLayout->addWidget(visualWorkersCheck_);

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

    detailsDrawerButton_ = createActionButton(QStringLiteral("Details"), statusStrip);
    statusLayout->addWidget(detailsDrawerButton_);
    canvasLayout()->addWidget(statusStrip);

    auto* cockpit = new QFrame(this);
    cockpit->setObjectName("workspaceCardGrid");
    cockpit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* cockpitLayout = new QHBoxLayout(cockpit);
    cockpitLayout->setContentsMargins(0, 0, 0, 0);
    cockpitLayout->setSpacing(10);

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

    auto* workflowStateStrip = new QHBoxLayout();
    workflowStateStrip->setContentsMargins(0, 0, 0, 0);
    workflowStateStrip->setSpacing(8);
    workflowStateStrip->addWidget(createStatusPill(QStringLiteral("Ready"), workflowPanel, &workflowReadyValueLabel_));
    workflowStateStrip->addWidget(createStatusPill(QStringLiteral("Queued"), workflowPanel, &workflowQueuedValueLabel_));
    workflowStateStrip->addWidget(createStatusPill(QStringLiteral("Waiting"), workflowPanel, &workflowWaitingValueLabel_));
    workflowStateStrip->addWidget(createStatusPill(QStringLiteral("Final"), workflowPanel, &workflowFinalValueLabel_));
    workflowStateStrip->addStretch();
    workflowLayout->addLayout(workflowStateStrip);

    workflowTable_ = new QTableWidget(workflowPanel);
    configureTable(workflowTable_);
    workflowTable_->verticalHeader()->setDefaultSectionSize(34);
    workflowTable_->setColumnCount(6);
    workflowTable_->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("Workflow"),
        QStringLiteral("Kind"),
        QStringLiteral("State"),
        QStringLiteral("Retryable"),
        QStringLiteral("Progress"),
        QStringLiteral("Current / Problems"),
    });
    workflowTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    workflowTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    workflowTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    workflowTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    workflowTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    workflowTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    workflowTable_->setItemDelegateForColumn(4, new savorqt::gui::SegmentedProgressDelegate(workflowTable_));
    workflowTable_->setContextMenuPolicy(Qt::CustomContextMenu);
    workflowTable_->installEventFilter(this);
    QObject::connect(workflowTable_, &QTableWidget::cellDoubleClicked, workflowTable_, [this](int row, int) {
        const auto* item = workflowTable_->item(row, 0);
        const qint64 workflowId = item ? item->data(kWorkflowIdRole).toLongLong() : 0;
        const qint64 expansionId = item ? item->data(kWorkflowExpansionIdRole).toLongLong() : 0;
        if (workflowId <= 0 && expansionId > 0) {
            if (collapsedExpansionIds_.contains(expansionId)) collapsedExpansionIds_.erase(expansionId);
            else collapsedExpansionIds_.insert(expansionId);
            refreshCockpit();
            return;
        }
        openSelectedWorkflow();
    });
    QObject::connect(workflowTable_, &QWidget::customContextMenuRequested, workflowTable_, [this](const QPoint& position) {
        showWorkflowContextMenu(position);
    });
    workflowLayout->addWidget(workflowTable_, 1);
    cockpitLayout->addWidget(workflowPanel, 1);

    detailTabs_ = new QTabWidget(this);
    detailTabs_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    detailTabs_->hide();

    auto* queuePanel = new QFrame(detailTabs_);
    auto* queueLayout = new QVBoxLayout(queuePanel);
    queueLayout->setContentsMargins(6, 8, 6, 6);
    queueLayout->setSpacing(8);
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
    detailTabs_->addTab(queuePanel, QStringLiteral("Queue"));

    auto* workerPanel = new QFrame(detailTabs_);
    auto* workerLayout = new QVBoxLayout(workerPanel);
    workerLayout->setContentsMargins(6, 8, 6, 6);
    workerLayout->setSpacing(8);
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
    detailTabs_->addTab(workerPanel, QStringLiteral("Workers"));

    auto* attentionPanel = new QFrame(detailTabs_);
    auto* attentionPanelLayout = new QVBoxLayout(attentionPanel);
    attentionPanelLayout->setContentsMargins(6, 8, 6, 6);
    attentionPanelLayout->setSpacing(8);
    auto* attentionHeader = new QHBoxLayout();
    attentionHeader->setContentsMargins(0, 0, 0, 0);
    attentionHeader->setSpacing(8);
    attentionSummaryLabel_ = new QLabel(QStringLiteral("--"), attentionPanel);
    attentionSummaryLabel_->setObjectName("sectionDescription");
    attentionSummaryLabel_->setWordWrap(true);
    attentionHeader->addWidget(attentionSummaryLabel_, 1);
    attentionPanelLayout->addLayout(attentionHeader);
    attentionPanelLayout->addWidget(createPanelScrollArea(attentionPanel, &attentionLayout_), 1);
    detailTabs_->addTab(attentionPanel, QStringLiteral("Attention"));

    QObject::connect(detailsDrawerButton_, &QPushButton::clicked, this, [this]() {
        if (isContextDrawerOpen()) {
            closeDrawer(true);
            refreshDetailsButton();
        } else {
            showOperationalDetails();
        }
    });

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
        request.coordinatorState = coordinatorController_->lifecycleState();
        request.coordinatorPaused = coordinatorController_->isPaused();
        request.activeWorkers = coordinatorController_->activeWorkers();
        request.targetWorkers = coordinatorController_->targetWorkers();
        request.visualWorkerPoolEnabled =
            coordinatorController_->visualWorkerPoolEnabled();
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
        lastAttentionItems_ = static_cast<int>(data.attentionItems.size());

        const bool stopped = data.coordinatorState == CoordinatorLifecycleState::Stopped;
        const bool starting = data.coordinatorState == CoordinatorLifecycleState::Starting;
        const bool running = data.coordinatorState == CoordinatorLifecycleState::Running;
        startCoordinatorButton_->setVisible(data.controllerAvailable && stopped && !data.hasValidation);
        pauseCoordinatorButton_->setText(data.coordinatorPaused ? QStringLiteral("Resume") : QStringLiteral("Pause"));
        pauseCoordinatorButton_->setVisible(data.controllerAvailable && running);
        stopCoordinatorButton_->setText(starting ? QStringLiteral("Cancel startup") : QStringLiteral("Stop"));
        stopCoordinatorButton_->setVisible(data.controllerAvailable && (running || starting));
        {
            const QSignalBlocker blocker(targetWorkersSpin_);
            targetWorkersSpin_->setValue(data.targetWorkers);
        }
        targetWorkersSpin_->setVisible(data.controllerAvailable);
        targetWorkersSpin_->setEnabled(stopped || running);
        {
            const QSignalBlocker blocker(visualWorkersCheck_);
            visualWorkersCheck_->setChecked(data.visualWorkerPoolEnabled);
        }
        visualWorkersCheck_->setVisible(data.controllerAvailable);
        visualWorkersCheck_->setEnabled(stopped);
        isoSetupButton_->setText(data.isoMissing ? QStringLiteral("Set ISO") : QStringLiteral("Fix ISO"));
        isoSetupButton_->setVisible(!data.isoReady);
        dolphinSetupButton_->setText(data.dolphinMissing ? QStringLiteral("Set Dolphin base") : QStringLiteral("Fix Dolphin base"));
        dolphinSetupButton_->setVisible(!data.dolphinReady);
        fixRuntimeSetupButton_->setVisible(data.hasValidation);
        fixRuntimeSetupButton_->setToolTip(data.validation);
        openFailuresButton_->setVisible(data.failedWorkflows > 0 || data.failedJobs > 0);
        if (detailsDrawerButton_ != nullptr) {
            refreshDetailsButton();
            detailsDrawerButton_->setToolTip(data.attentionSummary);
        }

        workflowSummaryLabel_->setText(data.workflowSummary);
        workflowReadyValueLabel_->setText(data.workflowReadyText);
        workflowQueuedValueLabel_->setText(data.workflowQueuedText);
        workflowWaitingValueLabel_->setText(data.workflowWaitingText);
        workflowFinalValueLabel_->setText(data.workflowFinalizedText);
        savorqt::gui::ApplyTableRowsByKey(
            workflowTable_,
            *workflowRows,
            data.workflowRows,
            [](const RunningWorkflowRow& row) { return row.rowKey; },
            runningWorkflowRowsEqual,
            populateRunningWorkflowRow);
        for (int row = 0; row < workflowTable_->rowCount(); ++row) {
            const auto* item = workflowTable_->item(row, 0);
            const qint64 expansionId = item ? item->data(kWorkflowExpansionIdRole).toLongLong() : 0;
            const qint64 workflowId = item ? item->data(kWorkflowIdRole).toLongLong() : 0;
            workflowTable_->setRowHidden(row, workflowId > 0 && expansionId > 0 &&
                collapsedExpansionIds_.contains(expansionId));
        }

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

void RunningTab::showOperationalDetails()
{
    if (detailTabs_ == nullptr) {
        return;
    }

    setContextWidget(
        savorqt::gui::UiEntityRef{
            QStringLiteral("running"),
            QStringLiteral("operational_details"),
            0,
            QStringLiteral("running:operational_details"),
        },
        QStringLiteral("Operational Details"),
        detailTabs_,
        QVector<std::pair<QString, std::function<void()>>>{},
        savorqt::gui::ContextDrawerMode::Expanded);
    refreshDetailsButton();
}

void RunningTab::refreshDetailsButton()
{
    if (detailsDrawerButton_ == nullptr) {
        return;
    }
    if (isContextDrawerOpen()) {
        detailsDrawerButton_->setText(QStringLiteral("Hide details"));
        return;
    }
    detailsDrawerButton_->setText(lastAttentionItems_ == 0
        ? QStringLiteral("Details")
        : QStringLiteral("Details (%1)").arg(lastAttentionItems_));
}
