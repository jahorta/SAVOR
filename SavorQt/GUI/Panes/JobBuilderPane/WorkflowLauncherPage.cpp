#include "WorkflowLauncherPage.h"

#include "WorkflowGraphEditorWindow.h"
#include "DB/SavorDbAuthoringService.h"
#include "DB/SavorDbWorkflowService.h"
#include "GUI/Refresh/RowUpdate.h"
#include "GUI/Widgets/ScrollBarStabilizer.h"

#include <QtCore/QDateTime>
#include <QtCore/QSignalBlocker>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QVBoxLayout>

#include <set>

WorkflowLauncherPage::WorkflowLauncherPage(QWidget* parent)
    : QWidget(parent)
{
    createWidgets();
    graphRefreshPipeline_ = new savorqt::gui::AsyncRefreshPipeline<int, WorkflowGraphListResult>(this);
    graphRefreshPipeline_->setAutoRefreshEnabled(false);
    graphRefreshPipeline_->setRequestBuilder([](savorqt::gui::RefreshReason) { return 0; });
    graphRefreshPipeline_->setLoadAndPrepare([](int) {
        return savorqt::gui::AsyncRefreshResult<WorkflowGraphListResult>::Ok(
            savorqt::db::SavorDbAuthoringService::ListWorkflowGraphs());
    });
    graphRefreshPipeline_->setApply([this](const WorkflowGraphListResult& result, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        if (!result.ok) {
            postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
            return;
        }

        captureLauncherDraft();
        const auto previousGraph = selectedGraph();
        const qint64 previousGraphId = previousGraph.has_value() ? previousGraph->workflow_graph_id : 0;
        workflowGraphs_ = result.value;
        int rowToSelect = -1;
        const ItemViewScrollSnapshot graphListScrollSnapshot = captureItemViewScrollSnapshot(graphList_);
        {
            QSignalBlocker blocker(graphList_);
            graphList_->clear();
            for (int row = 0; row < static_cast<int>(workflowGraphs_.size()); ++row) {
                const auto& graph = workflowGraphs_[static_cast<std::size_t>(row)];
                auto* item = new QListWidgetItem(graphListText(graph), graphList_);
                item->setData(Qt::UserRole, static_cast<qint64>(graph.workflow_graph_revision_id));
                if (graph.workflow_graph_id == previousGraphId) {
                    rowToSelect = row;
                }
            }
            if (!workflowGraphs_.empty()) {
                graphList_->setCurrentRow(rowToSelect >= 0 ? rowToSelect : 0);
            }
        }
        restoreItemViewScrollSnapshot(graphList_, graphListScrollSnapshot);
        if (!standaloneModeActive()) {
            renderCurrentGraphIfNeeded(rowToSelect < 0);
        }
    });
    graphRefreshPipeline_->setApplyError([this](const QString& error, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        postStatusMessage(error, StatusToast::Severity::Error);
    });
    graphRefreshPipeline_->setActive(true);

    unitRefreshPipeline_ = new savorqt::gui::AsyncRefreshPipeline<int, WorkflowUnitListResult>(this);
    unitRefreshPipeline_->setAutoRefreshEnabled(false);
    unitRefreshPipeline_->setRequestBuilder([](savorqt::gui::RefreshReason) { return 0; });
    unitRefreshPipeline_->setLoadAndPrepare([](int) {
        return savorqt::gui::AsyncRefreshResult<WorkflowUnitListResult>::Ok(
            savorqt::db::SavorDbWorkflowService::ListWorkflowUnits());
    });
    unitRefreshPipeline_->setApply([this](const WorkflowUnitListResult& result, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        if (!result.ok) {
            postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
            return;
        }

        captureLauncherDraft();
        const auto* previousUnit = selectedUnit();
        const QString previousUnitKind = previousUnit == nullptr ? QString() : QString::fromStdString(previousUnit->unit_kind);
        workflowUnits_ = result.value;
        int rowToSelect = -1;
        const ItemViewScrollSnapshot unitListScrollSnapshot = captureItemViewScrollSnapshot(unitList_);
        {
            QSignalBlocker blocker(unitList_);
            unitList_->clear();
            for (int row = 0; row < static_cast<int>(workflowUnits_.size()); ++row) {
                const auto& unit = workflowUnits_[static_cast<std::size_t>(row)];
                auto* item = new QListWidgetItem(unitListText(unit), unitList_);
                item->setData(Qt::UserRole, QString::fromStdString(unit.unit_kind));
                if (QString::fromStdString(unit.unit_kind) == previousUnitKind) {
                    rowToSelect = row;
                }
            }
            if (!workflowUnits_.empty()) {
                unitList_->setCurrentRow(rowToSelect >= 0 ? rowToSelect : 0);
            }
        }
        restoreItemViewScrollSnapshot(unitList_, unitListScrollSnapshot);
        if (standaloneModeActive()) {
            renderCurrentUnitIfNeeded(rowToSelect < 0);
        }
    });
    unitRefreshPipeline_->setApplyError([this](const QString& error, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        postStatusMessage(error, StatusToast::Severity::Error);
    });
    unitRefreshPipeline_->setActive(true);
    refreshWorkflowGraphs();
    refreshStandaloneUnits();
}

void WorkflowLauncherPage::createWidgets()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(10);

    auto* toolbar = new QFrame(this);
    toolbar->setObjectName("jobsToolbarPanel");
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(12, 10, 12, 10);
    toolbarLayout->setSpacing(10);

    refreshButton_ = new QPushButton(QStringLiteral("Refresh"), toolbar);
    editGraphsButton_ = new QPushButton(QStringLiteral("Edit Graphs"), toolbar);
    launchButton_ = new QPushButton(QStringLiteral("Launch Instance"), toolbar);
    refreshButton_->setObjectName("jobsSecondaryButton");
    editGraphsButton_->setObjectName("jobsSecondaryButton");
    launchButton_->setObjectName("jobsPrimaryButton");
    toolbarLayout->addWidget(refreshButton_);
    toolbarLayout->addWidget(editGraphsButton_);
    toolbarLayout->addStretch();
    toolbarLayout->addWidget(launchButton_);
    rootLayout->addWidget(toolbar);

    auto* body = new QFrame(this);
    body->setObjectName("jobsSurfacePanel");
    auto* bodyLayout = new QGridLayout(body);
    bodyLayout->setContentsMargins(18, 18, 18, 18);
    bodyLayout->setHorizontalSpacing(12);
    bodyLayout->setVerticalSpacing(10);

    auto* launchTargetTitle = new QLabel(QStringLiteral("Launch Target"), body);
    launchTargetTitle->setObjectName("panelTitle");
    launchModeTabs_ = new QTabWidget(body);
    auto* graphTab = new QWidget(launchModeTabs_);
    auto* graphTabLayout = new QVBoxLayout(graphTab);
    graphTabLayout->setContentsMargins(0, 0, 0, 0);
    graphTabLayout->setSpacing(8);
    graphList_ = new QListWidget(graphTab);
    graphList_->setSelectionMode(QAbstractItemView::SingleSelection);
    graphTabLayout->addWidget(graphList_, 1);
    auto* unitTab = new QWidget(launchModeTabs_);
    auto* unitTabLayout = new QVBoxLayout(unitTab);
    unitTabLayout->setContentsMargins(0, 0, 0, 0);
    unitTabLayout->setSpacing(8);
    unitList_ = new QListWidget(unitTab);
    unitList_->setSelectionMode(QAbstractItemView::SingleSelection);
    unitTabLayout->addWidget(unitList_, 1);
    launchModeTabs_->addTab(graphTab, QStringLiteral("Saved Graphs"));
    launchModeTabs_->addTab(unitTab, QStringLiteral("Standalone Units"));
    bodyLayout->addWidget(launchTargetTitle, 0, 0);
    bodyLayout->addWidget(launchModeTabs_, 1, 0, 4, 1);

    auto* instanceTitle = new QLabel(QStringLiteral("Instance Inputs"), body);
    instanceTitle->setObjectName("panelTitle");
    bodyLayout->addWidget(instanceTitle, 0, 1);

    auto* formPanel = new QFrame(body);
    auto* form = new QFormLayout(formPanel);
    form->setContentsMargins(0, 0, 0, 0);
    rootScopeKindEdit_ = new QLineEdit(formPanel);
    rootScopeKindEdit_->setText(QStringLiteral("manual"));
    rootScopeIdEdit_ = new QLineEdit(formPanel);
    rootScopeIdEdit_->setPlaceholderText(QStringLiteral("optional numeric id"));
    authoredRefLabel_ = new QLabel(QStringLiteral("Authored settings"), formPanel);
    authoredRefCombo_ = new QComboBox(formPanel);
    rtcRangeLabel_ = new QLabel(QStringLiteral("RTC range"), formPanel);
    rtcLowEdit_ = new QLineEdit(formPanel);
    rtcLowEdit_->setText(QStringLiteral("0"));
    rtcHighEdit_ = new QLineEdit(formPanel);
    rtcHighEdit_->setText(QStringLiteral("0"));
    rtcRangePanel_ = new QFrame(formPanel);
    auto* rtcLayout = new QHBoxLayout(rtcRangePanel_);
    rtcLayout->setContentsMargins(0, 0, 0, 0);
    rtcLayout->setSpacing(8);
    rtcLayout->addWidget(rtcLowEdit_);
    rtcLayout->addWidget(new QLabel(QStringLiteral("to"), rtcRangePanel_));
    rtcLayout->addWidget(rtcHighEdit_);
    seedSamplesLabel_ = new QLabel(QStringLiteral("Seed samples/axis"), formPanel);
    seedSamplesSpin_ = new QSpinBox(formPanel);
    seedSamplesSpin_->setRange(1, 255);
    seedSamplesSpin_->setValue(5);
    battleFakeOverrideCheck_ = new QCheckBox(QStringLiteral("Override battle fake range"), formPanel);
    battleFakeRangeLabel_ = new QLabel(QStringLiteral("Fake attack range"), formPanel);
    battleFakeMinSpin_ = new QSpinBox(formPanel);
    battleFakeMinSpin_->setRange(0, 100000);
    battleFakeMinSpin_->setValue(22);
    battleFakeMaxSpin_ = new QSpinBox(formPanel);
    battleFakeMaxSpin_->setRange(0, 100000);
    battleFakeMaxSpin_->setValue(25);
    battleFakeRangePanel_ = new QFrame(formPanel);
    auto* fakeLayout = new QHBoxLayout(battleFakeRangePanel_);
    fakeLayout->setContentsMargins(0, 0, 0, 0);
    fakeLayout->setSpacing(8);
    fakeLayout->addWidget(battleFakeMinSpin_);
    fakeLayout->addWidget(new QLabel(QStringLiteral("to"), battleFakeRangePanel_));
    fakeLayout->addWidget(battleFakeMaxSpin_);
    form->addRow(QStringLiteral("Root scope kind"), rootScopeKindEdit_);
    form->addRow(QStringLiteral("Root scope id"), rootScopeIdEdit_);
    form->addRow(authoredRefLabel_, authoredRefCombo_);
    form->addRow(rtcRangeLabel_, rtcRangePanel_);
    form->addRow(seedSamplesLabel_, seedSamplesSpin_);
    form->addRow(battleFakeOverrideCheck_);
    form->addRow(battleFakeRangeLabel_, battleFakeRangePanel_);
    bodyLayout->addWidget(formPanel, 1, 1);

    graphDetailLabel_ = new QLabel(body);
    graphDetailLabel_->setObjectName("sectionDescription");
    graphDetailLabel_->setWordWrap(true);
    bodyLayout->addWidget(graphDetailLabel_, 2, 1);

    externalInputsTable_ = new QTableWidget(body);
    externalInputsTable_->setColumnCount(5);
    externalInputsTable_->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("Node"),
        QStringLiteral("Input"),
        QStringLiteral("Data Kind"),
        QStringLiteral("Ref Kind"),
        QStringLiteral("Ref ID")
    });
    externalInputsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    externalInputsTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    externalInputsTable_->horizontalHeader()->setStretchLastSection(true);
    externalInputsTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    bodyLayout->addWidget(externalInputsTable_, 3, 1);

    launchStatusLabel_ = new QLabel(body);
    launchStatusLabel_->setObjectName("sectionDescription");
    launchStatusLabel_->setWordWrap(true);
    bodyLayout->addWidget(launchStatusLabel_, 4, 1);

    bodyLayout->setColumnStretch(0, 1);
    bodyLayout->setColumnStretch(1, 2);
    bodyLayout->setRowStretch(3, 1);
    rootLayout->addWidget(body, 1);

    connect(refreshButton_, &QPushButton::clicked, this, [this]() {
        refreshWorkflowGraphs();
        refreshStandaloneUnits();
    });
    connect(editGraphsButton_, &QPushButton::clicked, this, &WorkflowLauncherPage::openWorkflowGraphEditor);
    connect(launchButton_, &QPushButton::clicked, this, &WorkflowLauncherPage::launchSelectedGraph);
    connect(launchModeTabs_, &QTabWidget::currentChanged, this, &WorkflowLauncherPage::handleLaunchModeChanged);
    connect(graphList_, &QListWidget::currentRowChanged, this, &WorkflowLauncherPage::handleGraphSelectionChanged);
    connect(unitList_, &QListWidget::currentRowChanged, this, &WorkflowLauncherPage::handleStandaloneUnitSelectionChanged);
}

void WorkflowLauncherPage::openWorkflowGraphEditor()
{
    if (workflowGraphEditor_) {
        workflowGraphEditor_->show();
        workflowGraphEditor_->raise();
        workflowGraphEditor_->activateWindow();
        return;
    }

    auto* editor = new WorkflowGraphEditorWindow(nullptr);
    workflowGraphEditor_ = editor;
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) {
        postStatusMessage(text, severity);
    });
    editor->setSavedCallback([this]() { refreshWorkflowGraphs(); });
    connect(editor, &QObject::destroyed, this, [this]() { workflowGraphEditor_.clear(); });
    editor->show();
}

void WorkflowLauncherPage::refreshWorkflowGraphs()
{
    if (graphRefreshPipeline_ != nullptr) {
        graphRefreshPipeline_->requestRefresh(savorqt::gui::RefreshReason::Manual);
    }
}

void WorkflowLauncherPage::refreshStandaloneUnits()
{
    if (unitRefreshPipeline_ != nullptr) {
        unitRefreshPipeline_->requestRefresh(savorqt::gui::RefreshReason::Manual);
    }
}

void WorkflowLauncherPage::handleLaunchModeChanged()
{
    if (standaloneModeActive()) {
        handleStandaloneUnitSelectionChanged();
    } else {
        handleGraphSelectionChanged();
    }
}

void WorkflowLauncherPage::handleGraphSelectionChanged()
{
    if (standaloneModeActive()) {
        return;
    }
    captureLauncherDraft();
    renderCurrentGraphIfNeeded(true);
}

void WorkflowLauncherPage::renderCurrentGraphIfNeeded(bool forceRebuild)
{
    const auto graph = selectedGraph();
    const QString targetKey = currentDraftKey();
    const QString targetShape = graph.has_value() ? graphLaunchShapeSignature(*graph) : QString();
    if (!forceRebuild && targetKey == renderedTargetKey_ && targetShape == renderedTargetShape_) {
        return;
    }
    renderedTargetKey_ = targetKey;
    renderedTargetShape_ = targetShape;

    if (!graph.has_value()) {
        applyExternalInputs({});
        graphDetailLabel_->setText(QStringLiteral("No workflow graph selected."));
        launchStatusLabel_->setText(QString());
        launchButton_->setEnabled(false);
        authoredRefLabel_->hide();
        authoredRefCombo_->hide();
        rtcRangeLabel_->hide();
        rtcRangePanel_->hide();
        seedSamplesLabel_->hide();
        seedSamplesSpin_->hide();
        battleFakeOverrideCheck_->hide();
        battleFakeRangeLabel_->hide();
        battleFakeRangePanel_->hide();
        return;
    }

    populateExternalInputs(*graph);
    authoredRefLabel_->hide();
    authoredRefCombo_->hide();
    const bool hasTasMovie = !tasMovieNodeKeys(*graph).empty();
    const bool hasSeedProbe = !seedProbeNodeKeys(*graph).empty();
    const bool hasBattleChain = !battleChainNodeKeys(*graph).empty();
    rtcRangeLabel_->setVisible(hasTasMovie);
    rtcRangePanel_->setVisible(hasTasMovie);
    seedSamplesLabel_->setVisible(hasSeedProbe);
    seedSamplesSpin_->setVisible(hasSeedProbe);
    battleFakeOverrideCheck_->setVisible(hasBattleChain);
    battleFakeRangeLabel_->setVisible(hasBattleChain);
    battleFakeRangePanel_->setVisible(hasBattleChain);
    graphDetailLabel_->setText(QStringLiteral("%1 nodes, %2 edges, revision %3")
        .arg(static_cast<int>(graph->nodes.size()))
        .arg(static_cast<int>(graph->edges.size()))
        .arg(static_cast<qint64>(graph->workflow_graph_revision_id)));
    launchStatusLabel_->setText(externalInputs_.empty()
        ? (hasTasMovie
            ? QStringLiteral("All required inputs are supplied by graph edges. One workflow instance will be launched per RTC value.")
            : QStringLiteral("All required inputs are supplied by graph edges."))
        : (hasTasMovie
            ? QStringLiteral("Provide external references and RTC range. One workflow instance will be launched per RTC value.")
            : QStringLiteral("Provide one external reference for each required input below.")));
    launchButton_->setEnabled(true);
    restoreLauncherDraft();
}

void WorkflowLauncherPage::handleStandaloneUnitSelectionChanged()
{
    if (!standaloneModeActive()) {
        return;
    }
    captureLauncherDraft();
    renderCurrentUnitIfNeeded(true);
}

void WorkflowLauncherPage::renderCurrentUnitIfNeeded(bool forceRebuild)
{
    const auto* unit = selectedUnit();
    const QString targetKey = currentDraftKey();
    const QString targetShape = unit == nullptr ? QString() : unitLaunchShapeSignature(*unit);
    if (!forceRebuild && targetKey == renderedTargetKey_ && targetShape == renderedTargetShape_) {
        return;
    }
    renderedTargetKey_ = targetKey;
    renderedTargetShape_ = targetShape;

    if (unit == nullptr) {
        applyExternalInputs({});
        authoredRefOptions_.clear();
        authoredRefCombo_->clear();
        authoredRefLabel_->hide();
        authoredRefCombo_->hide();
        graphDetailLabel_->setText(QStringLiteral("No workflow unit selected."));
        launchStatusLabel_->setText(QString());
        launchButton_->setEnabled(false);
        rtcRangeLabel_->hide();
        rtcRangePanel_->hide();
        seedSamplesLabel_->hide();
        seedSamplesSpin_->hide();
        battleFakeOverrideCheck_->hide();
        battleFakeRangeLabel_->hide();
        battleFakeRangePanel_->hide();
        return;
    }

    refreshAuthoredRefsForStandaloneUnit(*unit);
    populateExternalInputsForUnit(*unit);
    const bool hasTasMovie = unit->unit_kind == "tas_movie";
    const bool hasSeedProbe = unit->unit_kind == "seed_probe_chain"
        || unit->unit_kind == "battle_seed_probe"
        || unit->unit_kind == "dungeon_seed_probe"
        || unit->unit_kind == "overworld_seed_probe";
    const bool hasBattleChain = unit->unit_kind == "battle_chain";
    rtcRangeLabel_->setVisible(hasTasMovie);
    rtcRangePanel_->setVisible(hasTasMovie);
    seedSamplesLabel_->setVisible(hasSeedProbe);
    seedSamplesSpin_->setVisible(hasSeedProbe);
    battleFakeOverrideCheck_->setVisible(hasBattleChain);
    battleFakeRangeLabel_->setVisible(hasBattleChain);
    battleFakeRangePanel_->setVisible(hasBattleChain);
    graphDetailLabel_->setText(QStringLiteral("%1\n%2 inputs, %3 possible outputs")
        .arg(QString::fromStdString(unit->description))
        .arg(static_cast<int>(unit->required_inputs.size()))
        .arg(static_cast<int>(unit->possible_outputs.size())));
    launchStatusLabel_->setText(hasTasMovie
        ? QStringLiteral("Select authored settings, provide external references, and choose the RTC range. A hidden single-unit graph will be reused or created before launch.")
        : QStringLiteral("Select authored settings if required and provide external references. A hidden single-unit graph will be reused or created before launch."));
    launchButton_->setEnabled(true);
    restoreLauncherDraft();
}

void WorkflowLauncherPage::captureLauncherDraft()
{
    if (renderedTargetKey_.isEmpty()) {
        return;
    }

    LauncherDraft draft;
    draft.rootScopeKind = rootScopeKindEdit_ == nullptr ? QString() : rootScopeKindEdit_->text();
    draft.rootScopeId = rootScopeIdEdit_ == nullptr ? QString() : rootScopeIdEdit_->text();
    draft.rtcLow = rtcLowEdit_ == nullptr ? QString() : rtcLowEdit_->text();
    draft.rtcHigh = rtcHighEdit_ == nullptr ? QString() : rtcHighEdit_->text();
    draft.seedSamplesPerAxis = seedSamplesSpin_ == nullptr ? 5 : seedSamplesSpin_->value();
    draft.battleFakeOverride = battleFakeOverrideCheck_ != nullptr && battleFakeOverrideCheck_->isChecked();
    draft.battleFakeMin = battleFakeMinSpin_ == nullptr ? 0 : battleFakeMinSpin_->value();
    draft.battleFakeMax = battleFakeMaxSpin_ == nullptr ? 0 : battleFakeMaxSpin_->value();
    draft.authoredRefId = authoredRefCombo_ == nullptr ? 0 : authoredRefCombo_->currentData().toLongLong();

    for (int row = 0; row < static_cast<int>(externalInputs_.size()) && row < externalInputsTable_->rowCount(); ++row) {
        const auto& input = externalInputs_[static_cast<std::size_t>(row)];
        const auto* refKindItem = externalInputsTable_->item(row, 3);
        const auto* refIdItem = externalInputsTable_->item(row, 4);
        draft.externalInputs[externalInputKey(input)] = ExternalInputDraft{
            refKindItem == nullptr ? QString() : refKindItem->text(),
            refIdItem == nullptr ? QString() : refIdItem->text(),
        };
    }

    launchDrafts_[renderedTargetKey_] = std::move(draft);
}

void WorkflowLauncherPage::restoreLauncherDraft()
{
    const auto it = launchDrafts_.find(renderedTargetKey_);
    if (it == launchDrafts_.end()) {
        return;
    }

    const LauncherDraft& draft = it->second;
    if (rootScopeKindEdit_ != nullptr) {
        rootScopeKindEdit_->setText(draft.rootScopeKind);
    }
    if (rootScopeIdEdit_ != nullptr) {
        rootScopeIdEdit_->setText(draft.rootScopeId);
    }
    if (rtcLowEdit_ != nullptr) {
        rtcLowEdit_->setText(draft.rtcLow);
    }
    if (rtcHighEdit_ != nullptr) {
        rtcHighEdit_->setText(draft.rtcHigh);
    }
    if (seedSamplesSpin_ != nullptr) {
        seedSamplesSpin_->setValue(draft.seedSamplesPerAxis);
    }
    if (battleFakeOverrideCheck_ != nullptr) {
        battleFakeOverrideCheck_->setChecked(draft.battleFakeOverride);
    }
    if (battleFakeMinSpin_ != nullptr) {
        battleFakeMinSpin_->setValue(draft.battleFakeMin);
    }
    if (battleFakeMaxSpin_ != nullptr) {
        battleFakeMaxSpin_->setValue(draft.battleFakeMax);
    }
    if (authoredRefCombo_ != nullptr && draft.authoredRefId > 0) {
        const int authoredIndex = authoredRefCombo_->findData(draft.authoredRefId);
        if (authoredIndex >= 0) {
            authoredRefCombo_->setCurrentIndex(authoredIndex);
        }
    }

    for (int row = 0; row < static_cast<int>(externalInputs_.size()) && row < externalInputsTable_->rowCount(); ++row) {
        const auto inputDraft = draft.externalInputs.find(externalInputKey(externalInputs_[static_cast<std::size_t>(row)]));
        if (inputDraft == draft.externalInputs.end()) {
            continue;
        }
        if (auto* refKindItem = externalInputsTable_->item(row, 3)) {
            refKindItem->setText(inputDraft->second.refKind);
        }
        if (auto* refIdItem = externalInputsTable_->item(row, 4)) {
            refIdItem->setText(inputDraft->second.refId);
        }
    }
}

void WorkflowLauncherPage::launchSelectedGraph()
{
    if (standaloneModeActive()) {
        launchStandaloneUnit();
        return;
    }

    const auto graph = selectedGraph();
    if (!graph.has_value()) {
        postStatusMessage(QStringLiteral("Select a workflow graph to launch."), StatusToast::Severity::Warn);
        return;
    }

    const auto tasNodes = tasMovieNodeKeys(*graph);
    const auto seedNodes = seedProbeNodeKeys(*graph);
    const auto battleNodes = battleChainNodeKeys(*graph);
    std::int64_t rtcLow = 0;
    std::int64_t rtcHigh = 0;
    if (!tasNodes.empty()) {
        bool lowOk = false;
        bool highOk = false;
        rtcLow = rtcLowEdit_->text().trimmed().toLongLong(&lowOk, 0);
        rtcHigh = rtcHighEdit_->text().trimmed().toLongLong(&highOk, 0);
        if (!lowOk || !highOk) {
            postStatusMessage(QStringLiteral("RTC low and high must be numeric."), StatusToast::Severity::Warn);
            return;
        }
        if (rtcHigh < rtcLow) {
            postStatusMessage(QStringLiteral("RTC high must be greater than or equal to RTC low."), StatusToast::Severity::Warn);
            return;
        }
    }
    const bool useBattleFakeOverride = !battleNodes.empty()
        && battleFakeOverrideCheck_ != nullptr
        && battleFakeOverrideCheck_->isChecked();
    int battleFakeMin = 0;
    int battleFakeMax = 0;
    if (useBattleFakeOverride) {
        battleFakeMin = battleFakeMinSpin_->value();
        battleFakeMax = battleFakeMaxSpin_->value();
        if (battleFakeMax < battleFakeMin) {
            postStatusMessage(QStringLiteral("Battle fake attack high must be greater than or equal to low."), StatusToast::Severity::Warn);
            return;
        }
    }

    const auto rootScopeKind = rootScopeKindEdit_->text().trimmed().isEmpty()
        ? std::string("manual")
        : rootScopeKindEdit_->text().trimmed().toStdString();
    std::optional<std::int64_t> rootScopeId;

    if (!rootScopeIdEdit_->text().trimmed().isEmpty()) {
        bool ok = false;
        const auto parsedRootScopeId = rootScopeIdEdit_->text().trimmed().toLongLong(&ok, 0);
        if (!ok || parsedRootScopeId <= 0) {
            postStatusMessage(QStringLiteral("Root scope id must be a positive number."), StatusToast::Severity::Warn);
            return;
        }
        rootScopeId = parsedRootScopeId;
    }

    std::vector<savorqt::db::WorkflowGraphInputBindingDraft> inputBindings;
    for (int row = 0; row < externalInputsTable_->rowCount(); ++row) {
        const auto refKindText = externalInputsTable_->item(row, 3)->text().trimmed();
        const auto refIdText = externalInputsTable_->item(row, 4)->text().trimmed();
        if (refKindText.isEmpty() || refIdText.isEmpty()) {
            postStatusMessage(QStringLiteral("Every external input requires ref kind and ref id."), StatusToast::Severity::Warn);
            return;
        }

        bool ok = false;
        const auto refId = refIdText.toLongLong(&ok, 0);
        if (!ok || refId <= 0) {
            postStatusMessage(QStringLiteral("External input ref id must be a positive number."), StatusToast::Severity::Warn);
            return;
        }

        const auto& input = externalInputs_[static_cast<std::size_t>(row)];
        inputBindings.push_back(savorqt::db::WorkflowGraphInputBindingDraft{
            .node_key = input.node_key.toStdString(),
            .input_key = input.input_key.toStdString(),
            .data_kind = input.data_kind.toStdString(),
            .ref_kind = refKindText.toStdString(),
            .ref_id = refId,
            .source_kind = "external",
        });
    }

    launchButton_->setEnabled(false);
    std::vector<std::int64_t> workflowIds;
    const std::int64_t launchLow = tasNodes.empty() ? 0 : rtcLow;
    const std::int64_t launchHigh = tasNodes.empty() ? 0 : rtcHigh;
    for (std::int64_t rtc = launchLow; rtc <= launchHigh; ++rtc) {
        savorqt::db::WorkflowGraphStartRequest request{};
        request.workflow_graph_revision_id = graph->workflow_graph_revision_id;
        request.root_scope_kind = rootScopeKind;
        request.root_scope_id = rootScopeId;
        request.input_bindings = inputBindings;
        for (const auto& nodeKey : tasNodes) {
            request.arguments.push_back(savorqt::db::WorkflowGraphArgumentDraft{
                .node_key = nodeKey.toStdString(),
                .argument_key = "rtc",
                .value_type = "integer",
                .integer_value = rtc,
                .source_kind = "launcher",
            });
        }
        for (const auto& nodeKey : seedNodes) {
            request.arguments.push_back(savorqt::db::WorkflowGraphArgumentDraft{
                .node_key = nodeKey.toStdString(),
                .argument_key = "samples_per_axis",
                .value_type = "integer",
                .integer_value = seedSamplesSpin_ == nullptr ? 5 : seedSamplesSpin_->value(),
                .source_kind = "launcher",
            });
        }
        if (useBattleFakeOverride) {
            for (const auto& nodeKey : battleNodes) {
                request.arguments.push_back(savorqt::db::WorkflowGraphArgumentDraft{
                    .node_key = nodeKey.toStdString(),
                    .argument_key = "fake_attack_min",
                    .value_type = "integer",
                    .integer_value = battleFakeMin,
                    .source_kind = "launcher",
                });
                request.arguments.push_back(savorqt::db::WorkflowGraphArgumentDraft{
                    .node_key = nodeKey.toStdString(),
                    .argument_key = "fake_attack_max",
                    .value_type = "integer",
                    .integer_value = battleFakeMax,
                    .source_kind = "launcher",
                });
            }
        }
        const auto result = savorqt::db::SavorDbWorkflowService::StartWorkflowGraphRevision(request);
        if (!result.ok) {
            launchButton_->setEnabled(true);
            postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
            return;
        }
        workflowIds.push_back(result.value);
    }
    launchButton_->setEnabled(true);
    if (workflowIds.size() == 1) {
        launchStatusLabel_->setText(QStringLiteral("Launched workflow instance %1.").arg(static_cast<qint64>(workflowIds.front())));
        postStatusMessage(QStringLiteral("Launched workflow instance %1.").arg(static_cast<qint64>(workflowIds.front())), StatusToast::Severity::Info);
    } else {
        launchStatusLabel_->setText(QStringLiteral("Launched %1 workflow instances for RTC %2-%3.")
            .arg(static_cast<int>(workflowIds.size()))
            .arg(static_cast<qint64>(rtcLow))
            .arg(static_cast<qint64>(rtcHigh)));
        postStatusMessage(QStringLiteral("Launched %1 workflow instances.").arg(static_cast<int>(workflowIds.size())), StatusToast::Severity::Info);
    }
}

void WorkflowLauncherPage::launchStandaloneUnit()
{
    const auto* unit = selectedUnit();
    if (unit == nullptr) {
        postStatusMessage(QStringLiteral("Select a workflow unit to launch."), StatusToast::Severity::Warn);
        return;
    }

    std::optional<std::string> authoredRefKind;
    std::optional<std::int64_t> authoredRefId;
    if (!unit->authored_refs.empty() && unit->authored_refs.front().required) {
        const int row = authoredRefCombo_ != nullptr ? authoredRefCombo_->currentIndex() : -1;
        if (row < 0 || row >= static_cast<int>(authoredRefOptions_.size())) {
            postStatusMessage(QStringLiteral("Select authored settings for this workflow unit."), StatusToast::Severity::Warn);
            return;
        }
        const auto& option = authoredRefOptions_[static_cast<std::size_t>(row)];
        authoredRefKind = option.ref_kind;
        authoredRefId = option.ref_id;
    }

    std::int64_t rtcLow = 0;
    std::int64_t rtcHigh = 0;
    if (unit->unit_kind == "tas_movie") {
        bool lowOk = false;
        bool highOk = false;
        rtcLow = rtcLowEdit_->text().trimmed().toLongLong(&lowOk, 0);
        rtcHigh = rtcHighEdit_->text().trimmed().toLongLong(&highOk, 0);
        if (!lowOk || !highOk) {
            postStatusMessage(QStringLiteral("RTC low and high must be numeric."), StatusToast::Severity::Warn);
            return;
        }
        if (rtcHigh < rtcLow) {
            postStatusMessage(QStringLiteral("RTC high must be greater than or equal to RTC low."), StatusToast::Severity::Warn);
            return;
        }
    }

    const bool useBattleFakeOverride = unit->unit_kind == "battle_chain"
        && battleFakeOverrideCheck_ != nullptr
        && battleFakeOverrideCheck_->isChecked();
    int battleFakeMin = 0;
    int battleFakeMax = 0;
    if (useBattleFakeOverride) {
        battleFakeMin = battleFakeMinSpin_->value();
        battleFakeMax = battleFakeMaxSpin_->value();
        if (battleFakeMax < battleFakeMin) {
            postStatusMessage(QStringLiteral("Battle fake attack high must be greater than or equal to low."), StatusToast::Severity::Warn);
            return;
        }
    }

    const auto rootScopeKind = rootScopeKindEdit_->text().trimmed().isEmpty()
        ? std::string("manual")
        : rootScopeKindEdit_->text().trimmed().toStdString();
    std::optional<std::int64_t> rootScopeId;
    if (!rootScopeIdEdit_->text().trimmed().isEmpty()) {
        bool ok = false;
        const auto parsedRootScopeId = rootScopeIdEdit_->text().trimmed().toLongLong(&ok, 0);
        if (!ok || parsedRootScopeId <= 0) {
            postStatusMessage(QStringLiteral("Root scope id must be a positive number."), StatusToast::Severity::Warn);
            return;
        }
        rootScopeId = parsedRootScopeId;
    }

    std::vector<savorqt::db::WorkflowGraphInputBindingDraft> inputBindings;
    for (int row = 0; row < externalInputsTable_->rowCount(); ++row) {
        const auto refKindText = externalInputsTable_->item(row, 3)->text().trimmed();
        const auto refIdText = externalInputsTable_->item(row, 4)->text().trimmed();
        if (refKindText.isEmpty() || refIdText.isEmpty()) {
            postStatusMessage(QStringLiteral("Every external input requires ref kind and ref id."), StatusToast::Severity::Warn);
            return;
        }

        bool ok = false;
        const auto refId = refIdText.toLongLong(&ok, 0);
        if (!ok || refId <= 0) {
            postStatusMessage(QStringLiteral("External input ref id must be a positive number."), StatusToast::Severity::Warn);
            return;
        }

        const auto& input = externalInputs_[static_cast<std::size_t>(row)];
        inputBindings.push_back(savorqt::db::WorkflowGraphInputBindingDraft{
            .node_key = input.node_key.toStdString(),
            .input_key = input.input_key.toStdString(),
            .data_kind = input.data_kind.toStdString(),
            .ref_kind = refKindText.toStdString(),
            .ref_id = refId,
            .source_kind = "external",
        });
    }

    launchButton_->setEnabled(false);
    const auto graphResult = savorqt::db::SavorDbWorkflowService::EnsureStandaloneWorkflowUnitGraph(
        savorqt::db::StandaloneWorkflowUnitGraphRequest{
            .unit_kind = unit->unit_kind,
            .authored_ref_kind = authoredRefKind,
            .authored_ref_id = authoredRefId,
            .hidden = true,
        });
    if (!graphResult.ok) {
        launchButton_->setEnabled(true);
        postStatusMessage(QString::fromStdString(graphResult.error.message), StatusToast::Severity::Error);
        return;
    }

    std::vector<std::int64_t> workflowIds;
    const std::int64_t launchLow = unit->unit_kind == "tas_movie" ? rtcLow : 0;
    const std::int64_t launchHigh = unit->unit_kind == "tas_movie" ? rtcHigh : 0;
    const auto nodeKey = standaloneNodeKey(*unit);
    for (std::int64_t rtc = launchLow; rtc <= launchHigh; ++rtc) {
        savorqt::db::WorkflowGraphStartRequest request{};
        request.workflow_graph_revision_id = graphResult.value.workflow_graph_revision_id;
        request.root_scope_kind = rootScopeKind;
        request.root_scope_id = rootScopeId;
        request.input_bindings = inputBindings;
        if (unit->unit_kind == "tas_movie") {
            request.arguments.push_back(savorqt::db::WorkflowGraphArgumentDraft{
                .node_key = nodeKey,
                .argument_key = "rtc",
                .value_type = "integer",
                .integer_value = rtc,
                .source_kind = "launcher",
            });
        }
        if (unit->unit_kind == "seed_probe_chain"
            || unit->unit_kind == "battle_seed_probe"
            || unit->unit_kind == "dungeon_seed_probe"
            || unit->unit_kind == "overworld_seed_probe") {
            request.arguments.push_back(savorqt::db::WorkflowGraphArgumentDraft{
                .node_key = nodeKey,
                .argument_key = "samples_per_axis",
                .value_type = "integer",
                .integer_value = seedSamplesSpin_ == nullptr ? 5 : seedSamplesSpin_->value(),
                .source_kind = "launcher",
            });
        }
        if (useBattleFakeOverride) {
            request.arguments.push_back(savorqt::db::WorkflowGraphArgumentDraft{
                .node_key = nodeKey,
                .argument_key = "fake_attack_min",
                .value_type = "integer",
                .integer_value = battleFakeMin,
                .source_kind = "launcher",
            });
            request.arguments.push_back(savorqt::db::WorkflowGraphArgumentDraft{
                .node_key = nodeKey,
                .argument_key = "fake_attack_max",
                .value_type = "integer",
                .integer_value = battleFakeMax,
                .source_kind = "launcher",
            });
        }

        const auto result = savorqt::db::SavorDbWorkflowService::StartWorkflowGraphRevision(request);
        if (!result.ok) {
            launchButton_->setEnabled(true);
            postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
            return;
        }
        workflowIds.push_back(result.value);
    }

    launchButton_->setEnabled(true);
    refreshWorkflowGraphs();
    if (workflowIds.size() == 1) {
        launchStatusLabel_->setText(QStringLiteral("Launched standalone %1 workflow instance %2 from graph revision %3.")
            .arg(QString::fromStdString(unit->display_name))
            .arg(static_cast<qint64>(workflowIds.front()))
            .arg(static_cast<qint64>(graphResult.value.workflow_graph_revision_id)));
        postStatusMessage(QStringLiteral("Launched workflow instance %1.").arg(static_cast<qint64>(workflowIds.front())), StatusToast::Severity::Info);
    } else {
        launchStatusLabel_->setText(QStringLiteral("Launched %1 standalone %2 workflow instances for RTC %3-%4 from graph revision %5.")
            .arg(static_cast<int>(workflowIds.size()))
            .arg(QString::fromStdString(unit->display_name))
            .arg(static_cast<qint64>(rtcLow))
            .arg(static_cast<qint64>(rtcHigh))
            .arg(static_cast<qint64>(graphResult.value.workflow_graph_revision_id)));
        postStatusMessage(QStringLiteral("Launched %1 workflow instances.").arg(static_cast<int>(workflowIds.size())), StatusToast::Severity::Info);
    }
}

void WorkflowLauncherPage::populateExternalInputs(const savor::db::WorkflowGraphSnapshot& graph)
{
    std::set<QString> suppliedByEdge;
    for (const auto& edge : graph.edges) {
        suppliedByEdge.emplace(QString::fromStdString(edge.to_node_key + "\n" + edge.input_key));
    }

    std::vector<ExternalInputRow> rows;
    for (const auto& node : graph.nodes) {
        for (const auto& input : node.inputs) {
            if (!input.required) {
                continue;
            }
            const auto edgeKey = QString::fromStdString(node.node_key + "\n" + input.input_key);
            if (suppliedByEdge.find(edgeKey) != suppliedByEdge.end()) {
                continue;
            }
            const auto dataKind = QString::fromStdString(input.data_kind);
            rows.push_back(ExternalInputRow{
                .node_key = QString::fromStdString(node.node_key),
                .node_name = nodeDisplayName(graph, node.node_key),
                .input_key = QString::fromStdString(input.input_key),
                .display_name = QString::fromStdString(input.display_name),
                .data_kind = dataKind,
                .default_ref_kind = defaultRefKindForDataKind(dataKind),
            });
        }
    }

    applyExternalInputs(std::move(rows));
}

void WorkflowLauncherPage::populateExternalInputsForUnit(const WorkflowUnitDefinition& unit)
{
    std::vector<ExternalInputRow> rows;
    const auto nodeKey = QString::fromStdString(standaloneNodeKey(unit));
    for (const auto& input : unit.required_inputs) {
        if (!input.required) {
            continue;
        }
        const auto dataKind = QString::fromStdString(input.data_kind);
        rows.push_back(ExternalInputRow{
            .node_key = nodeKey,
            .node_name = QString::fromStdString(unit.display_name),
            .input_key = QString::fromStdString(input.key),
            .display_name = QString::fromStdString(input.display_name),
            .data_kind = dataKind,
            .default_ref_kind = defaultRefKindForDataKind(dataKind),
        });
    }

    applyExternalInputs(std::move(rows));
}

void WorkflowLauncherPage::applyExternalInputs(std::vector<ExternalInputRow> rows)
{
    externalInputs_ = rows;
    savorqt::gui::ApplyTableRowsByKey(
        externalInputsTable_,
        currentExternalInputRows_,
        externalInputs_,
        [](const ExternalInputRow& row) { return externalInputKey(row); },
        [](const ExternalInputRow& lhs, const ExternalInputRow& rhs) {
            return lhs.node_key == rhs.node_key
                && lhs.node_name == rhs.node_name
                && lhs.input_key == rhs.input_key
                && lhs.display_name == rhs.display_name
                && lhs.data_kind == rhs.data_kind
                && lhs.default_ref_kind == rhs.default_ref_kind;
        },
        [](QTableWidget* table, int row, const ExternalInputRow& input) {
            table->setItem(row, 0, new QTableWidgetItem(input.node_name));
            table->setItem(row, 1, new QTableWidgetItem(input.display_name.isEmpty() ? input.input_key : input.display_name));
            table->setItem(row, 2, new QTableWidgetItem(input.data_kind));
            table->setItem(row, 3, new QTableWidgetItem(input.default_ref_kind));
            table->setItem(row, 4, new QTableWidgetItem(QString()));
            for (int column = 0; column < 3; ++column) {
                table->item(row, column)->setFlags(table->item(row, column)->flags() & ~Qt::ItemIsEditable);
            }
        });
}

void WorkflowLauncherPage::refreshAuthoredRefsForStandaloneUnit(const WorkflowUnitDefinition& unit)
{
    authoredRefOptions_.clear();
    authoredRefCombo_->clear();
    if (unit.authored_refs.empty()) {
        authoredRefLabel_->hide();
        authoredRefCombo_->hide();
        return;
    }

    const auto& requirement = unit.authored_refs.front();
    authoredRefLabel_->setText(QString::fromStdString(requirement.display_name.empty() ? requirement.ref_kind : requirement.display_name));
    authoredRefLabel_->show();
    authoredRefCombo_->show();

    if (requirement.ref_kind == "tas_spec") {
        const auto result = savorqt::db::SavorDbAuthoringService::ListTasSpecs();
        if (!result.ok) {
            postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
            return;
        }
        for (const auto& spec : result.value) {
            authoredRefOptions_.push_back(AuthoredRefOption{
                .label = QStringLiteral("#%1 %2")
                    .arg(static_cast<qint64>(spec.tas_spec_id))
                    .arg(QString::fromStdString(spec.base_name)),
                .ref_kind = "tas_spec",
                .ref_id = spec.tas_spec_id,
            });
        }
    } else if (requirement.ref_kind == "seed_probe_spec") {
        const auto result = savorqt::db::SavorDbAuthoringService::ListSeedProbeSpecs();
        if (!result.ok) {
            postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
            return;
        }
        for (const auto& spec : result.value) {
            authoredRefOptions_.push_back(AuthoredRefOption{
                .label = QStringLiteral("#%1 %2")
                    .arg(static_cast<qint64>(spec.seed_probe_spec_id))
                    .arg(QString::fromStdString(spec.name)),
                .ref_kind = "seed_probe_spec",
                .ref_id = spec.seed_probe_spec_id,
            });
        }
    } else if (requirement.ref_kind == "authoring.battle_chain_spec") {
        const auto result = savorqt::db::SavorDbAuthoringService::ListBattleChainSpecs();
        if (!result.ok) {
            postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
            return;
        }
        for (const auto& battle_chain_spec : result.value) {
            authoredRefOptions_.push_back(AuthoredRefOption{
                .label = QStringLiteral("#%1 %2 battle=%3 battle explorer=%4")
                    .arg(static_cast<qint64>(battle_chain_spec.battle_chain_spec_id))
                    .arg(QString::fromStdString(battle_chain_spec.name))
                    .arg(QString::number(battle_chain_spec.battle_run_spec_id))
                    .arg(QString::number(battle_chain_spec.explorer_settings_id)),
                .ref_kind = "authoring.battle_chain_spec",
                .ref_id = battle_chain_spec.battle_chain_spec_id,
            });
        }
    }

    for (const auto& option : authoredRefOptions_) {
        authoredRefCombo_->addItem(option.label, static_cast<qint64>(option.ref_id));
    }
    if (authoredRefOptions_.empty()) {
        authoredRefCombo_->addItem(QStringLiteral("(no authored settings available)"), 0);
    }
}

std::optional<savor::db::WorkflowGraphSnapshot> WorkflowLauncherPage::selectedGraph() const
{
    const int row = graphList_ != nullptr ? graphList_->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(workflowGraphs_.size())) {
        return std::nullopt;
    }
    return workflowGraphs_[static_cast<std::size_t>(row)];
}

const WorkflowLauncherPage::WorkflowUnitDefinition* WorkflowLauncherPage::selectedUnit() const
{
    const int row = unitList_ != nullptr ? unitList_->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(workflowUnits_.size())) {
        return nullptr;
    }
    return &workflowUnits_[static_cast<std::size_t>(row)];
}

bool WorkflowLauncherPage::standaloneModeActive() const
{
    return launchModeTabs_ != nullptr && launchModeTabs_->currentIndex() == 1;
}

QString WorkflowLauncherPage::currentDraftKey() const
{
    if (standaloneModeActive()) {
        const auto* unit = selectedUnit();
        return unit == nullptr
            ? QStringLiteral("unit:")
            : QStringLiteral("unit:%1").arg(QString::fromStdString(unit->unit_kind));
    }

    const auto graph = selectedGraph();
    return graph.has_value()
        ? QStringLiteral("graph:%1").arg(static_cast<qint64>(graph->workflow_graph_id))
        : QStringLiteral("graph:");
}

void WorkflowLauncherPage::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    if (text.isEmpty()) {
        return;
    }
    emit statusToastRequested(StatusToast{ severity, text, {}, 1, QDateTime{}, 4000 });
}

QString WorkflowLauncherPage::externalInputKey(const ExternalInputRow& input)
{
    return input.node_key + QChar(0x1f) + input.input_key;
}

QString WorkflowLauncherPage::graphLaunchShapeSignature(const savor::db::WorkflowGraphSnapshot& graph)
{
    QStringList parts;
    std::set<QString> suppliedByEdge;
    for (const auto& edge : graph.edges) {
        suppliedByEdge.emplace(QString::fromStdString(edge.to_node_key + "\n" + edge.input_key));
    }

    for (const auto& node : graph.nodes) {
        const QString unitKind = QString::fromStdString(node.unit_kind);
        parts << QStringLiteral("node:%1:%2").arg(QString::fromStdString(node.node_key), unitKind);
        if (unitKind == QStringLiteral("tas_movie")) {
            parts << QStringLiteral("rtc");
        }
        if (unitKind == QStringLiteral("seed_probe_chain")
            || unitKind == QStringLiteral("battle_seed_probe")
            || unitKind == QStringLiteral("dungeon_seed_probe")
            || unitKind == QStringLiteral("overworld_seed_probe")) {
            parts << QStringLiteral("samples_per_axis");
        }
        if (unitKind == QStringLiteral("battle_chain")) {
            parts << QStringLiteral("battle_fake");
        }
        for (const auto& input : node.inputs) {
            if (!input.required) {
                continue;
            }
            const auto edgeKey = QString::fromStdString(node.node_key + "\n" + input.input_key);
            if (suppliedByEdge.find(edgeKey) != suppliedByEdge.end()) {
                continue;
            }
            parts << QStringLiteral("input:%1:%2:%3")
                .arg(QString::fromStdString(node.node_key), QString::fromStdString(input.input_key), QString::fromStdString(input.data_kind));
        }
    }
    return parts.join(QChar(0x1d));
}

QString WorkflowLauncherPage::unitLaunchShapeSignature(const WorkflowUnitDefinition& unit)
{
    QStringList parts;
    parts << QStringLiteral("unit:%1").arg(QString::fromStdString(unit.unit_kind));
    if (unit.unit_kind == "tas_movie") {
        parts << QStringLiteral("rtc");
    }
    if (unit.unit_kind == "seed_probe_chain"
        || unit.unit_kind == "battle_seed_probe"
        || unit.unit_kind == "dungeon_seed_probe"
        || unit.unit_kind == "overworld_seed_probe") {
        parts << QStringLiteral("samples_per_axis");
    }
    if (unit.unit_kind == "battle_chain") {
        parts << QStringLiteral("battle_fake");
    }
    for (const auto& authoredRef : unit.authored_refs) {
        parts << QStringLiteral("authored:%1:%2")
            .arg(QString::fromStdString(authoredRef.ref_kind))
            .arg(authoredRef.required ? QStringLiteral("required") : QStringLiteral("optional"));
    }
    for (const auto& input : unit.required_inputs) {
        if (!input.required) {
            continue;
        }
        parts << QStringLiteral("input:%1:%2")
            .arg(QString::fromStdString(input.key), QString::fromStdString(input.data_kind));
    }
    return parts.join(QChar(0x1d));
}

QString WorkflowLauncherPage::graphListText(const savor::db::WorkflowGraphSnapshot& graph)
{
    return QStringLiteral("#%1 r%2  %3\n%4 nodes, %5 edges")
        .arg(static_cast<qint64>(graph.workflow_graph_id))
        .arg(graph.graph_version)
        .arg(QString::fromStdString(graph.name))
        .arg(static_cast<int>(graph.nodes.size()))
        .arg(static_cast<int>(graph.edges.size()));
}

QString WorkflowLauncherPage::nodeDisplayName(
    const savor::db::WorkflowGraphSnapshot& graph,
    const std::string& node_key)
{
    for (const auto& node : graph.nodes) {
        if (node.node_key == node_key) {
            return QStringLiteral("%1 (%2)")
                .arg(QString::fromStdString(node.display_name.empty() ? node.node_key : node.display_name))
                .arg(QString::fromStdString(node.node_key));
        }
    }
    return QString::fromStdString(node_key);
}

QString WorkflowLauncherPage::defaultRefKindForDataKind(const QString& data_kind)
{
    if (data_kind == QStringLiteral("state_artifact.dtm_artifact_id")) {
        return QStringLiteral("state_artifact");
    }
    if (data_kind == QStringLiteral("state.savestate_id")) {
        return QStringLiteral("state.savestate");
    }
    if (data_kind == QStringLiteral("analysis.input_frame_set_id")) {
        return QStringLiteral("au.input_set");
    }
    if (data_kind == QStringLiteral("analysis.battle_manual_followup_id")) {
        return QStringLiteral("analysis.battle_manual_followup");
    }
    return data_kind;
}

QString WorkflowLauncherPage::unitListText(const WorkflowUnitDefinition& unit)
{
    return QStringLiteral("%1\n%2 inputs, %3 possible outputs")
        .arg(QString::fromStdString(unit.display_name))
        .arg(static_cast<int>(unit.required_inputs.size()))
        .arg(static_cast<int>(unit.possible_outputs.size()));
}

std::string WorkflowLauncherPage::standaloneNodeKey(const WorkflowUnitDefinition& unit)
{
    return unit.unit_kind + "_standalone";
}

std::vector<QString> WorkflowLauncherPage::tasMovieNodeKeys(const savor::db::WorkflowGraphSnapshot& graph)
{
    std::vector<QString> keys;
    for (const auto& node : graph.nodes) {
        if (node.unit_kind == "tas_movie") {
            keys.push_back(QString::fromStdString(node.node_key));
        }
    }
    return keys;
}

std::vector<QString> WorkflowLauncherPage::seedProbeNodeKeys(const savor::db::WorkflowGraphSnapshot& graph)
{
    std::vector<QString> keys;
    for (const auto& node : graph.nodes) {
        if (node.unit_kind == "seed_probe_chain"
            || node.unit_kind == "battle_seed_probe"
            || node.unit_kind == "dungeon_seed_probe"
            || node.unit_kind == "overworld_seed_probe") {
            keys.push_back(QString::fromStdString(node.node_key));
        }
    }
    return keys;
}

std::vector<QString> WorkflowLauncherPage::battleChainNodeKeys(const savor::db::WorkflowGraphSnapshot& graph)
{
    std::vector<QString> keys;
    for (const auto& node : graph.nodes) {
        if (node.unit_kind == "battle_chain") {
            keys.push_back(QString::fromStdString(node.node_key));
        }
    }
    return keys;
}
