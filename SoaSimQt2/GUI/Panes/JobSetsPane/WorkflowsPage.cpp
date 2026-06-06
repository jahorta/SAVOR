#include "WorkflowsPage.h"

#include "JobSetsPage.h"
#include "GUI/Widgets/ScrollBarStabilizer.h"

#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
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

bool isFutureStepState(const simcore::db::UiWorkflowStepSummary& step)
{
    return step.state == "WAITING" && step.blocked_reason.empty();
}

bool isCurrentStepState(const simcore::db::UiWorkflowStepSummary& step)
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

QString stepProgressText(const simcore::db::UiWorkflowStepSummary& step)
{
    return progressText(step.job_completed_count, step.job_count, step.job_failed_count);
}

QString activeStepText(const simcore::db::UiWorkflowDetail& detail)
{
    const simcore::db::UiWorkflowStepSummary* fallback = nullptr;
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

void configureStepTree(QTreeWidget* tree)
{
    const auto headers = stepHeaders();
    QStringList labels;
    for (const char* header : headers) {
        labels.push_back(QString::fromLatin1(header));
    }
    tree->setColumnCount(labels.size());
    tree->setHeaderLabels(labels);
    tree->setRootIsDecorated(false);
    tree->setAlternatingRowColors(true);
    tree->setUniformRowHeights(true);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(6, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(7, QHeaderView::ResizeToContents);
}

void addStepRow(QTreeWidget* tree, const simcore::db::UiWorkflowStepSummary& step)
{
    auto* item = new QTreeWidgetItem(tree);
    item->setText(0, qstr(step.step_key));
    item->setText(1, qstr(step.step_kind));
    item->setText(2, qstr(step.state));
    item->setText(3, formatOptionalId(step.job_set_id));
    item->setText(4, stepProgressText(step));
    item->setText(5, QStringLiteral("%1/%2").arg(step.attempts).arg(step.max_attempts));
    item->setText(6, qstr(step.blocked_reason));
    item->setText(7, formatOptionalTime(step.started_at_utc));
}

void setEmptyStepRow(QTreeWidget* tree, const QString& text)
{
    tree->clear();
    auto* item = new QTreeWidgetItem(tree);
    item->setText(0, text);
    item->setFirstColumnSpanned(true);
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
        syncJobSetsActiveState();
        return;
    }

    pageActive_ = active;
    if (!pageActive_) {
        if (refreshTimer_ != nullptr) {
            refreshTimer_->stop();
        }
        syncJobSetsActiveState();
        return;
    }

    if (refreshTimer_ != nullptr && autoRefreshCheck_->isChecked()) {
        refreshTimer_->start(refreshSecondsSpin_->value() * 1000);
    }
    refreshWorkflows();
    syncJobSetsActiveState();
}

void WorkflowsPage::createWidgets()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(10);

    rootTabs_ = new QTabWidget(this);
    overviewTab_ = new QWidget(rootTabs_);
    rootTabs_->addTab(overviewTab_, QStringLiteral("Workflows"));

    auto* overviewLayout = new QVBoxLayout(overviewTab_);
    overviewLayout->setContentsMargins(0, 0, 0, 0);
    overviewLayout->setSpacing(10);

    auto* toolbarPanel = new QFrame(overviewTab_);
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
    toolbarLayout->addWidget(autoRefreshCheck_, 0, 8, 1, 2, Qt::AlignBottom);
    toolbarLayout->addWidget(refreshSecondsSpin_, 1, 8);
    toolbarLayout->setColumnStretch(1, 1);

    overviewLayout->addWidget(toolbarPanel);

    auto* statusPanel = new QFrame(overviewTab_);
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
    overviewLayout->addWidget(statusPanel);

    auto* splitter = new QSplitter(Qt::Vertical, overviewTab_);
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
    workflowTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    workflowTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    workflowTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    workflowTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    workflowTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    workflowTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    workflowTable_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    workflowTable_->horizontalHeader()->setSectionResizeMode(7, QHeaderView::ResizeToContents);
    workflowTable_->horizontalHeader()->setSectionResizeMode(8, QHeaderView::ResizeToContents);
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
    alertsTree_ = new QTreeWidget(detailTabs);
    configureStepTree(currentStepsTree_);
    configureStepTree(futureStepsTree_);
    configureStepTree(pastStepsTree_);
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
    alertsTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    alertsTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    alertsTree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    alertsTree_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    alertsTree_->header()->setSectionResizeMode(4, QHeaderView::Stretch);
    alertsTree_->header()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    detailTabs->addTab(currentStepsTree_, QStringLiteral("Current"));
    detailTabs->addTab(futureStepsTree_, QStringLiteral("Future"));
    detailTabs->addTab(pastStepsTree_, QStringLiteral("Past"));
    detailTabs->addTab(alertsTree_, QStringLiteral("Alerts"));
    detailLayout->addWidget(detailTabs, 1);
    splitter->addWidget(detailPanel);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    overviewLayout->addWidget(splitter, 1);

    jobSetsPage_ = new JobSetsPage(rootTabs_);
    rootTabs_->addTab(jobSetsPage_, QStringLiteral("Job Sets"));

    rootLayout->addWidget(rootTabs_, 1);

    refreshTimer_ = new QTimer(this);
}

void WorkflowsPage::wireSignals()
{
    connect(applyButton_, &QPushButton::clicked, this, &WorkflowsPage::applyFilters);
    connect(resetButton_, &QPushButton::clicked, this, [this]() {
        stateFilter_->setCurrentIndex(0);
        kindFilter_->clear();
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
        if (enabled && pageActive_ && rootTabs_->currentIndex() == 0) {
            refreshTimer_->start(refreshSecondsSpin_->value() * 1000);
        } else {
            refreshTimer_->stop();
        }
    });
    connect(refreshSecondsSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int seconds) {
        if (pageActive_ && autoRefreshCheck_->isChecked() && rootTabs_->currentIndex() == 0) {
            refreshTimer_->start(seconds * 1000);
        }
    });
    connect(refreshTimer_, &QTimer::timeout, this, [this]() {
        if (pageActive_ && autoRefreshCheck_->isChecked() && rootTabs_->currentIndex() == 0
            && !workflowFetchInFlight_ && !detailFetchInFlight_ && !before_.has_value() && !after_.has_value()) {
            refreshWorkflows();
        }
    });
    connect(rootTabs_, &QTabWidget::currentChanged, this, &WorkflowsPage::handleRootTabChanged);
    connect(jobSetsPage_, &JobSetsPage::statusToastRequested, this, &WorkflowsPage::statusToastRequested);
    connect(workflowTable_, &QTableWidget::itemSelectionChanged, this, &WorkflowsPage::handleWorkflowSelectionChanged);

    connect(&workflowWatcher_, &QFutureWatcher<WorkflowPageResult>::finished, this, [this]() {
        const bool refetch = pendingWorkflowRefresh_;
        pendingWorkflowRefresh_ = false;
        workflowFetchInFlight_ = false;
        try {
            const auto result = workflowWatcher_.result();
            if (result.ok) {
                workflowPage_ = result.value;
                lastRefresh_ = QDateTime::currentDateTime();
                errorMessage_.clear();
                infoMessage_.clear();
                updateWorkflowTable();
            } else {
                workflowPage_ = {};
                errorMessage_ = QStringLiteral("Workflows failed: %1").arg(qstr(result.error.message));
                updateWorkflowTable();
            }
        } catch (const std::exception& ex) {
            workflowPage_ = {};
            errorMessage_ = QStringLiteral("Workflows failed: %1").arg(QString::fromUtf8(ex.what()));
            updateWorkflowTable();
        } catch (...) {
            workflowPage_ = {};
            errorMessage_ = QStringLiteral("Workflows failed: unknown exception");
            updateWorkflowTable();
        }
        updateStatusWidgets();
        if (refetch) {
            refreshWorkflows();
        }
    });

    connect(&detailWatcher_, &QFutureWatcher<WorkflowDetailResult>::finished, this, [this]() {
        detailFetchInFlight_ = false;
        try {
            const auto result = detailWatcher_.result();
            if (result.ok && result.value.instance.workflow_instance_id == selectedWorkflowInstanceId_) {
                selectedWorkflowDetail_ = result.value;
                updateWorkflowDetail();
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
}

void WorkflowsPage::refreshWorkflows()
{
    if (workflowFetchInFlight_) {
        pendingWorkflowRefresh_ = true;
        return;
    }

    workflowFetchInFlight_ = true;
    errorMessage_.clear();

    soasimqt2::db::WorkflowListRequest request{};
    request.state = stateFilter_->currentData().toString().trimmed().toStdString();
    request.workflow_kind = kindFilter_->text().trimmed().toStdString();
    request.before = before_;
    request.after = after_;
    request.limit = pageSizeSpin_->value();

    workflowWatcher_.setFuture(QtConcurrent::run([request]() {
        return soasimqt2::db::SimCoreDbWorkflowService::ListWorkflowInstances(request);
    }));
    updateStatusWidgets();
}

void WorkflowsPage::fetchWorkflowDetail(std::int64_t workflowInstanceId)
{
    if (workflowInstanceId <= 0) {
        clearWorkflowDetail(QStringLiteral("Select a workflow"));
        return;
    }

    detailFetchInFlight_ = true;
    detailWatcher_.setFuture(QtConcurrent::run([workflowInstanceId]() {
        return soasimqt2::db::SimCoreDbWorkflowService::GetWorkflowDetail(workflowInstanceId);
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
    clearWorkflowDetail(QStringLiteral("Loading workflow detail..."));
    fetchWorkflowDetail(workflowId);
}

void WorkflowsPage::handleRootTabChanged(int index)
{
    syncJobSetsActiveState();
    if (refreshTimer_ == nullptr) {
        return;
    }
    if (pageActive_ && index == 0 && autoRefreshCheck_->isChecked()) {
        refreshTimer_->start(refreshSecondsSpin_->value() * 1000);
    } else {
        refreshTimer_->stop();
    }
}

void WorkflowsPage::syncJobSetsActiveState()
{
    if (jobSetsPage_ != nullptr) {
        jobSetsPage_->setPageActive(pageActive_ && rootTabs_ != nullptr && rootTabs_->currentIndex() == 1);
    }
}

void WorkflowsPage::updateWorkflowTable()
{
    const ItemViewScrollSnapshot scrollSnapshot = captureItemViewScrollSnapshot(workflowTable_);
    const std::int64_t priorSelection = selectedWorkflowInstanceId_;
    std::int64_t targetSelection = 0;

    {
        const QSignalBlocker blocker(workflowTable_);
        workflowTable_->setRowCount(static_cast<int>(workflowPage_.items.size()));
        int selectedRow = -1;
        for (int row = 0; row < static_cast<int>(workflowPage_.items.size()); ++row) {
            const auto& workflow = workflowPage_.items[static_cast<std::size_t>(row)];

            auto* idItem = new QTableWidgetItem(QString::number(static_cast<qint64>(workflow.workflow_instance_id)));
            idItem->setData(kWorkflowIdRole, static_cast<qint64>(workflow.workflow_instance_id));
            workflowTable_->setItem(row, 0, idItem);
            workflowTable_->setItem(row, 1, new QTableWidgetItem(qstr(workflow.workflow_kind)));
            workflowTable_->setItem(row, 2, new QTableWidgetItem(qstr(workflow.state)));
            workflowTable_->setItem(row, 3, new QTableWidgetItem(QStringLiteral("-")));
            workflowTable_->setItem(row, 4, new QTableWidgetItem(QStringLiteral("-")));
            workflowTable_->setItem(row, 5, new QTableWidgetItem(QStringLiteral("-")));
            workflowTable_->setItem(row, 6, new QTableWidgetItem(QStringLiteral("blocked %1 / failed %2")
                .arg(workflow.blocked_step_count)
                .arg(workflow.failed_step_count)));
            workflowTable_->setItem(row, 7, new QTableWidgetItem(formatTime(workflow.created_at_utc)));
            workflowTable_->setItem(row, 8, new QTableWidgetItem(formatOptionalTime(workflow.completed_at_utc)));

            if (workflow.workflow_instance_id == priorSelection) {
                selectedRow = row;
            }
        }

        if (selectedRow >= 0) {
            workflowTable_->selectRow(selectedRow);
            targetSelection = priorSelection;
        } else if (!workflowPage_.items.empty()) {
            workflowTable_->selectRow(0);
            targetSelection = workflowPage_.items.front().workflow_instance_id;
        } else {
            selectedWorkflowInstanceId_ = 0;
            selectedWorkflowDetail_.reset();
            clearWorkflowDetail(QStringLiteral("No workflow selected"));
        }
    }

    restoreItemViewScrollSnapshot(workflowTable_, scrollSnapshot);
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

    currentStepsTree_->clear();
    futureStepsTree_->clear();
    pastStepsTree_->clear();
    alertsTree_->clear();

    int currentCount = 0;
    int futureCount = 0;
    int pastCount = 0;
    std::int64_t totalJobs = 0;
    std::int64_t completedJobs = 0;
    std::int64_t failedJobs = 0;
    for (const auto& step : detail.steps) {
        totalJobs += step.job_count;
        completedJobs += step.job_completed_count;
        failedJobs += step.job_failed_count;
        if (isTerminalStepState(step.state)) {
            addStepRow(pastStepsTree_, step);
            ++pastCount;
        } else if (isFutureStepState(step)) {
            addStepRow(futureStepsTree_, step);
            ++futureCount;
        } else {
            addStepRow(currentStepsTree_, step);
            ++currentCount;
        }
    }

    if (currentCount == 0) {
        setEmptyStepRow(currentStepsTree_, QStringLiteral("No current steps."));
    }
    if (futureCount == 0) {
        setEmptyStepRow(futureStepsTree_, QStringLiteral("No future steps."));
    }
    if (pastCount == 0) {
        setEmptyStepRow(pastStepsTree_, QStringLiteral("No past steps."));
    }

    if (detail.alerts.empty()) {
        auto* item = new QTreeWidgetItem(alertsTree_);
        item->setText(0, QStringLiteral("No alerts."));
        item->setFirstColumnSpanned(true);
    } else {
        for (const auto& alert : detail.alerts) {
            auto* item = new QTreeWidgetItem(alertsTree_);
            item->setText(0, qstr(alert.alert_kind));
            item->setText(1, qstr(alert.alert_code));
            item->setText(2, formatOptionalId(alert.workflow_step_id));
            item->setText(3, alert.is_active ? QStringLiteral("yes") : QStringLiteral("no"));
            item->setText(4, qstr(alert.message));
            item->setText(5, formatTime(alert.last_seen_at_utc));
        }
    }

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

void WorkflowsPage::clearWorkflowDetail(const QString& message)
{
    detailHeaderLabel_->setText(message);
    detailMetaLabel_->clear();
    setEmptyStepRow(currentStepsTree_, message);
    setEmptyStepRow(futureStepsTree_, QStringLiteral("-"));
    setEmptyStepRow(pastStepsTree_, QStringLiteral("-"));
    alertsTree_->clear();
}

void WorkflowsPage::updateStatusWidgets()
{
    const bool busy = workflowFetchInFlight_ || detailFetchInFlight_;
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
