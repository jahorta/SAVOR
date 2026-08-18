#include "WorkflowLauncherPage.h"

#include "WorkflowGraphEditorWindow.h"
#include "DB/SavorDbAuthoringService.h"
#include "DB/SavorDbWorkflowService.h"
#include "GUI/Refresh/RowUpdate.h"
#include "GUI/Widgets/ScrollBarStabilizer.h"

#include <QtCore/QDateTime>
#include <QtCore/QSignalBlocker>
#include <QtCore/QVariant>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QCompleter>
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
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <numeric>
#include <limits>
#include <set>

namespace {
constexpr int kMemberUnitKindRole = Qt::UserRole + 1;
constexpr int kInputKeyRole = Qt::UserRole + 2;
constexpr int kDataKindRole = Qt::UserRole + 3;
constexpr int kRefKindRole = Qt::UserRole + 4;
}

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

    unitRefreshPipeline_ = new savorqt::gui::AsyncRefreshPipeline<int, StandaloneLaunchEntryListResult>(this);
    unitRefreshPipeline_->setAutoRefreshEnabled(false);
    unitRefreshPipeline_->setRequestBuilder([](savorqt::gui::RefreshReason) { return 0; });
    unitRefreshPipeline_->setLoadAndPrepare([](int) {
        return savorqt::gui::AsyncRefreshResult<StandaloneLaunchEntryListResult>::Ok(
            savorqt::db::SavorDbWorkflowService::ListStandaloneLaunchEntries());
    });
    unitRefreshPipeline_->setApply([this](const StandaloneLaunchEntryListResult& result, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        if (!result.ok) {
            postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
            return;
        }

        captureLauncherDraft();
        const auto* previousEntry = selectedStandaloneEntry();
        const QString previousEntryKey = previousEntry == nullptr
            ? QString()
            : QString::fromStdString(previousEntry->presentation_key);
        standaloneLaunchEntries_ = result.value;
        int rowToSelect = -1;
        const ItemViewScrollSnapshot unitListScrollSnapshot = captureItemViewScrollSnapshot(unitList_);
        {
            QSignalBlocker blocker(unitList_);
            unitList_->clear();
            for (int row = 0; row < static_cast<int>(standaloneLaunchEntries_.size()); ++row) {
                const auto& entry = standaloneLaunchEntries_[static_cast<std::size_t>(row)];
                auto* item = new QListWidgetItem(
                    QStringLiteral("%1\n%2 workflow contract%3")
                        .arg(QString::fromStdString(entry.display_name))
                        .arg(static_cast<int>(entry.members.size()))
                        .arg(entry.members.size() == 1u ? QString() : QStringLiteral("s")),
                    unitList_);
                item->setData(Qt::UserRole, QString::fromStdString(entry.presentation_key));
                if (QString::fromStdString(entry.presentation_key) == previousEntryKey) {
                    rowToSelect = row;
                }
                if (!pendingUnitKind_.isEmpty() &&
                    std::any_of(entry.members.begin(), entry.members.end(), [this](const auto& member) {
                        return QString::fromStdString(member.unit_kind) == pendingUnitKind_;
                    })) {
                    activeStandaloneMemberUnitKind_ = pendingUnitKind_;
                    rowToSelect = row;
                }
            }
            if (!standaloneLaunchEntries_.empty()) {
                unitList_->setCurrentRow(rowToSelect >= 0 ? rowToSelect : 0);
            }
        }
        restoreItemViewScrollSnapshot(unitList_, unitListScrollSnapshot);
        if (standaloneModeActive()) {
            renderCurrentUnitIfNeeded(rowToSelect < 0);
        }
        pendingUnitKind_.clear();
    });
    unitRefreshPipeline_->setApplyError([this](const QString& error, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        postStatusMessage(error, StatusToast::Severity::Error);
    });
    unitRefreshPipeline_->setActive(true);
    referenceRefreshPipeline_ = new savorqt::gui::AsyncRefreshPipeline<std::vector<ExternalInputRow>, ReferenceOptionsResult>(this);
    referenceRefreshPipeline_->setAutoRefreshEnabled(false);
    referenceRefreshPipeline_->setRequestBuilder([this](savorqt::gui::RefreshReason) { return externalInputs_; });
    referenceRefreshPipeline_->setLoadAndPrepare([](std::vector<ExternalInputRow> inputs) {
        ReferenceOptions options;
        for (const auto& input : inputs) {
            if (input.satisfied_by_edge) continue;
            const auto result = input.presentation_family_key.isEmpty()
                ? savorqt::db::WorkflowReferenceSelectorProvider::List(
                    input.ref_kind.toStdString(), input.data_kind.toStdString())
                : savorqt::db::WorkflowReferenceSelectorProvider::ListPresentationFamily(
                    input.presentation_family_key.toStdString());
            if (!result.ok) return savorqt::gui::AsyncRefreshResult<ReferenceOptionsResult>::Ok(ReferenceOptionsResult::Err(result.error));
            options.emplace(externalInputKey(input), result.value);
        }
        return savorqt::gui::AsyncRefreshResult<ReferenceOptionsResult>::Ok(ReferenceOptionsResult::Ok(std::move(options)));
    });
    referenceRefreshPipeline_->setApply([this](const ReferenceOptionsResult& result, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        if (!result.ok) { postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error); return; }
        const auto draft = launchDrafts_.find(renderedTargetKey_);
        for (int row=0; row<externalInputsTable_->rowCount() && row<static_cast<int>(externalInputs_.size()); ++row) {
            if (externalInputs_[static_cast<std::size_t>(row)].satisfied_by_edge) continue;
            auto* combo=qobject_cast<QComboBox*>(externalInputsTable_->cellWidget(row,4)); if(combo==nullptr)continue;
            const auto key=externalInputKey(externalInputs_[static_cast<std::size_t>(row)]);
            const auto priorDraft = draft!=launchDrafts_.end() && draft->second.externalInputs.contains(key)
                ? std::optional<ExternalInputDraft>(draft->second.externalInputs.at(key))
                : std::nullopt;
            const QString prior = priorDraft.has_value() ? priorDraft->refId : combo->currentText();
            QSignalBlocker blocker(combo); combo->clear(); combo->addItem(QStringLiteral("Select..."),QVariant::fromValue<qint64>(0));
            const auto it=result.value.find(key);
            if(it!=result.value.end()) for(const auto& option:it->second) {
                combo->addItem(QString::fromStdString(option.primary_label+" — "+option.secondary_evidence),QVariant::fromValue<qint64>(option.ref_id));
                const int index = combo->count() - 1;
                combo->setItemData(index, QString::fromStdString(option.member_unit_kind), kMemberUnitKindRole);
                combo->setItemData(index, QString::fromStdString(option.input_key), kInputKeyRole);
                combo->setItemData(index, QString::fromStdString(option.data_kind), kDataKindRole);
                combo->setItemData(index, QString::fromStdString(option.ref_kind), kRefKindRole);
            }
            bool ok=false;const auto priorId=prior.toLongLong(&ok);if(ok&&priorId>0){
                int index = -1;
                for (int candidate = 1; candidate < combo->count(); ++candidate) {
                    if (combo->itemData(candidate).toLongLong() != priorId) continue;
                    if (priorDraft.has_value() && !priorDraft->memberUnitKind.isEmpty() &&
                        combo->itemData(candidate, kMemberUnitKindRole).toString() != priorDraft->memberUnitKind) continue;
                    index = candidate;
                    break;
                }
                if(index>=0)combo->setCurrentIndex(index);else combo->setEditText(prior);
            }
            blocker.unblock();
            handleStandaloneSourceSelectionChanged(row);
        }
    });
    referenceRefreshPipeline_->setActive(true);
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
    seedSamplesSpin_->setRange(1, 64);
    seedSamplesSpin_->setValue(5);
    battleFakeRangeLabel_ = new QLabel(QStringLiteral("Fake attack range"), formPanel);
    battleFakeMinSpin_ = new QSpinBox(formPanel);
    battleFakeMinSpin_->setRange(0, std::numeric_limits<int>::max());
    battleFakeMinSpin_->setValue(0);
    battleFakeMaxSpin_ = new QSpinBox(formPanel);
    battleFakeMaxSpin_->setRange(0, std::numeric_limits<int>::max());
    battleFakeMaxSpin_->setValue(0);
    battleFakeRangePanel_ = new QFrame(formPanel);
    auto* fakeLayout = new QHBoxLayout(battleFakeRangePanel_);
    fakeLayout->setContentsMargins(0, 0, 0, 0);
    fakeLayout->setSpacing(8);
    fakeLayout->addWidget(battleFakeMinSpin_);
    fakeLayout->addWidget(new QLabel(QStringLiteral("to"), battleFakeRangePanel_));
    fakeLayout->addWidget(battleFakeMaxSpin_);
    continuationLabel_ = new QLabel(QStringLiteral("Continuation"), formPanel);
    continuationCombo_ = new QComboBox(formPanel);
    continuationCombo_->setEditable(false);
    form->addRow(authoredRefLabel_, authoredRefCombo_);
    form->addRow(rtcRangeLabel_, rtcRangePanel_);
    form->addRow(seedSamplesLabel_, seedSamplesSpin_);
    form->addRow(battleFakeRangeLabel_, battleFakeRangePanel_);
    form->addRow(continuationLabel_, continuationCombo_);
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
        battleFakeRangeLabel_->hide();
        battleFakeRangePanel_->hide();
        continuationLabel_->hide();
        continuationCombo_->hide();
        return;
    }

    populateExternalInputs(*graph);
    authoredRefLabel_->hide();
    authoredRefCombo_->hide();
    const bool hasTasMovie = !argumentNodeKeys(*graph,"rtc").empty();
    const bool hasSeedProbe = !argumentNodeKeys(*graph,"samples_per_axis").empty();
    const bool hasBattle = !argumentNodeKeys(*graph,"fake_attack_min").empty();
    const auto continuationNodes = argumentNodeKeys(*graph, "continuation_mode");
    rtcRangeLabel_->setVisible(hasTasMovie);
    rtcRangePanel_->setVisible(hasTasMovie);
    seedSamplesLabel_->setVisible(hasSeedProbe);
    seedSamplesSpin_->setVisible(hasSeedProbe);
    battleFakeRangeLabel_->setVisible(hasBattle);
    battleFakeRangePanel_->setVisible(hasBattle);
    continuationLabel_->setVisible(!continuationNodes.empty());
    continuationCombo_->setVisible(!continuationNodes.empty());
    continuationCombo_->clear();
    if (!continuationNodes.empty()) {
        const auto node = std::find_if(graph->nodes.begin(), graph->nodes.end(),
            [&](const auto& candidate) {
                return candidate.node_key == continuationNodes.front().toStdString();
            });
        if (node != graph->nodes.end()) {
            const auto argument = std::find_if(node->arguments.begin(), node->arguments.end(),
                [](const auto& candidate) {
                    return candidate.argument_key == "continuation_mode";
                });
            if (argument != node->arguments.end()) {
                continuationCombo_->addItem(QStringLiteral("Select continuation…"), QString());
                for (const auto& choice : argument->choices) {
                    continuationCombo_->addItem(
                        QString::fromStdString(choice.display_name),
                        QString::fromStdString(choice.value));
                }
            }
        }
    }
    graphDetailLabel_->setText(QStringLiteral("%1 nodes, %2 edges, revision %3")
        .arg(static_cast<int>(graph->nodes.size()))
        .arg(static_cast<int>(graph->edges.size()))
        .arg(static_cast<qint64>(graph->workflow_graph_revision_id)));
    const bool hasUnsatisfiedInputs = std::any_of(
        externalInputs_.begin(), externalInputs_.end(),
        [](const auto& input) { return !input.satisfied_by_edge; });
    launchStatusLabel_->setText(!hasUnsatisfiedInputs
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
    const auto* entry = selectedStandaloneEntry();
    if (entry != nullptr &&
        std::none_of(entry->members.begin(), entry->members.end(), [this](const auto& member) {
            return QString::fromStdString(member.unit_kind) == activeStandaloneMemberUnitKind_;
        })) {
        activeStandaloneMemberUnitKind_ = entry->members.empty()
            ? QString()
            : QString::fromStdString(entry->members.front().unit_kind);
    }
    const auto* unit = selectedUnit();
    const QString targetKey = currentDraftKey();
    const QString targetShape = entry == nullptr ? QString() : standaloneEntryShapeSignature(*entry);
    if (!forceRebuild && targetKey == renderedTargetKey_ && targetShape == renderedTargetShape_) {
        return;
    }
    renderedTargetKey_ = targetKey;
    renderedTargetShape_ = targetShape;

    if (entry == nullptr || unit == nullptr) {
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
        battleFakeRangeLabel_->hide();
        battleFakeRangePanel_->hide();
        continuationLabel_->hide();
        continuationCombo_->hide();
        return;
    }

    refreshAuthoredRefsForStandaloneUnit(*unit);
    populateExternalInputsForStandaloneEntry(*entry);
    updateStandaloneArgumentControls();
    graphDetailLabel_->setText(QStringLiteral("%1\n%2 inputs, %3 possible outputs")
        .arg(QString::fromStdString(entry->description))
        .arg(static_cast<int>(unit->required_inputs.size()))
        .arg(static_cast<int>(unit->possible_outputs.size())));
    const bool hasTasMovie = std::any_of(unit->launch_arguments.begin(), unit->launch_arguments.end(), [](const auto& argument) { return argument.key == "rtc"; });
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
    draft.rtcLow = rtcLowEdit_ == nullptr ? QString() : rtcLowEdit_->text();
    draft.rtcHigh = rtcHighEdit_ == nullptr ? QString() : rtcHighEdit_->text();
    draft.seedSamplesPerAxis = seedSamplesSpin_ == nullptr ? 5 : seedSamplesSpin_->value();
    draft.battleFakeMin = battleFakeMinSpin_ == nullptr ? 0 : battleFakeMinSpin_->value();
    draft.battleFakeMax = battleFakeMaxSpin_ == nullptr ? 0 : battleFakeMaxSpin_->value();
    draft.continuationMode = continuationCombo_ == nullptr
        ? QString() : continuationCombo_->currentData().toString();
    draft.authoredRefId = authoredRefCombo_ == nullptr ? 0 : authoredRefCombo_->currentData().toLongLong();

    for (int row = 0; row < static_cast<int>(externalInputs_.size()) && row < externalInputsTable_->rowCount(); ++row) {
        const auto& input = externalInputs_[static_cast<std::size_t>(row)];
        if (input.satisfied_by_edge) continue;
        const auto* combo = qobject_cast<QComboBox*>(externalInputsTable_->cellWidget(row,4));
        draft.externalInputs[externalInputKey(input)] = ExternalInputDraft{
            input.ref_kind,
            combo == nullptr ? QString() : QString::number(combo->currentData().toLongLong()),
            combo == nullptr ? QString() : combo->currentData(kMemberUnitKindRole).toString(),
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
    if (rtcLowEdit_ != nullptr) {
        rtcLowEdit_->setText(draft.rtcLow);
    }
    if (rtcHighEdit_ != nullptr) {
        rtcHighEdit_->setText(draft.rtcHigh);
    }
    if (seedSamplesSpin_ != nullptr) {
        seedSamplesSpin_->setValue(draft.seedSamplesPerAxis);
    }
    if (battleFakeMinSpin_ != nullptr) {
        battleFakeMinSpin_->setValue(draft.battleFakeMin);
    }
    if (battleFakeMaxSpin_ != nullptr) {
        battleFakeMaxSpin_->setValue(draft.battleFakeMax);
    }
    if (continuationCombo_ != nullptr && !draft.continuationMode.isEmpty()) {
        const int continuationIndex = continuationCombo_->findData(draft.continuationMode);
        if (continuationIndex >= 0) continuationCombo_->setCurrentIndex(continuationIndex);
    }
    if (authoredRefCombo_ != nullptr && draft.authoredRefId > 0) {
        const int authoredIndex = authoredRefCombo_->findData(draft.authoredRefId);
        if (authoredIndex >= 0) {
            authoredRefCombo_->setCurrentIndex(authoredIndex);
        }
    }

    for (int row = 0; row < static_cast<int>(externalInputs_.size()) && row < externalInputsTable_->rowCount(); ++row) {
        const auto& input = externalInputs_[static_cast<std::size_t>(row)];
        if (input.satisfied_by_edge) continue;
        const auto inputDraft = draft.externalInputs.find(externalInputKey(input));
        if (inputDraft == draft.externalInputs.end()) {
            continue;
        }
        if (auto* combo=qobject_cast<QComboBox*>(externalInputsTable_->cellWidget(row,4))) {
            bool ok=false;
            const auto id=inputDraft->second.refId.toLongLong(&ok);
            int index=-1;
            if (ok) {
                for (int candidate=1; candidate<combo->count(); ++candidate) {
                    if (combo->itemData(candidate).toLongLong()!=id) continue;
                    if (!inputDraft->second.memberUnitKind.isEmpty() &&
                        combo->itemData(candidate,kMemberUnitKindRole).toString()!=inputDraft->second.memberUnitKind) continue;
                    index=candidate;
                    break;
                }
            }
            if(index>=0)combo->setCurrentIndex(index);
            else if(ok&&id>0)combo->setEditText(inputDraft->second.refId);
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

    const auto tasNodes = argumentNodeKeys(*graph,"rtc");
    const auto seedNodes = argumentNodeKeys(*graph,"samples_per_axis");
    const auto battleNodes = argumentNodeKeys(*graph,"fake_attack_min");
    const auto continuationNodes = argumentNodeKeys(*graph,"continuation_mode");
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
    const bool useBattleFakeOverride = !battleNodes.empty();
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

    const QString continuationMode = continuationCombo_ == nullptr
        ? QString() : continuationCombo_->currentData().toString();
    if (!continuationNodes.empty() && continuationMode.isEmpty()) {
        postStatusMessage(QStringLiteral("Select how Battle waves should continue."), StatusToast::Severity::Warn);
        return;
    }

    std::vector<savorqt::db::WorkflowGraphInputBindingDraft> inputBindings;
    for (int row = 0; row < externalInputsTable_->rowCount(); ++row) {
        const auto& input = externalInputs_[static_cast<std::size_t>(row)];
        if (input.satisfied_by_edge) continue;
        const auto* combo=qobject_cast<QComboBox*>(externalInputsTable_->cellWidget(row,4));
        const auto refIdText=combo==nullptr?QString():combo->currentData().toString();
        if ((refIdText.isEmpty() || refIdText==QStringLiteral("0")) && !input.required) continue;
        if (input.ref_kind.isEmpty() || refIdText.isEmpty() || refIdText==QStringLiteral("0")) {
            postStatusMessage(QStringLiteral("Every external input requires a typed reference selection."), StatusToast::Severity::Warn);
            return;
        }

        bool ok = false;
        const auto refId = refIdText.toLongLong(&ok, 0);
        if (!ok || refId <= 0) {
            postStatusMessage(QStringLiteral("External input ref id must be a positive number."), StatusToast::Severity::Warn);
            return;
        }

        inputBindings.push_back(savorqt::db::WorkflowGraphInputBindingDraft{
            .node_key = input.node_key.toStdString(),
            .input_key = input.input_key.toStdString(),
            .data_kind = input.data_kind.toStdString(),
            .ref_kind = input.ref_kind.toStdString(),
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
        for (const auto& nodeKey : continuationNodes) {
            request.arguments.push_back(savorqt::db::WorkflowGraphArgumentDraft{
                .node_key = nodeKey.toStdString(),
                .argument_key = "continuation_mode",
                .value_type = "choice",
                .text_value = continuationMode.toStdString(),
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
    const auto hasArgument=[unit](std::string_view key){return std::any_of(unit->launch_arguments.begin(),unit->launch_arguments.end(),[key](const auto& value){return value.key==key;});};
    if (hasArgument("rtc")) {
        bool lowOk = false;
        bool highOk = false;
        rtcLow = rtcLowEdit_->text().trimmed().toLongLong(&lowOk, 0);
        rtcHigh = rtcHighEdit_->text().trimmed().toLongLong(&highOk, 0);
        if (!lowOk || !highOk) {
            postStatusMessage(QStringLiteral("RTC low and high must be numeric."), StatusToast::Severity::Warn);
            return;
        }
        if (rtcLow < 0 || rtcHigh < rtcLow) {
            postStatusMessage(QStringLiteral("RTC high must be greater than or equal to RTC low."), StatusToast::Severity::Warn);
            return;
        }
    }

    const bool useBattleFakeOverride = hasArgument("fake_attack_min");
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

    const QString continuationMode = continuationCombo_ == nullptr
        ? QString() : continuationCombo_->currentData().toString();
    if (hasArgument("continuation_mode") && continuationMode.isEmpty()) {
        postStatusMessage(QStringLiteral("Select how Battle waves should continue."), StatusToast::Severity::Warn);
        return;
    }

    std::vector<savorqt::db::WorkflowGraphInputBindingDraft> inputBindings;
    for (int row = 0; row < externalInputsTable_->rowCount(); ++row) {
        const auto& input=externalInputs_[static_cast<std::size_t>(row)];
        if (input.satisfied_by_edge) continue;
        const auto* combo=qobject_cast<QComboBox*>(externalInputsTable_->cellWidget(row,4));
        const auto refIdText=combo==nullptr?QString():combo->currentData().toString();
        if ((refIdText.isEmpty() || refIdText==QStringLiteral("0")) && !input.required) continue;
        if (input.ref_kind.isEmpty() || refIdText.isEmpty() || refIdText==QStringLiteral("0")) {
            postStatusMessage(QStringLiteral("Every external input requires a typed reference selection."), StatusToast::Severity::Warn);
            return;
        }

        bool ok = false;
        const auto refId = refIdText.toLongLong(&ok, 0);
        if (!ok || refId <= 0) {
            postStatusMessage(QStringLiteral("External input ref id must be a positive number."), StatusToast::Severity::Warn);
            return;
        }

        inputBindings.push_back(savorqt::db::WorkflowGraphInputBindingDraft{
            .node_key = input.node_key.toStdString(),
            .input_key = input.input_key.toStdString(),
            .data_kind = input.data_kind.toStdString(),
            .ref_kind = input.ref_kind.toStdString(),
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
    const std::int64_t launchLow = hasArgument("rtc") ? rtcLow : 0;
    const std::int64_t launchHigh = hasArgument("rtc") ? rtcHigh : 0;
    const auto nodeKey = standaloneNodeKey(*unit);
    for (std::int64_t rtc = launchLow; rtc <= launchHigh; ++rtc) {
        savorqt::db::WorkflowGraphStartRequest request{};
        request.workflow_graph_revision_id = graphResult.value.workflow_graph_revision_id;
        request.input_bindings = inputBindings;
        if (hasArgument("rtc")) {
            request.arguments.push_back(savorqt::db::WorkflowGraphArgumentDraft{
                .node_key = nodeKey,
                .argument_key = "rtc",
                .value_type = "integer",
                .integer_value = rtc,
                .source_kind = "launcher",
            });
        }
        if (hasArgument("samples_per_axis")) {
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
        if (hasArgument("continuation_mode")) {
            request.arguments.push_back(savorqt::db::WorkflowGraphArgumentDraft{
                .node_key = nodeKey,
                .argument_key = "continuation_mode",
                .value_type = "choice",
                .text_value = continuationMode.toStdString(),
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
    std::map<QString, QString> suppliedByEdge;
    for (const auto& edge : graph.edges) {
        suppliedByEdge.emplace(
            QString::fromStdString(edge.to_node_key + "\n" + edge.input_key),
            QString::fromStdString(edge.from_node_key + "." + edge.output_key));
    }

    std::vector<ExternalInputRow> rows;
    for (const auto& node : graph.nodes) {
        for (const auto& input : node.inputs) {
            const auto edgeKey = QString::fromStdString(node.node_key + "\n" + input.input_key);
            const auto edge = suppliedByEdge.find(edgeKey);
            const auto dataKind = QString::fromStdString(input.data_kind);
            rows.push_back(ExternalInputRow{
                .node_key = QString::fromStdString(node.node_key),
                .node_name = nodeDisplayName(graph, node.node_key),
                .input_key = QString::fromStdString(input.input_key),
                .display_name = QString::fromStdString(input.display_name),
                .data_kind = dataKind,
                .ref_kind = QString::fromStdString(input.ref_kind),
                .required = input.required,
                .satisfied_by_edge = edge != suppliedByEdge.end(),
                .edge_evidence = edge == suppliedByEdge.end() ? QString() : edge->second,
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
        const auto dataKind = QString::fromStdString(input.data_kind);
        rows.push_back(ExternalInputRow{
            .node_key = nodeKey,
            .node_name = QString::fromStdString(unit.display_name),
            .input_key = QString::fromStdString(input.key),
            .display_name = QString::fromStdString(input.display_name),
            .data_kind = dataKind,
            .ref_kind = QString::fromStdString(input.ref_kind),
            .required = input.required,
        });
    }

    applyExternalInputs(std::move(rows));
}

void WorkflowLauncherPage::populateExternalInputsForStandaloneEntry(
    const StandaloneLaunchEntry& entry)
{
    if (entry.members.size() == 1u) {
        populateExternalInputsForUnit(entry.members.front());
        return;
    }

    applyExternalInputs({ExternalInputRow{
        .node_key = QStringLiteral("family:%1").arg(
            QString::fromStdString(entry.presentation_key)),
        .node_name = QString::fromStdString(entry.display_name),
        .input_key = QStringLiteral("source"),
        .display_name = QStringLiteral("Root establishment or recorded TAS branch"),
        .data_kind = QStringLiteral("Selected source contract"),
        .ref_kind = QStringLiteral("Selected source contract"),
        .required = true,
        .presentation_family_key = QString::fromStdString(
            entry.presentation_key),
    }});
}

void WorkflowLauncherPage::handleStandaloneSourceSelectionChanged(int row)
{
    if (!standaloneModeActive() || row < 0 ||
        row >= static_cast<int>(externalInputs_.size()) ||
        externalInputs_[static_cast<std::size_t>(row)].presentation_family_key.isEmpty()) {
        return;
    }
    auto* combo = qobject_cast<QComboBox*>(
        externalInputsTable_->cellWidget(row, 4));
    if (combo == nullptr || combo->currentData().toLongLong() <= 0) return;
    const int index = combo->currentIndex();
    const QString member = combo->itemData(index, kMemberUnitKindRole).toString();
    const QString inputKey = combo->itemData(index, kInputKeyRole).toString();
    const QString dataKind = combo->itemData(index, kDataKindRole).toString();
    const QString refKind = combo->itemData(index, kRefKindRole).toString();
    if (member.isEmpty() || inputKey.isEmpty() || dataKind.isEmpty() || refKind.isEmpty()) return;

    activeStandaloneMemberUnitKind_ = member;
    auto& input = externalInputs_[static_cast<std::size_t>(row)];
    const auto* unit = selectedUnit();
    if (unit == nullptr) return;
    input.node_key = QString::fromStdString(standaloneNodeKey(*unit));
    input.node_name = QString::fromStdString(unit->display_name);
    input.input_key = inputKey;
    input.data_kind = dataKind;
    input.ref_kind = refKind;
    externalInputsTable_->item(row, 0)->setText(input.node_name);
    externalInputsTable_->item(row, 1)->setText(
        QString::fromStdString(unit->required_inputs.front().display_name));
    externalInputsTable_->item(row, 2)->setText(dataKind);
    externalInputsTable_->item(row, 3)->setText(refKind);
    refreshAuthoredRefsForStandaloneUnit(*unit);
    updateStandaloneArgumentControls();
}

void WorkflowLauncherPage::updateStandaloneArgumentControls()
{
    const auto* unit = selectedUnit();
    const auto hasArgument=[unit](std::string_view key){
        return unit != nullptr && std::any_of(
            unit->launch_arguments.begin(), unit->launch_arguments.end(),
            [key](const auto& value){return value.key==key;});
    };
    const bool hasTasMovie = hasArgument("rtc");
    const bool hasSeedProbe = hasArgument("samples_per_axis");
    const bool hasBattle = hasArgument("fake_attack_min");
    const bool hasContinuation = hasArgument("continuation_mode");
    rtcRangeLabel_->setVisible(hasTasMovie);
    rtcRangePanel_->setVisible(hasTasMovie);
    seedSamplesLabel_->setVisible(hasSeedProbe);
    seedSamplesSpin_->setVisible(hasSeedProbe);
    battleFakeRangeLabel_->setVisible(hasBattle);
    battleFakeRangePanel_->setVisible(hasBattle);
    continuationLabel_->setVisible(hasContinuation);
    continuationCombo_->setVisible(hasContinuation);
    continuationCombo_->clear();
    if (hasContinuation) {
        continuationCombo_->addItem(QStringLiteral("Select continuation…"), QString());
        const auto definition = std::find_if(
            unit->launch_arguments.begin(), unit->launch_arguments.end(),
            [](const auto& argument) {
                return argument.key == "continuation_mode";
            });
        if (definition != unit->launch_arguments.end()) {
            for (const auto& choice : definition->choices) {
                continuationCombo_->addItem(
                    QString::fromStdString(choice.display_name),
                    QString::fromStdString(choice.value));
            }
        }
    }
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
                && lhs.ref_kind == rhs.ref_kind
                && lhs.required == rhs.required
                && lhs.satisfied_by_edge == rhs.satisfied_by_edge
                && lhs.edge_evidence == rhs.edge_evidence
                && lhs.presentation_family_key == rhs.presentation_family_key;
        },
        [this](QTableWidget* table, int row, const ExternalInputRow& input) {
            table->setItem(row, 0, new QTableWidgetItem(input.node_name));
            table->setItem(row, 1, new QTableWidgetItem(input.display_name.isEmpty() ? input.input_key : input.display_name));
            table->setItem(row, 2, new QTableWidgetItem(input.data_kind));
            table->setItem(row, 3, new QTableWidgetItem(input.ref_kind));
            if (input.satisfied_by_edge) {
                table->setItem(
                    row,
                    4,
                    new QTableWidgetItem(
                        QStringLiteral("Upstream: %1").arg(input.edge_evidence)));
                table->item(row, 4)->setFlags(
                    table->item(row, 4)->flags() & ~Qt::ItemIsEditable);
            } else {
                auto* selector = new QComboBox(table);
                selector->setEditable(true);
                selector->setInsertPolicy(QComboBox::NoInsert);
                selector->setPlaceholderText(QStringLiteral("Search by ID or label"));
                selector->completer()->setCompletionMode(QCompleter::PopupCompletion);
                selector->completer()->setFilterMode(Qt::MatchContains);
                selector->completer()->setCaseSensitivity(Qt::CaseInsensitive);
                table->setCellWidget(row, 4, selector);
                if (!input.presentation_family_key.isEmpty()) {
                    connect(selector, qOverload<int>(&QComboBox::currentIndexChanged),
                        this, [this, row](int) {
                            handleStandaloneSourceSelectionChanged(row);
                        });
                }
            }
            for (int column = 0; column < 4; ++column) {
                table->item(row, column)->setFlags(table->item(row, column)->flags() & ~Qt::ItemIsEditable);
            }
        });
    refreshExternalInputOptions();
}

void WorkflowLauncherPage::preselectStandaloneInput(const QString& unit_kind,const QString& input_key,qint64 ref_id)
{
    pendingUnitKind_=unit_kind;
    const auto registry =
        savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
    const auto* unit = registry.Find(unit_kind.toStdString());
    const QString presentationKey = unit != nullptr &&
        !unit->standalone_presentation_family_key.empty()
        ? QString::fromStdString(unit->standalone_presentation_family_key)
        : unit_kind;
    LauncherDraft& draft=launchDrafts_[QStringLiteral("unit:%1").arg(presentationKey)];
    const bool family = unit != nullptr &&
        !unit->standalone_presentation_family_key.empty();
    const QString inputDraftKey = family
        ? QStringLiteral("family:%1").arg(presentationKey) + QChar(0x1f) + QStringLiteral("source")
        : unit_kind+QStringLiteral("_standalone")+QChar(0x1f)+input_key;
    draft.externalInputs[inputDraftKey]=ExternalInputDraft{
        unit != nullptr && !unit->required_inputs.empty()
            ? QString::fromStdString(unit->required_inputs.front().ref_kind)
            : QString(),
        QString::number(ref_id),
        unit_kind};
    if(launchModeTabs_!=nullptr)launchModeTabs_->setCurrentIndex(1);
    refreshStandaloneUnits();
}

void WorkflowLauncherPage::refreshExternalInputOptions(){if(referenceRefreshPipeline_!=nullptr)referenceRefreshPipeline_->requestRefresh(savorqt::gui::RefreshReason::Manual);}

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
    } else if (requirement.ref_kind == "authoring.battle_plan") {
        const auto result = savorqt::db::SavorDbAuthoringService::ListBattlePlans();
        if (!result.ok) {
            postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
            return;
        }
        for (const auto& plan : result.value) {
            const auto actionCount = std::accumulate(
                plan.turns.begin(), plan.turns.end(), std::size_t{0},
                [](std::size_t total, const auto& turn) {
                    return total + turn.actions.size();
                });
            authoredRefOptions_.push_back(AuthoredRefOption{
                .label = QStringLiteral("%1 — %2 turns, %3 actions — %4")
                    .arg(QString::fromStdString(plan.name))
                    .arg(static_cast<int>(plan.turns.size()))
                    .arg(static_cast<qulonglong>(actionCount))
                    .arg(QString::fromStdString(plan.fingerprint)),
                .ref_kind = "authoring.battle_plan",
                .ref_id = plan.plan_id,
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
    const auto* entry = selectedStandaloneEntry();
    if (entry == nullptr) return nullptr;
    const auto found = std::find_if(entry->members.begin(), entry->members.end(), [this](const auto& member) {
        return QString::fromStdString(member.unit_kind) == activeStandaloneMemberUnitKind_;
    });
    if (found != entry->members.end()) return &*found;
    return entry->members.empty() ? nullptr : &entry->members.front();
}

const WorkflowLauncherPage::StandaloneLaunchEntry*
WorkflowLauncherPage::selectedStandaloneEntry() const
{
    const int row = unitList_ != nullptr ? unitList_->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(standaloneLaunchEntries_.size())) {
        return nullptr;
    }
    return &standaloneLaunchEntries_[static_cast<std::size_t>(row)];
}

bool WorkflowLauncherPage::standaloneModeActive() const
{
    return launchModeTabs_ != nullptr && launchModeTabs_->currentIndex() == 1;
}

QString WorkflowLauncherPage::currentDraftKey() const
{
    if (standaloneModeActive()) {
        const auto* entry = selectedStandaloneEntry();
        return entry == nullptr
            ? QStringLiteral("unit:")
            : QStringLiteral("unit:%1").arg(QString::fromStdString(entry->presentation_key));
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
    if (!input.presentation_family_key.isEmpty()) {
        return QStringLiteral("family:%1").arg(input.presentation_family_key) +
            QChar(0x1f) + QStringLiteral("source");
    }
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
        for (const auto& argument : node.arguments) {
            parts << QStringLiteral("argument:%1:%2")
                .arg(QString::fromStdString(argument.argument_key),
                     QString::fromStdString(argument.value_type));
            for (const auto& choice : argument.choices) {
                parts << QStringLiteral("choice:%1:%2")
                    .arg(QString::fromStdString(choice.value),
                         QString::fromStdString(choice.display_name));
            }
        }
        for (const auto& input : node.inputs) {
            const auto edgeKey = QString::fromStdString(node.node_key + "\n" + input.input_key);
            parts << QStringLiteral("input:%1:%2:%3:%4:%5:%6")
                .arg(
                    QString::fromStdString(node.node_key),
                    QString::fromStdString(input.input_key),
                    QString::fromStdString(input.data_kind),
                    QString::fromStdString(input.ref_kind),
                    input.required ? QStringLiteral("required") : QStringLiteral("optional"),
                    suppliedByEdge.contains(edgeKey) ? QStringLiteral("edge") : QStringLiteral("external"));
        }
    }
    return parts.join(QChar(0x1d));
}

QString WorkflowLauncherPage::unitLaunchShapeSignature(const WorkflowUnitDefinition& unit)
{
    QStringList parts;
    parts << QStringLiteral("unit:%1").arg(QString::fromStdString(unit.unit_kind));
    for (const auto& argument : unit.launch_arguments) {
        parts << QStringLiteral("argument:%1:%2")
            .arg(QString::fromStdString(argument.key))
            .arg(static_cast<int>(argument.value_type));
        for (const auto& choice : argument.choices) {
            parts << QStringLiteral("choice:%1:%2")
                .arg(QString::fromStdString(choice.value),
                     QString::fromStdString(choice.display_name));
        }
    }
    for (const auto& authoredRef : unit.authored_refs) {
        parts << QStringLiteral("authored:%1:%2")
            .arg(QString::fromStdString(authoredRef.ref_kind))
            .arg(authoredRef.required ? QStringLiteral("required") : QStringLiteral("optional"));
    }
    for (const auto& input : unit.required_inputs) {
        parts << QStringLiteral("input:%1:%2:%3:%4")
            .arg(
                QString::fromStdString(input.key),
                QString::fromStdString(input.data_kind),
                QString::fromStdString(input.ref_kind),
                input.required ? QStringLiteral("required") : QStringLiteral("optional"));
    }
    return parts.join(QChar(0x1d));
}

QString WorkflowLauncherPage::standaloneEntryShapeSignature(
    const StandaloneLaunchEntry& entry)
{
    QStringList parts;
    parts << QStringLiteral("entry:%1").arg(
        QString::fromStdString(entry.presentation_key));
    for (const auto& member : entry.members) {
        parts << unitLaunchShapeSignature(member);
    }
    return parts.join(QChar(0x1c));
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

std::vector<QString> WorkflowLauncherPage::argumentNodeKeys(const savor::db::WorkflowGraphSnapshot& graph,std::string_view argument_key)
{
    std::vector<QString> keys;
    for (const auto& node : graph.nodes) {
        if(std::any_of(node.arguments.begin(),node.arguments.end(),[argument_key](const auto& argument){return argument.argument_key==argument_key;})) {
            keys.push_back(QString::fromStdString(node.node_key));
        }
    }
    return keys;
}
