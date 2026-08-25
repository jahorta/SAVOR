#include "VictoryResultsWidget.h"

#include "DB/SavorDbTasRouteService.h"
#include "GUI/Refresh/AsyncRefreshPipeline.h"

#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

VictoryResultsWidget::VictoryResultsWidget(Actions actions, QWidget* parent)
    : QWidget(parent), actions_(std::move(actions)) {
    auto* root = new QVBoxLayout(this);
    auto* filters = new QHBoxLayout();
    battleFilter_ = new QComboBox(this);
    battleFilter_->addItem(QStringLiteral("All BattleSets"), 0);
    completionFilter_ = new QComboBox(this);
    completionFilter_->addItems({QStringLiteral("All victories"), QStringLiteral("Completed rewards"), QStringLiteral("Awaiting completion")});
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(QStringLiteral("Filter RNG, job, route, or reward"));
    filters->addWidget(battleFilter_);
    filters->addWidget(completionFilter_);
    filters->addWidget(search_, 1);
    root->addLayout(filters);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    table_ = new QTableWidget(0, 7, splitter);
    table_->setHorizontalHeaderLabels({QStringLiteral("Battle"), QStringLiteral("RNG"), QStringLiteral("Job"),
        QStringLiteral("Fake"), QStringLiteral("VI"), QStringLiteral("Completion"), QStringLiteral("Rewards")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    auto* right = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(right);
    auto* heading = new QLabel(QStringLiteral("Victory details"), right);
    heading->setObjectName("panelTitle");
    detail_ = new QLabel(QStringLiteral("Select a Victory candidate."), right);
    detail_->setWordWrap(true);
    detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    record_ = new QPushButton(QStringLiteral("Record Victory"), right);
    openWorkflow_ = new QPushButton(QStringLiteral("Open workflow"), right);
    openJob_ = new QPushButton(QStringLiteral("Open job"), right);
    openArtifact_ = new QPushButton(QStringLiteral("Open manifest artifact"), right);
    rightLayout->addWidget(heading);
    rightLayout->addWidget(detail_, 1);
    rightLayout->addWidget(record_);
    rightLayout->addWidget(openWorkflow_);
    rightLayout->addWidget(openJob_);
    rightLayout->addWidget(openArtifact_);
    splitter->addWidget(table_);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    root->addWidget(splitter, 1);

    refresh_ = new savorqt::gui::AsyncRefreshPipeline<std::int64_t,
        std::vector<savorqt::db::VictoryResultSummary>>(this);
    refresh_->setAutoRefreshEnabled(true);
    refresh_->setRequestBuilder([this](savorqt::gui::RefreshReason) { return routeNodeId_; });
    refresh_->setLoadAndPrepare([](std::int64_t id) { return savorqt::db::SavorDbTasRouteService::FetchVictories(id); });
    refresh_->setApply([this](const auto& rows) { applyRows(rows); });
    refresh_->setActive(true);

    connect(table_, &QTableWidget::itemSelectionChanged, this, [this]() { showSelected(); });
    connect(battleFilter_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { rebuildTable(); });
    connect(completionFilter_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { rebuildTable(); });
    connect(search_, &QLineEdit::textChanged, this, [this]() { rebuildTable(); });
    connect(record_, &QPushButton::clicked, this, [this]() {
        const int row = table_->currentRow();
        if (row >= 0 && actions_.recordVictory) actions_.recordVictory(table_->item(row, 0)->data(Qt::UserRole).toLongLong());
    });
    connect(openWorkflow_, &QPushButton::clicked, this, [this]() {
        const int row = table_->currentRow();
        if (row >= 0 && actions_.openWorkflow) actions_.openWorkflow(table_->item(row, 0)->data(Qt::UserRole + 1).toLongLong());
    });
    connect(openJob_, &QPushButton::clicked, this, [this]() {
        const int row = table_->currentRow();
        if (row >= 0 && actions_.openJob) actions_.openJob(table_->item(row, 0)->data(Qt::UserRole + 2).toLongLong());
    });
    connect(openArtifact_, &QPushButton::clicked, this, [this]() { if (actions_.openArtifacts) actions_.openArtifacts(); });
}

void VictoryResultsWidget::showRoute(std::int64_t id) {
    routeNodeId_ = id;
    refresh_->requestRefresh(savorqt::gui::RefreshReason::Manual);
}

void VictoryResultsWidget::applyRows(const std::vector<savorqt::db::VictoryResultSummary>& rows) {
    rows_ = rows;
    const auto selectedBattle = battleFilter_->currentData().toLongLong();
    battleFilter_->blockSignals(true);
    battleFilter_->clear();
    battleFilter_->addItem(QStringLiteral("All BattleSets"), 0);
    std::vector<std::int64_t> ids;
    for (const auto& row : rows_) if (std::find(ids.begin(), ids.end(), row.battle_set_id) == ids.end()) ids.push_back(row.battle_set_id);
    std::sort(ids.begin(), ids.end());
    for (auto id : ids) battleFilter_->addItem(QStringLiteral("BattleSet %1").arg(id), id);
    battleFilter_->setCurrentIndex(std::max(0, battleFilter_->findData(selectedBattle)));
    battleFilter_->blockSignals(false);
    rebuildTable();
}

void VictoryResultsWidget::rebuildTable() {
    table_->setRowCount(0);
    const auto battle = battleFilter_->currentData().toLongLong();
    const QString needle = search_->text().trimmed();
    for (const auto& value : rows_) {
        if (battle > 0 && value.battle_set_id != battle) continue;
        if (completionFilter_->currentIndex() == 1 && !value.completion_ready) continue;
        if (completionFilter_->currentIndex() == 2 && value.completion_ready) continue;
        const QString haystack = QStringLiteral("%1 %2 %3 %4 %5")
            .arg(value.battle_set_id).arg(value.turn_job_id).arg(value.ending_rng)
            .arg(QString::fromStdString(value.route_kind)).arg(value.gold);
        if (!needle.isEmpty() && !haystack.contains(needle, Qt::CaseInsensitive)) continue;
        const int row = table_->rowCount();
        table_->insertRow(row);
        auto put = [&](int col, const QString& text) { table_->setItem(row, col, new QTableWidgetItem(text)); };
        put(0, QString::number(value.battle_set_id));
        table_->item(row, 0)->setData(Qt::UserRole, value.turn_job_id);
        table_->item(row, 0)->setData(Qt::UserRole + 1, value.workflow_instance_id.value_or(0));
        table_->item(row, 0)->setData(Qt::UserRole + 2, value.execution_job_id);
        table_->item(row, 0)->setData(Qt::UserRole + 3, static_cast<qlonglong>(value.manifest_artifact_id.value_or(0)));
        put(1, QString::number(value.ending_rng));
        put(2, QString::number(value.turn_job_id));
        put(3, QString::number(value.fake_attacks));
        put(4, QString::number(value.delta_vi));
        put(5, QString::fromStdString(value.completion_status));
        put(6, value.completion_ready ? QStringLiteral("%1 EXP / %2 Gold").arg(value.normal_experience).arg(value.gold) : QStringLiteral("Pending"));
    }
    if (table_->rowCount() > 0) table_->selectRow(0);
    else showSelected();
}

void VictoryResultsWidget::showSelected() {
    const int rowIndex = table_->currentRow();
    if (rowIndex < 0) {
        detail_->setText(QStringLiteral("No Victory matches the current filters."));
        record_->setEnabled(false); openWorkflow_->setEnabled(false); openJob_->setEnabled(false); openArtifact_->setEnabled(false);
        return;
    }
    const auto turnJob = table_->item(rowIndex, 0)->data(Qt::UserRole).toLongLong();
    const auto it = std::find_if(rows_.begin(), rows_.end(), [turnJob](const auto& v) { return v.turn_job_id == turnJob; });
    if (it == rows_.end()) return;
    QString items;
    for (const auto& item : it->items) {
        if (item.item_id < 0 || item.quantity == 0) continue;
        if (!items.isEmpty()) items += QLatin1Char('\n');
        items += QStringLiteral("Item %1 x%2").arg(item.item_id).arg(item.quantity);
    }
    if (items.isEmpty()) items = QStringLiteral("No item rewards");
    detail_->setText(QStringLiteral(
        "BattleSet %1, wave %2, turn %3\nEnding RNG: %4\nExecution job: %5\n"
        "Fake attacks: %6\nVI delta: %7\n\nRewards\nNormal EXP: %8\nMagic EXP: %9\nGold: %10\n%11\n\n"
        "Completion: %12\nRoute: %13\nTransition: %14\nEntry RNG: %15\nCompletion RNG: %16\n"
        "Presentation: %17 gold pages, %18 EXP pages, %19 item popups")
        .arg(it->battle_set_id).arg(it->wave_id).arg(it->turn_index).arg(it->ending_rng)
        .arg(it->execution_job_id).arg(it->fake_attacks).arg(it->delta_vi)
        .arg(it->normal_experience).arg(it->magic_experience).arg(it->gold).arg(items)
        .arg(QString::fromStdString(it->completion_status)).arg(QString::fromStdString(it->route_kind))
        .arg(QString::fromStdString(it->transition_filename)).arg(it->entry_rng).arg(it->completion_rng)
        .arg(it->presentation.gold_pages).arg(it->presentation.normal_exp_pages).arg(it->presentation.item_popups));
    record_->setEnabled(true);
    openWorkflow_->setEnabled(it->workflow_instance_id.has_value());
    openJob_->setEnabled(true);
    openArtifact_->setEnabled(it->manifest_artifact_id.has_value());
}
