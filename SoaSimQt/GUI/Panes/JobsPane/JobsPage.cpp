#include "JobsPage.h"

#include "ArtifactsTableModel.h"
#include "ArtifactsTableView.h"
#include "GUI/Widgets/ScrollBarStabilizer.h"
#include "JobsController.h"
#include "JobsTableModel.h"
#include "JobsTableView.h"

#include <QtCore/QDateTime>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtCore/QItemSelectionModel>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtCore/QDateTime>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace {
void selectFlatRow(QAbstractItemView* view, int row, bool ensureVisible = true)
{
    if (!view || !view->model()) {
        return;
    }

    const QModelIndex index = view->model()->index(row, 0);
    if (!index.isValid() || !view->selectionModel()) {
        return;
    }

    view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    if (ensureVisible) {
        view->scrollTo(index);
    }
}

}

JobsPage::JobsPage(QWidget* parent)
    : QWidget(parent)
    , controller_(new JobsController(this))
{
    createWidgets();
    wireSignals();
    syncControlsFromController(true);
    controller_->loadInitial();
}

void JobsPage::createWidgets()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(10);

    QFrame* filterPanel = new QFrame(this);
    filterPanel->setObjectName("jobsToolbarPanel");
    QGridLayout* filterLayout = new QGridLayout(filterPanel);
    filterLayout->setContentsMargins(12, 10, 12, 10);
    filterLayout->setHorizontalSpacing(10);
    filterLayout->setVerticalSpacing(10);

    kindFilter_ = new QComboBox(filterPanel);
    stateFilter_ = new QComboBox(filterPanel);
    jobSetFilter_ = new QLineEdit(filterPanel);
    pageSizeSpin_ = new QSpinBox(filterPanel);
    applyButton_ = new QPushButton(QStringLiteral("Apply"), filterPanel);
    resetButton_ = new QPushButton(QStringLiteral("Reset"), filterPanel);
    autoRefreshCheck_ = new QCheckBox(QStringLiteral("Auto refresh"), filterPanel);
    refreshSecondsSpin_ = new QSpinBox(filterPanel);

    kindFilter_->setObjectName("jobsFilterCombo");
    stateFilter_->setObjectName("jobsFilterCombo");
    jobSetFilter_->setObjectName("jobsFilterEdit");
    pageSizeSpin_->setObjectName("jobsRefreshSpin");
    applyButton_->setObjectName("jobsPrimaryButton");
    resetButton_->setObjectName("jobsSecondaryButton");
    autoRefreshCheck_->setObjectName("jobsCheckBox");
    refreshSecondsSpin_->setObjectName("jobsRefreshSpin");

    pageSizeSpin_->setRange(10, 500); pageSizeSpin_->setSingleStep(10);
    refreshSecondsSpin_->setRange(1, 5); refreshSecondsSpin_->setSuffix(QStringLiteral(" s"));
    jobSetFilter_->setPlaceholderText(QStringLiteral("job_set_id"));

    kindFilter_->addItem(QStringLiteral("All kinds"), QVariant());
    stateFilter_->addItem(QStringLiteral("All states"), QVariant());
    for (const QString& state : QStringList{QStringLiteral("QUEUED"), QStringLiteral("CLAIMED"), QStringLiteral("RUNNING"), QStringLiteral("INTERRUPTED"), QStringLiteral("SUCCEEDED"), QStringLiteral("FAILED"), QStringLiteral("CANCELED"), QStringLiteral("SUPERSEDED"), QStringLiteral("SUCCEEDED_WINNER"), QStringLiteral("SUCCEEDED_DUPLICATE")}) {
        stateFilter_->addItem(state, state);
    }

    filterLayout->addWidget(new QLabel(QStringLiteral("Kind"), filterPanel), 0, 0);
    filterLayout->addWidget(kindFilter_, 1, 0);
    filterLayout->addWidget(new QLabel(QStringLiteral("State"), filterPanel), 0, 1);
    filterLayout->addWidget(stateFilter_, 1, 1);
    filterLayout->addWidget(new QLabel(QStringLiteral("Job Set"), filterPanel), 0, 2);
    filterLayout->addWidget(jobSetFilter_, 1, 2);
    filterLayout->addWidget(new QLabel(QStringLiteral("Page size"), filterPanel), 0, 3);
    filterLayout->addWidget(pageSizeSpin_, 1, 3);
    filterLayout->addWidget(applyButton_, 1, 4);
    filterLayout->addWidget(resetButton_, 1, 5);
    filterLayout->addWidget(autoRefreshCheck_, 0, 6, 1, 2, Qt::AlignBottom);
    filterLayout->addWidget(refreshSecondsSpin_, 1, 6);
    filterLayout->addWidget(new QLabel(QStringLiteral("Interval"), filterPanel), 1, 7);
    filterLayout->setColumnStretch(2, 1);
    rootLayout->addWidget(filterPanel);

    QFrame* contentPanel = new QFrame(this);
    contentPanel->setObjectName("jobsContentPanel");
    QVBoxLayout* contentLayout = new QVBoxLayout(contentPanel);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(10);

    QFrame* pageControls = new QFrame(contentPanel);
    pageControls->setObjectName("jobsPagingPanel");
    QHBoxLayout* pageLayout = new QHBoxLayout(pageControls);
    pageLayout->setContentsMargins(12, 10, 12, 10);
    prevButton_ = new QPushButton(QStringLiteral("Prev"), pageControls);
    nextButton_ = new QPushButton(QStringLiteral("Next"), pageControls);
    refreshButton_ = new QPushButton(QStringLiteral("Refresh now"), pageControls);
    pageSummaryLabel_ = new QLabel(pageControls);
    lastRefreshLabel_ = new QLabel(pageControls);
    pageStatusLabel_ = new QLabel(pageControls);
    prevButton_->setObjectName("jobsSecondaryButton");
    nextButton_->setObjectName("jobsSecondaryButton");
    refreshButton_->setObjectName("jobsSecondaryButton");
    pageSummaryLabel_->setObjectName("jobsMetaText");
    lastRefreshLabel_->setObjectName("jobsMetaText");
    pageStatusLabel_->setObjectName("jobsMetaText");
    pageStatusLabel_->hide();
    pageLayout->addWidget(prevButton_); pageLayout->addWidget(nextButton_); pageLayout->addWidget(refreshButton_); pageLayout->addSpacing(8); pageLayout->addWidget(pageSummaryLabel_); pageLayout->addStretch(); pageLayout->addWidget(pageStatusLabel_); pageLayout->addSpacing(10); pageLayout->addWidget(lastRefreshLabel_);
    contentLayout->addWidget(pageControls);

    QSplitter* splitter = new QSplitter(Qt::Horizontal, contentPanel);
    splitter->setChildrenCollapsible(false);

    QFrame* tablePanel = new QFrame(splitter);
    tablePanel->setObjectName("jobsSurfacePanel");
    QVBoxLayout* tableLayout = new QVBoxLayout(tablePanel);
    tableLayout->setContentsMargins(12, 12, 12, 12);
    tableLayout->addWidget(new QLabel(QStringLiteral("Jobs Table"), tablePanel));
    jobsTable_ = new JobsTableView(tablePanel);
    jobsModel_ = new JobsTableModel(jobsTable_);
    jobsTable_->attachModel(jobsModel_);
    tableLayout->addWidget(jobsTable_, 1);

    QFrame* inspectorPanel = new QFrame(splitter);
    inspectorPanel->setObjectName("jobsSurfacePanel");
    QVBoxLayout* inspectorLayout = new QVBoxLayout(inspectorPanel);
    inspectorLayout->setContentsMargins(12, 12, 12, 12);
    inspectorSummary_ = new QLabel(QStringLiteral("Select a job to inspect details."), inspectorPanel);
    inspectorSummary_->setObjectName("jobsInspectorSummary");
    inspectorSummary_->setWordWrap(true);
    inspectorLayout->addWidget(new QLabel(QStringLiteral("Job Inspector"), inspectorPanel));
    inspectorLayout->addWidget(inspectorSummary_);

    QLabel* inspectorActionsHint = new QLabel(QStringLiteral("Right-click a job in the table to refresh details or run job actions."), inspectorPanel);
    inspectorActionsHint->setObjectName("jobsMetaText");
    inspectorActionsHint->setWordWrap(true);
    inspectorLayout->addWidget(inspectorActionsHint);

    inspectorTabs_ = new QTabWidget(inspectorPanel);
    QWidget* overviewTab = new QWidget(inspectorTabs_);
    QVBoxLayout* overviewLayout = new QVBoxLayout(overviewTab);
    QGridLayout* overviewGrid = new QGridLayout();
    overviewPriorityValue_ = new QLabel(QStringLiteral("--"), overviewTab);
    overviewQueuedValue_ = new QLabel(QStringLiteral("--"), overviewTab);
    overviewSelectionHint_ = new QLabel(QStringLiteral("Choose a row from the table to populate the inspector."), overviewTab);
    overviewSelectionHint_->setWordWrap(true);
    overviewGrid->addWidget(new QLabel(QStringLiteral("Priority"), overviewTab), 0, 0); overviewGrid->addWidget(overviewPriorityValue_, 0, 1); overviewGrid->addWidget(new QLabel(QStringLiteral("Queued At"), overviewTab), 1, 0); overviewGrid->addWidget(overviewQueuedValue_, 1, 1);
    overviewLayout->addLayout(overviewGrid); overviewLayout->addWidget(overviewSelectionHint_); overviewLayout->addStretch();

    eventsText_ = createReadOnlyTextEdit();
    payloadText_ = createReadOnlyTextEdit();
    progressText_ = createReadOnlyTextEdit();
    resultsText_ = createReadOnlyTextEdit();
    QWidget* artifactsTab = new QWidget(inspectorTabs_);
    QVBoxLayout* artifactsLayout = new QVBoxLayout(artifactsTab);
    artifactsTable_ = new ArtifactsTableView(artifactsTab);
    artifactsModel_ = new ArtifactsTableModel(artifactsTable_);
    artifactsTable_->attachModel(artifactsModel_);
    artifactsLayout->addWidget(artifactsTable_);
    inspectorTabs_->addTab(overviewTab, QStringLiteral("Overview"));
    inspectorTabs_->addTab(eventsText_, QStringLiteral("Events"));
    inspectorTabs_->addTab(payloadText_, QStringLiteral("Payload"));
    inspectorTabs_->addTab(artifactsTab, QStringLiteral("Artifacts"));
    inspectorTabs_->addTab(progressText_, QStringLiteral("Progress"));
    inspectorTabs_->addTab(resultsText_, QStringLiteral("Results"));
    inspectorLayout->addWidget(inspectorTabs_, 1);

    splitter->addWidget(tablePanel); splitter->addWidget(inspectorPanel); splitter->setStretchFactor(0, 2); splitter->setStretchFactor(1, 1);
    contentLayout->addWidget(splitter, 1);

    inlineMessageLabel_ = new QLabel(contentPanel);
    inlineMessageLabel_->setObjectName("jobSetsInlineMessage");
    inlineMessageLabel_->setWordWrap(true);
    contentLayout->addWidget(inlineMessageLabel_);

    loadingStateTimer_ = new QTimer(this);
    loadingStateTimer_->setSingleShot(true);

    rootLayout->addWidget(contentPanel, 1);
}

QTextEdit* JobsPage::createReadOnlyTextEdit()
{
    QTextEdit* edit = new QTextEdit(this);
    edit->setObjectName("jobsInspectorText");
    edit->setReadOnly(true);
    return edit;
}

void JobsPage::wireSignals()
{
    connect(controller_, &JobsController::stateChanged, this, [this]() {
        syncControlsFromController(false);
        refreshModel();
        updateInspector();
        updateStatusWidgets();
    });
    connect(applyButton_, &QPushButton::clicked, this, [this]() {
        controller_->applyFilters(selectedProgramKind(), selectedState(), selectedJobSetId(), pageSizeSpin_->value());
    });
    connect(resetButton_, &QPushButton::clicked, this, [this]() {
        controller_->resetFilters();
        syncControlsFromController(true);
    });
    connect(refreshButton_, &QPushButton::clicked, controller_, &JobsController::requestRefresh);
    connect(prevButton_, &QPushButton::clicked, controller_, &JobsController::requestPreviousPage);
    connect(nextButton_, &QPushButton::clicked, controller_, &JobsController::requestNextPage);
    connect(autoRefreshCheck_, &QCheckBox::toggled, controller_, &JobsController::setAutoRefreshEnabled);
    connect(refreshSecondsSpin_, qOverload<int>(&QSpinBox::valueChanged), controller_, &JobsController::setRefreshSeconds);
    connect(jobsTable_, &QWidget::customContextMenuRequested, this, &JobsPage::showJobsContextMenu);

    connect(jobsTable_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this](const QModelIndex& current, const QModelIndex&) {
        if (!current.isValid()) return;
        if (const JobsTableModel::Row* row = jobsModel_->rowAt(current.row())) controller_->selectJob(row->jobId);
    });

    connect(jobsTable_, &JobsTableView::doubleClicked, this, [this](const QModelIndex& current) {
        if (current.isValid()) {
            selectFlatRow(jobsTable_, current.row());
            inspectorTabs_->setCurrentIndex(0);
        }
    });
    connect(loadingStateTimer_, &QTimer::timeout, this, [this]() {
        if (controller_->viewState().loading && !controller_->viewState().page.items.empty()) {
            delayedLoadingVisible_ = true;
            updateStatusWidgets();
        }
    });
}

void JobsPage::showJobsContextMenu(const QPoint& position)
{
    const QModelIndex index = jobsTable_->indexAt(position);
    if (!index.isValid()) {
        return;
    }

    selectFlatRow(jobsTable_, index.row());
    const JobsTableModel::Row* row = jobsModel_->rowAt(index.row());
    if (!row) {
        return;
    }

    const bool actionsEnabled = !controller_->viewState().actionsBusy;
    const bool isFinished = row->state == QStringLiteral("SUCCEEDED")
        || row->state == QStringLiteral("FAILED")
        || row->state == QStringLiteral("CANCELED")
        || row->state == QStringLiteral("SUPERSEDED")
        || row->state == QStringLiteral("SUCCEEDED_WINNER")
        || row->state == QStringLiteral("SUCCEEDED_DUPLICATE");
    const bool canReplayVisual = actionsEnabled && isFinished;
    const bool canRequeue = actionsEnabled && row->state != QStringLiteral("QUEUED") && row->state != QStringLiteral("CLAIMED") && row->state != QStringLiteral("RUNNING") && row->state != QStringLiteral("FAILED");
    const bool canRestart = actionsEnabled && row->state == QStringLiteral("FAILED");
    const bool canCancel = actionsEnabled && row->state != QStringLiteral("SUCCEEDED") && row->state != QStringLiteral("CANCELED") && row->state != QStringLiteral("SUCCEEDED_WINNER") && row->state != QStringLiteral("SUCCEEDED_DUPLICATE");

    QMenu menu(jobsTable_);
    QAction* refreshDetailAction = menu.addAction(QStringLiteral("Refresh detail"));
    menu.addSeparator();
    QAction* replayVisualAction = menu.addAction(QStringLiteral("Replay Visually"));
    menu.addSeparator();
    QAction* requeueAction = menu.addAction(QStringLiteral("Requeue"));
    QAction* restartAction = menu.addAction(QStringLiteral("Edit INI + Restart"));
    QAction* cancelAction = menu.addAction(QStringLiteral("Cancel"));

    refreshDetailAction->setEnabled(actionsEnabled);
    replayVisualAction->setEnabled(canReplayVisual);
    requeueAction->setEnabled(canRequeue);
    restartAction->setEnabled(canRestart);
    cancelAction->setEnabled(canCancel);

    QAction* chosen = menu.exec(jobsTable_->viewport()->mapToGlobal(position));
    if (chosen == refreshDetailAction) {
        controller_->refreshSelectedJobDetail();
    } else if (chosen == replayVisualAction) {
        emit visualReplayRequested(row->jobId);
    } else if (chosen == requeueAction) {
        controller_->requeueSelectedJob();
    } else if (chosen == restartAction) {
        handleRestartRequested();
    } else if (chosen == cancelAction) {
        controller_->cancelSelectedJob();
    }
}

void JobsPage::syncControlsFromController(bool syncAll)
{
    const auto& state = controller_->viewState();
    {
        QSignalBlocker blocker(kindFilter_);
        QVariant targetData = kindFilter_->currentData();
        if (syncAll || !targetData.isValid()) {
            targetData = state.scope.program_kind.has_value() ? QVariant(*state.scope.program_kind) : QVariant();
        }

        kindFilter_->clear();
        kindFilter_->addItem(QStringLiteral("All kinds"), QVariant());
        QList<int> ids = state.programNames.keys();
        std::sort(ids.begin(), ids.end());
        for (int id : ids) {
            kindFilter_->addItem(state.programNames.value(id), id);
        }
        const int idx = targetData.isValid() ? kindFilter_->findData(targetData) : 0;
        kindFilter_->setCurrentIndex(idx >= 0 ? idx : 0);
    }

    if (syncAll) {
        {
            QSignalBlocker blocker(stateFilter_);
            const QVariant target = !state.scope.states.empty() ? QVariant(QString::fromStdString(state.scope.states.front())) : QVariant();
            const int idx = target.isValid() ? stateFilter_->findData(target) : 0;
            stateFilter_->setCurrentIndex(idx >= 0 ? idx : 0);
        }
        { QSignalBlocker blocker(jobSetFilter_); jobSetFilter_->setText(state.scope.job_set_id.has_value() ? QString::number(*state.scope.job_set_id) : QString()); }
        { QSignalBlocker blocker(pageSizeSpin_); pageSizeSpin_->setValue(state.pageLimit); }
        { QSignalBlocker blocker(autoRefreshCheck_); autoRefreshCheck_->setChecked(state.autoRefresh); }
        { QSignalBlocker blocker(refreshSecondsSpin_); refreshSecondsSpin_->setValue(state.refreshSeconds); }
    }

    const bool enabled = !state.actionsBusy;
    prevButton_->setEnabled(state.page.prev.has_value() && enabled);
    nextButton_->setEnabled(state.page.next.has_value() && enabled);
    refreshButton_->setEnabled(enabled);
    applyButton_->setEnabled(enabled);
    resetButton_->setEnabled(enabled);
}

void JobsPage::refreshModel()
{
    const auto& state = controller_->viewState();
    const ItemViewScrollSnapshot scrollSnapshot = captureItemViewScrollSnapshot(jobsTable_);

    std::vector<JobsTableModel::Row> rows;
    rows.reserve(state.page.items.size());
    for (const JobLite& job : state.page.items) {
        rows.push_back(JobsTableModel::Row{
            job.job_id,
            job.job_set_id,
            job.savestate_id,
            state.programNames.value(job.program_kind, QStringLiteral("kind %1").arg(job.program_kind)),
            QString::fromStdString(job.state),
            job.attempts,
            QDateTime::fromSecsSinceEpoch(job.queued_at).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
            state.progressSummary.value(job.job_id, QStringLiteral("..."))
        });
    }
    jobsModel_->setRows(rows);
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        if (rows[row].jobId == state.selectedJobId) {
            selectFlatRow(jobsTable_, row, false);
            break;
        }
    }
    restoreItemViewScrollSnapshot(jobsTable_, scrollSnapshot);
}

void JobsPage::updateInspector()
{
    const auto& state = controller_->viewState();
    const JobLite* selected = nullptr;
    for (const JobLite& job : state.page.items) if (job.job_id == state.selectedJobId) { selected = &job; break; }
    if (!selected) {
        inspectorSummary_->setText(QStringLiteral("Select a job to inspect details."));
        overviewPriorityValue_->setText(QStringLiteral("--")); overviewQueuedValue_->setText(QStringLiteral("--")); overviewSelectionHint_->setText(QStringLiteral("No jobs match the current filters."));
        eventsText_->clear(); payloadText_->clear(); progressText_->clear(); resultsText_->clear(); artifactsModel_->setArtifacts({});
        return;
    }

    inspectorSummary_->setText(QStringLiteral("Job %1 | Set %2 | ProgramKind %3 | State %4")
        .arg(selected->job_id).arg(selected->job_set_id).arg(state.programNames.value(selected->program_kind, QStringLiteral("kind %1").arg(selected->program_kind))).arg(QString::fromStdString(selected->state)));
    overviewPriorityValue_->setText(QString::number(selected->priority));
    overviewQueuedValue_->setText(QDateTime::fromSecsSinceEpoch(selected->queued_at).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    overviewSelectionHint_->setText(QStringLiteral("Attempts: %1\nProgress: %2").arg(selected->attempts).arg(state.progressSummary.value(selected->job_id, QStringLiteral("..."))));

    const ItemViewScrollSnapshot artifactsScrollSnapshot = captureItemViewScrollSnapshot(artifactsTable_);
    const ScrollAreaScrollSnapshot eventsScrollSnapshot = captureScrollAreaScrollSnapshot(eventsText_);
    const ScrollAreaScrollSnapshot payloadScrollSnapshot = captureScrollAreaScrollSnapshot(payloadText_);
    const ScrollAreaScrollSnapshot progressScrollSnapshot = captureScrollAreaScrollSnapshot(progressText_);
    const ScrollAreaScrollSnapshot resultsScrollSnapshot = captureScrollAreaScrollSnapshot(resultsText_);

    QStringList eventLines;
    for (const JobEventLite& event : state.detail.events) {
        eventLines << QStringLiteral("%1  %2%3").arg(QDateTime::fromSecsSinceEpoch(event.ts).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))).arg(QString::fromStdString(event.event_kind)).arg(event.payload_preview.has_value() ? QStringLiteral("  %1").arg(QString::fromStdString(*event.payload_preview)) : QString());
    }
    eventsText_->setPlainText(eventLines.join('\n'));
    payloadText_->setPlainText(state.detail.payloadText);
    progressText_->setPlainText(state.detail.decodedProgressText);
    resultsText_->setPlainText(state.detail.resultsText);
    artifactsModel_->setArtifacts(state.detail.artifacts);
    restoreScrollAreaScrollSnapshot(eventsText_, eventsScrollSnapshot);
    restoreScrollAreaScrollSnapshot(payloadText_, payloadScrollSnapshot);
    restoreScrollAreaScrollSnapshot(progressText_, progressScrollSnapshot);
    restoreScrollAreaScrollSnapshot(resultsText_, resultsScrollSnapshot);
    restoreItemViewScrollSnapshot(artifactsTable_, artifactsScrollSnapshot);

}

void JobsPage::updateStatusWidgets()
{
    const auto& state = controller_->viewState();
    updateLoadingIndicatorState();
    StatusToast::Severity toastSeverity = StatusToast::Severity::Info;
    QString toastMessage;

    pageSummaryLabel_->setText(QStringLiteral("Rows: %1 • page size: %2").arg(state.page.items.size()).arg(state.pageLimit));
    lastRefreshLabel_->setText(state.lastRefresh.isValid() ? QStringLiteral("Last refresh: %1").arg(state.lastRefresh.toString(QStringLiteral("hh:mm:ss AP"))) : QStringLiteral("Last refresh: --"));
    if (state.loading && (state.page.items.empty() || delayedLoadingVisible_)) {
        pageStatusLabel_->setText(QStringLiteral("Loading jobs…"));
        pageStatusLabel_->show();
    } else {
        pageStatusLabel_->hide();
    }

    if (!state.errorMessage.isEmpty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("error")); inlineMessageLabel_->setText(state.errorMessage); inlineMessageLabel_->show();
        toastSeverity = StatusToast::Severity::Error;
        toastMessage = state.errorMessage;
    } else if (!state.infoMessage.isEmpty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info")); inlineMessageLabel_->setText(state.infoMessage); inlineMessageLabel_->show();
        toastMessage = state.infoMessage;
    } else if (state.loading && state.page.items.empty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info")); inlineMessageLabel_->setText(QStringLiteral("Loading jobs…")); inlineMessageLabel_->show();
    } else if (state.page.items.empty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info")); inlineMessageLabel_->setText(QStringLiteral("No jobs matched the current filters.")); inlineMessageLabel_->show();
    } else {
        inlineMessageLabel_->hide();
    }
    style()->unpolish(inlineMessageLabel_); style()->polish(inlineMessageLabel_);

    if (!toastMessage.isEmpty()) {
        const QString signature = QStringLiteral("%1|%2").arg(static_cast<int>(toastSeverity)).arg(toastMessage);
        if (signature != lastToastSignature_) {
            lastToastSignature_ = signature;
            emit statusToastRequested(StatusToast{ toastSeverity, toastMessage, QString(), 1, QDateTime{}, 4000 });
        }
    }
}

void JobsPage::updateLoadingIndicatorState()
{
    const auto& state = controller_->viewState();
    if (!state.loading) {
        delayedLoadingVisible_ = false;
        loadingStateTimer_->stop();
        return;
    }

    if (state.page.items.empty()) {
        delayedLoadingVisible_ = true;
        loadingStateTimer_->stop();
        return;
    }

    if (delayedLoadingVisible_ || loadingStateTimer_->isActive()) {
        return;
    }

    loadingStateTimer_->start(1000);
}

std::optional<int> JobsPage::selectedProgramKind() const { const QVariant data = kindFilter_->currentData(); return data.isValid() ? std::optional<int>(data.toInt()) : std::nullopt; }
std::optional<QString> JobsPage::selectedState() const { const QVariant data = stateFilter_->currentData(); return data.isValid() ? std::optional<QString>(data.toString()) : std::nullopt; }
std::optional<qint64> JobsPage::selectedJobSetId() const { bool ok = false; const qint64 value = jobSetFilter_->text().trimmed().toLongLong(&ok); return ok ? std::optional<qint64>(value) : std::nullopt; }

void JobsPage::handleRestartRequested()
{
    const auto& state = controller_->viewState();
    if (state.selectedJobId <= 0) return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Restart Failed Job"));
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("Restart will reset attempts to 0 and queue the failed job again. You can optionally edit the current job INI first."), &dialog));
    QPlainTextEdit* iniEdit = new QPlainTextEdit(&dialog);
    iniEdit->setPlainText(state.detail.payloadText);
    layout->addWidget(iniEdit, 1);
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Save INI & Restart"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) controller_->restartSelectedFailedJob(iniEdit->toPlainText());
}
