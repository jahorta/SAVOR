#include "WorkflowsPage.h"

#include "GUI/Refresh/RowUpdate.h"

#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
#include <QtCore/QTimeZone>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeWidgetItem>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <array>
#include <vector>

namespace {
constexpr int kWorkflowIdRole = Qt::UserRole + 1;
constexpr int kDefaultPageSize = 50;

QString qstr(const std::string& value)
{
    return QString::fromStdString(value);
}

QString formatOptionalId(const std::optional<std::int64_t>& value)
{
    return value.has_value() ? QString::number(static_cast<qint64>(*value)) : QStringLiteral("-");
}

QString formatTime(std::int64_t epochSeconds)
{
    if (epochSeconds <= 0) {
        return QStringLiteral("-");
    }
    return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(epochSeconds), QTimeZone::fromSecondsAheadOfUtc(0))
        .toLocalTime()
        .toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
}

QString formatOptionalTime(const std::optional<std::int64_t>& epochSeconds)
{
    return epochSeconds.has_value() ? formatTime(*epochSeconds) : QStringLiteral("-");
}

bool isTerminalStepState(const std::string& state)
{
    return state == "COMPLETED" || state == "SKIPPED";
}

bool isTerminalActivationState(const std::string& state)
{
    return state == "COMPLETED" || state == "SKIPPED";
}

bool isFutureActivationState(const savor::db::UiWorkflowUnitActivationSummary& activation)
{
    return activation.state == "WAITING";
}

bool isFutureStepState(const savor::db::UiWorkflowStepSummary& step)
{
    return step.state == "WAITING" && step.blocked_reason.empty();
}

bool isCurrentStepState(const savor::db::UiWorkflowStepSummary& step)
{
    return !isTerminalStepState(step.state) && !isFutureStepState(step);
}

QString progressText(std::int64_t completed, std::int64_t total, std::int64_t failed)
{
    if (total <= 0) {
        return failed > 0 ? QStringLiteral("0/0 failed %1").arg(failed) : QStringLiteral("-");
    }
    const int pct = static_cast<int>((completed * 100) / total);
    if (failed > 0) {
        return QStringLiteral("%1/%2 (%3%) failed %4")
            .arg(completed)
            .arg(total)
            .arg(pct)
            .arg(failed);
    }
    return QStringLiteral("%1/%2 (%3%)").arg(completed).arg(total).arg(pct);
}

std::int64_t effectiveCompletedJobCount(const savor::db::UiWorkflowStepSummary& step)
{
    if (step.job_completed_count == 0
        && step.job_failed_count == 0
        && step.job_count > 0
        && step.state == "COMPLETED") {
        return step.job_count;
    }
    return step.job_completed_count;
}

QString stepProgressText(const savor::db::UiWorkflowStepSummary& step)
{
    return progressText(effectiveCompletedJobCount(step), step.job_count, step.job_failed_count);
}

QString activeStepText(const savor::db::UiWorkflowDetail& detail)
{
    const savor::db::UiWorkflowUnitActivationSummary* fallbackActivation = nullptr;
    for (const auto& activation : detail.unit_activations) {
        if (!isTerminalActivationState(activation.state) && !isFutureActivationState(activation)) {
            return QStringLiteral("%1 (%2)")
                .arg(qstr(activation.display_name.empty() ? activation.activation_key : activation.display_name))
                .arg(qstr(activation.state));
        }
        if (fallbackActivation == nullptr && isFutureActivationState(activation)) {
            fallbackActivation = &activation;
        }
    }
    if (fallbackActivation != nullptr) {
        return QStringLiteral("%1 (%2)")
            .arg(qstr(fallbackActivation->display_name.empty() ? fallbackActivation->activation_key : fallbackActivation->display_name))
            .arg(qstr(fallbackActivation->state));
    }

    const savor::db::UiWorkflowStepSummary* fallback = nullptr;
    for (const auto& step : detail.steps) {
        if (isCurrentStepState(step)) {
            return QStringLiteral("%1 (%2)")
                .arg(qstr(step.step_key))
                .arg(qstr(step.state));
        }
        if (fallback == nullptr && isFutureStepState(step)) {
            fallback = &step;
        }
    }
    if (fallback != nullptr) {
        return QStringLiteral("%1 (%2)")
            .arg(qstr(fallback->step_key))
            .arg(qstr(fallback->state));
    }
    return detail.steps.empty() ? QStringLiteral("-") : QStringLiteral("complete");
}

std::array<const char*, 8> stepHeaders()
{
    return {
        "Step",
        "Kind",
        "State",
        "Job Set",
        "Jobs",
        "Attempts",
        "Blocked",
        "Started",
    };
}

std::array<const char*, 8> jobSetHeaders()
{
    return {
        "Step",
        "Kind",
        "State",
        "Job Set",
        "Program",
        "Progress",
        "Jobs",
        "Created",
    };
}

void configureStepTree(QTreeWidget* tree)
{
    const auto headers = stepHeaders();
    QStringList labels;
    for (const char* header : headers) {
        labels.push_back(QString::fromLatin1(header));
    }
    tree->setColumnCount(labels.size());
    tree->setHeaderLabels(labels);
    tree->setRootIsDecorated(true);
    tree->setAlternatingRowColors(true);
    tree->setUniformRowHeights(true);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    tree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    tree->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    tree->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    tree->header()->setSectionResizeMode(4, QHeaderView::Interactive);
    tree->header()->setSectionResizeMode(5, QHeaderView::Interactive);
    tree->header()->setSectionResizeMode(6, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(7, QHeaderView::Interactive);
}

void configureJobSetsTree(QTreeWidget* tree)
{
    const auto headers = jobSetHeaders();
    QStringList labels;
    for (const char* header : headers) {
        labels.push_back(QString::fromLatin1(header));
    }
    tree->setColumnCount(labels.size());
    tree->setHeaderLabels(labels);
    tree->setRootIsDecorated(true);
    tree->setAlternatingRowColors(true);
    tree->setUniformRowHeights(true);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    tree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    tree->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    tree->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    tree->header()->setSectionResizeMode(4, QHeaderView::Interactive);
    tree->header()->setSectionResizeMode(5, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(6, QHeaderView::Interactive);
    tree->header()->setSectionResizeMode(7, QHeaderView::Interactive);
}

QTableWidgetItem* makeWorkflowTableItem(const QString& text)
{
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

bool workflowTableRowsEqual(const WorkflowsPage::WorkflowTableRow& lhs, const WorkflowsPage::WorkflowTableRow& rhs)
{
    return lhs.workflowInstanceId == rhs.workflowInstanceId
        && lhs.kind == rhs.kind
        && lhs.state == rhs.state
        && lhs.activeStep == rhs.activeStep
        && lhs.steps == rhs.steps
        && lhs.jobs == rhs.jobs
        && lhs.problems == rhs.problems
        && lhs.created == rhs.created
        && lhs.completed == rhs.completed;
}

void populateWorkflowTableRow(QTableWidget* table, int row, const WorkflowsPage::WorkflowTableRow& workflow)
{
    auto* idItem = makeWorkflowTableItem(QString::number(static_cast<qint64>(workflow.workflowInstanceId)));
    idItem->setData(kWorkflowIdRole, static_cast<qint64>(workflow.workflowInstanceId));
    table->setItem(row, 0, idItem);
    table->setItem(row, 1, makeWorkflowTableItem(workflow.kind));
    table->setItem(row, 2, makeWorkflowTableItem(workflow.state));
    table->setItem(row, 3, makeWorkflowTableItem(workflow.activeStep));
    table->setItem(row, 4, makeWorkflowTableItem(workflow.steps));
    table->setItem(row, 5, makeWorkflowTableItem(workflow.jobs));
    table->setItem(row, 6, makeWorkflowTableItem(workflow.problems));
    table->setItem(row, 7, makeWorkflowTableItem(workflow.created));
    table->setItem(row, 8, makeWorkflowTableItem(workflow.completed));
}

bool treeDisplayRowsEqual(const WorkflowsPage::TreeDisplayRow& lhs, const WorkflowsPage::TreeDisplayRow& rhs)
{
    return lhs.key == rhs.key
        && lhs.columns == rhs.columns
        && lhs.firstColumnSpanned == rhs.firstColumnSpanned
        && lhs.expanded == rhs.expanded
        && savorqt::gui::RowsEqual(lhs.children, rhs.children, treeDisplayRowsEqual);
}

void populateTreeDisplayRow(QTreeWidget* tree, QTreeWidgetItem* item, const WorkflowsPage::TreeDisplayRow& row)
{
    if (item == nullptr) {
        return;
    }

    const int columnCount = tree != nullptr ? tree->columnCount() : row.columns.size();
    for (int column = 0; column < columnCount; ++column) {
        item->setText(column, column < row.columns.size() ? row.columns[column] : QString());
    }
    item->setFirstColumnSpanned(row.firstColumnSpanned);

    const auto oldChildren = item->takeChildren();
    for (QTreeWidgetItem* child : oldChildren) {
        delete child;
    }
    for (const auto& childRow : row.children) {
        auto* child = new QTreeWidgetItem(item);
        populateTreeDisplayRow(tree, child, childRow);
    }
    item->setExpanded(row.expanded);
}

WorkflowsPage::TreeDisplayRow makeEmptyDisplayRow(const QString& key, const QString& text)
{
    WorkflowsPage::TreeDisplayRow row{};
    row.key = key;
    row.columns = QStringList{ text };
    row.firstColumnSpanned = true;
    return row;
}

WorkflowsPage::TreeDisplayRow makeStepDisplayRow(const savor::db::UiWorkflowStepSummary& step)
{
    WorkflowsPage::TreeDisplayRow row{};
    row.key = QStringLiteral("s:%1").arg(static_cast<qint64>(step.workflow_step_id));
    row.columns = QStringList{
        qstr(step.step_key),
        qstr(step.step_kind),
        qstr(step.state),
        formatOptionalId(step.job_set_id),
        stepProgressText(step),
        QStringLiteral("%1/%2").arg(step.attempts).arg(step.max_attempts),
        qstr(step.blocked_reason),
        formatOptionalTime(step.started_at_utc),
    };
    return row;
}

WorkflowsPage::TreeDisplayRow makeActivationDisplayRow(
    const savor::db::UiWorkflowUnitActivationSummary& activation,
    const savor::db::UiWorkflowDetail& detail)
{
    std::int64_t total = 0;
    std::int64_t completed = 0;
    std::int64_t failed = 0;
    for (const auto& step : detail.steps) {
        if (!step.workflow_unit_activation_id.has_value()
            || *step.workflow_unit_activation_id != activation.workflow_unit_activation_id) {
            continue;
        }
        total += step.job_count;
        completed += effectiveCompletedJobCount(step);
        failed += step.job_failed_count;
    }

    WorkflowsPage::TreeDisplayRow row{};
    row.key = QStringLiteral("a:%1").arg(static_cast<qint64>(activation.workflow_unit_activation_id));
    const auto label = activation.display_name.empty() ? activation.activation_key : activation.display_name;
    row.columns = QStringList{
        QStringLiteral("%1 [%2]").arg(qstr(label)).arg(qstr(activation.activation_key)),
        qstr(activation.unit_kind),
        qstr(activation.state),
        QStringLiteral("-"),
        progressText(completed, total, failed),
        QStringLiteral("-"),
        qstr(activation.failure_text),
        formatOptionalTime(activation.started_at_utc),
    };
    row.expanded = true;
    for (const auto& step : detail.steps) {
        if (step.workflow_unit_activation_id.has_value()
            && *step.workflow_unit_activation_id == activation.workflow_unit_activation_id) {
            row.children.push_back(makeStepDisplayRow(step));
        }
    }
    return row;
}

WorkflowsPage::TreeDisplayRow makeAlertDisplayRow(const savor::db::UiWorkflowAlertSummary& alert)
{
    WorkflowsPage::TreeDisplayRow row{};
    row.key = QStringLiteral("%1:%2:%3:%4")
        .arg(qstr(alert.alert_kind))
        .arg(qstr(alert.alert_code))
        .arg(formatOptionalId(alert.workflow_step_id))
        .arg(alert.is_active ? 1 : 0);
    row.columns = QStringList{
        qstr(alert.alert_kind),
        qstr(alert.alert_code),
        formatOptionalId(alert.workflow_step_id),
        alert.is_active ? QStringLiteral("yes") : QStringLiteral("no"),
        qstr(alert.message),
        formatTime(alert.last_seen_at_utc),
    };
    return row;
}

WorkflowsPage::TreeDisplayRow makeJobSetDetailDisplayRow(const savor::db::UiJobSummary& job)
{
    WorkflowsPage::TreeDisplayRow row{};
    row.key = QStringLiteral("j:%1").arg(static_cast<qint64>(job.job_id));
    row.columns = QStringList{
        QStringLiteral("Job #%1").arg(static_cast<qint64>(job.job_id)),
        QStringLiteral("priority %1").arg(job.priority),
        qstr(job.state),
        QString::number(static_cast<qint64>(job.job_set_id)),
        qstr(savorqt::db::SavorDbJobSetService::ProgramKindLabel(job.program_kind)),
        QStringLiteral("attempts %1/%2").arg(job.attempts).arg(job.max_attempts),
        qstr(job.error_code),
        formatTime(job.queued_at_utc),
    };
    return row;
}

WorkflowsPage::TreeDisplayRow makeWorkflowJobSetDisplayRow(const savorqt::db::WorkflowJobSetRow& workflowJobSet)
{
    const auto& step = workflowJobSet.step;
    WorkflowsPage::TreeDisplayRow row{};
    row.key = QStringLiteral("s:%1").arg(static_cast<qint64>(step.workflow_step_id));
    row.columns = QStringList{
        qstr(step.step_key),
        qstr(step.step_kind),
        qstr(step.state),
        formatOptionalId(step.job_set_id),
        qstr(workflowJobSet.program_kind_label),
        stepProgressText(step),
        QString::number(static_cast<qint64>(step.job_count)),
        formatTime(step.created_at_utc),
    };

    if (!workflowJobSet.detail.has_value()) {
        row.children.push_back(makeEmptyDisplayRow(QStringLiteral("pending"), QStringLiteral("Job set detail is not projected yet.")));
        return row;
    }

    const auto& detail = *workflowJobSet.detail;
    row.columns[4] = qstr(savorqt::db::SavorDbJobSetService::ProgramKindLabel(detail.summary.program_kind));
    row.columns[5] = progressText(detail.summary.completed_jobs, detail.summary.total_jobs, detail.summary.failed_jobs);
    row.columns[6] = QStringLiteral("%1 shown / %2 total")
        .arg(detail.jobs.size())
        .arg(static_cast<qint64>(detail.summary.total_jobs));
    row.columns[7] = formatTime(detail.summary.created_at_utc);

    if (detail.jobs.empty()) {
        row.children.push_back(makeEmptyDisplayRow(QStringLiteral("empty"), QStringLiteral("No jobs projected for this job set.")));
    } else {
        for (const auto& job : detail.jobs) {
            row.children.push_back(makeJobSetDetailDisplayRow(job));
        }
    }
    row.expanded = true;
    return row;
}
}

WorkflowsPage::WorkflowsPage(QWidget* parent)
    : QWidget(parent)
{
    createWidgets();
    wireSignals();
}

void WorkflowsPage::setPageActive(bool active)
{
    if (pageActive_ == active) {
        return;
    }

    pageActive_ = active;
    if (workflowRefreshPipeline_ != nullptr) {
        workflowRefreshPipeline_->setActive(pageActive_);
        workflowRefreshPipeline_->setAutoRefreshEnabled(autoRefreshCheck_->isChecked());
    }
    if (!pageActive_) {
        return;
    }
    refreshWorkflows();
}

void WorkflowsPage::createWidgets()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(10);

    auto* toolbarPanel = new QFrame(this);
    toolbarPanel->setObjectName("jobSetsToolbarPanel");
    auto* toolbarLayout = new QGridLayout(toolbarPanel);
    toolbarLayout->setContentsMargins(8, 7, 8, 7);
    toolbarLayout->setHorizontalSpacing(10);
    toolbarLayout->setVerticalSpacing(10);

    stateFilter_ = new QComboBox(toolbarPanel);
    stateFilter_->setObjectName("jobSetsFilterCombo");
    stateFilter_->addItem(QStringLiteral("All states"), QString());
    stateFilter_->addItem(QStringLiteral("Running"), QStringLiteral("RUNNING"));
    stateFilter_->addItem(QStringLiteral("Pending"), QStringLiteral("PENDING"));
    stateFilter_->addItem(QStringLiteral("Completed"), QStringLiteral("COMPLETED"));
    stateFilter_->addItem(QStringLiteral("Failed"), QStringLiteral("FAILED"));
    stateFilter_->addItem(QStringLiteral("Canceled"), QStringLiteral("CANCELED"));

    kindFilter_ = new QLineEdit(toolbarPanel);
    kindFilter_->setObjectName("jobSetsFilterCombo");
    kindFilter_->setPlaceholderText(QStringLiteral("workflow kind"));

    pageSizeSpin_ = new QSpinBox(toolbarPanel);
    pageSizeSpin_->setObjectName("jobSetsSpin");
    pageSizeSpin_->setRange(10, 250);
    pageSizeSpin_->setSingleStep(10);
    pageSizeSpin_->setValue(kDefaultPageSize);

    applyButton_ = new QPushButton(QStringLiteral("Apply"), toolbarPanel);
    applyButton_->setObjectName("jobSetsPrimaryButton");
    resetButton_ = new QPushButton(QStringLiteral("Reset"), toolbarPanel);
    resetButton_->setObjectName("jobSetsSecondaryButton");
    refreshButton_ = new QPushButton(QStringLiteral("Refresh now"), toolbarPanel);
    refreshButton_->setObjectName("jobSetsSecondaryButton");
    prevButton_ = new QPushButton(QStringLiteral("Prev"), toolbarPanel);
    prevButton_->setObjectName("jobSetsSecondaryButton");
    nextButton_ = new QPushButton(QStringLiteral("Next"), toolbarPanel);
    nextButton_->setObjectName("jobSetsSecondaryButton");

    autoRefreshCheck_ = new QCheckBox(QStringLiteral("Auto refresh"), toolbarPanel);
    autoRefreshCheck_->setObjectName("jobSetsCheckBox");
    autoRefreshCheck_->setChecked(true);
    victoryOnlyCheck_ = new QCheckBox(QStringLiteral("Victory only"), toolbarPanel);
    victoryOnlyCheck_->setObjectName("jobSetsCheckBox");

    refreshSecondsSpin_ = new QSpinBox(toolbarPanel);
    refreshSecondsSpin_->setObjectName("jobSetsSpin");
    refreshSecondsSpin_->setRange(1, 10);
    refreshSecondsSpin_->setSuffix(QStringLiteral(" s"));
    refreshSecondsSpin_->setValue(2);

    toolbarLayout->addWidget(new QLabel(QStringLiteral("State"), toolbarPanel), 0, 0);
    toolbarLayout->addWidget(stateFilter_, 1, 0);
    toolbarLayout->addWidget(new QLabel(QStringLiteral("Kind"), toolbarPanel), 0, 1);
    toolbarLayout->addWidget(kindFilter_, 1, 1);
    toolbarLayout->addWidget(new QLabel(QStringLiteral("Page size"), toolbarPanel), 0, 2);
    toolbarLayout->addWidget(pageSizeSpin_, 1, 2);
    toolbarLayout->addWidget(applyButton_, 1, 3);
    toolbarLayout->addWidget(resetButton_, 1, 4);
    toolbarLayout->addWidget(refreshButton_, 1, 5);
    toolbarLayout->addWidget(prevButton_, 1, 6);
    toolbarLayout->addWidget(nextButton_, 1, 7);
    toolbarLayout->addWidget(victoryOnlyCheck_, 0, 8, 1, 1, Qt::AlignBottom);
    toolbarLayout->addWidget(autoRefreshCheck_, 0, 9, 1, 2, Qt::AlignBottom);
    toolbarLayout->addWidget(refreshSecondsSpin_, 1, 9);
    toolbarLayout->setColumnStretch(1, 1);

    rootLayout->addWidget(toolbarPanel);

    auto* statusPanel = new QFrame(this);
    statusPanel->setObjectName("jobSetsPagingPanel");
    auto* statusLayout = new QHBoxLayout(statusPanel);
    statusLayout->setContentsMargins(8, 7, 8, 7);
    statusLayout->setSpacing(10);

    summaryLabel_ = new QLabel(statusPanel);
    summaryLabel_->setObjectName("jobSetsMetaText");
    lastRefreshLabel_ = new QLabel(statusPanel);
    lastRefreshLabel_->setObjectName("jobSetsMetaText");
    inlineMessageLabel_ = new QLabel(statusPanel);
    inlineMessageLabel_->setObjectName("jobSetsInlineMessage");
    inlineMessageLabel_->setWordWrap(true);
    inlineMessageLabel_->hide();

    statusLayout->addWidget(summaryLabel_);
    statusLayout->addStretch();
    statusLayout->addWidget(inlineMessageLabel_, 1);
    statusLayout->addWidget(lastRefreshLabel_);
    rootLayout->addWidget(statusPanel);

    auto* splitter = new QSplitter(Qt::Vertical, this);
    workflowTable_ = new QTableWidget(splitter);
    workflowTable_->setObjectName("jobSetsTreeView");
    workflowTable_->setColumnCount(9);
    workflowTable_->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("Workflow"),
        QStringLiteral("Kind"),
        QStringLiteral("State"),
        QStringLiteral("Active Step"),
        QStringLiteral("Steps"),
        QStringLiteral("Jobs"),
        QStringLiteral("Problems"),
        QStringLiteral("Created"),
        QStringLiteral("Completed"),
    });
    workflowTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    workflowTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    workflowTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    workflowTable_->setAlternatingRowColors(true);
    workflowTable_->verticalHeader()->hide();
    workflowTable_->horizontalHeader()->setStretchLastSection(false);
    workflowTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    workflowTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    workflowTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    workflowTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    workflowTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Interactive);
    workflowTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Interactive);
    workflowTable_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Interactive);
    workflowTable_->horizontalHeader()->setSectionResizeMode(7, QHeaderView::Interactive);
    workflowTable_->horizontalHeader()->setSectionResizeMode(8, QHeaderView::Interactive);
    splitter->addWidget(workflowTable_);

    auto* detailPanel = new QWidget(splitter);
    auto* detailLayout = new QVBoxLayout(detailPanel);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(8);

    detailHeaderLabel_ = new QLabel(QStringLiteral("Select a workflow"), detailPanel);
    detailHeaderLabel_->setObjectName("pageTitle");
    detailMetaLabel_ = new QLabel(detailPanel);
    detailMetaLabel_->setObjectName("jobSetsMetaText");
    detailMetaLabel_->setWordWrap(true);
    detailLayout->addWidget(detailHeaderLabel_);
    detailLayout->addWidget(detailMetaLabel_);

    auto* detailTabs = new QTabWidget(detailPanel);
    currentStepsTree_ = new QTreeWidget(detailTabs);
    futureStepsTree_ = new QTreeWidget(detailTabs);
    pastStepsTree_ = new QTreeWidget(detailTabs);
    jobSetsTree_ = new QTreeWidget(detailTabs);
    alertsTree_ = new QTreeWidget(detailTabs);
    configureStepTree(currentStepsTree_);
    configureStepTree(futureStepsTree_);
    configureStepTree(pastStepsTree_);
    configureJobSetsTree(jobSetsTree_);
    alertsTree_->setColumnCount(6);
    alertsTree_->setHeaderLabels(QStringList{
        QStringLiteral("Kind"),
        QStringLiteral("Code"),
        QStringLiteral("Step"),
        QStringLiteral("Active"),
        QStringLiteral("Message"),
        QStringLiteral("Last Seen"),
    });
    alertsTree_->setRootIsDecorated(false);
    alertsTree_->setAlternatingRowColors(true);
    alertsTree_->setUniformRowHeights(true);
    alertsTree_->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    alertsTree_->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    alertsTree_->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    alertsTree_->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    alertsTree_->header()->setSectionResizeMode(4, QHeaderView::Stretch);
    alertsTree_->header()->setSectionResizeMode(5, QHeaderView::Interactive);
    detailTabs->addTab(currentStepsTree_, QStringLiteral("Current"));
    detailTabs->addTab(futureStepsTree_, QStringLiteral("Future"));
    detailTabs->addTab(pastStepsTree_, QStringLiteral("Past"));
    detailTabs->addTab(jobSetsTree_, QStringLiteral("Job Sets"));
    detailTabs->addTab(alertsTree_, QStringLiteral("Alerts"));
    detailLayout->addWidget(detailTabs, 1);
    splitter->addWidget(detailPanel);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    rootLayout->addWidget(splitter, 1);

    workflowRefreshPipeline_ = new savorqt::gui::AsyncRefreshPipeline<savorqt::db::WorkflowListRequest, WorkflowPageResult>(this);
}

void WorkflowsPage::wireSignals()
{
    connect(applyButton_, &QPushButton::clicked, this, &WorkflowsPage::applyFilters);
    connect(resetButton_, &QPushButton::clicked, this, [this]() {
        stateFilter_->setCurrentIndex(0);
        kindFilter_->clear();
        victoryOnlyCheck_->setChecked(false);
        pageSizeSpin_->setValue(kDefaultPageSize);
        before_.reset();
        after_.reset();
        refreshWorkflows();
    });
    connect(refreshButton_, &QPushButton::clicked, this, [this]() {
        before_.reset();
        after_.reset();
        refreshWorkflows();
    });
    connect(prevButton_, &QPushButton::clicked, this, &WorkflowsPage::requestPreviousPage);
    connect(nextButton_, &QPushButton::clicked, this, &WorkflowsPage::requestNextPage);
    connect(autoRefreshCheck_, &QCheckBox::toggled, this, [this](bool enabled) {
        if (workflowRefreshPipeline_ != nullptr) {
            workflowRefreshPipeline_->setAutoRefreshEnabled(enabled);
        }
    });
    connect(victoryOnlyCheck_, &QCheckBox::toggled, this, &WorkflowsPage::applyFilters);
    connect(refreshSecondsSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int seconds) {
        if (workflowRefreshPipeline_ != nullptr) {
            workflowRefreshPipeline_->setRefreshIntervalMs(seconds * 1000);
        }
    });
    connect(workflowTable_, &QTableWidget::itemSelectionChanged, this, &WorkflowsPage::handleWorkflowSelectionChanged);

    workflowRefreshPipeline_->setRefreshIntervalMs(refreshSecondsSpin_->value() * 1000);
    workflowRefreshPipeline_->setAutoRefreshEnabled(autoRefreshCheck_->isChecked());
    workflowRefreshPipeline_->setRequestBuilder([this](savorqt::gui::RefreshReason reason) -> std::optional<savorqt::db::WorkflowListRequest> {
        if (reason == savorqt::gui::RefreshReason::Auto
            && (detailFetchInFlight_ || jobSetsFetchInFlight_ || before_.has_value() || after_.has_value())) {
            return std::nullopt;
        }

        workflowFetchInFlight_ = true;
        errorMessage_.clear();

        savorqt::db::WorkflowListRequest request{};
        request.state = stateFilter_->currentData().toString().trimmed().toStdString();
        request.workflow_kind = kindFilter_->text().trimmed().toStdString();
        request.before = before_;
        request.after = after_;
        request.limit = pageSizeSpin_->value();
        request.battle_final_victory_only = victoryOnlyCheck_->isChecked();
        updateStatusWidgets();
        return request;
    });
    workflowRefreshPipeline_->setLoadAndPrepare([](savorqt::db::WorkflowListRequest request) {
        return savorqt::gui::AsyncRefreshResult<WorkflowPageResult>::Ok(
            savorqt::db::SavorDbWorkflowService::ListWorkflowInstances(request));
    });
    workflowRefreshPipeline_->setApply([this](const WorkflowPageResult& result, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        workflowFetchInFlight_ = false;
        if (result.ok) {
            workflowPage_ = result.value;
            lastRefresh_ = QDateTime::currentDateTime();
            errorMessage_.clear();
            infoMessage_.clear();
        } else {
            workflowPage_ = {};
            errorMessage_ = QStringLiteral("Workflows failed: %1").arg(qstr(result.error.message));
        }
        updateWorkflowTable();
        updateStatusWidgets();
    });
    workflowRefreshPipeline_->setApplyError([this](const QString& error, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        workflowFetchInFlight_ = false;
        workflowPage_ = {};
        errorMessage_ = error;
        updateWorkflowTable();
        updateStatusWidgets();
    });

    connect(&detailWatcher_, &QFutureWatcher<WorkflowDetailResult>::finished, this, [this]() {
        detailFetchInFlight_ = false;
        try {
            const auto result = detailWatcher_.result();
            if (result.ok && result.value.instance.workflow_instance_id == selectedWorkflowInstanceId_) {
                selectedWorkflowDetail_ = result.value;
                updateWorkflowDetail();
                fetchWorkflowJobSets(result.value);
            } else if (!result.ok) {
                clearWorkflowDetail(QStringLiteral("Workflow detail failed: %1").arg(qstr(result.error.message)));
            }
        } catch (const std::exception& ex) {
            clearWorkflowDetail(QStringLiteral("Workflow detail failed: %1").arg(QString::fromUtf8(ex.what())));
        } catch (...) {
            clearWorkflowDetail(QStringLiteral("Workflow detail failed: unknown exception"));
        }
        updateStatusWidgets();
    });

    connect(&jobSetsWatcher_, &QFutureWatcher<WorkflowJobSetsResult>::finished, this, [this]() {
        jobSetsFetchInFlight_ = false;
        try {
            const auto result = jobSetsWatcher_.result();
            if (result.ok && jobSetsFetchWorkflowInstanceId_ == selectedWorkflowInstanceId_) {
                workflowJobSets_ = result.value;
                updateWorkflowJobSets();
            } else if (!result.ok) {
                clearWorkflowJobSets(QStringLiteral("Workflow job sets failed: %1").arg(qstr(result.error.message)));
            }
        } catch (const std::exception& ex) {
            clearWorkflowJobSets(QStringLiteral("Workflow job sets failed: %1").arg(QString::fromUtf8(ex.what())));
        } catch (...) {
            clearWorkflowJobSets(QStringLiteral("Workflow job sets failed: unknown exception"));
        }
        updateStatusWidgets();
    });
}

void WorkflowsPage::refreshWorkflows()
{
    if (workflowRefreshPipeline_ != nullptr) {
        workflowRefreshPipeline_->requestRefresh(savorqt::gui::RefreshReason::Manual);
    }
}

void WorkflowsPage::fetchWorkflowDetail(std::int64_t workflowInstanceId)
{
    if (workflowInstanceId <= 0) {
        clearWorkflowDetail(QStringLiteral("Select a workflow"));
        return;
    }

    detailFetchInFlight_ = true;
    detailWatcher_.setFuture(QtConcurrent::run([workflowInstanceId]() {
        return savorqt::db::SavorDbWorkflowService::GetWorkflowDetail(workflowInstanceId);
    }));
    updateStatusWidgets();
}

void WorkflowsPage::fetchWorkflowJobSets(const savor::db::UiWorkflowDetail& detail)
{
    jobSetsFetchWorkflowInstanceId_ = detail.instance.workflow_instance_id;
    jobSetsFetchInFlight_ = true;
    clearWorkflowJobSets(QStringLiteral("Loading workflow job sets..."));

    jobSetsWatcher_.setFuture(QtConcurrent::run([detail]() {
        return savorqt::db::SavorDbJobSetService::ListWorkflowJobSets(detail, 25);
    }));
    updateStatusWidgets();
}

void WorkflowsPage::applyFilters()
{
    before_.reset();
    after_.reset();
    refreshWorkflows();
}

void WorkflowsPage::requestNextPage()
{
    if (!workflowPage_.next.has_value()) {
        return;
    }
    before_ = workflowPage_.next;
    after_.reset();
    refreshWorkflows();
}

void WorkflowsPage::requestPreviousPage()
{
    if (!workflowPage_.prev.has_value()) {
        return;
    }
    after_ = workflowPage_.prev;
    before_.reset();
    refreshWorkflows();
}

void WorkflowsPage::handleWorkflowSelectionChanged()
{
    const auto selected = workflowTable_->selectedItems();
    if (selected.empty()) {
        return;
    }
    const int row = selected.front()->row();
    const auto* idItem = workflowTable_->item(row, 0);
    if (idItem == nullptr) {
        return;
    }
    const auto workflowId = idItem->data(kWorkflowIdRole).toLongLong();
    if (workflowId <= 0 || workflowId == selectedWorkflowInstanceId_) {
        return;
    }
    selectedWorkflowInstanceId_ = workflowId;
    selectedWorkflowDetail_.reset();
    workflowJobSets_.clear();
    clearWorkflowDetail(QStringLiteral("Loading workflow detail..."));
    fetchWorkflowDetail(workflowId);
}

void WorkflowsPage::updateWorkflowTable()
{
    std::vector<WorkflowTableRow> rows;
    rows.reserve(workflowPage_.items.size());
    for (const auto& workflow : workflowPage_.items) {
        rows.push_back(WorkflowTableRow{
            workflow.workflow_instance_id,
            qstr(workflow.workflow_kind),
            qstr(workflow.state),
            QStringLiteral("-"),
            QStringLiteral("-"),
            QStringLiteral("-"),
            QStringLiteral("blocked %1 / failed %2")
                .arg(workflow.blocked_step_count)
                .arg(workflow.failed_step_count),
            formatTime(workflow.created_at_utc),
            formatOptionalTime(workflow.completed_at_utc),
        });
    }

    const std::int64_t priorSelection = selectedWorkflowInstanceId_;
    std::int64_t targetSelection = 0;

    {
        const QSignalBlocker blocker(workflowTable_);
        savorqt::gui::ApplyTableRowsByKey(
            workflowTable_,
            currentWorkflowRows_,
            rows,
            [](const WorkflowTableRow& row) { return row.workflowInstanceId; },
            workflowTableRowsEqual,
            populateWorkflowTableRow);

        for (int row = 0; row < workflowTable_->rowCount(); ++row) {
            const auto* idItem = workflowTable_->item(row, 0);
            if (idItem != nullptr && idItem->data(kWorkflowIdRole).toLongLong() == priorSelection) {
                workflowTable_->selectRow(row);
                targetSelection = priorSelection;
                break;
            }
        }

        if (targetSelection == 0 && !workflowPage_.items.empty()) {
            workflowTable_->selectRow(0);
            targetSelection = workflowPage_.items.front().workflow_instance_id;
        } else if (workflowPage_.items.empty()) {
            selectedWorkflowInstanceId_ = 0;
            selectedWorkflowDetail_.reset();
            clearWorkflowDetail(QStringLiteral("No workflow selected"));
        }
    }

    prevButton_->setEnabled(workflowPage_.prev.has_value() && !workflowFetchInFlight_);
    nextButton_->setEnabled(workflowPage_.next.has_value() && !workflowFetchInFlight_);

    if (targetSelection > 0) {
        const bool needsDetail = !selectedWorkflowDetail_.has_value()
            || selectedWorkflowDetail_->instance.workflow_instance_id != targetSelection;
        selectedWorkflowInstanceId_ = targetSelection;
        if (needsDetail) {
            fetchWorkflowDetail(targetSelection);
        } else {
            updateWorkflowDetail();
        }
    }
}

void WorkflowsPage::updateWorkflowDetail()
{
    if (!selectedWorkflowDetail_.has_value()) {
        clearWorkflowDetail(QStringLiteral("No workflow selected"));
        return;
    }

    const auto& detail = *selectedWorkflowDetail_;
    detailHeaderLabel_->setText(QStringLiteral("Workflow #%1")
        .arg(static_cast<qint64>(detail.instance.workflow_instance_id)));
    detailMetaLabel_->setText(QStringLiteral("%1 - %2 - root %3 #%4 - created by %5")
        .arg(qstr(detail.instance.workflow_kind))
        .arg(qstr(detail.instance.state))
        .arg(qstr(detail.instance.root_scope_kind))
        .arg(formatOptionalId(detail.instance.root_scope_id))
        .arg(qstr(detail.instance.created_by)));

    int currentCount = 0;
    int futureCount = 0;
    int pastCount = 0;
    std::int64_t totalJobs = 0;
    std::int64_t completedJobs = 0;
    std::int64_t failedJobs = 0;
    std::vector<TreeDisplayRow> currentRows;
    std::vector<TreeDisplayRow> futureRows;
    std::vector<TreeDisplayRow> pastRows;
    std::vector<TreeDisplayRow> alertRows;

    for (const auto& step : detail.steps) {
        totalJobs += step.job_count;
        completedJobs += effectiveCompletedJobCount(step);
        failedJobs += step.job_failed_count;
    }

    for (const auto& activation : detail.unit_activations) {
        if (isTerminalActivationState(activation.state)) {
            pastRows.push_back(makeActivationDisplayRow(activation, detail));
            ++pastCount;
        } else if (isFutureActivationState(activation)) {
            futureRows.push_back(makeActivationDisplayRow(activation, detail));
            ++futureCount;
        } else {
            currentRows.push_back(makeActivationDisplayRow(activation, detail));
            ++currentCount;
        }
    }

    if (detail.unit_activations.empty()) {
        for (const auto& step : detail.steps) {
            if (isTerminalStepState(step.state)) {
                pastRows.push_back(makeStepDisplayRow(step));
                ++pastCount;
            } else if (isFutureStepState(step)) {
                futureRows.push_back(makeStepDisplayRow(step));
                ++futureCount;
            } else {
                currentRows.push_back(makeStepDisplayRow(step));
                ++currentCount;
            }
        }
    }

    if (currentCount == 0) {
        currentRows.push_back(makeEmptyDisplayRow(QStringLiteral("empty-current"), QStringLiteral("No current steps.")));
    }
    if (futureCount == 0) {
        futureRows.push_back(makeEmptyDisplayRow(QStringLiteral("empty-future"), QStringLiteral("No future steps.")));
    }
    if (pastCount == 0) {
        pastRows.push_back(makeEmptyDisplayRow(QStringLiteral("empty-past"), QStringLiteral("No past steps.")));
    }

    if (detail.alerts.empty()) {
        alertRows.push_back(makeEmptyDisplayRow(QStringLiteral("empty-alerts"), QStringLiteral("No alerts.")));
    } else {
        for (const auto& alert : detail.alerts) {
            alertRows.push_back(makeAlertDisplayRow(alert));
        }
    }

    savorqt::gui::ApplyTreeRowsByKey(
        currentStepsTree_,
        currentCurrentStepRows_,
        currentRows,
        [](const TreeDisplayRow& row) { return row.key; },
        treeDisplayRowsEqual,
        populateTreeDisplayRow);
    savorqt::gui::ApplyTreeRowsByKey(
        futureStepsTree_,
        currentFutureStepRows_,
        futureRows,
        [](const TreeDisplayRow& row) { return row.key; },
        treeDisplayRowsEqual,
        populateTreeDisplayRow);
    savorqt::gui::ApplyTreeRowsByKey(
        pastStepsTree_,
        currentPastStepRows_,
        pastRows,
        [](const TreeDisplayRow& row) { return row.key; },
        treeDisplayRowsEqual,
        populateTreeDisplayRow);
    savorqt::gui::ApplyTreeRowsByKey(
        alertsTree_,
        currentAlertRows_,
        alertRows,
        [](const TreeDisplayRow& row) { return row.key; },
        treeDisplayRowsEqual,
        populateTreeDisplayRow);

    for (int row = 0; row < workflowTable_->rowCount(); ++row) {
        const auto* idItem = workflowTable_->item(row, 0);
        if (idItem == nullptr || idItem->data(kWorkflowIdRole).toLongLong() != detail.instance.workflow_instance_id) {
            continue;
        }
        workflowTable_->item(row, 3)->setText(activeStepText(detail));
        workflowTable_->item(row, 4)->setText(QStringLiteral("%1 current / %2 future / %3 past")
            .arg(currentCount)
            .arg(futureCount)
            .arg(pastCount));
        workflowTable_->item(row, 5)->setText(progressText(completedJobs, totalJobs, failedJobs));
        break;
    }

}

void WorkflowsPage::updateWorkflowJobSets()
{
    std::vector<TreeDisplayRow> rows;
    if (workflowJobSets_.empty()) {
        rows.push_back(makeEmptyDisplayRow(QStringLiteral("empty-job-sets"), QStringLiteral("No job sets are attached to this workflow yet.")));
    } else if (selectedWorkflowDetail_.has_value() && !selectedWorkflowDetail_->unit_activations.empty()) {
        for (const auto& activation : selectedWorkflowDetail_->unit_activations) {
            TreeDisplayRow activationRow{};
            activationRow.key = QStringLiteral("a:%1").arg(static_cast<qint64>(activation.workflow_unit_activation_id));
            const auto label = activation.display_name.empty() ? activation.activation_key : activation.display_name;
            activationRow.columns = QStringList{
                QStringLiteral("%1 [%2]").arg(qstr(label)).arg(qstr(activation.activation_key)),
                qstr(activation.unit_kind),
                qstr(activation.state),
            };
            activationRow.expanded = true;
            for (const auto& row : workflowJobSets_) {
                if (!row.step.workflow_unit_activation_id.has_value()
                    || *row.step.workflow_unit_activation_id != activation.workflow_unit_activation_id) {
                    continue;
                }
                activationRow.children.push_back(makeWorkflowJobSetDisplayRow(row));
            }
            rows.push_back(std::move(activationRow));
        }
    } else {
        for (const auto& row : workflowJobSets_) {
            rows.push_back(makeWorkflowJobSetDisplayRow(row));
        }
    }

    savorqt::gui::ApplyTreeRowsByKey(
        jobSetsTree_,
        currentJobSetRows_,
        rows,
        [](const TreeDisplayRow& row) { return row.key; },
        treeDisplayRowsEqual,
        populateTreeDisplayRow);
}

void WorkflowsPage::clearWorkflowDetail(const QString& message)
{
    detailHeaderLabel_->setText(message);
    detailMetaLabel_->clear();
    const std::vector<TreeDisplayRow> currentRows{ makeEmptyDisplayRow(QStringLiteral("clear-current"), message) };
    const std::vector<TreeDisplayRow> futureRows{ makeEmptyDisplayRow(QStringLiteral("clear-future"), QStringLiteral("-")) };
    const std::vector<TreeDisplayRow> pastRows{ makeEmptyDisplayRow(QStringLiteral("clear-past"), QStringLiteral("-")) };
    const std::vector<TreeDisplayRow> alertRows{ makeEmptyDisplayRow(QStringLiteral("clear-alerts"), QStringLiteral("-")) };
    savorqt::gui::ApplyTreeRowsByKey(
        currentStepsTree_,
        currentCurrentStepRows_,
        currentRows,
        [](const TreeDisplayRow& row) { return row.key; },
        treeDisplayRowsEqual,
        populateTreeDisplayRow);
    savorqt::gui::ApplyTreeRowsByKey(
        futureStepsTree_,
        currentFutureStepRows_,
        futureRows,
        [](const TreeDisplayRow& row) { return row.key; },
        treeDisplayRowsEqual,
        populateTreeDisplayRow);
    savorqt::gui::ApplyTreeRowsByKey(
        pastStepsTree_,
        currentPastStepRows_,
        pastRows,
        [](const TreeDisplayRow& row) { return row.key; },
        treeDisplayRowsEqual,
        populateTreeDisplayRow);
    savorqt::gui::ApplyTreeRowsByKey(
        alertsTree_,
        currentAlertRows_,
        alertRows,
        [](const TreeDisplayRow& row) { return row.key; },
        treeDisplayRowsEqual,
        populateTreeDisplayRow);
    clearWorkflowJobSets(QStringLiteral("-"));
}

void WorkflowsPage::clearWorkflowJobSets(const QString& message)
{
    workflowJobSets_.clear();
    const std::vector<TreeDisplayRow> rows{ makeEmptyDisplayRow(QStringLiteral("clear-job-sets"), message) };
    savorqt::gui::ApplyTreeRowsByKey(
        jobSetsTree_,
        currentJobSetRows_,
        rows,
        [](const TreeDisplayRow& row) { return row.key; },
        treeDisplayRowsEqual,
        populateTreeDisplayRow);
}

void WorkflowsPage::updateStatusWidgets()
{
    const bool busy = workflowFetchInFlight_ || detailFetchInFlight_ || jobSetsFetchInFlight_;
    summaryLabel_->setText(QStringLiteral("Workflows: %1 - page size: %2%3")
        .arg(workflowPage_.items.size())
        .arg(pageSizeSpin_->value())
        .arg(busy ? QStringLiteral(" - loading") : QString()));
    lastRefreshLabel_->setText(lastRefresh_.isValid()
        ? QStringLiteral("Last refresh: %1").arg(lastRefresh_.toString(QStringLiteral("hh:mm:ss AP")))
        : QStringLiteral("Last refresh: --"));

    applyButton_->setEnabled(!workflowFetchInFlight_);
    resetButton_->setEnabled(!workflowFetchInFlight_);
    refreshButton_->setEnabled(!workflowFetchInFlight_);
    stateFilter_->setEnabled(!workflowFetchInFlight_);
    kindFilter_->setEnabled(!workflowFetchInFlight_);
    pageSizeSpin_->setEnabled(!workflowFetchInFlight_);
    prevButton_->setEnabled(workflowPage_.prev.has_value() && !workflowFetchInFlight_);
    nextButton_->setEnabled(workflowPage_.next.has_value() && !workflowFetchInFlight_);

    if (!errorMessage_.isEmpty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("error"));
        inlineMessageLabel_->setText(errorMessage_);
        inlineMessageLabel_->show();
        postStatusMessage(errorMessage_, StatusToast::Severity::Error);
    } else if (!infoMessage_.isEmpty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info"));
        inlineMessageLabel_->setText(infoMessage_);
        inlineMessageLabel_->show();
        postStatusMessage(infoMessage_, StatusToast::Severity::Info);
    } else if (workflowFetchInFlight_ && workflowPage_.items.empty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info"));
        inlineMessageLabel_->setText(QStringLiteral("Loading workflows..."));
        inlineMessageLabel_->show();
    } else if (workflowPage_.items.empty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info"));
        inlineMessageLabel_->setText(QStringLiteral("No workflows matched the current filters."));
        inlineMessageLabel_->show();
    } else {
        inlineMessageLabel_->hide();
    }

    style()->unpolish(inlineMessageLabel_);
    style()->polish(inlineMessageLabel_);
}

void WorkflowsPage::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    if (text.isEmpty()) {
        return;
    }
    const QString signature = QStringLiteral("%1|%2").arg(static_cast<int>(severity)).arg(text);
    if (signature == lastToastSignature_) {
        return;
    }
    lastToastSignature_ = signature;
    emit statusToastRequested(StatusToast{ severity, text, QString(), 1, QDateTime{}, 4000 });
}
