#include "ExplorerRunsPage.h"

#include "ExplorerRunsGroupTableModel.h"
#include "ExplorerRunsGroupTableView.h"
#include "ExplorerRunsJobsTableModel.h"
#include "ExplorerRunsJobsTableView.h"
#include "ExplorerRunsTurnInputsDialog.h"
#include "GUI/Widgets/ScrollBarStabilizer.h"

#include "Core/Input/SoaBattle/PlanWriter.h"
#include "DB/ProgramDB/BattleSingleTurnRunDBCodec.h"
#include "DB/Scheduling/JobEventsRepo.h"
#include "DB/Scheduling/JobsRepo.h"
#include "Phases/Programs/BattleRunner/BattleOutcome.h"
#include "Utils/IniDoc.h"

#include <QtCore/QDateTime>
#include <QtCore/QItemSelectionModel>
#include <QtCore/QMetaObject>
#include <QtCore/QScopedValueRollback>
#include <QtCore/QSettings>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
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
#include <QtWidgets/QMenu>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTreeView>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <optional>
#include <unordered_map>

using namespace simcore::db;
using namespace simcore::db::codec::battle::singleturn;

namespace {
constexpr int kWaveJobSetIdUserRole = Qt::UserRole + 1;
constexpr auto kSettingsGroup = "ExplorerRunsPage";
constexpr auto kWinnersOnlyKey = "winners_only";
constexpr auto kShowDuplicatesKey = "show_duplicates";
constexpr auto kSuccessOnlyKey = "success_only";
constexpr auto kSortMetricPrefix = "sort_metric_";
constexpr auto kSortAscendingPrefix = "sort_ascending_";
constexpr auto kOverrideFakeAttacksKey = "override_fake_attacks";
constexpr auto kMaxFakeAttacksKey = "max_fake_attacks";

bool isTurnInputEligibleState(const QString& state)
{
    return state == QStringLiteral("SUCCEEDED")
        || state == QStringLiteral("SUCCEEDED_WINNER")
        || state == QStringLiteral("SUCCEEDED_DUPLICATE");
}

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
    , coordinator_(new ExplorerRunsCoordinator(this))
    , groupsModel_(new ExplorerRunsGroupTableModel(this))
    , jobsModel_(new ExplorerRunsJobsTableModel(this))
    , wavesModel_(new QStandardItemModel(this))
{
    createWidgets();
    loadFilterSettings();
    wireSignals();
    syncControls();
    refreshView();
    coordinator_->requestGroupsRefresh();
}

ExplorerRunsPage::~ExplorerRunsPage() = default;

void ExplorerRunsPage::createWidgets()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(10);

    QFrame* toolbarPanel = new QFrame(this);
    toolbarPanel->setObjectName("jobsToolbarPanel");
    QHBoxLayout* toolbarLayout = new QHBoxLayout(toolbarPanel);
    toolbarLayout->setContentsMargins(12, 10, 12, 10);
    toolbarLayout->setSpacing(10);

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
    groupsLayout->setContentsMargins(12, 12, 12, 12);
    groupsLayout->addWidget(new QLabel(QStringLiteral("Run Groups"), groupsPanel));
    groupsView_ = new ExplorerRunsGroupTableView(groupsPanel);
    groupsView_->attachModel(groupsModel_);
    groupsLayout->addWidget(groupsView_, 1);

    QFrame* wavesPanel = new QFrame(topSplitter);
    wavesPanel->setObjectName("jobsSurfacePanel");
    QVBoxLayout* wavesLayout = new QVBoxLayout(wavesPanel);
    wavesLayout->setContentsMargins(12, 12, 12, 12);
    wavesLayout->addWidget(new QLabel(QStringLiteral("Wave Tree"), wavesPanel));
    wavesView_ = new QTreeView(wavesPanel);
    wavesView_->setRootIsDecorated(true);
    wavesView_->setItemsExpandable(true);
    wavesView_->setIndentation(10);
    wavesView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    wavesModel_->setHorizontalHeaderLabels({ QStringLiteral("Wave"), QStringLiteral("Status") });
    wavesView_->setModel(wavesModel_);
    wavesView_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    wavesView_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    wavesLayout->addWidget(wavesView_, 1);

    QFrame* jobsPanel = new QFrame(bottomSplitter);
    jobsPanel->setObjectName("jobsSurfacePanel");
    QVBoxLayout* jobsLayout = new QVBoxLayout(jobsPanel);
    jobsLayout->setContentsMargins(12, 12, 12, 12);

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
    jobsView_->setContextMenuPolicy(Qt::CustomContextMenu);
    jobsLayout->addWidget(jobsView_, 1);

    QFrame* detailsPanel = new QFrame(bottomSplitter);
    detailsPanel->setObjectName("jobsSurfacePanel");
    QVBoxLayout* detailsLayout = new QVBoxLayout(detailsPanel);
    detailsLayout->setContentsMargins(12, 12, 12, 12);
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
    connect(coordinator_, &ExplorerRunsCoordinator::stateChanged, this, &ExplorerRunsPage::handleCoordinatorStateChanged);
    connect(refreshButton_, &QPushButton::clicked, this, [this]() {
        coordinator_->requestGroupsRefresh();
        if (!state_.selectedWaves.empty()) {
            coordinator_->requestJobsRefresh(state_.selectedWaves);
        }
        if (state_.selectedJob > 0) {
            coordinator_->requestDetailsRefresh(state_.selectedJob);
        }
    });
    connect(autoRefreshCheck_, &QCheckBox::toggled, coordinator_, &ExplorerRunsCoordinator::setAutoRefreshEnabled);
    connect(refreshSecondsSpin_, qOverload<int>(&QSpinBox::valueChanged), coordinator_, &ExplorerRunsCoordinator::setRefreshSeconds);

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
        coordinator_->clearJobs();
        coordinator_->clearDetails();
        refreshWaveTree();
        refreshJobModel();
        refreshDetailPanel();
        selectFirstWaveIfNeeded();
    });

    connect(wavesView_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this](const QItemSelection&, const QItemSelection&) {
        if (refreshingWaveTree_) {
            return;
        }
        const std::vector<qint64> selected = selectedWaveIdsFromTree();
        if (selected == state_.selectedWaves) {
            return;
        }
        state_.selectedWaves = selected;
        state_.selectedJob = -1;
        coordinator_->clearJobs();
        coordinator_->clearDetails();
        refreshJobModel();
        refreshDetailPanel();
        if (!state_.selectedWaves.empty()) {
            coordinator_->requestJobsRefresh(state_.selectedWaves);
        }
    });

    auto refreshJobs = [this]() { refreshJobModel(); };
    connect(winnersOnlyCheck_, &QCheckBox::toggled, this, [this, refreshJobs](bool checked) {
        state_.winnersOnly = checked;
        if (!checked) {
            state_.showDuplicates = false;
        }
        persistFilterSettings();
        syncControls();
        refreshJobs();
    });
    connect(showDuplicatesCheck_, &QCheckBox::toggled, this, [this, refreshJobs](bool checked) {
        state_.showDuplicates = checked;
        persistFilterSettings();
        refreshJobs();
    });
    connect(successOnlyCheck_, &QCheckBox::toggled, this, [this, refreshJobs](bool checked) {
        state_.successOnly = checked;
        persistFilterSettings();
        refreshJobs();
    });

    for (int i = 0; i < 3; ++i) {
        connect(sortMetricBoxes_[i], qOverload<int>(&QComboBox::currentIndexChanged), this, [this, i, refreshJobs](int) {
            state_.sortKeys[static_cast<size_t>(i)].metric = static_cast<SortMetric>(sortMetricBoxes_[i]->currentData().toInt());
            persistFilterSettings();
            refreshJobs();
        });
        connect(sortAscendingChecks_[i], &QCheckBox::toggled, this, [this, i, refreshJobs](bool checked) {
            state_.sortKeys[static_cast<size_t>(i)].ascending = checked;
            persistFilterSettings();
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
        coordinator_->requestDetailsRefresh(row->jobId);
    });
    connect(jobsView_, &ExplorerRunsJobsTableView::customContextMenuRequested, this, &ExplorerRunsPage::showJobsContextMenu);

    connect(overrideFakeAttacksCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        state_.overrideMaxFakeAttacks = checked;
        persistFilterSettings();
        syncControls();
    });
    connect(fakeAttacksSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        state_.maxFakeAttacksOverride = value;
        persistFilterSettings();
    });
    connect(triggerButton_, &QPushButton::clicked, this, &ExplorerRunsPage::triggerNextWave);
}

void ExplorerRunsPage::syncControls()
{
    {
        QSignalBlocker blocker(autoRefreshCheck_);
        autoRefreshCheck_->setChecked(coordinator_->autoRefreshEnabled());
    }
    {
        QSignalBlocker blocker(refreshSecondsSpin_);
        refreshSecondsSpin_->setValue(coordinator_->refreshSeconds());
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
        {
            QSignalBlocker blocker(sortMetricBoxes_[i]);
            const int metricIndex = sortMetricBoxes_[i]->findData(static_cast<int>(state_.sortKeys[static_cast<size_t>(i)].metric));
            if (metricIndex >= 0) {
                sortMetricBoxes_[i]->setCurrentIndex(metricIndex);
            }
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
    const ItemViewScrollSnapshot scrollSnapshot = captureItemViewScrollSnapshot(groupsView_);
    std::vector<ExplorerRunsGroupRow> rows;
    rows.reserve(coordinator_->groups().size());
    for (const ExplorerRunsCoordinator::GroupRow& row : coordinator_->groups()) {
        rows.push_back(ExplorerRunsGroupRow{ row.rootGroupId, row.settingsLabel, row.resultsSummary, row.totalWaves, row.statusSummary });
    }
    groupsModel_->setRows(std::move(rows));
    restoreSelectedGroupRow();
    restoreItemViewScrollSnapshot(groupsView_, scrollSnapshot);

    summaryLabel_->setText(QStringLiteral("%1 groups · %2 selected waves")
        .arg(coordinator_->groups().size())
        .arg(state_.selectedWaves.size()));
}

void ExplorerRunsPage::refreshWaveTree()
{
    const ItemViewScrollSnapshot scrollSnapshot = captureItemViewScrollSnapshot(wavesView_);
    const auto restoreWaveTreeScroll = [this, scrollSnapshot]() {
        restoreItemViewScrollSnapshot(wavesView_, scrollSnapshot);
        QMetaObject::invokeMethod(wavesView_, [this, scrollSnapshot]() {
            restoreItemViewScrollSnapshot(wavesView_, scrollSnapshot);
        }, Qt::QueuedConnection);
    };
    QScopedValueRollback<bool> refreshingWaveTreeGuard(refreshingWaveTree_, true);
    QItemSelectionModel* selection = wavesView_->selectionModel();
    const std::optional<QSignalBlocker> selectionBlocker = selection
        ? std::optional<QSignalBlocker>(std::in_place, selection)
        : std::nullopt;

    wavesModel_->clear();
    wavesModel_->setHorizontalHeaderLabels({ QStringLiteral("Wave"), QStringLiteral("Status") });

    const ExplorerRunsCoordinator::GroupRow* group = selectedGroup();
    if (!group) {
        restoreWaveTreeScroll();
        return;
    }

    std::unordered_map<quint32, std::vector<const ExplorerRunsCoordinator::WaveRow*>> byTurn;
    for (const ExplorerRunsCoordinator::WaveRow& wave : group->waves) {
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
        std::sort(waves.begin(), waves.end(), [](const auto* a, const auto* b) {
            return a->createdAt < b->createdAt;
        });

        for (const auto* wave : waves) {
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

    if (state_.selectedWaves.empty()) {
        restoreWaveTreeScroll();
        return;
    }

    if (!selection) {
        restoreWaveTreeScroll();
        return;
    }
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

    restoreWaveTreeScroll();
}

void ExplorerRunsPage::refreshJobModel()
{
    const ItemViewScrollSnapshot scrollSnapshot = captureItemViewScrollSnapshot(jobsView_);
    const std::vector<ExplorerRunsCoordinator::JobViewRow> visible = buildVisibleSortedJobs();

    std::vector<ExplorerRunsJobRow> rows;
    rows.reserve(visible.size());
    for (const ExplorerRunsCoordinator::JobViewRow& row : visible) {
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

        rows.push_back(ExplorerRunsJobRow{ row.jobId, row.state, outcome, predicates, row.deltaVi, row.fakeUsed, rngSeed });
    }

    jobsModel_->setRows(std::move(rows));
    restoreSelectedJobRow();
    restoreItemViewScrollSnapshot(jobsView_, scrollSnapshot);
    jobsSummaryLabel_->setText(QStringLiteral("Visible jobs: %1 / %2").arg(visible.size()).arg(coordinator_->jobs().size()));
}

void ExplorerRunsPage::refreshDetailPanel()
{
    const bool canTrigger = selectedJobCanTrigger();
    triggerButton_->setEnabled(canTrigger);
    triggerHintLabel_->setText(canTrigger
        ? QStringLiteral("Selected job can enqueue the next wave.")
        : QStringLiteral("Select a successful next-turn winner job with an output savestate to enable triggering."));

    const ExplorerRunsCoordinator::DetailBundle& details = coordinator_->details();
    const ScrollAreaScrollSnapshot blueprintScrollSnapshot = captureScrollAreaScrollSnapshot(blueprintText_);
    const ScrollAreaScrollSnapshot progressScrollSnapshot = captureScrollAreaScrollSnapshot(progressText_);
    const ScrollAreaScrollSnapshot resultsScrollSnapshot = captureScrollAreaScrollSnapshot(resultsText_);
    blueprintText_->setPlainText(details.blueprintInfo.isEmpty() ? QStringLiteral("(select a job to view blueprint info)") : details.blueprintInfo);
    progressText_->setPlainText(details.progressLog.isEmpty() ? QStringLiteral("(no progress)") : details.progressLog);
    resultsText_->setPlainText(details.resultsLog.isEmpty() ? QStringLiteral("(no results)") : details.resultsLog);
    restoreScrollAreaScrollSnapshot(blueprintText_, blueprintScrollSnapshot);
    restoreScrollAreaScrollSnapshot(progressText_, progressScrollSnapshot);
    restoreScrollAreaScrollSnapshot(resultsText_, resultsScrollSnapshot);
}

void ExplorerRunsPage::setStatusMessage(const QString& text, bool error)
{
    inlineMessageLabel_->setText(text);
    inlineMessageLabel_->setProperty("error", error);
    inlineMessageLabel_->style()->unpolish(inlineMessageLabel_);
    inlineMessageLabel_->style()->polish(inlineMessageLabel_);
    maybeEmitStatusToast(text, error);
}

void ExplorerRunsPage::maybeEmitStatusToast(const QString& text, bool error)
{
    if (text.isEmpty()) {
        return;
    }

    const StatusToast::Severity severity = error ? StatusToast::Severity::Error : StatusToast::Severity::Info;
    const QString signature = QStringLiteral("%1|%2").arg(static_cast<int>(severity)).arg(text);
    if (signature == lastToastSignature_) {
        return;
    }

    lastToastSignature_ = signature;
    emit statusToastRequested(StatusToast{ severity, text, QString(), 1, QDateTime{}, 4000 });
}

void ExplorerRunsPage::handleCoordinatorStateChanged()
{
    auto hasGroup = [this]() {
        return std::any_of(coordinator_->groups().begin(), coordinator_->groups().end(), [this](const auto& row) {
            return row.rootGroupId == state_.selectedRoot;
        });
    };
    if (!hasGroup()) {
        state_.selectedRoot = coordinator_->groups().empty() ? -1 : coordinator_->groups().front().rootGroupId;
        state_.selectedWaves.clear();
        state_.selectedJob = -1;
        coordinator_->clearJobs();
        coordinator_->clearDetails();
    }

    const auto found = std::find_if(coordinator_->jobs().begin(), coordinator_->jobs().end(), [this](const auto& row) {
        return row.jobId == state_.selectedJob;
    });
    if (found == coordinator_->jobs().end()) {
        state_.selectedJob = -1;
        coordinator_->clearDetails();
    }

    refreshView();
    selectFirstWaveIfNeeded();
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

int ExplorerRunsPage::compareMetric(const ExplorerRunsCoordinator::JobViewRow& a, const ExplorerRunsCoordinator::JobViewRow& b, SortMetric metric) const
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

std::vector<ExplorerRunsCoordinator::JobViewRow> ExplorerRunsPage::buildVisibleSortedJobs() const
{
    std::vector<ExplorerRunsCoordinator::JobViewRow> out;
    out.reserve(coordinator_->jobs().size());
    for (const ExplorerRunsCoordinator::JobViewRow& job : coordinator_->jobs()) {
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

    std::sort(out.begin(), out.end(), [this](const auto& a, const auto& b) {
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

const ExplorerRunsCoordinator::GroupRow* ExplorerRunsPage::selectedGroup() const
{
    for (const ExplorerRunsCoordinator::GroupRow& group : coordinator_->groups()) {
        if (group.rootGroupId == state_.selectedRoot) {
            return &group;
        }
    }
    return nullptr;
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

    const auto result = BattleSingleTurnRunDBCodec::enqueue_next_wave_from_job(
        state_.selectedJob,
        false,
        overrideValue,
        true);
    if (result.ok) {
        setStatusMessage(QStringLiteral("Queued next wave job set %1.").arg(result.value));
        coordinator_->requestGroupsRefresh();
    } else {
        setStatusMessage(QStringLiteral("Unable to queue next wave: %1").arg(QString::fromStdString(result.error.message)), true);
    }
}

void ExplorerRunsPage::showJobsContextMenu(const QPoint& pos)
{
    if (!jobsView_) {
        return;
    }

    const QModelIndex index = jobsView_->indexAt(pos);
    if (!index.isValid()) {
        return;
    }

    const ExplorerRunsJobRow* row = jobsModel_->rowAt(index.row());
    if (!row) {
        return;
    }

    const bool isFinished = row->state == QStringLiteral("SUCCEEDED")
        || row->state == QStringLiteral("FAILED")
        || row->state == QStringLiteral("CANCELED")
        || row->state == QStringLiteral("SUPERSEDED")
        || row->state == QStringLiteral("SUCCEEDED_WINNER")
        || row->state == QStringLiteral("SUCCEEDED_DUPLICATE");

    QMenu menu(this);
    QAction* viewTurnInputsAction = menu.addAction(QStringLiteral("View Turn Inputs"));
    QAction* regurgitatePlanAction = menu.addAction(QStringLiteral("Regurgitate Battle Plan"));
    QAction* replayVisualAction = menu.addAction(QStringLiteral("Replay Visually"));
    viewTurnInputsAction->setEnabled(isTurnInputEligibleState(row->state));
    replayVisualAction->setEnabled(isFinished);
    QAction* selectedAction = menu.exec(jobsView_->viewport()->mapToGlobal(pos));
    if (selectedAction == viewTurnInputsAction && viewTurnInputsAction->isEnabled()) {
        openTurnInputsDialogForJob(*row);
    } else if (selectedAction == regurgitatePlanAction) {
        showBattlePlanDialogForJob(*row);
    } else if (selectedAction == replayVisualAction && replayVisualAction->isEnabled()) {
        emit visualReplayRequested(row->jobId);
    }
}

void ExplorerRunsPage::openTurnInputsDialogForJob(const ExplorerRunsJobRow& row)
{
    auto* dialog = new ExplorerRunsTurnInputsDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->setModal(false);
    dialog->loadForJob(row);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void ExplorerRunsPage::showBattlePlanDialogForJob(const ExplorerRunsJobRow& row)
{
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->setModal(false);
    dialog->setWindowTitle(QStringLiteral("Battle Plan for Job %1").arg(row.jobId));
    dialog->resize(800, 460);

    QVBoxLayout* layout = new QVBoxLayout(dialog);
    QPlainTextEdit* text = new QPlainTextEdit(dialog);
    text->setReadOnly(true);
    text->setPlainText(coordinator_->describeBattlePlanForJob(row.jobId));
    layout->addWidget(text);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
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

    const ExplorerRunsCoordinator::GroupRow* group = selectedGroup();
    if (!group || group->waves.empty()) {
        return;
    }

    state_.selectedWaves = { group->waves.front().jobSetId };
    refreshWaveTree();
    coordinator_->requestJobsRefresh(state_.selectedWaves);
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

void ExplorerRunsPage::loadFilterSettings()
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);

    state_.winnersOnly = settings.value(kWinnersOnlyKey, state_.winnersOnly).toBool();
    state_.showDuplicates = settings.value(kShowDuplicatesKey, state_.showDuplicates).toBool();
    state_.successOnly = settings.value(kSuccessOnlyKey, state_.successOnly).toBool();
    if (!state_.winnersOnly) {
        state_.showDuplicates = false;
    }

    for (int i = 0; i < 3; ++i) {
        const SortMetric fallbackMetric = state_.sortKeys[static_cast<size_t>(i)].metric;
        const int metricValue = settings.value(QStringLiteral("%1%2").arg(kSortMetricPrefix).arg(i), static_cast<int>(fallbackMetric)).toInt();
        const bool metricInRange = metricValue >= static_cast<int>(SortMetric::PredicatesPassed)
            && metricValue <= static_cast<int>(SortMetric::RngSeed);
        state_.sortKeys[static_cast<size_t>(i)].metric = metricInRange
            ? static_cast<SortMetric>(metricValue)
            : fallbackMetric;
        state_.sortKeys[static_cast<size_t>(i)].ascending = settings.value(
            QStringLiteral("%1%2").arg(kSortAscendingPrefix).arg(i),
            state_.sortKeys[static_cast<size_t>(i)].ascending).toBool();
    }

    state_.overrideMaxFakeAttacks = settings.value(kOverrideFakeAttacksKey, state_.overrideMaxFakeAttacks).toBool();
    state_.maxFakeAttacksOverride = settings.value(kMaxFakeAttacksKey, state_.maxFakeAttacksOverride).toInt();

    settings.endGroup();
}

void ExplorerRunsPage::persistFilterSettings() const
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);

    settings.setValue(kWinnersOnlyKey, state_.winnersOnly);
    settings.setValue(kShowDuplicatesKey, state_.showDuplicates);
    settings.setValue(kSuccessOnlyKey, state_.successOnly);
    for (int i = 0; i < 3; ++i) {
        settings.setValue(QStringLiteral("%1%2").arg(kSortMetricPrefix).arg(i), static_cast<int>(state_.sortKeys[static_cast<size_t>(i)].metric));
        settings.setValue(QStringLiteral("%1%2").arg(kSortAscendingPrefix).arg(i), state_.sortKeys[static_cast<size_t>(i)].ascending);
    }
    settings.setValue(kOverrideFakeAttacksKey, state_.overrideMaxFakeAttacks);
    settings.setValue(kMaxFakeAttacksKey, state_.maxFakeAttacksOverride);

    settings.endGroup();
}
