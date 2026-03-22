#include "ExplorerRunsPage.h"

#include "ExplorerRunsGroupTableModel.h"
#include "ExplorerRunsGroupTableView.h"
#include "ExplorerRunsJobsTableModel.h"
#include "ExplorerRunsJobsTableView.h"

#include "DB/BattlePlanAtomRepo.h"
#include "DB/BattlePlanTurnRepo.h"
#include "DB/DeltaSeedRepo.h"
#include "DB/ProgramDB/BattleSingleTurnRunDBCodec.h"
#include "DB/Querying/DataService.h"
#include "DB/Scheduling/JobEventsRepo.h"
#include "DB/Scheduling/JobsRepo.h"
#include "DB/Scheduling/JobSetsRepo.h"
#include "Core/Input/InputPlanFmt.h"
#include "Core/Input/SoaBattle/PlanWriter.h"
#include "Phases/Programs/BattleRunner/BattleOutcome.h"
#include "Runner/IPC/Wire.h"
#include "Utils/IniDoc.h"

#include <QtCore/QDateTime>
#include <QtCore/QItemSelectionModel>
#include <QtCore/QStringList>
#include <QtCore/QSignalBlocker>
#include <QtGui/QStandardItem>
#include <QtGui/QStandardItemModel>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTreeView>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <future>
#include <unordered_map>

using namespace simcore::db;
using namespace simcore::db::codec::battle::singleturn;
using namespace std::chrono_literals;

namespace {
constexpr int kWaveJobSetIdUserRole = Qt::UserRole + 1;

void configureFlatTreeView(QTreeView* view, const QString& objectName)
{
    view->setObjectName(objectName);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setAlternatingRowColors(true);
    view->setRootIsDecorated(false);
    view->setItemsExpandable(false);
    view->setAllColumnsShowFocus(true);
    view->setUniformRowHeights(true);
    view->setIndentation(0);
    view->header()->setStretchLastSection(true);
}

void selectFlatRow(QTreeView* view, int row)
{
    if (!view || !view->model() || !view->selectionModel()) {
        return;
    }

    const QModelIndex index = view->model()->index(row, 0);
    if (!index.isValid()) {
        return;
    }

    view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    view->scrollTo(index);
}
}

ExplorerRunsPage::ExplorerRunsPage(QWidget* parent)
    : QWidget(parent)
    , groupsModel_(new ExplorerRunsGroupTableModel(this))
    , jobsModel_(new ExplorerRunsJobsTableModel(this))
    , wavesModel_(new QStandardItemModel(this))
{
    createWidgets();
    wireSignals();
    kickGroupsFetch();
    refreshTimer_.start(250);
}

ExplorerRunsPage::~ExplorerRunsPage()
{
    if (state_.groupsFuture.valid()) state_.groupsFuture.wait();
    if (state_.jobsFuture.valid()) state_.jobsFuture.wait();
    if (state_.resultsFuture.valid()) state_.resultsFuture.wait();
    if (state_.progressFuture.valid()) state_.progressFuture.wait();
    if (state_.blueprintFuture.valid()) state_.blueprintFuture.wait();
}

void ExplorerRunsPage::createWidgets()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(12);

    QFrame* toolbarPanel = new QFrame(this);
    toolbarPanel->setObjectName("jobsToolbarPanel");
    QHBoxLayout* toolbarLayout = new QHBoxLayout(toolbarPanel);
    toolbarLayout->setContentsMargins(16, 12, 16, 12);
    toolbarLayout->setSpacing(12);

    refreshButton_ = new QPushButton(QStringLiteral("Refresh now"), toolbarPanel);
    refreshButton_->setObjectName("jobsSecondaryButton");
    autoRefreshCheck_ = new QCheckBox(QStringLiteral("Auto refresh"), toolbarPanel);
    autoRefreshCheck_->setObjectName("jobsCheckBox");
    refreshSecondsSpin_ = new QSpinBox(toolbarPanel);
    refreshSecondsSpin_->setObjectName("jobsRefreshSpin");
    refreshSecondsSpin_->setRange(1, 10);
    refreshSecondsSpin_->setSuffix(QStringLiteral(" s"));
    summaryLabel_ = new QLabel(toolbarPanel);
    summaryLabel_->setObjectName("jobsMetaText");

    toolbarLayout->addWidget(refreshButton_);
    toolbarLayout->addWidget(autoRefreshCheck_);
    toolbarLayout->addWidget(new QLabel(QStringLiteral("Interval"), toolbarPanel));
    toolbarLayout->addWidget(refreshSecondsSpin_);
    toolbarLayout->addStretch();
    toolbarLayout->addWidget(summaryLabel_);
    rootLayout->addWidget(toolbarPanel);

    QSplitter* verticalSplitter = new QSplitter(Qt::Vertical, this);
    verticalSplitter->setChildrenCollapsible(false);

    QSplitter* topSplitter = new QSplitter(Qt::Horizontal, verticalSplitter);
    topSplitter->setChildrenCollapsible(false);
    QSplitter* bottomSplitter = new QSplitter(Qt::Horizontal, verticalSplitter);
    bottomSplitter->setChildrenCollapsible(false);

    QFrame* groupsPanel = new QFrame(topSplitter);
    groupsPanel->setObjectName("jobsSurfacePanel");
    QVBoxLayout* groupsLayout = new QVBoxLayout(groupsPanel);
    groupsLayout->setContentsMargins(16, 16, 16, 16);
    groupsLayout->addWidget(new QLabel(QStringLiteral("Run Groups"), groupsPanel));
    groupsView_ = new ExplorerRunsGroupTableView(groupsPanel);
    groupsView_->attachModel(groupsModel_);
    groupsLayout->addWidget(groupsView_, 1);

    QFrame* wavesPanel = new QFrame(topSplitter);
    wavesPanel->setObjectName("jobsSurfacePanel");
    QVBoxLayout* wavesLayout = new QVBoxLayout(wavesPanel);
    wavesLayout->setContentsMargins(16, 16, 16, 16);
    wavesLayout->addWidget(new QLabel(QStringLiteral("Wave Tree"), wavesPanel));
    wavesView_ = new QTreeView(wavesPanel);
    configureFlatTreeView(wavesView_, QStringLiteral("explorerRunsWavesTree"));
    wavesView_->setRootIsDecorated(true);
    wavesView_->setItemsExpandable(true);
    wavesView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    wavesModel_->setHorizontalHeaderLabels({ QStringLiteral("Wave"), QStringLiteral("Status") });
    wavesView_->setModel(wavesModel_);
    wavesView_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    wavesView_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    wavesLayout->addWidget(wavesView_, 1);

    QFrame* jobsPanel = new QFrame(bottomSplitter);
    jobsPanel->setObjectName("jobsSurfacePanel");
    QVBoxLayout* jobsLayout = new QVBoxLayout(jobsPanel);
    jobsLayout->setContentsMargins(16, 16, 16, 16);

    QHBoxLayout* jobsHeaderLayout = new QHBoxLayout();
    jobsHeaderLayout->addWidget(new QLabel(QStringLiteral("Wave Jobs"), jobsPanel));
    jobsSummaryLabel_ = new QLabel(jobsPanel);
    jobsSummaryLabel_->setObjectName("jobsMetaText");
    jobsHeaderLayout->addStretch();
    jobsHeaderLayout->addWidget(jobsSummaryLabel_);
    jobsLayout->addLayout(jobsHeaderLayout);

    QGridLayout* filtersLayout = new QGridLayout();
    winnersOnlyCheck_ = new QCheckBox(QStringLiteral("Winner only subset"), jobsPanel);
    winnersOnlyCheck_->setObjectName("jobsCheckBox");
    showDuplicatesCheck_ = new QCheckBox(QStringLiteral("Show duplicates"), jobsPanel);
    showDuplicatesCheck_->setObjectName("jobsCheckBox");
    successOnlyCheck_ = new QCheckBox(QStringLiteral("Success outcome only"), jobsPanel);
    successOnlyCheck_->setObjectName("jobsCheckBox");
    filtersLayout->addWidget(winnersOnlyCheck_, 0, 0);
    filtersLayout->addWidget(showDuplicatesCheck_, 0, 1);
    filtersLayout->addWidget(successOnlyCheck_, 0, 2);

    for (int i = 0; i < 3; ++i) {
        sortMetricBoxes_[i] = new QComboBox(jobsPanel);
        sortAscendingChecks_[i] = new QCheckBox(QStringLiteral("Asc"), jobsPanel);
        sortMetricBoxes_[i]->setObjectName("jobsFilterCombo");
        sortAscendingChecks_[i]->setObjectName("jobsCheckBox");
        filtersLayout->addWidget(new QLabel(QStringLiteral("Sort %1").arg(i + 1), jobsPanel), 1, i * 2);
        filtersLayout->addWidget(sortMetricBoxes_[i], 1, i * 2 + 1);
        filtersLayout->addWidget(sortAscendingChecks_[i], 2, i * 2 + 1);
    }
    jobsLayout->addLayout(filtersLayout);

    jobsView_ = new ExplorerRunsJobsTableView(jobsPanel);
    jobsView_->attachModel(jobsModel_);
    jobsLayout->addWidget(jobsView_, 1);

    QFrame* detailsPanel = new QFrame(bottomSplitter);
    detailsPanel->setObjectName("jobsSurfacePanel");
    QVBoxLayout* detailsLayout = new QVBoxLayout(detailsPanel);
    detailsLayout->setContentsMargins(16, 16, 16, 16);
    detailsLayout->addWidget(new QLabel(QStringLiteral("Details"), detailsPanel));

    QHBoxLayout* triggerLayout = new QHBoxLayout();
    overrideFakeAttacksCheck_ = new QCheckBox(QStringLiteral("Add more fake attacks"), detailsPanel);
    overrideFakeAttacksCheck_->setObjectName("jobsCheckBox");
    fakeAttacksSpin_ = new QSpinBox(detailsPanel);
    fakeAttacksSpin_->setObjectName("jobsRefreshSpin");
    fakeAttacksSpin_->setRange(0, 9999);
    triggerButton_ = new QPushButton(QStringLiteral("Trigger Next Wave"), detailsPanel);
    triggerButton_->setObjectName("jobsPrimaryButton");
    triggerHintLabel_ = new QLabel(detailsPanel);
    triggerHintLabel_->setObjectName("jobsMetaText");
    triggerHintLabel_->setWordWrap(true);
    triggerLayout->addWidget(overrideFakeAttacksCheck_);
    triggerLayout->addWidget(fakeAttacksSpin_);
    triggerLayout->addWidget(triggerButton_);
    triggerLayout->addStretch();
    detailsLayout->addLayout(triggerLayout);
    detailsLayout->addWidget(triggerHintLabel_);

    detailsLayout->addWidget(new QLabel(QStringLiteral("Job Blueprint"), detailsPanel));
    blueprintText_ = new QPlainTextEdit(detailsPanel);
    blueprintText_->setObjectName("jobsInspectorText");
    blueprintText_->setReadOnly(true);
    blueprintText_->setMinimumHeight(90);
    detailsLayout->addWidget(blueprintText_);

    QSplitter* logsSplitter = new QSplitter(Qt::Horizontal, detailsPanel);
    logsSplitter->setChildrenCollapsible(false);
    progressText_ = new QPlainTextEdit(logsSplitter);
    progressText_->setObjectName("jobsInspectorText");
    progressText_->setReadOnly(true);
    resultsText_ = new QPlainTextEdit(logsSplitter);
    resultsText_->setObjectName("jobsInspectorText");
    resultsText_->setReadOnly(true);
    detailsLayout->addWidget(new QLabel(QStringLiteral("Progress / Results"), detailsPanel));
    detailsLayout->addWidget(logsSplitter, 1);

    verticalSplitter->addWidget(topSplitter);
    verticalSplitter->addWidget(bottomSplitter);
    verticalSplitter->setStretchFactor(0, 1);
    verticalSplitter->setStretchFactor(1, 1);
    topSplitter->setStretchFactor(0, 3);
    topSplitter->setStretchFactor(1, 2);
    bottomSplitter->setStretchFactor(0, 3);
    bottomSplitter->setStretchFactor(1, 2);
    rootLayout->addWidget(verticalSplitter, 1);

    inlineMessageLabel_ = new QLabel(this);
    inlineMessageLabel_->setObjectName("jobSetsInlineMessage");
    inlineMessageLabel_->setWordWrap(true);
    rootLayout->addWidget(inlineMessageLabel_);

    for (int i = 0; i < 3; ++i) {
        sortMetricBoxes_[i]->addItem(sortMetricLabel(SortMetric::PredicatesPassed), static_cast<int>(SortMetric::PredicatesPassed));
        sortMetricBoxes_[i]->addItem(sortMetricLabel(SortMetric::DeltaVI), static_cast<int>(SortMetric::DeltaVI));
        sortMetricBoxes_[i]->addItem(sortMetricLabel(SortMetric::FakeAttacks), static_cast<int>(SortMetric::FakeAttacks));
        sortMetricBoxes_[i]->addItem(sortMetricLabel(SortMetric::RngSeed), static_cast<int>(SortMetric::RngSeed));
        sortMetricBoxes_[i]->addItem(sortMetricLabel(SortMetric::JobId), static_cast<int>(SortMetric::JobId));
    }

    syncControls();
    refreshView();
}

void ExplorerRunsPage::wireSignals()
{
    connect(&refreshTimer_, &QTimer::timeout, this, [this]() { consumeFutures(); });
    connect(refreshButton_, &QPushButton::clicked, this, [this]() {
        kickGroupsFetch();
        if (!state_.selectedWaves.empty()) {
            kickJobsFetch();
        }
        if (state_.selectedJob > 0) {
            kickDetailsFetch(state_.selectedJob);
        }
    });
    connect(autoRefreshCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        state_.autoRefresh = checked;
    });
    connect(refreshSecondsSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        state_.refreshSeconds = value;
    });

    connect(groupsView_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this](const QModelIndex& current, const QModelIndex&) {
        if (!current.isValid()) {
            return;
        }
        const ExplorerRunsGroupRow* row = groupsModel_->rowAt(current.row());
        if (!row || state_.selectedRoot == row->rootGroupId) {
            return;
        }
        state_.selectedRoot = row->rootGroupId;
        state_.selectedWaves.clear();
        state_.selectedJob = -1;
        state_.jobs.clear();
        state_.resultsLog.clear();
        state_.progressLog.clear();
        state_.blueprintInfo.clear();
        refreshWaveTree();
        refreshJobModel();
        refreshDetailPanel();
        selectFirstWaveIfNeeded();
    });

    connect(wavesView_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this](const QItemSelection&, const QItemSelection&) {
        const std::vector<qint64> selected = selectedWaveIdsFromTree();
        if (selected == state_.selectedWaves) {
            return;
        }
        state_.selectedWaves = selected;
        state_.selectedJob = -1;
        state_.jobs.clear();
        state_.resultsLog.clear();
        state_.progressLog.clear();
        state_.blueprintInfo.clear();
        refreshJobModel();
        refreshDetailPanel();
        if (!state_.selectedWaves.empty()) {
            kickJobsFetch();
        }
    });

    auto refreshJobs = [this]() { refreshJobModel(); };
    connect(winnersOnlyCheck_, &QCheckBox::toggled, this, [this, refreshJobs](bool checked) {
        state_.winnersOnly = checked;
        if (!checked) {
            state_.showDuplicates = false;
        }
        syncControls();
        refreshJobs();
    });
    connect(showDuplicatesCheck_, &QCheckBox::toggled, this, [this, refreshJobs](bool checked) {
        state_.showDuplicates = checked;
        refreshJobs();
    });
    connect(successOnlyCheck_, &QCheckBox::toggled, this, [this, refreshJobs](bool checked) {
        state_.successOnly = checked;
        refreshJobs();
    });

    for (int i = 0; i < 3; ++i) {
        connect(sortMetricBoxes_[i], qOverload<int>(&QComboBox::currentIndexChanged), this, [this, i, refreshJobs](int) {
            state_.sortKeys[static_cast<size_t>(i)].metric = static_cast<SortMetric>(sortMetricBoxes_[i]->currentData().toInt());
            refreshJobs();
        });
        connect(sortAscendingChecks_[i], &QCheckBox::toggled, this, [this, i, refreshJobs](bool checked) {
            state_.sortKeys[static_cast<size_t>(i)].ascending = checked;
            refreshJobs();
        });
    }

    connect(jobsView_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this](const QModelIndex& current, const QModelIndex&) {
        if (!current.isValid()) {
            return;
        }
        const ExplorerRunsJobRow* row = jobsModel_->rowAt(current.row());
        if (!row || state_.selectedJob == row->jobId) {
            return;
        }
        state_.selectedJob = row->jobId;
        kickDetailsFetch(row->jobId);
    });

    connect(overrideFakeAttacksCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        state_.overrideMaxFakeAttacks = checked;
        syncControls();
    });
    connect(fakeAttacksSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        state_.maxFakeAttacksOverride = value;
    });
    connect(triggerButton_, &QPushButton::clicked, this, &ExplorerRunsPage::triggerNextWave);
}

void ExplorerRunsPage::syncControls()
{
    {
        QSignalBlocker blocker(autoRefreshCheck_);
        autoRefreshCheck_->setChecked(state_.autoRefresh);
    }
    {
        QSignalBlocker blocker(refreshSecondsSpin_);
        refreshSecondsSpin_->setValue(state_.refreshSeconds);
    }
    {
        QSignalBlocker blocker(winnersOnlyCheck_);
        winnersOnlyCheck_->setChecked(state_.winnersOnly);
    }
    {
        QSignalBlocker blocker(showDuplicatesCheck_);
        showDuplicatesCheck_->setChecked(state_.showDuplicates);
    }
    {
        QSignalBlocker blocker(successOnlyCheck_);
        successOnlyCheck_->setChecked(state_.successOnly);
    }
    showDuplicatesCheck_->setEnabled(state_.winnersOnly);
    {
        QSignalBlocker blocker(overrideFakeAttacksCheck_);
        overrideFakeAttacksCheck_->setChecked(state_.overrideMaxFakeAttacks);
    }
    {
        QSignalBlocker blocker(fakeAttacksSpin_);
        fakeAttacksSpin_->setValue(state_.maxFakeAttacksOverride);
    }
    fakeAttacksSpin_->setEnabled(state_.overrideMaxFakeAttacks);

    for (int i = 0; i < 3; ++i) {
        const int metricIndex = sortMetricBoxes_[i]->findData(static_cast<int>(state_.sortKeys[static_cast<size_t>(i)].metric));
        {
            QSignalBlocker blocker(sortMetricBoxes_[i]);
            sortMetricBoxes_[i]->setCurrentIndex(metricIndex >= 0 ? metricIndex : 0);
        }
        {
            QSignalBlocker blocker(sortAscendingChecks_[i]);
            sortAscendingChecks_[i]->setChecked(state_.sortKeys[static_cast<size_t>(i)].ascending);
        }
    }
}

void ExplorerRunsPage::refreshView()
{
    syncControls();
    refreshGroupModel();
    refreshWaveTree();
    refreshJobModel();
    refreshDetailPanel();
}

void ExplorerRunsPage::refreshGroupModel()
{
    std::vector<ExplorerRunsGroupRow> rows;
    rows.reserve(state_.groups.size());
    for (const GroupRow& row : state_.groups) {
        rows.push_back(ExplorerRunsGroupRow{
            row.rootGroupId,
            row.settingsLabel,
            row.resultsSummary,
            row.totalWaves,
            row.statusSummary
        });
    }
    groupsModel_->setRows(std::move(rows));
    restoreSelectedGroupRow();

    summaryLabel_->setText(QStringLiteral("%1 groups · %2 selected waves")
        .arg(state_.groups.size())
        .arg(state_.selectedWaves.size()));
}

void ExplorerRunsPage::refreshWaveTree()
{
    wavesModel_->clear();
    wavesModel_->setHorizontalHeaderLabels({ QStringLiteral("Wave"), QStringLiteral("Status") });

    const GroupRow* group = selectedGroup();
    if (!group) {
        return;
    }

    std::unordered_map<quint32, std::vector<const WaveRow*>> byTurn;
    for (const WaveRow& wave : group->waves) {
        byTurn[wave.waveTurn].push_back(&wave);
    }

    std::vector<quint32> turns;
    turns.reserve(byTurn.size());
    for (const auto& entry : byTurn) {
        turns.push_back(entry.first);
    }
    std::sort(turns.begin(), turns.end());

    for (quint32 turn : turns) {
        QStandardItem* turnItem = new QStandardItem(QStringLiteral("Turn %1").arg(turn));
        turnItem->setSelectable(false);
        QStandardItem* turnStatus = new QStandardItem(QStringLiteral("%1 waves").arg(byTurn[turn].size()));
        turnStatus->setSelectable(false);

        auto waves = byTurn[turn];
        std::sort(waves.begin(), waves.end(), [](const WaveRow* a, const WaveRow* b) {
            return a->createdAt < b->createdAt;
        });

        for (const WaveRow* wave : waves) {
            QStandardItem* waveItem = new QStandardItem(QStringLiteral("%1 Wave %2")
                .arg(waveStatusIcon(wave->hasWinner, wave->hasSuccessOutcome))
                .arg(wave->jobSetId));
            waveItem->setData(wave->jobSetId, kWaveJobSetIdUserRole);
            QStandardItem* statusItem = new QStandardItem(wave->statusSummary);
            statusItem->setData(wave->jobSetId, kWaveJobSetIdUserRole);
            turnItem->appendRow({ waveItem, statusItem });
        }

        wavesModel_->appendRow({ turnItem, turnStatus });
        wavesView_->expand(turnItem->index());
    }

    if (!state_.selectedWaves.empty()) {
        QItemSelectionModel* selection = wavesView_->selectionModel();
        if (selection) {
            selection->clearSelection();
            for (int row = 0; row < wavesModel_->rowCount(); ++row) {
                const QStandardItem* turnItem = wavesModel_->item(row, 0);
                if (!turnItem) {
                    continue;
                }
                for (int childRow = 0; childRow < turnItem->rowCount(); ++childRow) {
                    QStandardItem* waveItem = turnItem->child(childRow, 0);
                    if (!waveItem) {
                        continue;
                    }
                    const qint64 waveId = waveItem->data(kWaveJobSetIdUserRole).toLongLong();
                    if (std::find(state_.selectedWaves.begin(), state_.selectedWaves.end(), waveId) != state_.selectedWaves.end()) {
                        selection->select(waveItem->index(), QItemSelectionModel::Select | QItemSelectionModel::Rows);
                        selection->setCurrentIndex(waveItem->index(), QItemSelectionModel::NoUpdate);
                    }
                }
            }
        }
    }
}

void ExplorerRunsPage::refreshJobModel()
{
    const std::vector<JobViewRow> visible = buildVisibleSortedJobs();

    std::vector<ExplorerRunsJobRow> rows;
    rows.reserve(visible.size());
    for (const JobViewRow& row : visible) {
        QString outcome = QStringLiteral("---");
        QString predicates = QStringLiteral("---");
        QString rngSeed = QStringLiteral("---");
        if (row.state != QStringLiteral("QUEUED") && row.state != QStringLiteral("CLAIMED") && row.state != QStringLiteral("RUNNING")) {
            if (static_cast<simcore::battle::Outcome>(row.battleOutcome) == simcore::battle::Outcome::PlanMaterializeFailure) {
                outcome = QStringLiteral("M:%1").arg(QString::fromStdString(soa::battle::actions::get_materialize_err_string(static_cast<soa::battle::actions::MaterializeErr>(row.planMaterializeErr))));
            } else {
                outcome = QString::fromStdString(simcore::battle::get_outcome_string(static_cast<simcore::battle::Outcome>(row.battleOutcome)));
            }
            predicates = QStringLiteral("%1/%2%3")
                .arg(row.predPassed)
                .arg(row.predTotal)
                .arg(row.predAbortRun ? QStringLiteral(" (ABORT)") : QString());
            rngSeed = QStringLiteral("0x%1").arg(row.rngSeed, 8, 16, QChar('0')).toUpper();
        }

        rows.push_back(ExplorerRunsJobRow{
            row.jobId,
            row.state,
            outcome,
            predicates,
            row.deltaVi,
            row.fakeUsed,
            rngSeed
        });
    }

    jobsModel_->setRows(std::move(rows));
    restoreSelectedJobRow();
    jobsSummaryLabel_->setText(QStringLiteral("Visible jobs: %1 / %2").arg(visible.size()).arg(state_.jobs.size()));
}

void ExplorerRunsPage::refreshDetailPanel()
{
    const bool canTrigger = selectedJobCanTrigger();
    triggerButton_->setEnabled(canTrigger);
    triggerHintLabel_->setText(canTrigger
        ? QStringLiteral("Selected job can enqueue the next wave.")
        : QStringLiteral("Select a successful next-turn winner job with an output savestate to enable triggering."));

    blueprintText_->setPlainText(state_.blueprintInfo.isEmpty() ? QStringLiteral("(select a job to view blueprint info)") : state_.blueprintInfo);
    progressText_->setPlainText(state_.progressLog.isEmpty() ? QStringLiteral("(no progress)") : state_.progressLog);
    resultsText_->setPlainText(state_.resultsLog.isEmpty() ? QStringLiteral("(no results)") : state_.resultsLog);
}

void ExplorerRunsPage::setStatusMessage(const QString& text, bool error)
{
    inlineMessageLabel_->setText(text);
    inlineMessageLabel_->setProperty("error", error);
    inlineMessageLabel_->style()->unpolish(inlineMessageLabel_);
    inlineMessageLabel_->style()->polish(inlineMessageLabel_);
}

void ExplorerRunsPage::kickGroupsFetch()
{
    if (state_.groupsInFlight) {
        return;
    }

    state_.groupsInFlight = true;
    state_.groupsFuture = std::async(std::launch::async, [this]() {
        return buildGroups();
    });
}

void ExplorerRunsPage::kickJobsFetch()
{
    if (state_.jobsInFlight || state_.selectedWaves.empty()) {
        return;
    }

    const std::vector<qint64> selectedWaves = state_.selectedWaves;
    state_.jobsInFlight = true;
    state_.jobsFuture = std::async(std::launch::async, [this, selectedWaves]() {
        return buildJobsForWaves(selectedWaves);
    });
}

void ExplorerRunsPage::kickDetailsFetch(qint64 jobId)
{
    if (jobId <= 0) {
        return;
    }

    state_.detailsInFlight = true;
    state_.resultsFuture = std::async(std::launch::async, [this, jobId]() {
        return fetchResultsIniText(jobId);
    });
    state_.progressFuture = std::async(std::launch::async, [this, jobId]() {
        return buildProgressLog(jobId);
    });
    state_.blueprintFuture = std::async(std::launch::async, [this, jobId]() {
        return buildBlueprintInfo(jobId);
    });
}

void ExplorerRunsPage::consumeFutures()
{
    bool changed = false;

    if (state_.groupsInFlight && state_.groupsFuture.valid() && state_.groupsFuture.wait_for(0ms) == std::future_status::ready) {
        state_.groups = state_.groupsFuture.get();
        state_.groupsInFlight = false;
        changed = true;

        auto hasGroup = [this]() {
            return std::any_of(state_.groups.begin(), state_.groups.end(), [this](const GroupRow& row) { return row.rootGroupId == state_.selectedRoot; });
        };
        if (!hasGroup()) {
            state_.selectedRoot = state_.groups.empty() ? -1 : state_.groups.front().rootGroupId;
            state_.selectedWaves.clear();
            state_.selectedJob = -1;
            state_.jobs.clear();
            state_.blueprintInfo.clear();
            state_.progressLog.clear();
            state_.resultsLog.clear();
        }
        refreshGroupModel();
        refreshWaveTree();
        selectFirstWaveIfNeeded();
    }

    if (state_.jobsInFlight && state_.jobsFuture.valid() && state_.jobsFuture.wait_for(0ms) == std::future_status::ready) {
        state_.jobs = state_.jobsFuture.get();
        state_.jobsInFlight = false;
        changed = true;

        const auto found = std::find_if(state_.jobs.begin(), state_.jobs.end(), [this](const JobViewRow& row) { return row.jobId == state_.selectedJob; });
        if (found == state_.jobs.end()) {
            state_.selectedJob = -1;
            state_.blueprintInfo.clear();
            state_.progressLog.clear();
            state_.resultsLog.clear();
        }
        refreshJobModel();
        refreshDetailPanel();
    }

    if (state_.detailsInFlight
        && state_.resultsFuture.valid() && state_.resultsFuture.wait_for(0ms) == std::future_status::ready
        && state_.progressFuture.valid() && state_.progressFuture.wait_for(0ms) == std::future_status::ready
        && state_.blueprintFuture.valid() && state_.blueprintFuture.wait_for(0ms) == std::future_status::ready) {
        const std::optional<QString> results = state_.resultsFuture.get();
        state_.progressLog = state_.progressFuture.get();
        state_.blueprintInfo = state_.blueprintFuture.get();
        state_.resultsLog = results.has_value() ? *results : QStringLiteral("(no results)");
        state_.detailsInFlight = false;
        changed = true;
        refreshDetailPanel();
    }

    if (state_.autoRefresh && !state_.groupsInFlight) {
        static QDateTime lastRefresh = QDateTime::currentDateTimeUtc();
        if (lastRefresh.secsTo(QDateTime::currentDateTimeUtc()) >= state_.refreshSeconds) {
            lastRefresh = QDateTime::currentDateTimeUtc();
            kickGroupsFetch();
        }
        if (changed) {
            lastRefresh = QDateTime::currentDateTimeUtc();
        }
    }
}

std::vector<ExplorerRunsPage::GroupRow> ExplorerRunsPage::buildGroups() const
{
    JobSetsListScope scope{};
    scope.program_kind = simcore::PK_BattleSingleTurnRunner;
    auto page = JobSetsRepo::ListRecentAsync(scope, std::nullopt, 400).get();
    if (!page.ok) {
        return {};
    }

    std::unordered_map<qint64, GroupRow> groupMap;
    for (const JobSetLite& lite : page.value.items) {
        auto js = JobSetsRepo::Get(lite.job_set_id);
        if (!js.ok) {
            continue;
        }

        const WaveMeta meta = parseWaveMeta(js.value.meta_text);
        const qint64 rootId = resolveRoot(js.value);

        GroupRow& group = groupMap[rootId];
        group.rootGroupId = rootId;
        if (group.createdAt == 0 || js.value.created_at < group.createdAt) {
            group.createdAt = js.value.created_at;
        }

        QString settingsLabel = QStringLiteral("settings_id=%1").arg(meta.settingsId);
        if (!meta.settingsName.empty()) {
            settingsLabel = QString::fromStdString(meta.settingsName);
        }
        group.settingsLabel = settingsLabel;

        auto jobs = JobsRepo::GetByJobSet(js.value.job_set_id);
        QString status = jobs.ok ? summarizeStates(jobs.value) : QStringLiteral("(error)");
        bool hasSuccess = false;
        bool hasWinner = false;
        QString resultsSummary;
        if (jobs.ok && !jobs.value.empty()) {
            std::vector<qint64> ids;
            ids.reserve(jobs.value.size());
            for (const JobRow& job : jobs.value) {
                ids.push_back(job.job_id);
            }
            const auto resultMap = loadJobResultsMap(ids);
            std::unordered_map<quint32, bool> turnHasSuccess;
            turnHasSuccess[meta.waveTurn] = false;
            for (const JobRow& job : jobs.value) {
                hasWinner = hasWinner || isWinnerState(QString::fromStdString(job.state));
                const auto it = resultMap.find(job.job_id);
                if (it != resultMap.end() && it->second.successOutcome) {
                    hasSuccess = true;
                    turnHasSuccess[meta.waveTurn] = true;
                }
            }
            resultsSummary = turnHasSuccess[meta.waveTurn] ? QStringLiteral("●") : QStringLiteral("○");
        }

        group.waves.push_back(WaveRow{ js.value.job_set_id, js.value.created_at, meta.waveTurn, status, hasWinner, hasSuccess });
        group.hasSuccessOutcome = group.hasSuccessOutcome || hasSuccess;
    }

    std::vector<GroupRow> groups;
    groups.reserve(groupMap.size());
    for (auto& entry : groupMap) {
        GroupRow& group = entry.second;
        std::sort(group.waves.begin(), group.waves.end(), [](const WaveRow& a, const WaveRow& b) {
            if (a.waveTurn != b.waveTurn) {
                return a.waveTurn < b.waveTurn;
            }
            return a.createdAt < b.createdAt;
        });
        group.totalWaves = static_cast<int>(group.waves.size());
        group.statusSummary = group.waves.empty() ? QString() : group.waves.back().statusSummary;

        std::unordered_map<quint32, bool> turnHasSuccess;
        for (const WaveRow& wave : group.waves) {
            turnHasSuccess[wave.waveTurn] = turnHasSuccess[wave.waveTurn] || wave.hasSuccessOutcome;
        }
        std::vector<quint32> turns;
        turns.reserve(turnHasSuccess.size());
        for (const auto& summary : turnHasSuccess) {
            turns.push_back(summary.first);
        }
        std::sort(turns.begin(), turns.end());
        QStringList icons;
        for (quint32 turn : turns) {
            icons.append(turnHasSuccess[turn] ? QStringLiteral("●") : QStringLiteral("○"));
        }
        group.resultsSummary = icons.join(QStringLiteral(" "));
        groups.push_back(group);
    }

    std::sort(groups.begin(), groups.end(), [](const GroupRow& a, const GroupRow& b) {
        return a.createdAt > b.createdAt;
    });
    return groups;
}

std::vector<ExplorerRunsPage::JobViewRow> ExplorerRunsPage::buildJobsForSelectedWaves() const
{
    return buildJobsForWaves(state_.selectedWaves);
}

std::vector<ExplorerRunsPage::JobViewRow> ExplorerRunsPage::buildJobsForWaves(const std::vector<qint64>& waveJobSetIds) const
{
    std::vector<JobViewRow> out;
    for (qint64 waveJobSetId : waveJobSetIds) {
        auto jobs = JobsRepo::GetByJobSet(waveJobSetId);
        if (!jobs.ok) {
            continue;
        }

        std::vector<qint64> ids;
        ids.reserve(jobs.value.size());
        for (const JobRow& job : jobs.value) {
            ids.push_back(job.job_id);
        }
        const auto resultMap = loadJobResultsMap(ids);

        for (const JobRow& job : jobs.value) {
            JobResultSummary summary{};
            const auto it = resultMap.find(job.job_id);
            if (it != resultMap.end()) {
                summary = it->second;
            }
            out.push_back(JobViewRow{
                job.job_id,
                QString::fromStdString(job.state),
                summary.fakeUsed,
                summary.deltaVi,
                summary.viStart,
                summary.viEnd,
                summary.rngSeed,
                summary.battleOutcome,
                summary.planMaterializeErr,
                summary.predPassed,
                summary.predTotal,
                summary.predAbortRun,
                summary.hasResults
            });
        }
    }
    return out;
}

std::unordered_map<qint64, ExplorerRunsPage::JobResultSummary> ExplorerRunsPage::loadJobResultsMap(const std::vector<qint64>& ids) const
{
    std::unordered_map<qint64, JobResultSummary> out;
    if (ids.empty()) {
        return out;
    }

    auto res = JobEventsRepo::GetLatestPayloadByJobs(ids, "RESULTS");
    if (!res.ok) {
        return out;
    }

    for (const JobEventsRepo::JobIdPayload& row : res.value) {
        if (!row.payload.has_value()) {
            continue;
        }
        IniDoc ini = IniDoc::parse(*row.payload);
        if (!ini.has_section(ResultsIni::SECTION_NAME)) {
            continue;
        }
        const ResultsIni results = ResultsIni::from_section(ini);
        JobResultSummary summary{};
        summary.hasResults = true;
        summary.fakeUsed = results.fake_attacks_used;
        summary.viStart = results.vi_start;
        summary.viEnd = results.vi_end;
        summary.deltaVi = results.vi_end >= results.vi_start ? (results.vi_end - results.vi_start) : 0;
        summary.rngSeed = results.rng_seed;
        summary.battleOutcome = results.battle_outcome;
        summary.planMaterializeErr = results.plan_materialize_err;
        summary.predPassed = results.pred_passed;
        summary.predTotal = results.pred_total;
        summary.predAbortRun = results.pred_abort_run;
        summary.successOutcome = isSuccessOutcome(summary.battleOutcome);
        out[row.job_id] = summary;
    }

    return out;
}

QString ExplorerRunsPage::buildProgressLog(qint64 jobId) const
{
    auto progress = JobEventsRepo::ListByJobAndKind(jobId, "PROGRESS");
    if (!progress.ok) {
        return QStringLiteral("(no progress)");
    }

    QStringList lines;
    for (const JobEventRow& row : progress.value) {
        if (row.payload.has_value()) {
            lines.append(QString::fromStdString(*row.payload));
        }
    }
    return lines.isEmpty() ? QStringLiteral("(no progress)") : lines.join(QStringLiteral("\n"));
}

QString ExplorerRunsPage::buildBlueprintInfo(qint64 jobId) const
{
    auto job = JobsRepo::Get(jobId);
    if (!job.ok) {
        return QStringLiteral("(no blueprint info)");
    }
    if (!job.value.vm_kv.has_value()) {
        return QStringLiteral("(job has no vm_kv)");
    }

    IniDoc jobIni = IniDoc::parse(*job.value.vm_kv);
    const JobIni singleTurnJob = JobIni::from_section(jobIni);

    QString initialFrame = QStringLiteral("(none)");
    if (singleTurnJob.delta_seed_id > 0) {
        auto deltaSeed = DeltaSeedRepo::Get(singleTurnJob.delta_seed_id);
        if (deltaSeed.ok && deltaSeed.value.has_value()) {
            initialFrame = QString::fromStdString(DescribeFrameCompact(deltaSeed.value->input));
        }
    }

    QString planSummary = QStringLiteral("(unknown)");
    if (singleTurnJob.plan_id > 0 && singleTurnJob.turn_index > 0) {
        soa::battle::actions::TurnPlan turnPlan{ .fake_attack_count = singleTurnJob.fake_attacks_this_turn };
        auto actors = BattlePlanTurnRepo::ListActorsByPlan(singleTurnJob.plan_id, static_cast<int32_t>(singleTurnJob.turn_index - 1));
        if (actors.ok) {
            for (const auto& actor : actors.value) {
                auto atom = BattlePlanAtomRepo::Get(actor.atom_id);
                if (!atom.ok) {
                    continue;
                }
                soa::battle::actions::ActionPlan actionPlan{
                    .actor_slot = static_cast<uint8_t>(atom.value.actor_slot),
                    .macro = static_cast<soa::battle::actions::BattleAction>(atom.value.action_type)
                };
                if (atom.value.target_slot >= -1 && atom.value.target_slot < 12) {
                    actionPlan.params.target_slot = static_cast<uint8_t>(atom.value.target_slot);
                }
                if (atom.value.param_item_id >= 0) {
                    actionPlan.params.item_id = static_cast<uint16_t>(atom.value.param_item_id);
                }
                turnPlan.spec.push_back(std::move(actionPlan));
            }
        }
        planSummary = QString::fromStdString(soa::battle::actions::get_turn_plan_summary(turnPlan));
    }

    return QStringLiteral("delta_seed_id: %1\ninitial_frame_input: %2\nbattle_action_plan: %3")
        .arg(singleTurnJob.delta_seed_id)
        .arg(initialFrame)
        .arg(planSummary);
}

std::optional<QString> ExplorerRunsPage::fetchResultsIniText(qint64 jobId) const
{
    auto results = DataService::FetchJobResultsIniAsync(jobId).get();
    if (!results.ok) {
        return std::nullopt;
    }
    return QString::fromStdString(results.value.to_string_sorted());
}

ExplorerRunsPage::WaveMeta ExplorerRunsPage::parseWaveMeta(const std::optional<std::string>& text) const
{
    WaveMeta meta{};
    if (!text.has_value() || text->empty()) {
        return meta;
    }

    IniDoc ini = IniDoc::parse(*text);
    constexpr const char* section = "BattleSingleTurn.WaveMeta";
    if (!ini.has_section(section)) {
        return meta;
    }

    IniKV kv = ini.section_kv(section);
    meta.rootGroupId = kv.get_i64("root_group_id", -1);
    meta.waveTurn = kv.get_u32("wave_turn", 1);
    meta.settingsId = kv.get_i64("settings_id", -1);
    meta.seedProbeId = kv.get_i64("seed_probe_id", -1);
    meta.settingsName = kv.get("settings_name", "");
    meta.tasMovieId = kv.get_i64("tas_movie_id", -1);
    return meta;
}

qint64 ExplorerRunsPage::resolveRoot(const JobSetRow& js) const
{
    const WaveMeta meta = parseWaveMeta(js.meta_text);
    if (meta.rootGroupId > 0) {
        return meta.rootGroupId;
    }

    qint64 current = js.job_set_id;
    while (true) {
        auto parent = JobSetsRepo::GetParent(current);
        if (!parent.ok || !parent.value.has_value()) {
            break;
        }
        current = *parent.value;
    }
    return current;
}

QString ExplorerRunsPage::summarizeStates(const std::vector<JobRow>& jobs) const
{
    int queued = 0;
    int running = 0;
    int failures = 0;
    int winners = 0;
    int duplicates = 0;
    for (const JobRow& job : jobs) {
        if (job.state == "QUEUED" || job.state == "CLAIMED") {
            ++queued;
        } else if (job.state == "RUNNING") {
            ++running;
        } else if (job.state == "FAILED" || job.state == "CANCELED") {
            ++failures;
        } else if (job.state == "SUCCEEDED_WINNER") {
            ++winners;
        } else if (job.state == "SUCCEEDED_DUPLICATE") {
            ++duplicates;
        }
    }

    return QStringLiteral("Q:%1 R:%2 F:%3 W:%4 D:%5")
        .arg(queued)
        .arg(running)
        .arg(failures)
        .arg(winners)
        .arg(duplicates);
}

bool ExplorerRunsPage::isSuccessOutcome(quint32 battleOutcome) const
{
    return battleOutcome == static_cast<quint32>(simcore::battle::Outcome::ReachedNextTurn);
}

bool ExplorerRunsPage::isWinnerState(const QString& state) const
{
    return state == QStringLiteral("SUCCEEDED_WINNER");
}

bool ExplorerRunsPage::isDuplicateState(const QString& state) const
{
    return state == QStringLiteral("SUCCEEDED_DUPLICATE");
}

QString ExplorerRunsPage::waveStatusIcon(bool hasWinner, bool hasSuccessOutcome) const
{
    if (hasWinner) {
        return QStringLiteral("●");
    }
    if (hasSuccessOutcome) {
        return QStringLiteral("◐");
    }
    return QStringLiteral("○");
}

QString ExplorerRunsPage::sortMetricLabel(SortMetric metric) const
{
    switch (metric) {
    case SortMetric::PredicatesPassed: return QStringLiteral("Predicates Passed");
    case SortMetric::DeltaVI: return QStringLiteral("Delta VI");
    case SortMetric::FakeAttacks: return QStringLiteral("Fake Attacks");
    case SortMetric::RngSeed: return QStringLiteral("RNG Seed");
    case SortMetric::JobId:
    default: return QStringLiteral("Job ID");
    }
}

int ExplorerRunsPage::compareMetric(const JobViewRow& a, const JobViewRow& b, SortMetric metric) const
{
    switch (metric) {
    case SortMetric::PredicatesPassed:
        return a.predPassed < b.predPassed ? -1 : (a.predPassed > b.predPassed ? 1 : 0);
    case SortMetric::DeltaVI:
        return a.deltaVi < b.deltaVi ? -1 : (a.deltaVi > b.deltaVi ? 1 : 0);
    case SortMetric::FakeAttacks:
        return a.fakeUsed < b.fakeUsed ? -1 : (a.fakeUsed > b.fakeUsed ? 1 : 0);
    case SortMetric::RngSeed:
        return a.rngSeed < b.rngSeed ? -1 : (a.rngSeed > b.rngSeed ? 1 : 0);
    case SortMetric::JobId:
    default:
        return a.jobId < b.jobId ? -1 : (a.jobId > b.jobId ? 1 : 0);
    }
}

std::vector<ExplorerRunsPage::JobViewRow> ExplorerRunsPage::buildVisibleSortedJobs() const
{
    std::vector<JobViewRow> out;
    out.reserve(state_.jobs.size());
    for (const JobViewRow& job : state_.jobs) {
        const bool winner = isWinnerState(job.state);
        const bool duplicate = isDuplicateState(job.state);
        if (state_.winnersOnly) {
            if (!winner && !(state_.showDuplicates && duplicate)) {
                continue;
            }
        }
        if (state_.successOnly && (!job.hasResults || !isSuccessOutcome(job.battleOutcome))) {
            continue;
        }
        out.push_back(job);
    }

    std::sort(out.begin(), out.end(), [this](const JobViewRow& a, const JobViewRow& b) {
        for (const SortKey& key : state_.sortKeys) {
            const int cmp = compareMetric(a, b, key.metric);
            if (cmp == 0) {
                continue;
            }
            return key.ascending ? (cmp < 0) : (cmp > 0);
        }
        return a.jobId < b.jobId;
    });
    return out;
}

const ExplorerRunsPage::GroupRow* ExplorerRunsPage::selectedGroup() const
{
    for (const GroupRow& group : state_.groups) {
        if (group.rootGroupId == state_.selectedRoot) {
            return &group;
        }
    }
    return nullptr;
}

bool ExplorerRunsPage::selectedJobCanTrigger() const
{
    if (state_.selectedJob <= 0) {
        return false;
    }
    auto job = JobsRepo::Get(state_.selectedJob);
    if (!job.ok) {
        return false;
    }
    auto resultsPayload = JobEventsRepo::GetLatestPayload(state_.selectedJob, "RESULTS");
    if (!resultsPayload.ok || !resultsPayload.value.has_value()) {
        return false;
    }

    IniDoc ini = IniDoc::parse(*resultsPayload.value);
    if (!ini.has_section(ResultsIni::SECTION_NAME)) {
        return false;
    }
    ResultsIni results = ResultsIni::from_section(ini);
    return results.output_savestate_id > 0 && results.battle_outcome == static_cast<quint32>(simcore::battle::Outcome::ReachedNextTurn);
}

void ExplorerRunsPage::triggerNextWave()
{
    if (state_.selectedJob <= 0) {
        return;
    }

    std::optional<quint32> overrideValue;
    if (state_.overrideMaxFakeAttacks) {
        overrideValue = static_cast<quint32>(state_.maxFakeAttacksOverride);
    }

    const auto result = BattleSingleTurnRunDBCodec::enqueue_next_wave_from_job(state_.selectedJob, false, overrideValue);
    if (result.ok) {
        setStatusMessage(QStringLiteral("Queued next wave job set %1.").arg(result.value));
        kickGroupsFetch();
    } else {
        setStatusMessage(QStringLiteral("Unable to queue next wave: %1").arg(QString::fromStdString(result.error.message)), true);
    }
}

void ExplorerRunsPage::restoreSelectedGroupRow()
{
    for (int row = 0; row < groupsModel_->rowCount(); ++row) {
        const ExplorerRunsGroupRow* item = groupsModel_->rowAt(row);
        if (item && item->rootGroupId == state_.selectedRoot) {
            selectFlatRow(groupsView_, row);
            return;
        }
    }
}

void ExplorerRunsPage::restoreSelectedJobRow()
{
    for (int row = 0; row < jobsModel_->rowCount(); ++row) {
        const ExplorerRunsJobRow* item = jobsModel_->rowAt(row);
        if (item && item->jobId == state_.selectedJob) {
            selectFlatRow(jobsView_, row);
            return;
        }
    }
}

void ExplorerRunsPage::selectFirstWaveIfNeeded()
{
    if (!state_.selectedWaves.empty()) {
        return;
    }

    const GroupRow* group = selectedGroup();
    if (!group || group->waves.empty()) {
        return;
    }

    state_.selectedWaves = { group->waves.front().jobSetId };
    refreshWaveTree();
    kickJobsFetch();
}

std::vector<qint64> ExplorerRunsPage::selectedWaveIdsFromTree() const
{
    std::vector<qint64> ids;
    if (!wavesView_->selectionModel()) {
        return ids;
    }

    const QModelIndexList rows = wavesView_->selectionModel()->selectedRows(0);
    ids.reserve(rows.size());
    for (const QModelIndex& index : rows) {
        const QVariant value = index.data(kWaveJobSetIdUserRole);
        if (value.isValid()) {
            ids.push_back(value.toLongLong());
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}
