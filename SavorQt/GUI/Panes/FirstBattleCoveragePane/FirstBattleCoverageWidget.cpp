#include "FirstBattleCoverageWidget.h"

#include "DB/SavorDbWorkflowService.h"
#include "GUI/Refresh/AsyncRefreshPipeline.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QAbstractTableModel>
#include <QtCore/QDateTime>
#include <QtCore/QFutureWatcher>
#include <QtCore/QItemSelectionModel>
#include <QtGui/QPainter>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStyledItemDelegate>
#include <QtWidgets/QTableView>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <map>
#include <set>

namespace {
using namespace savor::db::execution::workflow;
constexpr int kLifecycleRole = Qt::UserRole + 1;
constexpr int kInvariantRole = Qt::UserRole + 2;

QString StageText(FirstBattleCoverageStage stage) {
    switch (stage) {
    case FirstBattleCoverageStage::Validated: return QStringLiteral("Validated");
    case FirstBattleCoverageStage::Sterilized: return QStringLiteral("Sterilized");
    case FirstBattleCoverageStage::SeedProbed: return QStringLiteral("Seed probed");
    case FirstBattleCoverageStage::BattleTested: return QStringLiteral("Battle tested");
    default: return QStringLiteral("Not run");
    }
}

QColor StageColor(FirstBattleCoverageStage stage) {
    switch (stage) {
    case FirstBattleCoverageStage::Validated: return QColor(QStringLiteral("#31516b"));
    case FirstBattleCoverageStage::Sterilized: return QColor(QStringLiteral("#27666a"));
    case FirstBattleCoverageStage::SeedProbed: return QColor(QStringLiteral("#36744d"));
    case FirstBattleCoverageStage::BattleTested: return QColor(QStringLiteral("#778b38"));
    default: return QColor(QStringLiteral("#252a31"));
    }
}

class CoverageTableModel final : public QAbstractTableModel {
public:
    explicit CoverageTableModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

    void setCoverage(FirstBattleCoverageSnapshot value) {
        beginResetModel();
        coverage_ = std::move(value);
        cells_.clear();
        preparations_.clear();
        for (const auto& cell : coverage_.cells)
            cells_[{cell.rtc_value, cell.neutral_epoch_count}] = &cell;
        for (const auto& row : coverage_.delay_preparations)
            preparations_[row.neutral_epoch_count] = &row;
        endResetModel();
    }

    int rowCount(const QModelIndex& = {}) const override {
        return coverage_.rtc_max >= coverage_.rtc_min
            ? static_cast<int>(coverage_.rtc_max - coverage_.rtc_min + 1) : 0;
    }
    int columnCount(const QModelIndex& = {}) const override {
        return coverage_.max_neutral_epochs >= 0
            ? static_cast<int>(coverage_.max_neutral_epochs + 1) : 0;
    }
    QVariant data(const QModelIndex& index, int role) const override {
        const auto* cell = cellAt(index);
        if (!cell) return {};
        if (role == Qt::DisplayRole) {
            QString text = StageText(cell->stage);
            if (cell->lifecycle != "NOT_RUN" && cell->lifecycle != "COMPLETED")
                text += QStringLiteral("\n%1").arg(QString::fromStdString(cell->lifecycle));
            return text;
        }
        if (role == Qt::BackgroundRole) return QBrush(StageColor(cell->stage));
        if (role == Qt::ForegroundRole) return QBrush(Qt::white);
        if (role == Qt::TextAlignmentRole) return Qt::AlignCenter;
        if (role == Qt::ToolTipRole) {
            QString text = QStringLiteral("RTC %1, delay %2\nStage: %3\nState: %4\nConfirmed seeds: %5")
                .arg(cell->rtc_value).arg(cell->neutral_epoch_count)
                .arg(StageText(cell->stage))
                .arg(QString::fromStdString(cell->lifecycle))
                .arg(cell->confirmed_seed_count);
            if (!cell->diagnostic.empty()) text += QStringLiteral("\n%1").arg(QString::fromStdString(cell->diagnostic));
            return text;
        }
        if (role == kLifecycleRole) return QString::fromStdString(cell->lifecycle);
        if (role == kInvariantRole) return cell->invariant_violation;
        return {};
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole) return {};
        if (orientation == Qt::Vertical)
            return QStringLiteral("RTC %1").arg(coverage_.rtc_min + section);
        const auto delay = static_cast<std::int64_t>(section);
        const auto found = preparations_.find(delay);
        const QString state = found == preparations_.end()
            ? QStringLiteral("NOT STARTED") : QString::fromStdString(found->second->state);
        if (role == Qt::ToolTipRole && found != preparations_.end() && !found->second->diagnostic.empty())
            return QStringLiteral("Delay %1 preparation: %2\n%3").arg(delay).arg(state)
                .arg(QString::fromStdString(found->second->diagnostic));
        return QStringLiteral("Delay %1\n%2").arg(delay).arg(state);
    }
    const FirstBattleCoverageCellSnapshot* cellAt(const QModelIndex& index) const {
        if (!index.isValid()) return nullptr;
        const auto found = cells_.find({coverage_.rtc_min + index.row(), index.column()});
        return found == cells_.end() ? nullptr : found->second;
    }
private:
    FirstBattleCoverageSnapshot coverage_;
    std::map<std::pair<std::int64_t, std::int64_t>, const FirstBattleCoverageCellSnapshot*> cells_;
    std::map<std::int64_t, const FirstBattleDelayPreparationSnapshot*> preparations_;
};

class CoverageDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        const QString lifecycle = index.data(kLifecycleRole).toString();
        const bool invalid = index.data(kInvariantRole).toBool();
        QColor border;
        if (invalid || lifecycle == QStringLiteral("FAILED") || lifecycle == QStringLiteral("INTERRUPTED"))
            border = QColor(QStringLiteral("#ef6a67"));
        else if (lifecycle == QStringLiteral("RUNNING") || lifecycle == QStringLiteral("PENDING") ||
                 lifecycle == QStringLiteral("WAITING"))
            border = QColor(QStringLiteral("#68aee8"));
        else if (lifecycle == QStringLiteral("CANCELED"))
            border = QColor(QStringLiteral("#9b9b9b"));
        if (!border.isValid()) return;
        painter->save();
        painter->setPen(QPen(border, 3));
        painter->drawRect(option.rect.adjusted(1, 1, -2, -2));
        painter->restore();
    }
};

struct RetryBatchResult { bool ok = true; int count = 0; QString error; };
}

FirstBattleCoverageWidget::FirstBattleCoverageWidget(Actions actions, QWidget* parent)
    : QWidget(parent), actions_(std::move(actions)) {
    auto* root = new QVBoxLayout(this);
    auto* controls = new QHBoxLayout();
    source_ = new QComboBox(this);
    source_->setMinimumWidth(340);
    rtcMin_ = new QSpinBox(this); rtcMin_->setRange(0, 1000000); rtcMin_->setPrefix(QStringLiteral("RTC min: "));
    rtcMax_ = new QSpinBox(this); rtcMax_->setRange(0, 1000000); rtcMax_->setPrefix(QStringLiteral("RTC max: "));
    maxDelay_ = new QSpinBox(this); maxDelay_->setRange(0, 10000); maxDelay_->setPrefix(QStringLiteral("Max delay: "));
    auto* refreshButton = new QPushButton(QStringLiteral("Refresh"), this);
    controls->addWidget(source_, 1);
    controls->addWidget(rtcMin_);
    controls->addWidget(rtcMax_);
    controls->addWidget(maxDelay_);
    controls->addWidget(refreshButton);
    root->addLayout(controls);
    summary_ = new QLabel(QStringLiteral("Select a root DTM to view first-battle coverage."), this);
    summary_->setObjectName(QStringLiteral("sectionDescription"));
    root->addWidget(summary_);

    auto* split = new QSplitter(Qt::Horizontal, this);
    table_ = new QTableView(split);
    table_->setModel(new CoverageTableModel(table_));
    table_->setItemDelegate(new CoverageDelegate(table_));
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setSelectionBehavior(QAbstractItemView::SelectItems);
    table_->setAlternatingRowColors(false);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table_->horizontalHeader()->setDefaultSectionSize(132);
    table_->horizontalHeader()->setMinimumSectionSize(110);
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table_->verticalHeader()->setDefaultSectionSize(54);
    table_->verticalHeader()->setMinimumWidth(86);
    auto* inspector = new QWidget(split);
    auto* inspectorLayout = new QVBoxLayout(inspector);
    auto* title = new QLabel(QStringLiteral("Coverage details"), inspector);
    title->setObjectName(QStringLiteral("panelTitle"));
    details_ = new QLabel(QStringLiteral("Select one or more cells."), inspector);
    details_->setWordWrap(true);
    selectionPreview_ = new QLabel(inspector);
    selectionPreview_->setObjectName(QStringLiteral("sectionDescription"));
    selectionPreview_->setWordWrap(true);
    runMissing_ = new QPushButton(QStringLiteral("Run missing through Battle"), inspector);
    retry_ = new QPushButton(QStringLiteral("Retry failed or interrupted"), inspector);
    openWorkflow_ = new QPushButton(QStringLiteral("Open workflow"), inspector);
    inspectorLayout->addWidget(title);
    inspectorLayout->addWidget(details_);
    inspectorLayout->addWidget(selectionPreview_);
    inspectorLayout->addWidget(runMissing_);
    inspectorLayout->addWidget(retry_);
    inspectorLayout->addWidget(openWorkflow_);
    inspectorLayout->addStretch();
    split->addWidget(table_); split->addWidget(inspector);
    split->setStretchFactor(0, 5); split->setStretchFactor(1, 2);
    root->addWidget(split, 1);

    refresh_ = new savorqt::gui::AsyncRefreshPipeline<FirstBattleCoverageRefreshRequest,
        FirstBattleCoverageRefreshData>(this);
    refresh_->setRefreshIntervalMs(1000);
    refresh_->setAutoRefreshEnabled(true);
    refresh_->setRequestBuilder([this](savorqt::gui::RefreshReason) {
        const auto source = source_->currentData().toList();
        return FirstBattleCoverageRefreshRequest{
            .source_annotation_attempt_id=source.value(0).toLongLong(),
            .workflow_expansion_id=focusExpansionId_,
            .rtc_min=rtcMin_->value(), .rtc_max=rtcMax_->value(),
            .max_neutral_epochs=maxDelay_->value()};
    });
    refresh_->setLoadAndPrepare([](FirstBattleCoverageRefreshRequest request) {
        FirstBattleCoverageRefreshData data{};
        const auto sources = savorqt::db::SavorDbWorkflowService::ListPreparedTasRootSources(1000);
        if (!sources.ok) {
            data.error = QString::fromStdString(sources.error.message);
            return savorqt::gui::AsyncRefreshResult<FirstBattleCoverageRefreshData>::Ok(std::move(data));
        }
        data.sources = sources.value;
        if (request.source_annotation_attempt_id <= 0
            && request.workflow_expansion_id <= 0 && !data.sources.empty()) {
            request.source_annotation_attempt_id = data.sources.front().annotation_attempt_id;
        }
        if (request.source_annotation_attempt_id <= 0 && request.workflow_expansion_id <= 0) {
            data.ok = true;
            return savorqt::gui::AsyncRefreshResult<FirstBattleCoverageRefreshData>::Ok(std::move(data));
        }
        const auto coverage = savorqt::db::SavorDbWorkflowService::ReadFirstBattleCoverage({
            .source_annotation_attempt_id=request.source_annotation_attempt_id,
            .workflow_expansion_id=request.workflow_expansion_id > 0
                ? std::optional<std::int64_t>(request.workflow_expansion_id) : std::nullopt,
            .rtc_min=request.rtc_min, .rtc_max=request.rtc_max,
            .max_neutral_epochs=request.max_neutral_epochs});
        if (!coverage.ok) data.error = QString::fromStdString(coverage.error.message);
        else { data.ok = true; data.coverage = coverage.value; }
        return savorqt::gui::AsyncRefreshResult<FirstBattleCoverageRefreshData>::Ok(std::move(data));
    });
    refresh_->setApply([this](const FirstBattleCoverageRefreshData& data,
        savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) { applyRefresh(data); });
    refresh_->setActive(false);

    connect(refreshButton, &QPushButton::clicked, this, &FirstBattleCoverageWidget::requestRefresh);
    connect(source_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        focusExpansionId_ = 0; requestRefresh();
    });
    const auto boundsChanged = [this](int) { focusExpansionId_ = 0; requestRefresh(); };
    connect(rtcMin_, qOverload<int>(&QSpinBox::valueChanged), this, boundsChanged);
    connect(rtcMax_, qOverload<int>(&QSpinBox::valueChanged), this, boundsChanged);
    connect(maxDelay_, qOverload<int>(&QSpinBox::valueChanged), this, boundsChanged);
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged,
        this, [this]() { refreshSelectionDetails(); });
    connect(table_, &QTableView::doubleClicked, this, [this](const QModelIndex&) { openSelectedWorkflow(); });
    connect(runMissing_, &QPushButton::clicked, this, &FirstBattleCoverageWidget::launchSelected);
    connect(retry_, &QPushButton::clicked, this, &FirstBattleCoverageWidget::retrySelected);
    connect(openWorkflow_, &QPushButton::clicked, this, &FirstBattleCoverageWidget::openSelectedWorkflow);
    refreshSelectionDetails();
}

void FirstBattleCoverageWidget::setPageActive(bool active) {
    active_ = active;
    refresh_->setActive(active);
    if (active) requestRefresh();
}

void FirstBattleCoverageWidget::showExpansion(std::int64_t workflow_expansion_id) {
    focusExpansionId_ = workflow_expansion_id;
    requestRefresh();
}

void FirstBattleCoverageWidget::requestRefresh() {
    if (active_) refresh_->requestRefresh(savorqt::gui::RefreshReason::Manual);
}

void FirstBattleCoverageWidget::applyRefresh(const FirstBattleCoverageRefreshData& data) {
    if (!data.ok) { summary_->setText(data.error); return; }
    const auto selectedAnnotation = data.coverage.source_annotation_attempt_id.value_or(
        source_->currentData().toLongLong());
    source_->blockSignals(true);
    source_->clear();
    int sourceIndex = -1;
    for (const auto& option : data.sources) {
        source_->addItem(QString::fromStdString(option.display_name),
            option.annotation_attempt_id);
        if (option.annotation_attempt_id == selectedAnnotation)
            sourceIndex = source_->count() - 1;
    }
    if (sourceIndex >= 0) source_->setCurrentIndex(sourceIndex);
    source_->blockSignals(false);
    if (focusExpansionId_ > 0 && data.coverage.source_dtm_artifact_id > 0) {
        rtcMin_->blockSignals(true); rtcMax_->blockSignals(true); maxDelay_->blockSignals(true);
        rtcMin_->setValue(static_cast<int>(data.coverage.rtc_min));
        rtcMax_->setValue(static_cast<int>(data.coverage.rtc_max));
        maxDelay_->setValue(static_cast<int>(data.coverage.max_neutral_epochs));
        rtcMin_->blockSignals(false); rtcMax_->blockSignals(false); maxDelay_->blockSignals(false);
        focusExpansionId_ = 0;
    }
    std::set<std::pair<std::int64_t, std::int64_t>> selected;
    if (auto* model = dynamic_cast<CoverageTableModel*>(table_->model()))
        for (const auto& index : table_->selectionModel()->selectedIndexes())
            if (const auto* cell = model->cellAt(index)) selected.insert({cell->rtc_value, cell->neutral_epoch_count});
    coverage_ = data.coverage;
    static_cast<CoverageTableModel*>(table_->model())->setCoverage(coverage_);
    QItemSelection selection;
    for (int row = 0; row < table_->model()->rowCount(); ++row)
        for (int column = 0; column < table_->model()->columnCount(); ++column)
            if (selected.contains({coverage_.rtc_min + row, column})) {
                const auto index = table_->model()->index(row, column);
                selection.select(index, index);
            }
    table_->selectionModel()->select(selection, QItemSelectionModel::Select);
    std::int64_t tested = 0, probed = 0, attention = 0, active = 0;
    for (const auto& cell : coverage_.cells) {
        if (cell.stage == FirstBattleCoverageStage::BattleTested) ++tested;
        if (cell.stage >= FirstBattleCoverageStage::SeedProbed) ++probed;
        if (cell.retryable || cell.invariant_violation) ++attention;
        if (cell.active) ++active;
    }
    summary_->setText(QStringLiteral("%1 RTC rows × %2 delay columns · %3 Battle tested · %4 Seed probed · %5 active · %6 attention")
        .arg(table_->model()->rowCount()).arg(table_->model()->columnCount())
        .arg(tested).arg(probed).arg(active).arg(attention));
    refreshSelectionDetails();
}

void FirstBattleCoverageWidget::refreshSelectionDetails() {
    auto* model = dynamic_cast<CoverageTableModel*>(table_ ? table_->model() : nullptr);
    if (!model || !table_->selectionModel()) return;
    const auto indexes = table_->selectionModel()->selectedIndexes();
    std::set<WorkflowExpansionTarget> selected;
    std::set<std::int64_t> retryable;
    const FirstBattleCoverageCellSnapshot* current = nullptr;
    for (const auto& index : indexes) {
        const auto* cell = model->cellAt(index);
        if (!cell) continue;
        selected.insert({cell->neutral_epoch_count, cell->rtc_value});
        retryable.insert(cell->retryable_workflow_instance_ids.begin(),
                         cell->retryable_workflow_instance_ids.end());
        if (index == table_->currentIndex()) current = cell;
    }
    const std::vector<WorkflowExpansionTarget> raw(selected.begin(), selected.end());
    const auto normalized = NormalizeExactFirstBattleExpansionTargets(raw);
    selectionPreview_->setText(selected.empty() ? QStringLiteral("No cells selected.")
        : QStringLiteral("%1 exact cell%2 selected.")
            .arg(normalized.size()).arg(normalized.size() == 1 ? QString() : QStringLiteral("s")));
    runMissing_->setEnabled(!operationInFlight_ && !selected.empty() && source_->currentData().toLongLong() > 0);
    retry_->setEnabled(!operationInFlight_ && !retryable.empty());
    openWorkflow_->setEnabled(current && !current->workflow_instance_ids.empty());
    if (!current) { details_->setText(QStringLiteral("Select one or more cells.")); return; }
    QString text = QStringLiteral("RTC %1 · delay %2\n%3 · %4\nConfirmed seeds: %5\nAttempts: %6")
        .arg(current->rtc_value).arg(current->neutral_epoch_count)
        .arg(StageText(current->stage)).arg(QString::fromStdString(current->lifecycle))
        .arg(current->confirmed_seed_count).arg(current->workflow_instance_ids.size());
    if (!current->diagnostic.empty()) text += QStringLiteral("\n\n%1").arg(QString::fromStdString(current->diagnostic));
    details_->setText(text);
}

void FirstBattleCoverageWidget::launchSelected() {
    if (operationInFlight_) return;
    auto* model = dynamic_cast<CoverageTableModel*>(table_->model());
    std::set<WorkflowExpansionTarget> selected;
    for (const auto& index : table_->selectionModel()->selectedIndexes())
        if (const auto* cell = model->cellAt(index)) selected.insert({cell->neutral_epoch_count, cell->rtc_value});
    if (selected.empty()) return;
    LaunchMissingFirstBattleCoverageRequest request{};
    request.source_annotation_attempt_id = source_->currentData().toLongLong();
    request.targets.assign(selected.begin(), selected.end());
    request.created_by = "SavorQt.FirstBattleCoverage";
    operationInFlight_ = true; refreshSelectionDetails();
    auto* watcher = new QFutureWatcher<savorqt::db::ServiceResult<LaunchMissingFirstBattleCoverageReceipt>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher]() {
        const auto result = watcher->result(); watcher->deleteLater(); operationInFlight_ = false;
        if (!result.ok) emit statusToastRequested({StatusToast::Severity::Error,
            QStringLiteral("First-battle coverage launch failed: %1").arg(QString::fromStdString(result.error.message)),
            {}, 1, QDateTime::currentDateTimeUtc(), 4000});
        else emit statusToastRequested({StatusToast::Severity::Success,
            QStringLiteral("Launched %1 cells; %2 covered, %3 active, %4 retryable.")
                .arg(result.value.launched_count).arg(result.value.already_covered_count)
                .arg(result.value.active_count).arg(result.value.retryable_count),
            {}, 1, QDateTime::currentDateTimeUtc(), 4000});
        requestRefresh(); refreshSelectionDetails();
    });
    watcher->setFuture(QtConcurrent::run([request]() {
        return savorqt::db::SavorDbWorkflowService::LaunchMissingFirstBattleCoverage(request);
    }));
}

void FirstBattleCoverageWidget::retrySelected() {
    if (operationInFlight_) return;
    auto* model = dynamic_cast<CoverageTableModel*>(table_->model());
    std::set<std::int64_t> workflows;
    for (const auto& index : table_->selectionModel()->selectedIndexes())
        if (const auto* cell = model->cellAt(index))
            workflows.insert(cell->retryable_workflow_instance_ids.begin(),
                             cell->retryable_workflow_instance_ids.end());
    if (workflows.empty()) return;
    operationInFlight_ = true; refreshSelectionDetails();
    auto* watcher = new QFutureWatcher<RetryBatchResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher]() {
        const auto result = watcher->result(); watcher->deleteLater(); operationInFlight_ = false;
        emit statusToastRequested({result.ok ? StatusToast::Severity::Success : StatusToast::Severity::Error,
            result.ok ? QStringLiteral("Requeued failed or interrupted jobs in %1 workflow(s).").arg(result.count)
                      : QStringLiteral("Coverage retry failed: %1").arg(result.error),
            {}, 1, QDateTime::currentDateTimeUtc(), 4000});
        requestRefresh(); refreshSelectionDetails();
    });
    watcher->setFuture(QtConcurrent::run([workflows]() {
        RetryBatchResult result{};
        for (const auto workflow : workflows) {
            const auto retry = savorqt::db::SavorDbWorkflowService::RetryFailedJobs(workflow);
            if (!retry.ok) { result.ok = false; result.error = QString::fromStdString(retry.error.message); break; }
            ++result.count;
        }
        return result;
    }));
}

void FirstBattleCoverageWidget::openSelectedWorkflow() {
    auto* model = dynamic_cast<CoverageTableModel*>(table_->model());
    const auto* cell = model ? model->cellAt(table_->currentIndex()) : nullptr;
    if (!cell || cell->workflow_instance_ids.empty() || !actions_.openWorkflow) return;
    actions_.openWorkflow(*std::max_element(cell->workflow_instance_ids.begin(),
                                            cell->workflow_instance_ids.end()));
}
