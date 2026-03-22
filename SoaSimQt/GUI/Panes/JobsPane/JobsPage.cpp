#include "JobsPage.h"

#include "ArtifactsTableModel.h"
#include "ArtifactsTableView.h"
#include "JobsController.h"
#include "JobsTableModel.h"
#include "JobsTableView.h"

#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
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
void selectFlatRow(QAbstractItemView* view, int row)
{
    if (!view || !view->model()) {
        return;
    }

    const QModelIndex index = view->model()->index(row, 0);
    if (!index.isValid() || !view->selectionModel()) {
        return;
    }

    view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    view->scrollTo(index);
}
}

JobsPage::JobsPage(QWidget* parent)
    : QWidget(parent)
    , controller_(new JobsController(this))
{
    createWidgets();
    wireSignals();
    controller_->loadInitial();
}

void JobsPage::createWidgets()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(12);

    QFrame* filterPanel = new QFrame(this);
    filterPanel->setObjectName("jobsToolbarPanel");
    QGridLayout* filterLayout = new QGridLayout(filterPanel);
    filterLayout->setContentsMargins(16, 14, 16, 14);
    filterLayout->setHorizontalSpacing(12);
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
    contentLayout->setSpacing(12);

    QFrame* pageControls = new QFrame(contentPanel);
    pageControls->setObjectName("jobsPagingPanel");
    QHBoxLayout* pageLayout = new QHBoxLayout(pageControls);
    pageLayout->setContentsMargins(16, 12, 16, 12);
    prevButton_ = new QPushButton(QStringLiteral("Prev"), pageControls);
    nextButton_ = new QPushButton(QStringLiteral("Next"), pageControls);
    refreshButton_ = new QPushButton(QStringLiteral("Refresh now"), pageControls);
    pageSummaryLabel_ = new QLabel(pageControls);
    lastRefreshLabel_ = new QLabel(pageControls);
    prevButton_->setObjectName("jobsSecondaryButton");
    nextButton_->setObjectName("jobsSecondaryButton");
    refreshButton_->setObjectName("jobsSecondaryButton");
    pageSummaryLabel_->setObjectName("jobsMetaText");
    lastRefreshLabel_->setObjectName("jobsMetaText");
    pageLayout->addWidget(prevButton_); pageLayout->addWidget(nextButton_); pageLayout->addWidget(refreshButton_); pageLayout->addSpacing(8); pageLayout->addWidget(pageSummaryLabel_); pageLayout->addStretch(); pageLayout->addWidget(lastRefreshLabel_);
    contentLayout->addWidget(pageControls);

    QSplitter* splitter = new QSplitter(Qt::Horizontal, contentPanel);
    splitter->setChildrenCollapsible(false);

    QFrame* tablePanel = new QFrame(splitter);
    tablePanel->setObjectName("jobsSurfacePanel");
    QVBoxLayout* tableLayout = new QVBoxLayout(tablePanel);
    tableLayout->setContentsMargins(16, 16, 16, 16);
    tableLayout->addWidget(new QLabel(QStringLiteral("Jobs Table"), tablePanel));
    jobsTable_ = new JobsTableView(tablePanel);
    jobsModel_ = new JobsTableModel(jobsTable_);
    jobsTable_->attachModel(jobsModel_);
    tableLayout->addWidget(jobsTable_, 1);

    QFrame* inspectorPanel = new QFrame(splitter);
    inspectorPanel->setObjectName("jobsSurfacePanel");
    QVBoxLayout* inspectorLayout = new QVBoxLayout(inspectorPanel);
    inspectorLayout->setContentsMargins(16, 16, 16, 16);
    inspectorSummary_ = new QLabel(QStringLiteral("Select a job to inspect details."), inspectorPanel);
    inspectorSummary_->setObjectName("jobsInspectorSummary");
    inspectorSummary_->setWordWrap(true);
    inspectorLayout->addWidget(new QLabel(QStringLiteral("Job Inspector"), inspectorPanel));
    inspectorLayout->addWidget(inspectorSummary_);

    QHBoxLayout* actionLayout = new QHBoxLayout();
    requeueButton_ = new QPushButton(QStringLiteral("Requeue"), inspectorPanel);
    restartButton_ = new QPushButton(QStringLiteral("Restart"), inspectorPanel);
    cancelButton_ = new QPushButton(QStringLiteral("Cancel"), inspectorPanel);
    bumpDeltaSpin_ = new QSpinBox(inspectorPanel);
    applyBumpButton_ = new QPushButton(QStringLiteral("Apply bump"), inspectorPanel);
    inspectorRefreshButton_ = new QPushButton(QStringLiteral("Refresh detail"), inspectorPanel);
    bumpDeltaSpin_->setRange(-9, 9); bumpDeltaSpin_->setValue(1);
    requeueButton_->setObjectName("jobsSecondaryButton"); restartButton_->setObjectName("jobsSecondaryButton"); cancelButton_->setObjectName("jobsSecondaryButton"); applyBumpButton_->setObjectName("jobsPrimaryButton"); inspectorRefreshButton_->setObjectName("jobsSecondaryButton");
    actionLayout->addWidget(requeueButton_); actionLayout->addWidget(restartButton_); actionLayout->addWidget(cancelButton_); actionLayout->addWidget(new QLabel(QStringLiteral("Delta"), inspectorPanel)); actionLayout->addWidget(bumpDeltaSpin_); actionLayout->addWidget(applyBumpButton_); actionLayout->addStretch(); actionLayout->addWidget(inspectorRefreshButton_);
    inspectorLayout->addLayout(actionLayout);

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

    splitter->addWidget(tablePanel); splitter->addWidget(inspectorPanel); splitter->setStretchFactor(0, 3); splitter->setStretchFactor(1, 2);
    contentLayout->addWidget(splitter, 1);

    inlineMessageLabel_ = new QLabel(contentPanel);
    inlineMessageLabel_->setObjectName("jobSetsInlineMessage");
    inlineMessageLabel_->setWordWrap(true);
    contentLayout->addWidget(inlineMessageLabel_);

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
        syncControlsFromController();
        refreshModel();
        updateInspector();
        updateStatusWidgets();
    });
    connect(applyButton_, &QPushButton::clicked, this, [this]() {
        controller_->applyFilters(selectedProgramKind(), selectedState(), selectedJobSetId(), pageSizeSpin_->value());
    });
    connect(resetButton_, &QPushButton::clicked, controller_, &JobsController::resetFilters);
    connect(refreshButton_, &QPushButton::clicked, controller_, &JobsController::requestRefresh);
    connect(prevButton_, &QPushButton::clicked, controller_, &JobsController::requestPreviousPage);
    connect(nextButton_, &QPushButton::clicked, controller_, &JobsController::requestNextPage);
    connect(autoRefreshCheck_, &QCheckBox::toggled, controller_, &JobsController::setAutoRefreshEnabled);
    connect(refreshSecondsSpin_, qOverload<int>(&QSpinBox::valueChanged), controller_, &JobsController::setRefreshSeconds);
    connect(requeueButton_, &QPushButton::clicked, controller_, &JobsController::requeueSelectedJob);
    connect(cancelButton_, &QPushButton::clicked, controller_, &JobsController::cancelSelectedJob);
    connect(applyBumpButton_, &QPushButton::clicked, this, [this]() { controller_->bumpSelectedJobPriority(bumpDeltaSpin_->value()); });
    connect(inspectorRefreshButton_, &QPushButton::clicked, controller_, &JobsController::refreshSelectedJobDetail);
    connect(restartButton_, &QPushButton::clicked, this, &JobsPage::handleRestartRequested);

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
}

void JobsPage::syncControlsFromController()
{
    const auto& state = controller_->viewState();
    {
        QSignalBlocker blocker(kindFilter_);
        const QVariant currentData = state.scope.program_kind.has_value() ? QVariant(*state.scope.program_kind) : QVariant();
        kindFilter_->clear(); kindFilter_->addItem(QStringLiteral("All kinds"), QVariant());
        QList<int> ids = state.programNames.keys(); std::sort(ids.begin(), ids.end());
        for (int id : ids) kindFilter_->addItem(state.programNames.value(id), id);
        const int idx = currentData.isValid() ? kindFilter_->findData(currentData) : 0;
        kindFilter_->setCurrentIndex(idx >= 0 ? idx : 0);
    }
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
            selectFlatRow(jobsTable_, row);
            break;
        }
    }
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
        requeueButton_->setEnabled(false); restartButton_->setEnabled(false); cancelButton_->setEnabled(false); applyBumpButton_->setEnabled(false); inspectorRefreshButton_->setEnabled(false);
        return;
    }

    inspectorSummary_->setText(QStringLiteral("Job %1 | Set %2 | ProgramKind %3 | State %4")
        .arg(selected->job_id).arg(selected->job_set_id).arg(state.programNames.value(selected->program_kind, QStringLiteral("kind %1").arg(selected->program_kind))).arg(QString::fromStdString(selected->state)));
    overviewPriorityValue_->setText(QString::number(selected->priority));
    overviewQueuedValue_->setText(QDateTime::fromSecsSinceEpoch(selected->queued_at).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    overviewSelectionHint_->setText(QStringLiteral("Attempts: %1\nProgress: %2").arg(selected->attempts).arg(state.progressSummary.value(selected->job_id, QStringLiteral("..."))));

    QStringList eventLines;
    for (const JobEventLite& event : state.detail.events) {
        eventLines << QStringLiteral("%1  %2%3").arg(QDateTime::fromSecsSinceEpoch(event.ts).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))).arg(QString::fromStdString(event.event_kind)).arg(event.payload_preview.has_value() ? QStringLiteral("  %1").arg(QString::fromStdString(*event.payload_preview)) : QString());
    }
    eventsText_->setPlainText(eventLines.join('\n'));
    payloadText_->setPlainText(state.detail.payloadText);
    progressText_->setPlainText(state.detail.decodedProgressText);
    resultsText_->setPlainText(state.detail.resultsText);
    artifactsModel_->setArtifacts(state.detail.artifacts);
    artifactsTable_->resizeColumnsToContents();

    const bool actionsEnabled = !state.actionsBusy;
    requeueButton_->setEnabled(actionsEnabled && selected->state != "QUEUED" && selected->state != "CLAIMED" && selected->state != "RUNNING" && selected->state != "FAILED");
    restartButton_->setEnabled(actionsEnabled && selected->state == "FAILED");
    cancelButton_->setEnabled(actionsEnabled && selected->state != "SUCCEEDED" && selected->state != "CANCELED" && selected->state != "SUCCEEDED_WINNER" && selected->state != "SUCCEEDED_DUPLICATE");
    applyBumpButton_->setEnabled(actionsEnabled);
    inspectorRefreshButton_->setEnabled(actionsEnabled && state.selectedJobId > 0);
}

void JobsPage::updateStatusWidgets()
{
    const auto& state = controller_->viewState();
    pageSummaryLabel_->setText(QStringLiteral("Rows: %1 • page size: %2").arg(state.page.items.size()).arg(state.pageLimit));
    lastRefreshLabel_->setText(state.lastRefresh.isValid() ? QStringLiteral("Last refresh: %1").arg(state.lastRefresh.toString(QStringLiteral("hh:mm:ss AP"))) : QStringLiteral("Last refresh: --"));
    if (!state.errorMessage.isEmpty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("error")); inlineMessageLabel_->setText(state.errorMessage); inlineMessageLabel_->show();
    } else if (!state.infoMessage.isEmpty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info")); inlineMessageLabel_->setText(state.infoMessage); inlineMessageLabel_->show();
    } else if (state.loading) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info")); inlineMessageLabel_->setText(QStringLiteral("Loading jobs…")); inlineMessageLabel_->show();
    } else if (state.page.items.empty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info")); inlineMessageLabel_->setText(QStringLiteral("No jobs matched the current filters.")); inlineMessageLabel_->show();
    } else {
        inlineMessageLabel_->hide();
    }
    style()->unpolish(inlineMessageLabel_); style()->polish(inlineMessageLabel_);
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
