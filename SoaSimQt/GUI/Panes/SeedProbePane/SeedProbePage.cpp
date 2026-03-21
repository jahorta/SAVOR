#include "SeedProbePage.h"

#include "SeedProbeController.h"
#include "SeedProbeGridWidget.h"
#include "SeedProbeTableModels.h"

#include <QtCore/QSignalBlocker>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTableView>
#include <QtCore/QItemSelectionModel>
#include <QtWidgets/QVBoxLayout>

namespace {
QFrame* createLegendSwatch(const QString& text, QWidget* parent)
{
    QFrame* frame = new QFrame(parent);
    frame->setObjectName("seedProbeLegendChip");
    QHBoxLayout* layout = new QHBoxLayout(frame);
    layout->setContentsMargins(10, 4, 10, 4);
    QLabel* label = new QLabel(text, frame);
    layout->addWidget(label);
    return frame;
}
}

SeedProbePage::SeedProbePage(QWidget* parent)
    : QWidget(parent)
    , controller_(new SeedProbeController(this))
    , listModel_(new SeedProbeListModel(this))
    , uniqueModel_(new SeedProbeUniqueTableModel(this))
{
    createWidgets();
    wireSignals();
    controller_->loadInitial();
}

void SeedProbePage::createWidgets()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(12);

    titleLabel_ = new QLabel(QStringLiteral("Seed Probe"), this);
    titleLabel_->setObjectName("pageTitle");
    descriptionLabel_ = new QLabel(QStringLiteral("Qt migration of the Seed Probe workspace with paged probe browsing, live probe details, custom delta maps, and unique-seed inspection."), this);
    descriptionLabel_->setObjectName("pageDescription");
    descriptionLabel_->setWordWrap(true);
    rootLayout->addWidget(titleLabel_);
    rootLayout->addWidget(descriptionLabel_);

    QFrame* toolbarPanel = new QFrame(this);
    toolbarPanel->setObjectName("jobsToolbarPanel");
    QGridLayout* toolbarLayout = new QGridLayout(toolbarPanel);
    toolbarLayout->setContentsMargins(16, 14, 16, 14);
    toolbarLayout->setHorizontalSpacing(12);
    toolbarLayout->setVerticalSpacing(10);

    searchEdit_ = new QLineEdit(toolbarPanel);
    searchEdit_->setObjectName("jobsFilterEdit");
    doneOnlyCheck_ = new QCheckBox(QStringLiteral("Done only"), toolbarPanel);
    pageSizeSpin_ = new QSpinBox(toolbarPanel);
    pageSizeSpin_->setObjectName("jobsRefreshSpin");
    pageSizeSpin_->setRange(10, 250);
    pageSizeSpin_->setSingleStep(10);
    autoRefreshCheck_ = new QCheckBox(QStringLiteral("Auto refresh"), toolbarPanel);
    refreshSecondsSpin_ = new QSpinBox(toolbarPanel);
    refreshSecondsSpin_->setObjectName("jobsRefreshSpin");
    refreshSecondsSpin_->setRange(1, 10);
    refreshSecondsSpin_->setSuffix(QStringLiteral(" s"));
    applyButton_ = new QPushButton(QStringLiteral("Apply"), toolbarPanel);
    applyButton_->setObjectName("jobsPrimaryButton");
    resetButton_ = new QPushButton(QStringLiteral("Reset"), toolbarPanel);
    resetButton_->setObjectName("jobsSecondaryButton");

    toolbarLayout->addWidget(new QLabel(QStringLiteral("Search"), toolbarPanel), 0, 0);
    toolbarLayout->addWidget(searchEdit_, 1, 0, 1, 2);
    toolbarLayout->addWidget(doneOnlyCheck_, 1, 2);
    toolbarLayout->addWidget(new QLabel(QStringLiteral("Page size"), toolbarPanel), 0, 3);
    toolbarLayout->addWidget(pageSizeSpin_, 1, 3);
    toolbarLayout->addWidget(applyButton_, 1, 4);
    toolbarLayout->addWidget(resetButton_, 1, 5);
    toolbarLayout->addWidget(autoRefreshCheck_, 0, 6, 1, 2, Qt::AlignBottom);
    toolbarLayout->addWidget(refreshSecondsSpin_, 1, 6);
    toolbarLayout->addWidget(new QLabel(QStringLiteral("Interval"), toolbarPanel), 1, 7);
    toolbarLayout->setColumnStretch(1, 1);
    rootLayout->addWidget(toolbarPanel);

    QFrame* contentPanel = new QFrame(this);
    contentPanel->setObjectName("jobsContentPanel");
    QVBoxLayout* contentLayout = new QVBoxLayout(contentPanel);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(12);

    QFrame* pagingPanel = new QFrame(contentPanel);
    pagingPanel->setObjectName("jobsPagingPanel");
    QHBoxLayout* pagingLayout = new QHBoxLayout(pagingPanel);
    pagingLayout->setContentsMargins(16, 12, 16, 12);
    prevButton_ = new QPushButton(QStringLiteral("Prev"), pagingPanel);
    prevButton_->setObjectName("jobsSecondaryButton");
    nextButton_ = new QPushButton(QStringLiteral("Next"), pagingPanel);
    nextButton_->setObjectName("jobsSecondaryButton");
    refreshButton_ = new QPushButton(QStringLiteral("Refresh now"), pagingPanel);
    refreshButton_->setObjectName("jobsSecondaryButton");
    pageSummaryLabel_ = new QLabel(pagingPanel);
    pageSummaryLabel_->setObjectName("jobsMetaText");
    lastRefreshLabel_ = new QLabel(pagingPanel);
    lastRefreshLabel_->setObjectName("jobsMetaText");
    pagingLayout->addWidget(prevButton_);
    pagingLayout->addWidget(nextButton_);
    pagingLayout->addWidget(refreshButton_);
    pagingLayout->addSpacing(8);
    pagingLayout->addWidget(pageSummaryLabel_);
    pagingLayout->addStretch();
    pagingLayout->addWidget(lastRefreshLabel_);
    contentLayout->addWidget(pagingPanel);

    QSplitter* splitter = new QSplitter(Qt::Horizontal, contentPanel);
    splitter->setChildrenCollapsible(false);

    QFrame* leftPanel = new QFrame(splitter);
    leftPanel->setObjectName("jobsSurfacePanel");
    QVBoxLayout* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(16, 16, 16, 16);
    leftLayout->addWidget(new QLabel(QStringLiteral("Seed Probes"), leftPanel));
    listTable_ = new QTableView(leftPanel);
    listTable_->setModel(listModel_);
    listTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    listTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    listTable_->setAlternatingRowColors(true);
    listTable_->setSortingEnabled(false);
    listTable_->horizontalHeader()->setStretchLastSection(true);
    listTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    listTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    listTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    leftLayout->addWidget(listTable_, 1);

    QScrollArea* detailScroll = new QScrollArea(splitter);
    detailScroll->setWidgetResizable(true);
    detailScroll->setFrameShape(QFrame::NoFrame);
    QWidget* detailWidget = new QWidget(detailScroll);
    QVBoxLayout* detailLayout = new QVBoxLayout(detailWidget);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(12);

    QFrame* summaryPanel = new QFrame(detailWidget);
    summaryPanel->setObjectName("jobsSurfacePanel");
    QGridLayout* summaryLayout = new QGridLayout(summaryPanel);
    summaryLayout->setContentsMargins(16, 16, 16, 16);
    summaryLayout->addWidget(new QLabel(QStringLiteral("Neutral RNG"), summaryPanel), 0, 0);
    neutralSeedValue_ = new QLabel(QStringLiteral("--"), summaryPanel);
    summaryLayout->addWidget(neutralSeedValue_, 0, 1);
    summaryLayout->addWidget(new QLabel(QStringLiteral("Probe ID"), summaryPanel), 0, 2);
    probeIdValue_ = new QLabel(QStringLiteral("--"), summaryPanel);
    summaryLayout->addWidget(probeIdValue_, 0, 3);
    summaryLayout->addWidget(new QLabel(QStringLiteral("Status"), summaryPanel), 1, 0);
    statusValue_ = new QLabel(QStringLiteral("--"), summaryPanel);
    summaryLayout->addWidget(statusValue_, 1, 1);
    summaryLayout->addWidget(new QLabel(QStringLiteral("Codec"), summaryPanel), 1, 2);
    codecValue_ = new QLabel(QStringLiteral("--"), summaryPanel);
    summaryLayout->addWidget(codecValue_, 1, 3);
    summaryLayout->addWidget(new QLabel(QStringLiteral("Savestate"), summaryPanel), 2, 0);
    savestateValue_ = new QLabel(QStringLiteral("--"), summaryPanel);
    savestateValue_->setWordWrap(true);
    summaryLayout->addWidget(savestateValue_, 2, 1, 1, 3);
    detailLayout->addWidget(summaryPanel);

    QFrame* graphsPanel = new QFrame(detailWidget);
    graphsPanel->setObjectName("jobsSurfacePanel");
    QVBoxLayout* graphsLayout = new QVBoxLayout(graphsPanel);
    graphsLayout->setContentsMargins(16, 16, 16, 16);
    graphsLayout->addWidget(new QLabel(QStringLiteral("Delta Maps"), graphsPanel));
    QHBoxLayout* graphRow = new QHBoxLayout();
    graphRow->setSpacing(12);
    mainGrid_ = new SeedProbeGridWidget(graphsPanel);
    mainGrid_->setTitle(QStringLiteral("Main Stick"));
    cStickGrid_ = new SeedProbeGridWidget(graphsPanel);
    cStickGrid_->setTitle(QStringLiteral("C-Stick"));
    triggerGrid_ = new SeedProbeGridWidget(graphsPanel);
    triggerGrid_->setTitle(QStringLiteral("Triggers"));
    graphRow->addWidget(mainGrid_, 1);
    graphRow->addWidget(cStickGrid_, 1);
    graphRow->addWidget(triggerGrid_, 1);
    graphsLayout->addLayout(graphRow);
    legendLayout_ = new QHBoxLayout();
    legendLayout_->setSpacing(8);
    graphsLayout->addLayout(legendLayout_);
    detailLayout->addWidget(graphsPanel);

    QFrame* uniquePanel = new QFrame(detailWidget);
    uniquePanel->setObjectName("jobsSurfacePanel");
    QVBoxLayout* uniqueLayout = new QVBoxLayout(uniquePanel);
    uniqueLayout->setContentsMargins(16, 16, 16, 16);
    uniqueLayout->addWidget(new QLabel(QStringLiteral("Unique Seeds"), uniquePanel));
    uniqueTable_ = new QTableView(uniquePanel);
    uniqueTable_->setModel(uniqueModel_);
    uniqueTable_->horizontalHeader()->setStretchLastSection(true);
    uniqueTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    uniqueTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    uniqueTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    uniqueTable_->setSelectionMode(QAbstractItemView::NoSelection);
    uniqueLayout->addWidget(uniqueTable_);
    detailLayout->addWidget(uniquePanel, 1);

    detailLayout->addStretch();
    detailScroll->setWidget(detailWidget);

    splitter->addWidget(leftPanel);
    splitter->addWidget(detailScroll);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 5);
    contentLayout->addWidget(splitter, 1);

    inlineMessageLabel_ = new QLabel(contentPanel);
    inlineMessageLabel_->setObjectName("jobSetsInlineMessage");
    inlineMessageLabel_->setWordWrap(true);
    contentLayout->addWidget(inlineMessageLabel_);

    rootLayout->addWidget(contentPanel, 1);
}

void SeedProbePage::wireSignals()
{
    connect(controller_, &SeedProbeController::stateChanged, this, [this]() {
        syncControlsFromController();
        refreshList();
        refreshDetails();
        updateStatusWidgets();
    });

    connect(applyButton_, &QPushButton::clicked, this, [this]() {
        controller_->setSearch(searchEdit_->text());
        controller_->setOnlyDone(doneOnlyCheck_->isChecked());
        controller_->applyFilters(pageSizeSpin_->value());
    });
    connect(resetButton_, &QPushButton::clicked, controller_, &SeedProbeController::resetFilters);
    connect(refreshButton_, &QPushButton::clicked, controller_, &SeedProbeController::requestRefresh);
    connect(prevButton_, &QPushButton::clicked, controller_, &SeedProbeController::requestPreviousPage);
    connect(nextButton_, &QPushButton::clicked, controller_, &SeedProbeController::requestNextPage);
    connect(autoRefreshCheck_, &QCheckBox::toggled, controller_, &SeedProbeController::setAutoRefreshEnabled);
    connect(refreshSecondsSpin_, qOverload<int>(&QSpinBox::valueChanged), controller_, &SeedProbeController::setRefreshSeconds);
    connect(listTable_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this](const QModelIndex& current, const QModelIndex&) {
        if (!current.isValid()) {
            return;
        }
        if (const auto* row = listModel_->rowAt(current.row())) {
            controller_->selectProbe(row->probeId);
        }
    });
}

void SeedProbePage::syncControlsFromController()
{
    const auto& state = controller_->viewState();
    { QSignalBlocker blocker(searchEdit_); searchEdit_->setText(state.search); }
    { QSignalBlocker blocker(doneOnlyCheck_); doneOnlyCheck_->setChecked(state.onlyDone); }
    { QSignalBlocker blocker(pageSizeSpin_); pageSizeSpin_->setValue(state.pageLimit); }
    { QSignalBlocker blocker(autoRefreshCheck_); autoRefreshCheck_->setChecked(state.autoRefresh); }
    { QSignalBlocker blocker(refreshSecondsSpin_); refreshSecondsSpin_->setValue(state.refreshSeconds); }
    prevButton_->setEnabled(state.page.prev.has_value() && !state.loadingList);
    nextButton_->setEnabled(state.page.next.has_value() && !state.loadingList);
}

void SeedProbePage::refreshList()
{
    const auto& state = controller_->viewState();
    QVector<SeedProbeListModel::Row> rows;
    rows.reserve(state.probeRows.size());
    for (const auto& item : state.probeRows) {
        rows.push_back({ item.probeId, item.status, item.savestateId, item.filename });
    }
    listModel_->setRows(rows);

    for (int row = 0; row < rows.size(); ++row) {
        if (rows[row].probeId == state.selectedProbeId) {
            listTable_->selectRow(row);
            break;
        }
    }

    pageSummaryLabel_->setText(QStringLiteral("Rows: %1 • page size: %2").arg(rows.size()).arg(state.pageLimit));
    lastRefreshLabel_->setText(state.lastRefresh.isValid()
        ? QStringLiteral("Last refresh: %1").arg(state.lastRefresh.toString(QStringLiteral("hh:mm:ss AP")))
        : QStringLiteral("Last refresh: --"));
}

void SeedProbePage::refreshDetails()
{
    const auto& state = controller_->viewState();
    neutralSeedValue_->setText(state.neutralSeedText.isEmpty() ? QStringLiteral("--") : state.neutralSeedText);
    probeIdValue_->setText(state.probeIdText.isEmpty() ? QStringLiteral("--") : state.probeIdText);
    statusValue_->setText(state.statusText.isEmpty() ? QStringLiteral("--") : state.statusText);
    codecValue_->setText(state.codecVersionText.isEmpty() ? QStringLiteral("--") : state.codecVersionText);
    savestateValue_->setText(state.savestateText.isEmpty() ? QStringLiteral("--") : state.savestateText);

    SeedProbeGridWidget::GridData mainData;
    mainData.minNeg = state.mainGrid.minNeg;
    mainData.maxPos = state.mainGrid.maxPos;
    mainData.hasData = state.mainGrid.hasData;
    for (const auto& point : state.mainGrid.points) {
        mainData.cells.push_back({ point.x, point.y, point.xSpan, point.ySpan, point.delta });
    }
    mainGrid_->setGridData(mainData);

    SeedProbeGridWidget::GridData cData;
    cData.minNeg = state.cStickGrid.minNeg;
    cData.maxPos = state.cStickGrid.maxPos;
    cData.hasData = state.cStickGrid.hasData;
    for (const auto& point : state.cStickGrid.points) {
        cData.cells.push_back({ point.x, point.y, point.xSpan, point.ySpan, point.delta });
    }
    cStickGrid_->setGridData(cData);

    SeedProbeGridWidget::GridData tData;
    tData.minNeg = state.triggerGrid.minNeg;
    tData.maxPos = state.triggerGrid.maxPos;
    tData.hasData = state.triggerGrid.hasData;
    for (const auto& point : state.triggerGrid.points) {
        tData.cells.push_back({ point.x, point.y, point.xSpan, point.ySpan, point.delta });
    }
    triggerGrid_->setGridData(tData);

    rebuildLegend(state.legendDeltas);

    QVector<SeedProbeUniqueTableModel::Row> uniqueRows;
    uniqueRows.reserve(state.uniqueRows.size());
    for (const auto& row : state.uniqueRows) {
        uniqueRows.push_back({ row.input, row.seedHex });
    }
    uniqueModel_->setRows(uniqueRows);
}

void SeedProbePage::updateStatusWidgets()
{
    const auto& state = controller_->viewState();
    if (!state.errorMessage.isEmpty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("error"));
        inlineMessageLabel_->setText(state.errorMessage);
        inlineMessageLabel_->show();
    } else if (!state.infoMessage.isEmpty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info"));
        inlineMessageLabel_->setText(state.infoMessage);
        inlineMessageLabel_->show();
    } else if (state.loadingList) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info"));
        inlineMessageLabel_->setText(QStringLiteral("Loading seed probes…"));
        inlineMessageLabel_->show();
    } else if (state.probeRows.isEmpty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info"));
        inlineMessageLabel_->setText(QStringLiteral("No seed probes matched the current filters."));
        inlineMessageLabel_->show();
    } else {
        inlineMessageLabel_->hide();
    }

    style()->unpolish(inlineMessageLabel_);
    style()->polish(inlineMessageLabel_);
}

void SeedProbePage::rebuildLegend(const QVector<int>& deltas)
{
    while (QLayoutItem* item = legendLayout_->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }

    if (deltas.isEmpty()) {
        legendLayout_->addWidget(createLegendSwatch(QStringLiteral("No legend data"), this));
        legendLayout_->addStretch();
        return;
    }

    const int step = std::max(1, deltas.size() / 12);
    for (int i = 0; i < deltas.size(); i += step) {
        const int value = deltas[i];
        const QString label = value > 0 ? QStringLiteral("+%1").arg(value) : QString::number(value);
        legendLayout_->addWidget(createLegendSwatch(label, this));
    }
    if ((deltas.size() - 1) % step != 0) {
        const int value = deltas.back();
        const QString label = value > 0 ? QStringLiteral("+%1").arg(value) : QString::number(value);
        legendLayout_->addWidget(createLegendSwatch(label, this));
    }
    legendLayout_->addStretch();
}
