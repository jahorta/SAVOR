#include "GUI/Panes/BattleRunsPane/BattleRunsWidget.h"

#include "GUI/Refresh/AsyncRefreshPipeline.h"
#include "GUI/Refresh/RowUpdate.h"
#include "GUI/Widgets/ScrollBarStabilizer.h"

#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
#include <QtCore/QTimeZone>
#include <QtCore/QVariant>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace savorqt::gui {
namespace {

constexpr int kIdRole = Qt::UserRole + 1;
constexpr int kExecJobIdRole = Qt::UserRole + 2;

QString qs(const std::string& value)
{
    return QString::fromStdString(value);
}

QString formatTimestamp(std::int64_t epochMillis)
{
    if (epochMillis <= 0) {
        return QStringLiteral("--");
    }
    return QDateTime::fromMSecsSinceEpoch(epochMillis, QTimeZone::UTC)
        .toLocalTime()
        .toString(QStringLiteral("MM-dd HH:mm:ss"));
}

QString optionalInt64Text(const std::optional<std::int64_t>& value)
{
    return value.has_value() ? QString::number(*value) : QStringLiteral("--");
}

QString optionalIntText(const std::optional<int>& value)
{
    return value.has_value() ? QString::number(*value) : QStringLiteral("--");
}

QTableWidgetItem* makeItem(const QString& text)
{
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QTableWidgetItem* makeIdItem(std::int64_t id)
{
    auto* item = makeItem(QStringLiteral("#%1").arg(id));
    item->setData(kIdRole, static_cast<qint64>(id));
    return item;
}

void configureTable(QTableWidget* table)
{
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(30);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
}

QFrame* makePanel(const QString& title, QWidget* parent)
{
    auto* panel = new QFrame(parent);
    panel->setObjectName("workspaceHeroPanel");
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);
    auto* label = new QLabel(title, panel);
    label->setObjectName("panelTitle");
    layout->addWidget(label);
    return panel;
}

bool groupRowsEqual(const BattleRunsWidget::GroupRow& lhs, const BattleRunsWidget::GroupRow& rhs)
{
    return lhs.battleSetId == rhs.battleSetId
        && lhs.group == rhs.group
        && lhs.name == rhs.name
        && lhs.results == rhs.results
        && lhs.waves == rhs.waves
        && lhs.status == rhs.status;
}

bool jobRowsEqual(const BattleRunsWidget::JobRow& lhs, const BattleRunsWidget::JobRow& rhs)
{
    return lhs.jobId == rhs.jobId
        && lhs.execJobId == rhs.execJobId
        && lhs.waveId == rhs.waveId
        && lhs.state == rhs.state
        && lhs.outcome == rhs.outcome
        && lhs.predicates == rhs.predicates
        && lhs.deltaVi == rhs.deltaVi
        && lhs.fakeAttacks == rhs.fakeAttacks
        && lhs.rngSeed == rhs.rngSeed
        && lhs.selectedForAdvancement == rhs.selectedForAdvancement
        && lhs.desiredOutcome == rhs.desiredOutcome
        && lhs.finalVictoryOutcome == rhs.finalVictoryOutcome
        && lhs.advancementRank == rhs.advancementRank;
}

bool waveRowsEqual(const BattleRunsWidget::WaveRow& lhs, const BattleRunsWidget::WaveRow& rhs)
{
    return lhs.waveId == rhs.waveId
        && lhs.parentWaveId == rhs.parentWaveId
        && lhs.turnIndex == rhs.turnIndex
        && lhs.label == rhs.label
        && lhs.jobs == rhs.jobs
        && lhs.status == rhs.status
        && lhs.advancementRank == rhs.advancementRank
        && lhs.hasFailure == rhs.hasFailure
        && lhs.selected == rhs.selected;
}

bool waveTurnRowsEqual(const BattleRunsWidget::WaveTurnRow& lhs, const BattleRunsWidget::WaveTurnRow& rhs)
{
    return lhs.turnIndex == rhs.turnIndex
        && lhs.label == rhs.label
        && RowsEqual(lhs.waves, rhs.waves, waveRowsEqual);
}

void populateGroupRow(QTableWidget* table, int row, const BattleRunsWidget::GroupRow& item)
{
    table->setItem(row, 0, makeIdItem(item.battleSetId));
    table->setItem(row, 1, makeItem(item.name));
    auto* resultsItem = makeItem(item.results);
    resultsItem->setToolTip(item.results);
    table->setItem(row, 2, resultsItem);
    table->setItem(row, 3, makeItem(item.waves));
    table->setItem(row, 4, makeItem(item.status));
}

void populateJobRow(QTableWidget* table, int row, const BattleRunsWidget::JobRow& item)
{
    auto* idItem = makeIdItem(item.jobId);
    idItem->setText(item.execJobId.has_value()
        ? QStringLiteral("#%1").arg(*item.execJobId)
        : QStringLiteral("turn #%1").arg(item.jobId));
    idItem->setData(kExecJobIdRole, item.execJobId.has_value() ? QVariant::fromValue<qint64>(*item.execJobId) : QVariant());
    table->setItem(row, 0, idItem);
    table->setItem(row, 1, makeItem(item.state));
    table->setItem(row, 2, makeItem(item.outcome));
    table->setItem(row, 3, makeItem(item.predicates));
    table->setItem(row, 4, makeItem(item.deltaVi));
    table->setItem(row, 5, makeItem(item.fakeAttacks));
    table->setItem(row, 6, makeItem(item.rngSeed));
}

void populateWaveTurnRow(QTreeWidget*, QTreeWidgetItem* item, const BattleRunsWidget::WaveTurnRow& row)
{
    if (item == nullptr) {
        return;
    }

    item->setText(0, row.label);
    item->setText(1, QString());
    item->setText(2, QString());
    item->setData(0, kIdRole, QVariant());
    item->setFlags(item->flags() & ~Qt::ItemIsSelectable);

    const auto oldChildren = item->takeChildren();
    for (QTreeWidgetItem* child : oldChildren) {
        delete child;
    }

    for (const auto& wave : row.waves) {
        auto* child = new QTreeWidgetItem(item);
        const QString marker = wave.advancementRank >= 2
            ? QString(QChar(0x25CF)) + QStringLiteral(" ")
            : (wave.advancementRank == 1 ? QString(QChar(0x25D0)) + QStringLiteral(" ") : QString(QChar(0x25CB)) + QStringLiteral(" "));
        child->setText(0, marker + wave.label);
        child->setText(1, wave.jobs);
        child->setText(2, wave.status);
        child->setData(0, kIdRole, static_cast<qint64>(wave.waveId));
        child->setSelected(wave.selected);
    }
    item->setExpanded(true);
}

std::vector<BattleRunsWidget::WaveTurnRow> makeWaveTurnRows(std::vector<BattleRunsWidget::WaveRow> waves)
{
    std::vector<BattleRunsWidget::WaveTurnRow> turns;
    for (const auto& wave : waves) {
        auto it = std::find_if(turns.begin(), turns.end(), [&wave](const auto& turn) {
            return turn.turnIndex == wave.turnIndex;
        });
        if (it == turns.end()) {
            const int displayTurn = wave.turnIndex <= 0 ? 1 : wave.turnIndex;
            BattleRunsWidget::WaveTurnRow turn{};
            turn.turnIndex = wave.turnIndex;
            turn.label = QStringLiteral("Turn %1").arg(displayTurn);
            turn.waves.push_back(wave);
            turns.push_back(std::move(turn));
        } else {
            it->waves.push_back(wave);
        }
    }
    return turns;
}

QString groupStatusText(const savor::db::UiBattleGroupSummary& group)
{
    if (group.job_count <= 0) {
        return qs(group.status).isEmpty() ? QStringLiteral("Empty") : qs(group.status);
    }
    if (group.failed_count > 0) {
        return QStringLiteral("%1 with failures").arg(qs(group.status).isEmpty() ? QStringLiteral("Active") : qs(group.status));
    }
    if (group.completed_at_utc.has_value()) {
        return QStringLiteral("Completed");
    }
    return qs(group.status).isEmpty() ? QStringLiteral("Active") : qs(group.status);
}

QString waveStatusText(const savor::db::UiBattleWaveSummary& wave)
{
    if (wave.failed_count > 0) {
        return QStringLiteral("%1 fail").arg(wave.failed_count);
    }
    if (wave.selected_count > 0) {
        return QStringLiteral("%1 selected").arg(wave.selected_count);
    }
    const auto candidateCount = std::max<std::int64_t>(0, wave.desired_outcome_count - wave.selected_count);
    if (candidateCount > 0) {
        return QStringLiteral("%1 candidate").arg(candidateCount);
    }
    if (!wave.status.empty()) {
        return qs(wave.status);
    }
    return QStringLiteral("--");
}

QString outcomeText(const savor::db::UiBattleTurnJobSummary& job)
{
    QStringList parts;
    if (job.selected_for_advancement) {
        parts.push_back(QStringLiteral("selected"));
    } else if (job.has_desired_outcome) {
        parts.push_back(QStringLiteral("candidate"));
    } else {
        parts.push_back(QStringLiteral("miss"));
    }
    if (job.battle_outcome.has_value()) {
        parts.push_back(QStringLiteral("outcome %1").arg(*job.battle_outcome));
    }
    if (!job.advancement_decision_kind.empty()) {
        parts.push_back(qs(job.advancement_decision_kind));
    }
    if (job.manual_followup.has_value() && !job.manual_followup->manual_followup_status.empty()) {
        parts.push_back(QStringLiteral("manual %1").arg(qs(job.manual_followup->manual_followup_status)));
    }
    return parts.isEmpty() ? QStringLiteral("--") : parts.join(QStringLiteral(" / "));
}

QString battleTurnIndicatorText(const std::vector<savor::db::UiBattleWaveSummary>& waves)
{
    std::map<int, int> turnRank;
    for (const auto& wave : waves) {
        const int rank = wave.advancement_rank;
        auto [it, inserted] = turnRank.emplace(wave.turn_index, rank);
        if (!inserted) {
            it->second = std::max(it->second, rank);
        }
    }

    QStringList indicators;
    for (const auto& entry : turnRank) {
        const int rank = entry.second;
        if (rank >= 2) {
            indicators.push_back(QString(QChar(0x25CF)));
        } else if (rank == 1) {
            indicators.push_back(QString(QChar(0x25D0)));
        } else {
            indicators.push_back(QString(QChar(0x25CB)));
        }
    }
    return indicators.join(QStringLiteral(" "));
}

BattleRunsWidget::GroupRow makeGroupRow(
    const savor::db::UiBattleGroupSummary& group,
    const std::vector<savor::db::UiBattleWaveSummary>& waves)
{
    BattleRunsWidget::GroupRow row{};
    row.battleSetId = group.battle_set_id;
    row.group = QStringLiteral("#%1").arg(group.battle_set_id);
    row.name = group.name.empty() ? QStringLiteral("Battle set %1").arg(group.battle_set_id) : qs(group.name);
    const auto candidateCount = std::max<std::int64_t>(0, group.desired_outcome_count - group.selected_count);
    const auto missCount = std::max<std::int64_t>(0, group.job_count - group.desired_outcome_count);
    const QString counts = QStringLiteral("sel:%1 cand:%2 miss:%3 fail:%4 jobs:%5")
        .arg(group.selected_count)
        .arg(candidateCount)
        .arg(missCount)
        .arg(group.failed_count)
        .arg(group.job_count);
    const QString indicators = battleTurnIndicatorText(waves);
    row.results = indicators.isEmpty()
        ? counts
        : QStringLiteral("%1  %2").arg(indicators, counts);
    row.waves = QString::number(group.wave_count);
    row.status = groupStatusText(group);
    return row;
}

BattleRunsWidget::WaveRow makeWaveRow(const savor::db::UiBattleWaveSummary& wave)
{
    BattleRunsWidget::WaveRow row{};
    row.waveId = wave.wave_id;
    row.parentWaveId = wave.parent_wave_id;
    row.turnIndex = wave.turn_index;
    row.label = QStringLiteral("Wave %1").arg(wave.wave_id);
    const auto candidateCount = std::max<std::int64_t>(0, wave.desired_outcome_count - wave.selected_count);
    row.jobs = QStringLiteral("%1 jobs, %2 selected, %3 candidates")
        .arg(wave.job_count)
        .arg(wave.selected_count)
        .arg(candidateCount);
    row.status = waveStatusText(wave);
    row.advancementRank = wave.advancement_rank;
    row.hasFailure = wave.failed_count > 0;
    return row;
}

BattleRunsWidget::JobRow makeJobRow(const savor::db::UiBattleTurnJobSummary& job)
{
    BattleRunsWidget::JobRow row{};
    row.jobId = job.turn_job_id;
    row.execJobId = job.exec_job_id;
    row.waveId = job.wave_id;
    row.state = qs(job.job_state);
    row.outcome = outcomeText(job);
    row.predicates = job.pred_passed.has_value() || job.pred_total.has_value()
        ? QStringLiteral("%1/%2").arg(optionalIntText(job.pred_passed), optionalIntText(job.pred_total))
        : QStringLiteral("--");
    row.deltaVi = optionalInt64Text(job.delta_vi);
    row.fakeAttacks = QStringLiteral("%1 turn / %2 before")
        .arg(job.fake_attacks_this_turn)
        .arg(job.fake_attacks_used_before);
    row.rngSeed = optionalInt64Text(job.rng_seed);
    row.selectedForAdvancement = job.selected_for_advancement;
    row.desiredOutcome = job.has_desired_outcome;
    row.finalVictoryOutcome = job.has_final_victory_outcome;
    row.advancementRank = job.advancement_rank;
    row.predPassed = job.pred_passed.value_or(-1);
    row.predTotal = job.pred_total.value_or(-1);
    row.deltaViSort = job.delta_vi.value_or(0);
    row.fakeAttackSort = job.fake_attacks_this_turn + job.fake_attacks_used_before;
    row.rngSeedSort = job.rng_seed.value_or(0);
    return row;
}

bool isFinishedState(const QString& state)
{
    return state == QStringLiteral("SUCCEEDED")
        || state == QStringLiteral("FAILED")
        || state == QStringLiteral("INTERRUPTED")
        || state == QStringLiteral("CANCELED")
        || state == QStringLiteral("SUPERSEDED")
        || state == QStringLiteral("COMPLETED")
        || state == QStringLiteral("SUCCEEDED_WINNER")
        || state == QStringLiteral("SUCCEEDED_DUPLICATE");
}

void sortJobs(std::vector<BattleRunsWidget::JobRow>& jobs, int primary, int secondary)
{
    auto keyLess = [](const BattleRunsWidget::JobRow& lhs, const BattleRunsWidget::JobRow& rhs, int field) {
        switch (field) {
        case 1:
            if (lhs.predPassed != rhs.predPassed) {
                return lhs.predPassed > rhs.predPassed;
            }
            return lhs.predTotal > rhs.predTotal;
        case 2:
            return lhs.deltaViSort < rhs.deltaViSort;
        case 3:
            return lhs.fakeAttackSort < rhs.fakeAttackSort;
        case 4:
            return lhs.rngSeedSort < rhs.rngSeedSort;
        case 0:
        default:
            return lhs.jobId < rhs.jobId;
        }
    };
    std::stable_sort(jobs.begin(), jobs.end(), [=](const auto& lhs, const auto& rhs) {
        if (primary != secondary && !jobRowsEqual(lhs, rhs)) {
            if (keyLess(lhs, rhs, primary)) {
                return true;
            }
            if (keyLess(rhs, lhs, primary)) {
                return false;
            }
            if (keyLess(lhs, rhs, secondary)) {
                return true;
            }
            if (keyLess(rhs, lhs, secondary)) {
                return false;
            }
        }
        return lhs.jobId < rhs.jobId;
    });
}

AsyncRefreshResult<BattleRunsWidget::RefreshData> loadBattleRuns(BattleRunsWidget::RefreshRequest request)
{
    db::BattleRunGroupQuery groupQuery{};
    groupQuery.before = request.before;
    groupQuery.after = request.after;
    groupQuery.limit = request.limit;
    groupQuery.child_selected_only = request.childSelectedOnly;
    groupQuery.final_victory_only = request.finalVictoryOnly;

    const auto groups = db::SavorDbExplorerRunService::ListBattleGroups(groupQuery);
    if (!groups.ok) {
        return AsyncRefreshResult<BattleRunsWidget::RefreshData>::Err(QString::fromStdString(groups.error.message));
    }

    BattleRunsWidget::RefreshData data{};
    data.groupPage = groups.value;
    data.refreshedAt = QDateTime::currentDateTime();
    data.selectionEpoch = request.selectionEpoch;

    std::map<std::int64_t, std::vector<savor::db::UiBattleWaveSummary>> wavesByGroup;
    for (const auto& group : data.groupPage.groups) {
        const auto waves = db::SavorDbExplorerRunService::ListBattleWaves(group.battle_set_id);
        if (!waves.ok) {
            return AsyncRefreshResult<BattleRunsWidget::RefreshData>::Err(QString::fromStdString(waves.error.message));
        }
        wavesByGroup[group.battle_set_id] = waves.value;
    }

    data.groups.reserve(data.groupPage.groups.size());
    for (const auto& group : data.groupPage.groups) {
        data.groups.push_back(makeGroupRow(group, wavesByGroup[group.battle_set_id]));
    }

    bool selectedGroupVisible = false;
    for (const auto& group : data.groupPage.groups) {
        if (group.battle_set_id == request.selectedBattleSetId) {
            selectedGroupVisible = true;
            break;
        }
    }
    data.selectedBattleSetId = selectedGroupVisible
        ? request.selectedBattleSetId
        : (data.groupPage.groups.empty() ? 0 : data.groupPage.groups.front().battle_set_id);

    if (data.selectedBattleSetId > 0) {
        const auto wavesIt = wavesByGroup.find(data.selectedBattleSetId);
        const auto* selectedWaves = wavesIt != wavesByGroup.end() ? &wavesIt->second : nullptr;
        data.waves.reserve(selectedWaves != nullptr ? selectedWaves->size() : 0);
        std::set<std::int64_t> visibleWaveIds;
        if (selectedWaves != nullptr) {
            for (const auto& wave : *selectedWaves) {
                data.waves.push_back(makeWaveRow(wave));
                visibleWaveIds.insert(wave.wave_id);
            }
        }

        for (std::int64_t waveId : request.selectedWaveIds) {
            if (visibleWaveIds.count(waveId) != 0) {
                data.selectedWaveIds.push_back(waveId);
            }
        }
        if (data.selectedWaveIds.empty() && !data.waves.empty()) {
            data.selectedWaveIds.push_back(data.waves.front().waveId);
        }

        const auto jobs = db::SavorDbExplorerRunService::ListBattleTurnJobsForWaves(data.selectedWaveIds, request.finalVictoryOnly);
        if (!jobs.ok) {
            return AsyncRefreshResult<BattleRunsWidget::RefreshData>::Err(QString::fromStdString(jobs.error.message));
        }
        data.jobs.reserve(jobs.value.size());
        for (const auto& job : jobs.value) {
            auto row = makeJobRow(job);
            if (request.selectedOnly && !row.selectedForAdvancement) {
                continue;
            }
            if (request.desiredOutcomeOnly && !row.desiredOutcome) {
                continue;
            }
            if (request.finalVictoryOnly && !row.finalVictoryOutcome) {
                continue;
            }
            if (!request.showCandidates && row.advancementRank == 1) {
                continue;
            }
            data.jobs.push_back(std::move(row));
        }
        sortJobs(data.jobs, request.primarySort, request.secondarySort);
    }

    data.selectedJobId = 0;
    for (const auto& row : data.jobs) {
        if (row.jobId == request.selectedJobId) {
            data.selectedJobId = row.jobId;
            break;
        }
    }
    if (data.selectedJobId == 0 && !data.jobs.empty()) {
        data.selectedJobId = data.jobs.front().jobId;
    }

    data.summary = QStringLiteral("%1 battle run groups, %2 waves in selected group, %3 jobs for selected waves.")
        .arg(data.groups.size())
        .arg(data.waves.size())
        .arg(data.jobs.size());
    if (data.groups.empty()) {
        data.message = QStringLiteral("No battle run projections are visible yet.");
    } else if (data.waves.empty()) {
        data.message = QStringLiteral("Select a battle run group with projected waves.");
    } else if (data.jobs.empty()) {
        data.message = QStringLiteral("No jobs match the current wave selection and filters.");
    }
    return AsyncRefreshResult<BattleRunsWidget::RefreshData>::Ok(std::move(data));
}

} // namespace

BattleRunsWidget::BattleRunsWidget(Actions actions, QWidget* parent)
    : QWidget(parent)
    , actions_(std::move(actions))
{
    build();
    wireSignals();
}

void BattleRunsWidget::setPageActive(bool active)
{
    pageActive_ = active;
    if (refreshPipeline_ != nullptr) {
        refreshPipeline_->setActive(active);
    }
    if (active) {
        requestRefresh();
    }
}

void BattleRunsWidget::requestRefresh()
{
    if (refreshPipeline_ != nullptr) {
        refreshPipeline_->requestRefresh(RefreshReason::Manual);
    }
}

void BattleRunsWidget::build()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(10);

    auto* toolbar = new QFrame(this);
    toolbar->setObjectName("workspaceHeroPanel");
    auto* toolbarLayout = new QGridLayout(toolbar);
    toolbarLayout->setContentsMargins(10, 10, 10, 10);
    toolbarLayout->setHorizontalSpacing(8);
    toolbarLayout->setVerticalSpacing(8);

    refreshButton_ = new QPushButton(QStringLiteral("Refresh"), toolbar);
    prevButton_ = new QPushButton(QStringLiteral("Prev"), toolbar);
    nextButton_ = new QPushButton(QStringLiteral("Next"), toolbar);
    autoRefreshCheck_ = new QCheckBox(QStringLiteral("Auto refresh"), toolbar);
    refreshSecondsSpin_ = new QSpinBox(toolbar);
    pageSizeSpin_ = new QSpinBox(toolbar);
    childSelectedOnlyCheck_ = new QCheckBox(QStringLiteral("Child selected only"), toolbar);
    selectedOnlyCheck_ = new QCheckBox(QStringLiteral("Selected only"), toolbar);
    showCandidatesCheck_ = new QCheckBox(QStringLiteral("Show candidates"), toolbar);
    desiredOutcomeOnlyCheck_ = new QCheckBox(QStringLiteral("Desired outcome only"), toolbar);
    finalVictoryOnlyCheck_ = new QCheckBox(QStringLiteral("Victory only"), toolbar);
    primarySortCombo_ = new QComboBox(toolbar);
    secondarySortCombo_ = new QComboBox(toolbar);
    summaryLabel_ = new QLabel(QStringLiteral("--"), toolbar);
    lastRefreshLabel_ = new QLabel(QStringLiteral("Last refresh: --"), toolbar);

    for (auto* button : { refreshButton_, prevButton_, nextButton_ }) {
        button->setObjectName("jobsSecondaryButton");
    }
    for (auto* check : { autoRefreshCheck_, childSelectedOnlyCheck_, selectedOnlyCheck_, showCandidatesCheck_, desiredOutcomeOnlyCheck_, finalVictoryOnlyCheck_ }) {
        check->setObjectName("jobsCheckBox");
    }
    refreshSecondsSpin_->setObjectName("jobsSpin");
    pageSizeSpin_->setObjectName("jobsSpin");
    summaryLabel_->setObjectName("sectionDescription");
    lastRefreshLabel_->setObjectName("sectionDescription");

    refreshSecondsSpin_->setRange(1, 30);
    refreshSecondsSpin_->setValue(2);
    refreshSecondsSpin_->setSuffix(QStringLiteral(" s"));
    pageSizeSpin_->setRange(10, 500);
    pageSizeSpin_->setSingleStep(10);
    pageSizeSpin_->setValue(50);
    autoRefreshCheck_->setChecked(true);
    showCandidatesCheck_->setChecked(true);

    const QStringList sortLabels{
        QStringLiteral("Job ID"),
        QStringLiteral("Predicates passed"),
        QStringLiteral("Delta VI"),
        QStringLiteral("Fake attacks"),
        QStringLiteral("RNG seed"),
    };
    primarySortCombo_->addItems(sortLabels);
    secondarySortCombo_->addItems(sortLabels);
    secondarySortCombo_->setCurrentIndex(0);

    toolbarLayout->addWidget(refreshButton_, 0, 0);
    toolbarLayout->addWidget(prevButton_, 0, 1);
    toolbarLayout->addWidget(nextButton_, 0, 2);
    toolbarLayout->addWidget(autoRefreshCheck_, 0, 3);
    toolbarLayout->addWidget(new QLabel(QStringLiteral("Interval"), toolbar), 0, 4);
    toolbarLayout->addWidget(refreshSecondsSpin_, 0, 5);
    toolbarLayout->addWidget(new QLabel(QStringLiteral("Page size"), toolbar), 0, 6);
    toolbarLayout->addWidget(pageSizeSpin_, 0, 7);
    toolbarLayout->addWidget(summaryLabel_, 0, 8);
    toolbarLayout->addWidget(lastRefreshLabel_, 0, 9);
    toolbarLayout->setColumnStretch(8, 1);
    rootLayout->addWidget(toolbar);

    auto* mainSplitter = new QSplitter(Qt::Horizontal, this);
    mainSplitter->setChildrenCollapsible(false);

    auto* leftSplitter = new QSplitter(Qt::Vertical, mainSplitter);
    leftSplitter->setChildrenCollapsible(false);

    auto* groupsPanel = makePanel(QStringLiteral("Run Groups"), leftSplitter);
    auto* groupsLayout = qobject_cast<QVBoxLayout*>(groupsPanel->layout());
    groupsTable_ = new QTableWidget(groupsPanel);
    configureTable(groupsTable_);
    groupsTable_->setColumnCount(5);
    groupsTable_->setHorizontalHeaderLabels({
        QStringLiteral("Group"),
        QStringLiteral("Name"),
        QStringLiteral("Results"),
        QStringLiteral("Waves"),
        QStringLiteral("Status"),
    });
    groupsTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    groupsTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    groupsTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    groupsLayout->addWidget(groupsTable_, 1);

    auto* wavesPanel = makePanel(QStringLiteral("Wave Tree"), leftSplitter);
    auto* wavesLayout = qobject_cast<QVBoxLayout*>(wavesPanel->layout());
    waveTree_ = new QTreeWidget(wavesPanel);
    waveTree_->setHeaderLabels({ QStringLiteral("Wave"), QStringLiteral("Jobs"), QStringLiteral("Status") });
    waveTree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    waveTree_->setRootIsDecorated(true);
    waveTree_->header()->setStretchLastSection(true);
    wavesLayout->addWidget(waveTree_, 1);

    leftSplitter->addWidget(groupsPanel);
    leftSplitter->addWidget(wavesPanel);
    leftSplitter->setStretchFactor(0, 1);
    leftSplitter->setStretchFactor(1, 1);

    auto* jobsPanel = makePanel(QStringLiteral("Wave Jobs"), mainSplitter);
    auto* jobsLayout = qobject_cast<QVBoxLayout*>(jobsPanel->layout());
    auto* jobsFilterLayout = new QGridLayout();
    jobsFilterLayout->setContentsMargins(0, 0, 0, 0);
    jobsFilterLayout->setHorizontalSpacing(8);
    jobsFilterLayout->setVerticalSpacing(6);
    jobsFilterLayout->addWidget(childSelectedOnlyCheck_, 0, 0);
    jobsFilterLayout->addWidget(selectedOnlyCheck_, 0, 1);
    jobsFilterLayout->addWidget(showCandidatesCheck_, 0, 2);
    jobsFilterLayout->addWidget(desiredOutcomeOnlyCheck_, 0, 3);
    jobsFilterLayout->addWidget(finalVictoryOnlyCheck_, 0, 4);
    jobsFilterLayout->addWidget(new QLabel(QStringLiteral("Sort"), jobsPanel), 0, 5);
    jobsFilterLayout->addWidget(primarySortCombo_, 0, 6);
    jobsFilterLayout->addWidget(secondarySortCombo_, 0, 7);
    jobsFilterLayout->setColumnStretch(8, 1);
    jobsLayout->addLayout(jobsFilterLayout);

    jobsTable_ = new QTableWidget(jobsPanel);
    configureTable(jobsTable_);
    jobsTable_->setContextMenuPolicy(Qt::CustomContextMenu);
    jobsTable_->setColumnCount(7);
    jobsTable_->setHorizontalHeaderLabels({
        QStringLiteral("Job"),
        QStringLiteral("State"),
        QStringLiteral("Outcome"),
        QStringLiteral("Predicates"),
        QStringLiteral("Delta VI"),
        QStringLiteral("Fake Attacks"),
        QStringLiteral("RNG Seed"),
    });
    jobsTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    jobsTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    jobsTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    jobsTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    jobsTable_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    jobsLayout->addWidget(jobsTable_, 1);

    mainSplitter->addWidget(leftSplitter);
    mainSplitter->addWidget(jobsPanel);
    mainSplitter->setStretchFactor(0, 1);
    mainSplitter->setStretchFactor(1, 2);
    mainSplitter->setSizes({ 1, 2 });
    rootLayout->addWidget(mainSplitter, 1);

    inlineMessageLabel_ = new QLabel(this);
    inlineMessageLabel_->setObjectName("sectionDescription");
    inlineMessageLabel_->setWordWrap(true);
    rootLayout->addWidget(inlineMessageLabel_);

    refreshPipeline_ = new AsyncRefreshPipeline<RefreshRequest, RefreshData>(this);
    refreshPipeline_->setRefreshIntervalMs(refreshSecondsSpin_->value() * 1000);
    refreshPipeline_->setAutoRefreshEnabled(autoRefreshCheck_->isChecked());
    refreshPipeline_->setRequestBuilder([this](RefreshReason reason) -> std::optional<RefreshRequest> {
        if (reason == RefreshReason::Auto && !pageActive_) {
            return std::nullopt;
        }
        RefreshRequest request{};
        request.before = before_;
        request.after = after_;
        request.selectedBattleSetId = selectedBattleSetId_;
        request.selectedJobId = selectedJobId_;
        request.selectedWaveIds = selectedWaveIds_;
        request.limit = pageSizeSpin_->value();
        request.childSelectedOnly = childSelectedOnlyCheck_->isChecked();
        request.selectedOnly = selectedOnlyCheck_->isChecked();
        request.showCandidates = showCandidatesCheck_->isChecked();
        request.desiredOutcomeOnly = desiredOutcomeOnlyCheck_->isChecked();
        request.finalVictoryOnly = finalVictoryOnlyCheck_->isChecked();
        request.primarySort = primarySortCombo_->currentIndex();
        request.secondarySort = secondarySortCombo_->currentIndex();
        request.selectionEpoch = selectionEpoch_;
        return request;
    });
    refreshPipeline_->setLoadAndPrepare([](RefreshRequest request) {
        return loadBattleRuns(std::move(request));
    });
    refreshPipeline_->setApply([this](const RefreshData& data, RefreshReason, const RefreshStatus&) {
        applyRefresh(data);
    });
    refreshPipeline_->setApplyError([this](const QString& error, RefreshReason, const RefreshStatus&) {
        applyError(error);
    });
}

void BattleRunsWidget::wireSignals()
{
    connect(refreshButton_, &QPushButton::clicked, this, [this]() {
        before_.reset();
        after_.reset();
        requestRefresh();
    });
    connect(prevButton_, &QPushButton::clicked, this, [this]() {
        if (!prev_.has_value()) {
            return;
        }
        after_ = prev_;
        before_.reset();
        requestRefresh();
    });
    connect(nextButton_, &QPushButton::clicked, this, [this]() {
        if (!next_.has_value()) {
            return;
        }
        before_ = next_;
        after_.reset();
        requestRefresh();
    });
    connect(autoRefreshCheck_, &QCheckBox::toggled, this, [this](bool enabled) {
        refreshPipeline_->setAutoRefreshEnabled(enabled);
    });
    connect(refreshSecondsSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        refreshPipeline_->setRefreshIntervalMs(value * 1000);
    });
    connect(pageSizeSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        before_.reset();
        after_.reset();
        requestRefresh();
    });
    for (QCheckBox* check : { childSelectedOnlyCheck_, selectedOnlyCheck_, showCandidatesCheck_, desiredOutcomeOnlyCheck_, finalVictoryOnlyCheck_ }) {
        connect(check, &QCheckBox::toggled, this, [this]() {
            before_.reset();
            after_.reset();
            requestRefresh();
        });
    }
    connect(primarySortCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { requestRefresh(); });
    connect(secondarySortCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { requestRefresh(); });
    connect(groupsTable_, &QTableWidget::currentCellChanged, this, [this](int currentRow, int, int, int) {
        if (refreshingSelection_ || currentRow < 0) {
            return;
        }
        const auto* item = groupsTable_->item(currentRow, 0);
        if (item == nullptr) {
            return;
        }
        selectCurrentGroup(item->data(kIdRole).toLongLong());
    });
    connect(waveTree_, &QTreeWidget::itemSelectionChanged, this, [this]() {
        if (refreshingSelection_) {
            return;
        }
        requestSelectedWavesRefresh();
    });
    connect(jobsTable_, &QTableWidget::currentCellChanged, this, [this](int currentRow, int, int, int) {
        if (refreshingSelection_ || currentRow < 0) {
            return;
        }
        const auto* item = jobsTable_->item(currentRow, 0);
        if (item != nullptr) {
            selectedJobId_ = item->data(kIdRole).toLongLong();
        }
    });
    connect(jobsTable_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        const auto* item = jobsTable_->item(row, 0);
        const auto jobId = item != nullptr ? item->data(kIdRole).toLongLong() : 0;
        if (jobId > 0 && actions_.showJobDetails) {
            actions_.showJobDetails(jobId);
        }
    });
    connect(jobsTable_, &QWidget::customContextMenuRequested, this, &BattleRunsWidget::showJobContextMenu);
}

void BattleRunsWidget::applyRefresh(const RefreshData& data)
{
    if (data.selectionEpoch != selectionEpoch_) {
        next_ = data.groupPage.next;
        prev_ = data.groupPage.prev;
        lastRefresh_ = data.refreshedAt;

        summaryLabel_->setText(data.summary);
        lastRefreshLabel_->setText(QStringLiteral("Last refresh: %1").arg(lastRefresh_.toString(QStringLiteral("HH:mm:ss"))));
        inlineMessageLabel_->setText(data.message);
        refreshGroups(data.groups);
        refreshControlState();
        return;
    }

    selectedBattleSetId_ = data.selectedBattleSetId;
    selectedWaveIds_ = data.selectedWaveIds;
    selectedJobId_ = data.selectedJobId;
    next_ = data.groupPage.next;
    prev_ = data.groupPage.prev;
    lastRefresh_ = data.refreshedAt;

    summaryLabel_->setText(data.summary);
    lastRefreshLabel_->setText(QStringLiteral("Last refresh: %1").arg(lastRefresh_.toString(QStringLiteral("HH:mm:ss"))));
    inlineMessageLabel_->setText(data.message);
    refreshGroups(data.groups);
    refreshWaves(data.waves);
    refreshJobs(data.jobs);
    refreshControlState();
}

void BattleRunsWidget::applyError(const QString& error)
{
    inlineMessageLabel_->setText(error);
    if (actions_.statusToast) {
        actions_.statusToast(StatusToast{ StatusToast::Severity::Error, error, QString(), 1, QDateTime{}, 4000 });
    }
    refreshControlState();
}

void BattleRunsWidget::refreshGroups(const std::vector<GroupRow>& rows)
{
    refreshingSelection_ = true;
    ApplyTableRowsByKey(
        groupsTable_,
        currentGroups_,
        rows,
        [](const GroupRow& row) { return row.battleSetId; },
        groupRowsEqual,
        populateGroupRow);
    for (int row = 0; row < groupsTable_->rowCount(); ++row) {
        const auto* item = groupsTable_->item(row, 0);
        if (item != nullptr && item->data(kIdRole).toLongLong() == selectedBattleSetId_) {
            groupsTable_->selectRow(row);
            break;
        }
    }
    refreshingSelection_ = false;
}

void BattleRunsWidget::refreshWaves(const std::vector<WaveRow>& rows)
{
    refreshingSelection_ = true;
    std::vector<WaveRow> waveRows = rows;
    const std::set<std::int64_t> selected(selectedWaveIds_.begin(), selectedWaveIds_.end());
    for (auto& row : waveRows) {
        row.selected = selected.count(row.waveId) != 0;
    }
    const auto turnRows = makeWaveTurnRows(std::move(waveRows));
    ApplyTreeRowsByKey(
        waveTree_,
        currentWaveTurns_,
        turnRows,
        [](const WaveTurnRow& row) { return row.turnIndex; },
        waveTurnRowsEqual,
        populateWaveTurnRow);
    refreshingSelection_ = false;
}

void BattleRunsWidget::refreshJobs(const std::vector<JobRow>& rows)
{
    ApplyTableRowsByKey(
        jobsTable_,
        currentJobs_,
        rows,
        [](const JobRow& row) { return row.jobId; },
        jobRowsEqual,
        populateJobRow);
    for (int row = 0; row < jobsTable_->rowCount(); ++row) {
        const auto* item = jobsTable_->item(row, 0);
        if (item != nullptr && item->data(kIdRole).toLongLong() == selectedJobId_) {
            jobsTable_->selectRow(row);
            break;
        }
    }
}

void BattleRunsWidget::refreshControlState()
{
    prevButton_->setEnabled(prev_.has_value());
    nextButton_->setEnabled(next_.has_value());
}

void BattleRunsWidget::selectCurrentGroup(std::int64_t battleSetId)
{
    if (battleSetId <= 0 || selectedBattleSetId_ == battleSetId) {
        return;
    }
    selectedBattleSetId_ = battleSetId;
    selectedWaveIds_.clear();
    selectedJobId_ = 0;
    ++selectionEpoch_;
    requestRefresh();
}

void BattleRunsWidget::requestSelectedWavesRefresh()
{
    std::vector<std::int64_t> waveIds;
    for (QTreeWidgetItem* item : waveTree_->selectedItems()) {
        if (item == nullptr) {
            continue;
        }
        const auto waveId = item->data(0, kIdRole).toLongLong();
        if (waveId > 0) {
            waveIds.push_back(waveId);
        }
    }
    std::sort(waveIds.begin(), waveIds.end());
    waveIds.erase(std::unique(waveIds.begin(), waveIds.end()), waveIds.end());
    if (waveIds == selectedWaveIds_) {
        return;
    }
    selectedWaveIds_ = std::move(waveIds);
    selectedJobId_ = 0;
    ++selectionEpoch_;
    requestRefresh();
}

void BattleRunsWidget::showJobContextMenu(const QPoint& position)
{
    const QModelIndex index = jobsTable_->indexAt(position);
    if (!index.isValid()) {
        return;
    }
    jobsTable_->selectRow(index.row());
    const std::int64_t jobId = selectedJobId();
    if (jobId <= 0) {
        return;
    }

    const auto* stateItem = jobsTable_->item(index.row(), 1);
    const QString state = stateItem != nullptr ? stateItem->text() : QString();
    const auto* idItem = jobsTable_->item(index.row(), 0);
    const QVariant execJobValue = idItem != nullptr ? idItem->data(kExecJobIdRole) : QVariant();
    const std::int64_t execJobId = execJobValue.isValid() ? execJobValue.toLongLong() : 0;

    QMenu menu(jobsTable_);
    QAction* detailsAction = menu.addAction(QStringLiteral("View Details"));
    QAction* turnInputsAction = menu.addAction(QStringLiteral("View Turn Inputs"));
    QAction* replicationAction = menu.addAction(QStringLiteral("Replication Details"));
    QAction* battlePlanAction = menu.addAction(QStringLiteral("Battle Plan"));
    menu.addSeparator();
    QAction* recordVictoryAction = menu.addAction(QStringLiteral("Record Victory"));
    const auto row = std::ranges::find(
        currentJobs_, jobId, &JobRow::jobId);
    recordVictoryAction->setEnabled(
        row != currentJobs_.end() && row->state == QStringLiteral("SUCCEEDED")
        && row->finalVictoryOutcome);
    QAction* replayAction = menu.addAction(QStringLiteral("Replay Visually"));
    replayAction->setEnabled(execJobId > 0 && isFinishedState(state));

    QAction* chosen = menu.exec(jobsTable_->viewport()->mapToGlobal(position));
    if (chosen == detailsAction && actions_.showJobDetails) {
        actions_.showJobDetails(jobId);
    } else if (chosen == turnInputsAction && actions_.viewTurnInputs) {
        actions_.viewTurnInputs(jobId);
    } else if (chosen == replicationAction && actions_.showReplicationDetails) {
        actions_.showReplicationDetails(jobId);
    } else if (chosen == battlePlanAction && actions_.showBattlePlan) {
        actions_.showBattlePlan(jobId);
    } else if (chosen == recordVictoryAction && actions_.recordVictory) {
        actions_.recordVictory(jobId);
    } else if (chosen == replayAction && actions_.replayVisual) {
        actions_.replayVisual(execJobId);
    }
}

std::int64_t BattleRunsWidget::selectedJobId() const
{
    const int row = jobsTable_->currentRow();
    const auto* item = row >= 0 ? jobsTable_->item(row, 0) : nullptr;
    return item != nullptr ? item->data(kIdRole).toLongLong() : 0;
}

} // namespace savorqt::gui
