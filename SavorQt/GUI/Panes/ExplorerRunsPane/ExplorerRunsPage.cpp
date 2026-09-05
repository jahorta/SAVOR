#include "ExplorerRunsPage.h"

#include "ExplorerRunsController.h"
#include "GUI/Refresh/RowUpdate.h"

#include <QtCore/QDateTime>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
#include <QtCore/QTimeZone>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace {
constexpr int kIdRole = Qt::UserRole + 1;

QTableWidgetItem* makeItem(const QString& text)
{
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QTableWidgetItem* makeIdItem(qint64 id)
{
    auto* item = makeItem(QString::number(id));
    item->setData(kIdRole, id);
    return item;
}
}

ExplorerRunsPage::ExplorerRunsPage(QWidget* parent)
    : QWidget(parent)
    , controller_(new ExplorerRunsController(this))
{
    createWidgets();
    wireSignals();
    syncControls();
}

void ExplorerRunsPage::setPageActive(bool active)
{
    controller_->setPageActive(active);
}

void ExplorerRunsPage::createWidgets()
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

    refreshButton_ = new QPushButton(QStringLiteral("Refresh now"), toolbarPanel);
    prevButton_ = new QPushButton(QStringLiteral("Prev"), toolbarPanel);
    nextButton_ = new QPushButton(QStringLiteral("Next"), toolbarPanel);
    autoRefreshCheck_ = new QCheckBox(QStringLiteral("Auto refresh"), toolbarPanel);
    refreshSecondsSpin_ = new QSpinBox(toolbarPanel);
    pageSizeSpin_ = new QSpinBox(toolbarPanel);
    childVictoryOnlyCheck_ = new QCheckBox(QStringLiteral("Child victory only"), toolbarPanel);
    winnersOnlyCheck_ = new QCheckBox(QStringLiteral("Winners only"), toolbarPanel);
    showDuplicatesCheck_ = new QCheckBox(QStringLiteral("Show duplicates"), toolbarPanel);
    successOnlyCheck_ = new QCheckBox(QStringLiteral("Success only"), toolbarPanel);
    summaryLabel_ = new QLabel(toolbarPanel);
    lastRefreshLabel_ = new QLabel(toolbarPanel);

    refreshButton_->setObjectName("jobSetsSecondaryButton");
    prevButton_->setObjectName("jobSetsSecondaryButton");
    nextButton_->setObjectName("jobSetsSecondaryButton");
    autoRefreshCheck_->setObjectName("jobSetsCheckBox");
    childVictoryOnlyCheck_->setObjectName("jobSetsCheckBox");
    winnersOnlyCheck_->setObjectName("jobSetsCheckBox");
    showDuplicatesCheck_->setObjectName("jobSetsCheckBox");
    successOnlyCheck_->setObjectName("jobSetsCheckBox");
    refreshSecondsSpin_->setObjectName("jobSetsSpin");
    pageSizeSpin_->setObjectName("jobSetsSpin");
    summaryLabel_->setObjectName("jobSetsMetaText");
    lastRefreshLabel_->setObjectName("jobSetsMetaText");

    refreshSecondsSpin_->setRange(1, 10);
    refreshSecondsSpin_->setSuffix(QStringLiteral(" s"));
    pageSizeSpin_->setRange(10, 500);
    pageSizeSpin_->setSingleStep(10);

    childVictoryOnlyCheck_->setEnabled(false);
    winnersOnlyCheck_->setEnabled(false);
    showDuplicatesCheck_->setEnabled(false);
    successOnlyCheck_->setEnabled(false);
    childVictoryOnlyCheck_->setToolTip(QStringLiteral("Waiting on typed explorer-run victory projection."));
    winnersOnlyCheck_->setToolTip(QStringLiteral("Waiting on typed explorer-run result projection."));
    showDuplicatesCheck_->setToolTip(QStringLiteral("Waiting on typed explorer-run duplicate projection."));
    successOnlyCheck_->setToolTip(QStringLiteral("Waiting on typed explorer-run result projection."));

    toolbarLayout->addWidget(refreshButton_, 0, 0);
    toolbarLayout->addWidget(prevButton_, 0, 1);
    toolbarLayout->addWidget(nextButton_, 0, 2);
    toolbarLayout->addWidget(autoRefreshCheck_, 0, 3);
    toolbarLayout->addWidget(new QLabel(QStringLiteral("Interval"), toolbarPanel), 0, 4);
    toolbarLayout->addWidget(refreshSecondsSpin_, 0, 5);
    toolbarLayout->addWidget(new QLabel(QStringLiteral("Page size"), toolbarPanel), 0, 6);
    toolbarLayout->addWidget(pageSizeSpin_, 0, 7);
    toolbarLayout->addWidget(summaryLabel_, 0, 8);
    toolbarLayout->addWidget(lastRefreshLabel_, 0, 9);
    toolbarLayout->addWidget(childVictoryOnlyCheck_, 1, 0, 1, 2);
    toolbarLayout->addWidget(winnersOnlyCheck_, 1, 2);
    toolbarLayout->addWidget(showDuplicatesCheck_, 1, 3);
    toolbarLayout->addWidget(successOnlyCheck_, 1, 4);
    toolbarLayout->setColumnStretch(8, 1);
    rootLayout->addWidget(toolbarPanel);

    auto* mainSplitter = new QSplitter(Qt::Vertical, this);
    mainSplitter->setChildrenCollapsible(false);

    auto* topSplitter = new QSplitter(Qt::Horizontal, mainSplitter);
    topSplitter->setChildrenCollapsible(false);

    auto* groupsPanel = new QFrame(topSplitter);
    groupsPanel->setObjectName("jobSetsContentPanel");
    auto* groupsLayout = new QVBoxLayout(groupsPanel);
    groupsLayout->setContentsMargins(8, 8, 8, 8);
    groupsLayout->addWidget(new QLabel(QStringLiteral("Run Groups"), groupsPanel));
    groupsTable_ = new QTableWidget(groupsPanel);
    groupsTable_->setColumnCount(5);
    groupsTable_->setHorizontalHeaderLabels({ QStringLiteral("Job Set"), QStringLiteral("Created"), QStringLiteral("Jobs"), QStringLiteral("Results"), QStringLiteral("Status") });
    groupsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    groupsTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    groupsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    groupsTable_->setAlternatingRowColors(true);
    groupsTable_->verticalHeader()->hide();
    groupsTable_->horizontalHeader()->setStretchLastSection(true);
    groupsTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    groupsTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    groupsLayout->addWidget(groupsTable_, 1);

    auto* wavesPanel = new QFrame(topSplitter);
    wavesPanel->setObjectName("jobSetsContentPanel");
    auto* wavesLayout = new QVBoxLayout(wavesPanel);
    wavesLayout->setContentsMargins(8, 8, 8, 8);
    wavesLayout->addWidget(new QLabel(QStringLiteral("Wave Tree"), wavesPanel));
    waveTree_ = new QTreeWidget(wavesPanel);
    waveTree_->setHeaderLabels({ QStringLiteral("Wave"), QStringLiteral("Jobs"), QStringLiteral("Status") });
    waveTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    waveTree_->setRootIsDecorated(true);
    waveTree_->header()->setStretchLastSection(true);
    wavesLayout->addWidget(waveTree_, 1);

    topSplitter->addWidget(groupsPanel);
    topSplitter->addWidget(wavesPanel);
    topSplitter->setStretchFactor(0, 2);
    topSplitter->setStretchFactor(1, 1);

    auto* bottomSplitter = new QSplitter(Qt::Horizontal, mainSplitter);
    bottomSplitter->setChildrenCollapsible(false);

    auto* jobsPanel = new QFrame(bottomSplitter);
    jobsPanel->setObjectName("jobSetsContentPanel");
    auto* jobsLayout = new QVBoxLayout(jobsPanel);
    jobsLayout->setContentsMargins(8, 8, 8, 8);
    jobsLayout->addWidget(new QLabel(QStringLiteral("Wave Jobs"), jobsPanel));
    jobsTable_ = new QTableWidget(jobsPanel);
    jobsTable_->setColumnCount(7);
    jobsTable_->setHorizontalHeaderLabels({ QStringLiteral("Job"), QStringLiteral("State"), QStringLiteral("Outcome"), QStringLiteral("Predicates"), QStringLiteral("Delta VI"), QStringLiteral("Fake Attacks"), QStringLiteral("RNG Seed") });
    jobsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    jobsTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    jobsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    jobsTable_->setAlternatingRowColors(true);
    jobsTable_->setContextMenuPolicy(Qt::CustomContextMenu);
    jobsTable_->verticalHeader()->hide();
    jobsTable_->horizontalHeader()->setStretchLastSection(true);
    jobsTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    jobsTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    jobsLayout->addWidget(jobsTable_, 1);

    auto* detailPanel = new QFrame(bottomSplitter);
    detailPanel->setObjectName("jobSetsContentPanel");
    auto* detailLayout = new QVBoxLayout(detailPanel);
    detailLayout->setContentsMargins(8, 8, 8, 8);
    auto* detailHeader = new QHBoxLayout();
    detailHeader->addWidget(new QLabel(QStringLiteral("Details"), detailPanel));
    detailHeader->addStretch();
    triggerNextWaveButton_ = new QPushButton(QStringLiteral("Trigger Next Wave"), detailPanel);
    triggerNextWaveButton_->setObjectName("jobSetsSecondaryButton");
    triggerNextWaveButton_->setEnabled(false);
    triggerNextWaveButton_->setToolTip(QStringLiteral("Waiting on typed next-wave command service."));
    detailHeader->addWidget(triggerNextWaveButton_);
    detailLayout->addLayout(detailHeader);

    auto* detailSplitter = new QSplitter(Qt::Vertical, detailPanel);
    detailSplitter->setChildrenCollapsible(false);
    blueprintText_ = new QTextEdit(detailSplitter);
    progressText_ = new QTextEdit(detailSplitter);
    resultsText_ = new QTextEdit(detailSplitter);
    for (QTextEdit* edit : { blueprintText_, progressText_, resultsText_ }) {
        edit->setReadOnly(true);
        edit->setObjectName("jobsInspectorText");
    }
    detailSplitter->addWidget(blueprintText_);
    detailSplitter->addWidget(progressText_);
    detailSplitter->addWidget(resultsText_);
    detailLayout->addWidget(detailSplitter, 1);

    bottomSplitter->addWidget(jobsPanel);
    bottomSplitter->addWidget(detailPanel);
    bottomSplitter->setStretchFactor(0, 2);
    bottomSplitter->setStretchFactor(1, 1);

    mainSplitter->addWidget(topSplitter);
    mainSplitter->addWidget(bottomSplitter);
    mainSplitter->setStretchFactor(0, 1);
    mainSplitter->setStretchFactor(1, 2);
    rootLayout->addWidget(mainSplitter, 1);

    inlineMessageLabel_ = new QLabel(this);
    inlineMessageLabel_->setObjectName("jobSetsInlineMessage");
    inlineMessageLabel_->setWordWrap(true);
    rootLayout->addWidget(inlineMessageLabel_);
}

void ExplorerRunsPage::wireSignals()
{
    connect(controller_, &ExplorerRunsController::stateChanged, this, [this]() {
        syncControls();
        refreshGroupsTable();
        refreshWaveTree();
        refreshJobsTable();
        refreshDetail();
        updateStatusWidgets();
    });
    connect(refreshButton_, &QPushButton::clicked, controller_, &ExplorerRunsController::requestRefresh);
    connect(prevButton_, &QPushButton::clicked, controller_, &ExplorerRunsController::requestPreviousPage);
    connect(nextButton_, &QPushButton::clicked, controller_, &ExplorerRunsController::requestNextPage);
    connect(autoRefreshCheck_, &QCheckBox::toggled, controller_, &ExplorerRunsController::setAutoRefreshEnabled);
    connect(refreshSecondsSpin_, qOverload<int>(&QSpinBox::valueChanged), controller_, &ExplorerRunsController::setRefreshSeconds);
    connect(pageSizeSpin_, qOverload<int>(&QSpinBox::valueChanged), controller_, &ExplorerRunsController::setPageLimit);
    connect(groupsTable_, &QTableWidget::currentCellChanged, this, [this](int currentRow, int, int, int) {
        if (refreshingSelection_ || currentRow < 0) {
            return;
        }
        const auto* item = groupsTable_->item(currentRow, 0);
        if (item != nullptr) {
            controller_->selectGroup(item->data(kIdRole).toLongLong());
        }
    });
    connect(jobsTable_, &QTableWidget::currentCellChanged, this, [this](int currentRow, int, int, int) {
        if (refreshingSelection_ || currentRow < 0) {
            return;
        }
        const auto* item = jobsTable_->item(currentRow, 0);
        if (item != nullptr) {
            controller_->selectJob(item->data(kIdRole).toLongLong());
        }
    });
    connect(jobsTable_, &QWidget::customContextMenuRequested, this, &ExplorerRunsPage::showJobContextMenu);
}

void ExplorerRunsPage::syncControls()
{
    const auto& state = controller_->viewState();
    QSignalBlocker autoRefreshBlock(autoRefreshCheck_);
    QSignalBlocker refreshBlock(refreshSecondsSpin_);
    QSignalBlocker pageSizeBlock(pageSizeSpin_);
    autoRefreshCheck_->setChecked(state.autoRefresh);
    refreshSecondsSpin_->setValue(state.refreshSeconds);
    pageSizeSpin_->setValue(state.pageLimit);
    prevButton_->setEnabled(state.groupPage.prev.has_value() && !state.loadingGroups);
    nextButton_->setEnabled(state.groupPage.next.has_value() && !state.loadingGroups);
    refreshButton_->setEnabled(!state.loadingGroups);
}

void ExplorerRunsPage::refreshGroupsTable()
{
    const auto& state = controller_->viewState();

    std::vector<GroupRow> rows;
    rows.reserve(state.groupPage.groups.size());
    for (const auto& group : state.groupPage.groups) {
        rows.push_back(GroupRow{
            group.job_set_id,
            formatTimestamp(group.created_at_utc),
            QStringLiteral("%1/%2").arg(group.settled_jobs).arg(group.total_jobs),
            QStringLiteral("%1 succeeded, %2 failed, %3 interrupted, %4 superseded, %5 canceled")
                .arg(group.succeeded_jobs).arg(group.failed_jobs)
                .arg(group.interrupted_jobs).arg(group.superseded_jobs)
                .arg(group.canceled_jobs),
            groupStatusText(group.settled_jobs, group.total_jobs, group.failed_jobs,
                group.interrupted_jobs, group.canceled_jobs),
        });
    }

    refreshingSelection_ = true;
    savorqt::gui::ReplaceTableProjectionByKey(
        groupsTable_,
        currentGroupRows_,
        rows,
        [](const GroupRow& row) { return row.jobSetId; },
        [](const GroupRow& lhs, const GroupRow& rhs) {
            return lhs.jobSetId == rhs.jobSetId
                && lhs.created == rhs.created
                && lhs.jobs == rhs.jobs
                && lhs.results == rhs.results
                && lhs.status == rhs.status;
        },
        [](QTableWidget* table, int row, const GroupRow& item) {
            table->setItem(row, 0, makeIdItem(item.jobSetId));
            table->setItem(row, 1, makeItem(item.created));
            table->setItem(row, 2, makeItem(item.jobs));
            table->setItem(row, 3, makeItem(item.results));
            table->setItem(row, 4, makeItem(item.status));
        });
    for (int row = 0; row < groupsTable_->rowCount(); ++row) {
        const auto* item = groupsTable_->item(row, 0);
        if (item != nullptr && item->data(kIdRole).toLongLong() == state.selectedJobSetId) {
            groupsTable_->selectRow(row);
            break;
        }
    }
    refreshingSelection_ = false;
}

void ExplorerRunsPage::refreshWaveTree()
{
    const auto& state = controller_->viewState();
    std::vector<WaveTreeRow> rows;
    if (state.selectedGroup.has_value()) {
        const auto& summary = state.selectedGroup->summary;
        rows.push_back(WaveTreeRow{
            summary.job_set_id,
            QStringLiteral("Job set %1").arg(summary.job_set_id),
            QString::number(summary.total_jobs),
            state.selectedGroup->hierarchy_projection_available
                ? groupStatusText(summary.settled_jobs, summary.total_jobs,
                    summary.failed_jobs, summary.interrupted_jobs,
                    summary.canceled_jobs)
                : QStringLiteral("Wave hierarchy projection pending"),
        });
    }

    savorqt::gui::ReplaceTreeProjectionByKey(
        waveTree_,
        currentWaveRows_,
        rows,
        [](const WaveTreeRow& row) { return row.jobSetId; },
        [](const WaveTreeRow& lhs, const WaveTreeRow& rhs) {
            return lhs.jobSetId == rhs.jobSetId
                && lhs.label == rhs.label
                && lhs.jobs == rhs.jobs
                && lhs.status == rhs.status;
        },
        [](QTreeWidget*, QTreeWidgetItem* item, const WaveTreeRow& row) {
            item->setText(0, row.label);
            item->setText(1, row.jobs);
            item->setText(2, row.status);
            item->setData(0, kIdRole, row.jobSetId);
            item->setExpanded(true);
        });
}

void ExplorerRunsPage::refreshJobsTable()
{
    const auto& state = controller_->viewState();
    std::vector<JobRow> rows;
    if (state.selectedGroup.has_value()) {
        rows.reserve(state.selectedGroup->jobs.size());
        for (const auto& job : state.selectedGroup->jobs) {
            rows.push_back(JobRow{
                job.job_id,
                QString::fromStdString(job.state),
            });
        }
    }

    refreshingSelection_ = true;
    savorqt::gui::ReplaceTableProjectionByKey(
        jobsTable_,
        currentJobRows_,
        rows,
        [](const JobRow& row) { return row.jobId; },
        [](const JobRow& lhs, const JobRow& rhs) {
            return lhs.jobId == rhs.jobId
                && lhs.state == rhs.state;
        },
        [](QTableWidget* table, int row, const JobRow& item) {
            table->setItem(row, 0, makeIdItem(item.jobId));
            table->setItem(row, 1, makeItem(item.state));
            table->setItem(row, 2, makeItem(item.state));
            table->setItem(row, 3, makeItem(QStringLiteral("projection pending")));
            table->setItem(row, 4, makeItem(QStringLiteral("--")));
            table->setItem(row, 5, makeItem(QStringLiteral("--")));
            table->setItem(row, 6, makeItem(QStringLiteral("--")));
        });
    for (int row = 0; row < jobsTable_->rowCount(); ++row) {
        const auto* item = jobsTable_->item(row, 0);
        if (item != nullptr && item->data(kIdRole).toLongLong() == state.selectedJobId) {
            jobsTable_->selectRow(row);
            break;
        }
    }
    refreshingSelection_ = false;
}

void ExplorerRunsPage::refreshDetail()
{
    const auto& state = controller_->viewState();
    if (!state.selectedJob.has_value()) {
        updatePlainText(blueprintText_, QStringLiteral("Select a job to inspect typed details."));
        updatePlainText(progressText_, QString());
        updatePlainText(resultsText_, QStringLiteral("Typed explorer-run result projection is not available yet. Raw result INI is intentionally not loaded or parsed here."));
        return;
    }

    const auto& detail = *state.selectedJob;
    const auto& job = detail.job.summary;
    QString blueprint;
    blueprint += QStringLiteral("Job: %1\n").arg(job.job_id);
    blueprint += QStringLiteral("Job Set: %1\n").arg(job.job_set_id);
    blueprint += QStringLiteral("Program Kind: %1\n").arg(job.program_kind);
    blueprint += QStringLiteral("State: %1\n").arg(QString::fromStdString(job.state));
    blueprint += QStringLiteral("Queued: %1\n").arg(formatTimestamp(job.queued_at_utc));
    blueprint += QStringLiteral("Started: %1\n").arg(job.started_at_utc.has_value() ? formatTimestamp(*job.started_at_utc) : QStringLiteral("--"));
    blueprint += QStringLiteral("Ended: %1\n").arg(job.ended_at_utc.has_value() ? formatTimestamp(*job.ended_at_utc) : QStringLiteral("--"));
    blueprint += QStringLiteral("Attempts: %1/%2\n").arg(job.attempts).arg(job.max_attempts);
    blueprint += QStringLiteral("Fingerprint: %1\n").arg(QString::fromStdString(detail.job.fingerprint));
    blueprint += QStringLiteral("\nTyped turn-input and replication detail projections are not available yet.");

    if (!detail.artifacts.empty()) {
        blueprint += QStringLiteral("\n\nArtifacts:\n");
        for (const auto& artifact : detail.artifacts) {
            blueprint += QStringLiteral("  %1 | %2 | %3 | %4 bytes\n")
                .arg(artifact.artifact_id)
                .arg(QString::fromStdString(artifact.role_kind))
                .arg(QString::fromStdString(artifact.filename))
                .arg(artifact.size_bytes);
        }
    }
    updatePlainText(blueprintText_, blueprint);

    QString events;
    for (const auto& event : detail.events) {
        events += QStringLiteral("%1  %2")
            .arg(formatTimestamp(event.event_ts_utc))
            .arg(QString::fromStdString(event.event_kind));
        if (!event.message.empty()) {
            events += QStringLiteral("  %1").arg(QString::fromStdString(event.message));
        }
        if (event.artifact_id.has_value()) {
            events += QStringLiteral("  artifact %1").arg(*event.artifact_id);
        }
        events += QLatin1Char('\n');
    }
    updatePlainText(progressText_, events.isEmpty()
        ? QStringLiteral("No typed job events were projected for this job.")
        : events);
    updatePlainText(resultsText_, QStringLiteral("Typed result metrics pending:\nOutcome, predicates, delta VI, fake attacks, RNG seed, winner, duplicate, output savestate, and next-wave eligibility need SavorDb/UIRead projection fields. This page does not parse result INI."));
}

void ExplorerRunsPage::updateStatusWidgets()
{
    const auto& state = controller_->viewState();
    summaryLabel_->setText(QStringLiteral("Groups: %1 | page size: %2").arg(state.groupPage.groups.size()).arg(state.pageLimit));
    lastRefreshLabel_->setText(state.lastRefresh.isValid()
        ? QStringLiteral("Last refresh: %1").arg(state.lastRefresh.toString(QStringLiteral("hh:mm:ss AP")))
        : QStringLiteral("Last refresh: --"));

    StatusToast::Severity toastSeverity = StatusToast::Severity::Info;
    QString toastMessage;
    if (!state.errorMessage.isEmpty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("error"));
        inlineMessageLabel_->setText(state.errorMessage);
        inlineMessageLabel_->show();
        toastSeverity = StatusToast::Severity::Error;
        toastMessage = state.errorMessage;
    } else if (state.loadingGroups && state.groupPage.groups.empty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info"));
        inlineMessageLabel_->setText(QStringLiteral("Loading explorer runs..."));
        inlineMessageLabel_->show();
    } else if (state.groupPage.groups.empty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info"));
        inlineMessageLabel_->setText(QStringLiteral("No BattleSingleTurn explorer run job sets are visible in UIRead."));
        inlineMessageLabel_->show();
    } else {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info"));
        inlineMessageLabel_->setText(QStringLiteral("Explorer-specific wave/result/turn-input projections are pending. Current data is typed job-set/job/event/artifact data only."));
        inlineMessageLabel_->show();
    }
    style()->unpolish(inlineMessageLabel_);
    style()->polish(inlineMessageLabel_);

    if (!toastMessage.isEmpty()) {
        const QString signature = QStringLiteral("%1|%2").arg(static_cast<int>(toastSeverity)).arg(toastMessage);
        if (signature != lastToastSignature_) {
            lastToastSignature_ = signature;
            emit statusToastRequested(StatusToast{ toastSeverity, toastMessage, QString(), 1, QDateTime{}, 4000 });
        }
    }
}

void ExplorerRunsPage::showJobContextMenu(const QPoint& position)
{
    const QModelIndex index = jobsTable_->indexAt(position);
    if (!index.isValid()) {
        return;
    }
    jobsTable_->selectRow(index.row());
    const qint64 jobId = selectedJobId();
    if (jobId <= 0) {
        return;
    }

    const auto* stateItem = jobsTable_->item(index.row(), 1);
    const QString state = stateItem != nullptr ? stateItem->text() : QString();
    QMenu menu(jobsTable_);
    QAction* refreshDetailAction = menu.addAction(QStringLiteral("Refresh detail"));
    QAction* viewTurnInputsAction = menu.addAction(QStringLiteral("View Turn Inputs"));
    QAction* replicationAction = menu.addAction(QStringLiteral("Replication Details"));
    QAction* battlePlanAction = menu.addAction(QStringLiteral("Regurgitate Battle Plan"));
    menu.addSeparator();
    QAction* replayVisualAction = menu.addAction(QStringLiteral("Replay Visually"));

    viewTurnInputsAction->setEnabled(false);
    replicationAction->setEnabled(false);
    battlePlanAction->setEnabled(false);
    replayVisualAction->setEnabled(isFinishedState(state));

    viewTurnInputsAction->setToolTip(QStringLiteral("Waiting on typed turn-input projection."));
    replicationAction->setToolTip(QStringLiteral("Waiting on typed replication DTO service."));
    battlePlanAction->setToolTip(QStringLiteral("Waiting on typed battle-plan description service."));

    QAction* chosen = menu.exec(jobsTable_->viewport()->mapToGlobal(position));
    if (chosen == refreshDetailAction) {
        controller_->refreshSelectedJob();
    } else if (chosen == replayVisualAction) {
        emit visualReplayRequested(jobId);
    }
}

qint64 ExplorerRunsPage::selectedJobId() const
{
    const int row = jobsTable_->currentRow();
    const auto* item = row >= 0 ? jobsTable_->item(row, 0) : nullptr;
    return item != nullptr ? item->data(kIdRole).toLongLong() : 0;
}

bool ExplorerRunsPage::isFinishedState(const QString& state) const
{
    return state == QStringLiteral("SUCCEEDED")
        || state == QStringLiteral("FAILED")
        || state == QStringLiteral("INTERRUPTED")
        || state == QStringLiteral("CANCELED")
        || state == QStringLiteral("SUPERSEDED")
        || state == QStringLiteral("SUCCEEDED_WINNER")
        || state == QStringLiteral("SUCCEEDED_DUPLICATE");
}

QString ExplorerRunsPage::formatTimestamp(qint64 epochMillis) const
{
    if (epochMillis <= 0) {
        return QStringLiteral("--");
    }
    return QDateTime::fromMSecsSinceEpoch(epochMillis, QTimeZone::UTC).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QString ExplorerRunsPage::groupStatusText(std::int64_t settled, std::int64_t total,
    std::int64_t failed, std::int64_t interrupted,
    std::int64_t canceled) const
{
    if (total <= 0) {
        return QStringLiteral("Empty");
    }
    if (settled >= total) {
        if (interrupted > 0) {
            return QStringLiteral("Interrupted");
        }
        return failed > 0 || canceled > 0 ? QStringLiteral("Terminal with issues") : QStringLiteral("Completed");
    }
    return QStringLiteral("Running or queued");
}

bool ExplorerRunsPage::updatePlainText(QTextEdit* edit, const QString& text)
{
    if (edit == nullptr || edit->toPlainText() == text) {
        return false;
    }
    edit->setPlainText(text);
    return true;
}
