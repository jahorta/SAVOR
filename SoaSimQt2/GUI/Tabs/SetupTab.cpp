#include "GUI/Tabs/SetupTab.h"

#include "DB/SimCoreDbArtifactService.h"
#include "DB/SimCoreDbAuthoringService.h"
#include "DB/SimCoreDbWorkflowService.h"
#include "GUI/Panes/CoordinatorPane/CoordinatorController.h"
#include "SimCoreDbRuntime.h"

#include <QtCore/QSize>
#include <QtCore/QSignalBlocker>
#include <QtGui/QIcon>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

namespace {

constexpr int kWorkflowGraphIndexRole = Qt::UserRole + 1;

struct LaunchInputEditor {
    QString nodeKey;
    QString inputKey;
    QString dataKind;
    QString sourceKind;
    QCheckBox* overrideCheck = nullptr;
    QLineEdit* refKindEdit = nullptr;
    QLineEdit* refIdEdit = nullptr;
};

struct LaunchArgumentEditor {
    QString nodeKey;
    QString argumentKey;
    QLineEdit* edit = nullptr;
};

struct TasRtcEditor {
    QString nodeKey;
    QCheckBox* rangeCheck = nullptr;
    QLineEdit* singleEdit = nullptr;
    QLineEdit* minEdit = nullptr;
    QLineEdit* maxEdit = nullptr;
};

struct LaunchEditors {
    std::vector<LaunchInputEditor> inputs;
    std::vector<LaunchArgumentEditor> arguments;
    std::vector<TasRtcEditor> tasRtc;
};

QString qs(const std::string& value)
{
    return QString::fromStdString(value);
}

QString defaultRefKindForDataKind(const QString& dataKind)
{
    if (dataKind.contains(QStringLiteral("artifact"), Qt::CaseInsensitive)
        || dataKind.contains(QStringLiteral("dtm"), Qt::CaseInsensitive)
        || dataKind.contains(QStringLiteral("movie"), Qt::CaseInsensitive)) {
        return QStringLiteral("artifact");
    }
    if (dataKind.contains(QStringLiteral("savestate"), Qt::CaseInsensitive)) {
        return QStringLiteral("state.savestate");
    }
    if (dataKind == QStringLiteral("analysis.input_frame_set_id")) {
        return QStringLiteral("au.input_set");
    }
    if (dataKind.contains(QStringLiteral("seed"), Qt::CaseInsensitive)) {
        return QStringLiteral("sp_probe_run");
    }
    return dataKind.isEmpty() ? QStringLiteral("artifact") : dataKind;
}

QString nodeDisplayName(const simcore::db::WorkflowGraphNodeSnapshot& node)
{
    return node.display_name.empty() ? qs(node.unit_kind) : qs(node.display_name);
}

QString inputDisplayName(const simcore::db::WorkflowGraphNodeInputSnapshot& input)
{
    return input.display_name.empty() ? qs(input.input_key) : qs(input.display_name);
}

QString outputDisplayName(const simcore::db::WorkflowGraphNodeOutputSnapshot& output)
{
    return output.display_name.empty() ? qs(output.output_key) : qs(output.display_name);
}

const simcore::db::WorkflowGraphEdgeSnapshot* findIncomingEdge(
    const simcore::db::WorkflowGraphSnapshot& graph,
    const std::string& nodeKey,
    const std::string& inputKey)
{
    const auto it = std::find_if(graph.edges.begin(), graph.edges.end(), [&](const auto& edge) {
        return edge.to_node_key == nodeKey && edge.input_key == inputKey;
    });
    return it == graph.edges.end() ? nullptr : &*it;
}

const simcore::db::WorkflowGraphNodeSnapshot* findGraphNode(
    const simcore::db::WorkflowGraphSnapshot& graph,
    const std::string& nodeKey)
{
    const auto it = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const auto& node) {
        return node.node_key == nodeKey;
    });
    return it == graph.nodes.end() ? nullptr : &*it;
}

const simcore::db::WorkflowGraphNodeOutputSnapshot* findGraphOutput(
    const simcore::db::WorkflowGraphNodeSnapshot& node,
    const std::string& outputKey)
{
    const auto it = std::find_if(node.possible_outputs.begin(), node.possible_outputs.end(), [&](const auto& output) {
        return output.output_key == outputKey;
    });
    return it == node.possible_outputs.end() ? nullptr : &*it;
}

bool isTasMovieUnit(const QString& unitKind)
{
    return unitKind == QStringLiteral("tas_movie");
}

bool isSeedProbeUnit(const QString& unitKind)
{
    return unitKind.contains(QStringLiteral("seed_probe"), Qt::CaseInsensitive);
}

bool isBattleChainUnit(const QString& unitKind)
{
    return unitKind == QStringLiteral("battle_chain");
}

QFrame* createStatusPill(const QString& label, const QString& value, QWidget* parent)
{
    auto* pill = new QFrame(parent);
    pill->setObjectName("workspaceMetricTile");
    auto* layout = new QHBoxLayout(pill);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(6);

    auto* labelText = new QLabel(label + QStringLiteral(":"), pill);
    labelText->setObjectName("sectionDescription");
    auto* valueText = new QLabel(value, pill);
    valueText->setObjectName("panelTitle");

    layout->addWidget(labelText);
    layout->addWidget(valueText);
    return pill;
}

QFrame* createSectionPanel(const QString& title, QWidget* parent)
{
    auto* panel = new QFrame(parent);
    panel->setObjectName("workspaceHeroPanel");
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    auto* titleLabel = new QLabel(title, panel);
    titleLabel->setObjectName("panelTitle");
    layout->addWidget(titleLabel);
    return panel;
}

QTableWidgetItem* createTableItem(const QString& text, int graphIndex)
{
    auto* item = new QTableWidgetItem(text);
    item->setData(kWorkflowGraphIndexRole, graphIndex);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QString describeSpecRef(const std::optional<std::string>& refKind, const std::optional<std::int64_t>& refId)
{
    if (!refKind.has_value() || !refId.has_value() || *refId <= 0) {
        return QStringLiteral("No authored spec reference.");
    }

    const QString kind = qs(*refKind);
    const auto id = *refId;
    if (*refKind == "tas_spec") {
        const auto spec = soasimqt2::db::SimCoreDbAuthoringService::GetTasSpec(id);
        if (!spec.ok) {
            return QStringLiteral("TAS spec #%1 unavailable: %2").arg(id).arg(qs(spec.error.message));
        }
        return QStringLiteral("TAS spec: %1 (#%2)\n  rtc: %3-%4\n  headroom x10: %5\n  base DTM artifact: %6")
            .arg(qs(spec.value.base_name))
            .arg(id)
            .arg(spec.value.rtc_low)
            .arg(spec.value.rtc_high)
            .arg(spec.value.headroom_x10)
            .arg(spec.value.base_dtm_artifact_id);
    }
    if (*refKind == "seed_probe_spec") {
        const auto spec = soasimqt2::db::SimCoreDbAuthoringService::GetSeedProbeSpec(id);
        if (!spec.ok) {
            return QStringLiteral("Seed probe spec #%1 unavailable: %2").arg(id).arg(qs(spec.error.message));
        }
        return QStringLiteral("Seed probe spec: %1 (#%2)\n  samples/axis: %3\n  combo attempts: %4\n  value range: %5 to %6")
            .arg(qs(spec.value.name))
            .arg(id)
            .arg(spec.value.samples_per_axis)
            .arg(spec.value.combo_attempts_per_target)
            .arg(spec.value.min_value)
            .arg(spec.value.max_value);
    }
    if (*refKind == "battle_run_spec") {
        const auto spec = soasimqt2::db::SimCoreDbAuthoringService::GetBattleRunSpec(id);
        if (!spec.ok) {
            return QStringLiteral("Battle run spec #%1 unavailable: %2").arg(id).arg(qs(spec.error.message));
        }
        return QStringLiteral("Battle run spec: %1 (#%2)\n  fake attacks: %3-%4\n  run ms: %5\n  single-turn runner: %6")
            .arg(qs(spec.value.name))
            .arg(id)
            .arg(spec.value.min_fake_attacks)
            .arg(spec.value.max_fake_attacks)
            .arg(spec.value.run_ms)
            .arg(spec.value.use_single_turn_runner ? QStringLiteral("yes") : QStringLiteral("no"));
    }
    if (*refKind == "authoring.battle_chain_spec" || *refKind == "battle_chain_spec") {
        const auto spec = soasimqt2::db::SimCoreDbAuthoringService::GetBattleChainSpec(id);
        if (!spec.ok) {
            return QStringLiteral("Battle chain spec #%1 unavailable: %2").arg(id).arg(qs(spec.error.message));
        }
        QString text = QStringLiteral("Battle chain spec: %1 (#%2)\n  battle run spec: #%3\n  explorer settings: #%4")
            .arg(qs(spec.value.name))
            .arg(id)
            .arg(spec.value.battle_run_spec_id)
            .arg(spec.value.explorer_settings_id);
        const auto battleRun = soasimqt2::db::SimCoreDbAuthoringService::GetBattleRunSpec(spec.value.battle_run_spec_id);
        if (battleRun.ok) {
            text += QStringLiteral("\n  battle run: %1, fake attacks %2-%3")
                .arg(qs(battleRun.value.name))
                .arg(battleRun.value.min_fake_attacks)
                .arg(battleRun.value.max_fake_attacks);
        }
        const auto explorer = soasimqt2::db::SimCoreDbAuthoringService::GetExplorerSettings(spec.value.explorer_settings_id);
        if (explorer.ok) {
            text += QStringLiteral("\n  explorer: %1").arg(qs(explorer.value.name));
        }
        return text;
    }
    return QStringLiteral("%1 #%2").arg(kind).arg(id);
}

QString workflowDetailText(const simcore::db::WorkflowGraphSnapshot& graph)
{
    QStringList lines;
    lines << QStringLiteral("Name: %1").arg(qs(graph.name));
    lines << QStringLiteral("Kind: Graph");
    lines << QStringLiteral("Status: %1").arg(graph.status.empty() ? QStringLiteral("ready") : qs(graph.status));
    lines << QStringLiteral("Revision: #%1").arg(graph.workflow_graph_revision_id);
    lines << QString();
    lines << QStringLiteral("Activation chain:");
    if (graph.nodes.empty()) {
        lines << QStringLiteral("  (no nodes)");
    } else {
        for (const auto& node : graph.nodes) {
            lines << QStringLiteral("  %1: %2")
                .arg(qs(node.node_key))
                .arg(node.display_name.empty() ? qs(node.unit_kind) : qs(node.display_name));
        }
    }
    lines << QString();
    lines << QStringLiteral("Referenced specs:");
    bool anySpec = false;
    for (const auto& node : graph.nodes) {
        if (node.authored_ref_kind.has_value() && node.authored_ref_id.has_value()) {
            anySpec = true;
            lines << QStringLiteral("  Node %1").arg(qs(node.node_key));
            const QString specText = describeSpecRef(node.authored_ref_kind, node.authored_ref_id);
            for (const auto& specLine : specText.split(QLatin1Char('\n'))) {
                lines << QStringLiteral("    %1").arg(specLine);
            }
        }
    }
    if (!anySpec) {
        lines << QStringLiteral("  (none)");
    }
    lines << QString();
    lines << QStringLiteral("Required external inputs:");
    bool anyInput = false;
    for (const auto& node : graph.nodes) {
        for (const auto& input : node.inputs) {
            if (!input.required) {
                continue;
            }
            anyInput = true;
            lines << QStringLiteral("  %1.%2 (%3)")
                .arg(qs(node.node_key))
                .arg(qs(input.input_key))
                .arg(input.display_name.empty() ? qs(input.data_kind) : qs(input.display_name));
        }
    }
    if (!anyInput) {
        lines << QStringLiteral("  (none)");
    }
    if (!graph.description.empty()) {
        lines << QString();
        lines << QStringLiteral("Description:");
        lines << QStringLiteral("  %1").arg(qs(graph.description));
    }
    return lines.join(QLatin1Char('\n'));
}

void clearLayout(QLayout* layout)
{
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        if (QLayout* childLayout = item->layout()) {
            clearLayout(childLayout);
            delete childLayout;
        }
        delete item;
    }
}

} // namespace

SetupTab::SetupTab(CoordinatorController* coordinatorController, Actions actions, QWidget* parent)
    : WorkspacePageShell(
        QStringLiteral("setup"),
        QStringLiteral("Setup"),
        QStringLiteral("Create workflow definitions and instantiate launch-ready workflow groups."),
        parent)
    , coordinatorController_(coordinatorController)
    , actions_(std::move(actions))
{
    build();
}

void SetupTab::build()
{
    const bool dbReady = soasimqt2::SimCoreDbRuntime::instance().isRunning();
    const bool artifactStorageReady = soasimqt2::db::SimCoreDbArtifactService::StorageReady();
    const auto graphs = soasimqt2::db::SimCoreDbAuthoringService::ListWorkflowGraphs(100, true);
    const auto units = soasimqt2::db::SimCoreDbWorkflowService::ListWorkflowUnits();

    auto* statusStrip = new QFrame(this);
    statusStrip->setObjectName("workspaceHeroPanel");
    auto* statusLayout = new QHBoxLayout(statusStrip);
    statusLayout->setContentsMargins(10, 10, 10, 10);
    statusLayout->setSpacing(8);
    statusLayout->addWidget(createStatusPill(QStringLiteral("Database"), dbReady ? QStringLiteral("Ready") : QStringLiteral("Down"), statusStrip));
    statusLayout->addWidget(createStatusPill(QStringLiteral("Storage"), artifactStorageReady ? QStringLiteral("Ready") : QStringLiteral("Needs setup"), statusStrip));
    statusLayout->addWidget(createStatusPill(QStringLiteral("Graphs"), graphs.ok ? QString::number(static_cast<int>(graphs.value.size())) : QStringLiteral("--"), statusStrip));
    statusLayout->addWidget(createStatusPill(QStringLiteral("Units"), units.ok ? QString::number(static_cast<int>(units.value.size())) : QStringLiteral("--"), statusStrip));
    statusLayout->addStretch();
    if (!artifactStorageReady) {
        auto* fixButton = new QPushButton(QStringLiteral("Fix storage"), statusStrip);
        fixButton->setObjectName("jobsPrimaryButton");
        QObject::connect(fixButton, &QPushButton::clicked, statusStrip, [this]() {
            if (actions_.openSettings) {
                actions_.openSettings();
            }
        });
        statusLayout->addWidget(fixButton);
    }
    canvasLayout()->addWidget(statusStrip);

    auto* workbench = new QFrame(this);
    workbench->setObjectName("workspaceCardGrid");
    auto* workbenchLayout = new QHBoxLayout(workbench);
    workbenchLayout->setContentsMargins(0, 0, 0, 0);
    workbenchLayout->setSpacing(10);

    auto workflowGraphs = std::make_shared<std::vector<simcore::db::WorkflowGraphSnapshot>>(
        graphs.ok ? graphs.value : std::vector<simcore::db::WorkflowGraphSnapshot>{});

    auto* createWorkflowPanel = createSectionPanel(QStringLiteral("Workflows"), workbench);
    auto* createLayout = qobject_cast<QVBoxLayout*>(createWorkflowPanel->layout());
    auto* createActions = new QHBoxLayout();
    createActions->setContentsMargins(0, 0, 0, 0);
    createActions->setSpacing(8);
    auto* createButton = new QToolButton(createWorkflowPanel);
    createButton->setObjectName("newEntityButton");
    createButton->setIcon(QIcon(QStringLiteral(":/MainWindow/Resources/Icons/new-document-plus.svg")));
    createButton->setIconSize(QSize(26, 26));
    createButton->setToolTip(QStringLiteral("Create workflow graph"));
    QObject::connect(createButton, &QToolButton::clicked, createWorkflowPanel, [this]() {
        if (actions_.openGraphEditor) {
            actions_.openGraphEditor();
        }
    });
    createActions->addWidget(createButton);
    auto* showHiddenWorkflowsCheck = new QCheckBox(QStringLiteral("Show hidden"), createWorkflowPanel);
    showHiddenWorkflowsCheck->setObjectName("jobSetsCheckBox");
    showHiddenWorkflowsCheck->setToolTip(QStringLiteral("Include hidden workflow graphs in this table and launcher."));
    createActions->addWidget(showHiddenWorkflowsCheck);
    createActions->addStretch();
    createLayout->addLayout(createActions);

    auto* workflowTable = new QTableWidget(createWorkflowPanel);
    workflowTable->setColumnCount(3);
    workflowTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("Workflow"),
        QStringLiteral("Nodes"),
        QStringLiteral("Rev."),
    });
    workflowTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    workflowTable->setSelectionMode(QAbstractItemView::SingleSelection);
    workflowTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    workflowTable->setAlternatingRowColors(true);
    workflowTable->setContextMenuPolicy(Qt::CustomContextMenu);
    workflowTable->verticalHeader()->hide();
    workflowTable->horizontalHeader()->setStretchLastSection(false);
    workflowTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    workflowTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    workflowTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    workflowTable->setColumnWidth(1, 56);
    workflowTable->setColumnWidth(2, 64);
    createLayout->addWidget(workflowTable, 1);

    auto* workflowSummaryLabel = new QLabel(createWorkflowPanel);
    workflowSummaryLabel->setObjectName("sectionDescription");
    workflowSummaryLabel->setWordWrap(true);
    createLayout->addWidget(workflowSummaryLabel);
    workbenchLayout->addWidget(createWorkflowPanel, 1);

    auto* launchPanel = createSectionPanel(QStringLiteral("Launch workflow"), workbench);
    auto* launchLayout = qobject_cast<QVBoxLayout*>(launchPanel->layout());
    auto* form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(8);

    auto* workflowCombo = new QComboBox(launchPanel);
    form->addRow(QStringLiteral("Workflow"), workflowCombo);
    launchLayout->addLayout(form);

    auto* launchGroupsHost = new QFrame(launchPanel);
    launchGroupsHost->setObjectName("workspaceCardGrid");
    auto* launchGroupsLayout = new QVBoxLayout(launchGroupsHost);
    launchGroupsLayout->setContentsMargins(0, 0, 0, 0);
    launchGroupsLayout->setSpacing(8);
    launchLayout->addWidget(launchGroupsHost);

    auto launchEditors = std::make_shared<LaunchEditors>();
    auto refreshLaunchSelection = [workflowGraphs, workflowCombo, launchGroupsLayout, launchGroupsHost, launchEditors]() {
        clearLayout(launchGroupsLayout);
        launchEditors->inputs.clear();
        launchEditors->arguments.clear();
        launchEditors->tasRtc.clear();
        if (workflowCombo->currentIndex() < 0 || workflowCombo->currentIndex() >= workflowCombo->count()) {
            auto* none = new QLabel(QStringLiteral("No workflow selected."), launchGroupsHost);
            none->setObjectName("sectionDescription");
            launchGroupsLayout->addWidget(none);
            return;
        }
        const int graphIndex = workflowCombo->currentData().toInt();
        if (graphIndex < 0 || graphIndex >= static_cast<int>(workflowGraphs->size())) {
            auto* invalid = new QLabel(QStringLiteral("Invalid workflow selection."), launchGroupsHost);
            invalid->setObjectName("sectionDescription");
            launchGroupsLayout->addWidget(invalid);
            return;
        }
        const auto& graph = (*workflowGraphs)[static_cast<std::size_t>(graphIndex)];
        for (const auto& node : graph.nodes) {
            auto* group = new QFrame(launchGroupsHost);
            group->setObjectName("workspaceToolCard");
            auto* groupLayout = new QVBoxLayout(group);
            groupLayout->setContentsMargins(10, 10, 10, 10);
            groupLayout->setSpacing(8);

            auto* heading = new QLabel(nodeDisplayName(node), group);
            heading->setObjectName("panelTitle");
            groupLayout->addWidget(heading);

            auto* groupForm = new QFormLayout();
            groupForm->setContentsMargins(0, 0, 0, 0);
            groupForm->setSpacing(6);
            groupLayout->addLayout(groupForm);

            const QString unitKind = qs(node.unit_kind);
            const QString nodeKey = qs(node.node_key);
            const auto addArgumentRow = [&](const QString& label, const QString& argumentKey) {
                auto* edit = new QLineEdit(group);
                groupForm->addRow(label, edit);
                launchEditors->arguments.push_back(LaunchArgumentEditor{ nodeKey, argumentKey, edit });
            };

            if (isTasMovieUnit(unitKind)) {
                auto* rtcRow = new QFrame(group);
                auto* rtcLayout = new QHBoxLayout(rtcRow);
                rtcLayout->setContentsMargins(0, 0, 0, 0);
                rtcLayout->setSpacing(6);

                auto* rangeCheck = new QCheckBox(QStringLiteral("Range"), rtcRow);
                auto* singleEdit = new QLineEdit(rtcRow);
                singleEdit->setPlaceholderText(QStringLiteral("rtc"));
                auto* minEdit = new QLineEdit(rtcRow);
                minEdit->setPlaceholderText(QStringLiteral("min"));
                auto* maxEdit = new QLineEdit(rtcRow);
                maxEdit->setPlaceholderText(QStringLiteral("max"));
                minEdit->setVisible(false);
                maxEdit->setVisible(false);

                QObject::connect(rangeCheck, &QCheckBox::toggled, rtcRow, [singleEdit, minEdit, maxEdit](bool checked) {
                    singleEdit->setVisible(!checked);
                    minEdit->setVisible(checked);
                    maxEdit->setVisible(checked);
                });

                rtcLayout->addWidget(rangeCheck);
                rtcLayout->addWidget(singleEdit, 1);
                rtcLayout->addWidget(minEdit, 1);
                rtcLayout->addWidget(maxEdit, 1);
                groupForm->addRow(QStringLiteral("RTC"), rtcRow);
                launchEditors->tasRtc.push_back(TasRtcEditor{ nodeKey, rangeCheck, singleEdit, minEdit, maxEdit });
                addArgumentRow(QStringLiteral("Headroom"), QStringLiteral("headroom"));
            } else if (isSeedProbeUnit(unitKind)) {
                addArgumentRow(QStringLiteral("samples/axis"), QStringLiteral("samples_per_axis"));
                addArgumentRow(QStringLiteral("combo attempts"), QStringLiteral("combo_attempts_per_target"));
            } else if (isBattleChainUnit(unitKind)) {
                addArgumentRow(QStringLiteral("fake attack low"), QStringLiteral("fake_attack_min"));
                addArgumentRow(QStringLiteral("fake attack high"), QStringLiteral("fake_attack_max"));
            }

            for (const auto& input : node.inputs) {
                if (!input.required) {
                    continue;
                }
                auto* rowHost = new QFrame(group);
                auto* rowLayout = new QHBoxLayout(rowHost);
                rowLayout->setContentsMargins(0, 0, 0, 0);
                rowLayout->setSpacing(6);

                const auto* incomingEdge = findIncomingEdge(graph, node.node_key, input.input_key);
                QLabel* edgeLabel = nullptr;
                QCheckBox* overrideCheck = nullptr;
                if (incomingEdge != nullptr) {
                    const auto* sourceNode = findGraphNode(graph, incomingEdge->from_node_key);
                    const auto* sourceOutput = sourceNode == nullptr ? nullptr : findGraphOutput(*sourceNode, incomingEdge->output_key);
                    const QString sourceText = sourceNode == nullptr
                        ? qs(incomingEdge->from_node_key)
                        : nodeDisplayName(*sourceNode);
                    const QString outputText = sourceOutput == nullptr
                        ? qs(incomingEdge->output_key)
                        : outputDisplayName(*sourceOutput);
                    edgeLabel = new QLabel(
                        QStringLiteral("Using %1.%2 as %3")
                            .arg(sourceText, outputText, inputDisplayName(input)),
                        rowHost);
                    edgeLabel->setObjectName("sectionDescription");
                    edgeLabel->setWordWrap(true);
                    overrideCheck = new QCheckBox(QStringLiteral("Override"), rowHost);
                    rowLayout->addWidget(edgeLabel, 2);
                    rowLayout->addWidget(overrideCheck);
                }

                auto* refKindEdit = new QLineEdit(defaultRefKindForDataKind(qs(input.data_kind)), rowHost);
                auto* refIdEdit = new QLineEdit(rowHost);
                refIdEdit->setPlaceholderText(QStringLiteral("ref id"));
                rowLayout->addWidget(refKindEdit, 2);
                rowLayout->addWidget(refIdEdit, 1);
                if (incomingEdge != nullptr) {
                    refKindEdit->setVisible(false);
                    refIdEdit->setVisible(false);
                    QObject::connect(overrideCheck, &QCheckBox::toggled, rowHost, [edgeLabel, refKindEdit, refIdEdit](bool checked) {
                        edgeLabel->setVisible(!checked);
                        refKindEdit->setVisible(checked);
                        refIdEdit->setVisible(checked);
                    });
                }

                groupForm->addRow(inputDisplayName(input), rowHost);
                launchEditors->inputs.push_back(LaunchInputEditor{
                    nodeKey,
                    qs(input.input_key),
                    qs(input.data_kind),
                    incomingEdge == nullptr ? QStringLiteral("external") : QStringLiteral("external_override"),
                    overrideCheck,
                    refKindEdit,
                    refIdEdit,
                });
            }
            launchGroupsLayout->addWidget(group);
        }
        if (graph.nodes.empty()) {
            auto* none = new QLabel(QStringLiteral("This workflow has no units."), launchGroupsHost);
            none->setObjectName("sectionDescription");
            launchGroupsLayout->addWidget(none);
        }
    };
    QObject::connect(workflowCombo, &QComboBox::currentIndexChanged, launchPanel, [refreshLaunchSelection](int) {
        refreshLaunchSelection();
    });

    auto selectLaunchWorkflow = [workflowCombo](int graphIndex) {
        const int comboIndex = workflowCombo->findData(graphIndex);
        if (comboIndex >= 0) {
            workflowCombo->setCurrentIndex(comboIndex);
        }
    };

    auto refreshWorkflowViews = std::make_shared<std::function<void()>>();
    *refreshWorkflowViews = [=]() {
        const bool showHidden = showHiddenWorkflowsCheck->isChecked();
        qint64 selectedGraphId = -1;
        const int selectedIndex = workflowCombo->currentIndex() >= 0 ? workflowCombo->currentData().toInt() : -1;
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(workflowGraphs->size())) {
            selectedGraphId = (*workflowGraphs)[static_cast<std::size_t>(selectedIndex)].workflow_graph_id;
        }

        int visibleCount = 0;
        int hiddenCount = 0;
        int comboIndexToSelect = -1;

        {
            QSignalBlocker comboBlocker(workflowCombo);
            QSignalBlocker tableBlocker(workflowTable);
            workflowCombo->clear();
            workflowTable->setRowCount(0);

            for (int graphIndex = 0; graphIndex < static_cast<int>(workflowGraphs->size()); ++graphIndex) {
                const auto& graph = (*workflowGraphs)[static_cast<std::size_t>(graphIndex)];
                if (graph.hidden) {
                    ++hiddenCount;
                    if (!showHidden) {
                        continue;
                    }
                } else {
                    ++visibleCount;
                }

                const QString name = qs(graph.name);
                const QString comboText = graph.hidden ? name + QStringLiteral(" (hidden)") : name;
                workflowCombo->addItem(comboText, graphIndex);
                if (graph.workflow_graph_id == selectedGraphId) {
                    comboIndexToSelect = workflowCombo->count() - 1;
                }

                const int row = workflowTable->rowCount();
                workflowTable->setRowCount(row + 1);
                workflowTable->setItem(row, 0, createTableItem(name, graphIndex));
                workflowTable->setItem(row, 1, createTableItem(QString::number(static_cast<int>(graph.nodes.size())), graphIndex));
                workflowTable->setItem(row, 2, createTableItem(QStringLiteral("#%1").arg(graph.graph_version), graphIndex));
            }

            if (workflowCombo->count() > 0) {
                workflowCombo->setCurrentIndex(comboIndexToSelect >= 0 ? comboIndexToSelect : 0);
            }
        }

        if (!graphs.ok) {
            workflowSummaryLabel->setText(qs(graphs.error.message));
        } else if (workflowTable->rowCount() == 0) {
            workflowSummaryLabel->setText(showHidden
                ? QStringLiteral("No workflow graphs found.")
                : QStringLiteral("No visible workflow graphs found. Enable Show hidden to include hidden graphs."));
        } else {
            workflowSummaryLabel->setText(QStringLiteral("%1 visible, %2 hidden workflow graphs.")
                .arg(visibleCount)
                .arg(hiddenCount));
        }
        refreshLaunchSelection();
    };

    QObject::connect(showHiddenWorkflowsCheck, &QCheckBox::toggled, createWorkflowPanel, [refreshWorkflowViews](bool) {
        if (*refreshWorkflowViews) {
            (*refreshWorkflowViews)();
        }
    });
    QObject::connect(workflowTable, &QTableWidget::currentCellChanged, createWorkflowPanel, [workflowTable, selectLaunchWorkflow](int currentRow, int, int, int) {
        if (currentRow < 0) {
            return;
        }
        const auto* item = workflowTable->item(currentRow, 0);
        if (item != nullptr) {
            selectLaunchWorkflow(item->data(kWorkflowGraphIndexRole).toInt());
        }
    });
    QObject::connect(workflowTable, &QTableWidget::cellDoubleClicked, createWorkflowPanel, [this, workflowGraphs, workflowTable](int row, int) {
        const auto* item = workflowTable->item(row, 0);
        if (item == nullptr) {
            return;
        }
        const int graphIndex = item->data(kWorkflowGraphIndexRole).toInt();
        if (graphIndex < 0 || graphIndex >= static_cast<int>(workflowGraphs->size())) {
            return;
        }
        const auto graph = (*workflowGraphs)[static_cast<std::size_t>(graphIndex)];
        setContext(
            { QStringLiteral("setup"), QStringLiteral("workflow_graph"), graph.workflow_graph_id, qs(graph.name) },
            QStringLiteral("Workflow details"),
            workflowDetailText(graph),
            QVector<std::pair<QString, std::function<void()>>>{
                { QStringLiteral("Edit"), [this, graph]() {
                    if (actions_.openGraphEditorSnapshot) {
                        actions_.openGraphEditorSnapshot(graph, false);
                    }
                } },
                { QStringLiteral("Duplicate"), [this, graph]() {
                    if (actions_.openGraphEditorSnapshot) {
                        actions_.openGraphEditorSnapshot(graph, true);
                    }
                } },
                { QStringLiteral("Open authoring library"), actions_.openAuthoring },
            },
            soasimqt2::gui::ContextDrawerMode::Expanded);
    });
    QObject::connect(workflowTable, &QWidget::customContextMenuRequested, createWorkflowPanel, [this, workflowGraphs, workflowTable, workflowSummaryLabel, refreshWorkflowViews](const QPoint& pos) {
        const auto* item = workflowTable->itemAt(pos);
        if (item == nullptr) {
            return;
        }
        const int row = item->row();
        workflowTable->selectRow(row);
        const auto* graphItem = workflowTable->item(row, 0);
        if (graphItem == nullptr) {
            return;
        }
        const int graphIndex = graphItem->data(kWorkflowGraphIndexRole).toInt();
        if (graphIndex < 0 || graphIndex >= static_cast<int>(workflowGraphs->size())) {
            return;
        }

        const auto graph = (*workflowGraphs)[static_cast<std::size_t>(graphIndex)];
        QMenu menu(workflowTable);
        menu.addAction(QStringLiteral("Edit"), workflowTable, [this, graph]() {
            if (actions_.openGraphEditorSnapshot) {
                actions_.openGraphEditorSnapshot(graph, false);
            }
        });
        menu.addAction(QStringLiteral("Duplicate"), workflowTable, [this, graph]() {
            if (actions_.openGraphEditorSnapshot) {
                actions_.openGraphEditorSnapshot(graph, true);
            }
        });
        menu.addSeparator();
        const bool hide = !graph.hidden;
        menu.addAction(hide ? QStringLiteral("Hide") : QStringLiteral("Unhide"), workflowTable, [workflowGraphs, graphIndex, hide, workflowSummaryLabel, refreshWorkflowViews]() {
            const auto graphId = (*workflowGraphs)[static_cast<std::size_t>(graphIndex)].workflow_graph_id;
            const auto result = soasimqt2::db::SimCoreDbAuthoringService::SetWorkflowGraphHidden(graphId, hide);
            if (!result.ok) {
                workflowSummaryLabel->setText(qs(result.error.message));
                return;
            }
            (*workflowGraphs)[static_cast<std::size_t>(graphIndex)].hidden = hide;
            if (*refreshWorkflowViews) {
                (*refreshWorkflowViews)();
            }
        });
        menu.exec(workflowTable->viewport()->mapToGlobal(pos));
    });
    if (*refreshWorkflowViews) {
        (*refreshWorkflowViews)();
    }

    auto* launchButton = new QPushButton(QStringLiteral("Launch"), launchPanel);
    launchButton->setObjectName("jobsPrimaryButton");
    QObject::connect(launchButton, &QPushButton::clicked, launchPanel, [=, this]() {
        if (workflowCombo->currentIndex() < 0) {
            setContext(
                { QStringLiteral("setup"), QStringLiteral("launch"), 0, QStringLiteral("launch") },
                QStringLiteral("Launch workflow"),
                QStringLiteral("No workflow graph is available to launch."),
                {},
                soasimqt2::gui::ContextDrawerMode::Expanded);
            return;
        }

        const int graphIndex = workflowCombo->currentData().toInt();
        if (graphIndex < 0 || graphIndex >= static_cast<int>(workflowGraphs->size())) {
            return;
        }
        const auto& graph = (*workflowGraphs)[static_cast<std::size_t>(graphIndex)];

        const auto blockLaunch = [&](const QString& message) {
            setContext(
                { QStringLiteral("setup"), QStringLiteral("launch"), graph.workflow_graph_id, qs(graph.name) },
                QStringLiteral("Launch blocked"),
                message,
                QVector<std::pair<QString, std::function<void()>>>{
                    { QStringLiteral("Open full launcher"), actions_.openLauncher },
                },
                soasimqt2::gui::ContextDrawerMode::Expanded);
        };

        std::vector<soasimqt2::db::WorkflowGraphInputBindingDraft> baseBindings;
        for (const auto& editor : launchEditors->inputs) {
            if (editor.overrideCheck != nullptr && !editor.overrideCheck->isChecked()) {
                continue;
            }
            bool ok = false;
            const auto refId = editor.refIdEdit->text().toLongLong(&ok);
            if (!ok || refId <= 0 || editor.refKindEdit->text().trimmed().isEmpty()) {
                blockLaunch(QStringLiteral("Required input %1.%2 needs a ref kind and positive ref id.")
                    .arg(editor.nodeKey, editor.inputKey));
                return;
            }
            baseBindings.push_back(soasimqt2::db::WorkflowGraphInputBindingDraft{
                .node_key = editor.nodeKey.toStdString(),
                .input_key = editor.inputKey.toStdString(),
                .data_kind = editor.dataKind.toStdString(),
                .ref_kind = editor.refKindEdit->text().trimmed().toStdString(),
                .ref_id = refId,
                .source_kind = editor.sourceKind.toStdString(),
            });
        }

        std::vector<soasimqt2::db::WorkflowGraphArgumentDraft> commonArguments;
        const auto addIntegerArgument = [&](const LaunchArgumentEditor& editor) -> bool {
            const QString text = editor.edit->text().trimmed();
            if (text.isEmpty()) {
                return true;
            }
            bool ok = false;
            const auto value = text.toLongLong(&ok);
            if (!ok) {
                blockLaunch(QStringLiteral("Argument %1.%2 must be an integer.")
                    .arg(editor.nodeKey, editor.argumentKey));
                return false;
            }
            commonArguments.push_back(soasimqt2::db::WorkflowGraphArgumentDraft{
                .node_key = editor.nodeKey.toStdString(),
                .argument_key = editor.argumentKey.toStdString(),
                .value_type = "integer",
                .integer_value = value,
                .text_value = std::nullopt,
                .source_kind = "setup_tab",
            });
            return true;
        };
        for (const auto& editor : launchEditors->arguments) {
            if (!addIntegerArgument(editor)) {
                return;
            }
        }

        struct RtcLaunchValue {
            QString nodeKey;
            bool range = false;
            bool hasSingle = false;
            qint64 single = 0;
        };
        std::vector<RtcLaunchValue> rtcLaunchValues;
        bool hasRtcRange = false;
        qint64 rtcMin = 0;
        qint64 rtcMax = 0;
        for (const auto& rtcEditor : launchEditors->tasRtc) {
            RtcLaunchValue value{};
            value.nodeKey = rtcEditor.nodeKey;
            value.range = rtcEditor.rangeCheck->isChecked();
            if (value.range) {
                bool minOk = false;
                bool maxOk = false;
                const auto parsedMin = rtcEditor.minEdit->text().trimmed().toLongLong(&minOk);
                const auto parsedMax = rtcEditor.maxEdit->text().trimmed().toLongLong(&maxOk);
                if (!minOk || !maxOk || parsedMin > parsedMax) {
                    blockLaunch(QStringLiteral("RTC range for %1 needs valid min and max integers.").arg(rtcEditor.nodeKey));
                    return;
                }
                if (!hasRtcRange) {
                    hasRtcRange = true;
                    rtcMin = parsedMin;
                    rtcMax = parsedMax;
                } else if (rtcMin != parsedMin || rtcMax != parsedMax) {
                    blockLaunch(QStringLiteral("Multiple TAS Movie RTC ranges must use the same min and max."));
                    return;
                }
            } else {
                const QString singleText = rtcEditor.singleEdit->text().trimmed();
                if (!singleText.isEmpty()) {
                    bool ok = false;
                    const auto parsedSingle = singleText.toLongLong(&ok);
                    if (!ok) {
                        blockLaunch(QStringLiteral("RTC for %1 must be an integer.").arg(rtcEditor.nodeKey));
                        return;
                    }
                    value.hasSingle = true;
                    value.single = parsedSingle;
                }
            }
            rtcLaunchValues.push_back(value);
        }

        std::vector<qint64> launchedIds;
        const qint64 launchLow = hasRtcRange ? rtcMin : 0;
        const qint64 launchHigh = hasRtcRange ? rtcMax : 0;
        for (qint64 rtc = launchLow; rtc <= launchHigh; ++rtc) {
            soasimqt2::db::WorkflowGraphStartRequest request{};
            request.workflow_graph_revision_id = graph.workflow_graph_revision_id;
            request.created_by = "SoaSimQt2.SetupTab";
            request.input_bindings = baseBindings;
            request.arguments = commonArguments;

            for (const auto& rtcValue : rtcLaunchValues) {
                if (hasRtcRange && rtcValue.range) {
                    request.arguments.push_back(soasimqt2::db::WorkflowGraphArgumentDraft{
                        .node_key = rtcValue.nodeKey.toStdString(),
                        .argument_key = "rtc",
                        .value_type = "integer",
                        .integer_value = rtc,
                        .text_value = std::nullopt,
                        .source_kind = "setup_tab",
                    });
                } else if (rtcValue.hasSingle) {
                    request.arguments.push_back(soasimqt2::db::WorkflowGraphArgumentDraft{
                        .node_key = rtcValue.nodeKey.toStdString(),
                        .argument_key = "rtc",
                        .value_type = "integer",
                        .integer_value = rtcValue.single,
                        .text_value = std::nullopt,
                        .source_kind = "setup_tab",
                    });
                }
            }

            const auto result = soasimqt2::db::SimCoreDbWorkflowService::StartWorkflowGraphRevision(request);
            if (!result.ok) {
                blockLaunch(QStringLiteral("%1\n\nCheck required input bindings above, then retry.").arg(qs(result.error.message)));
                return;
            }
            launchedIds.push_back(result.value);
            if (!hasRtcRange) {
                break;
            }
        }

        if (!launchedIds.empty()) {
            const QString message = launchedIds.size() == 1
                ? QStringLiteral("Started workflow instance #%1 from %2. Continue in Running for activation/job progress.")
                    .arg(launchedIds.front())
                    .arg(qs(graph.name))
                : QStringLiteral("Started %1 workflow instances from %2 for RTC %3-%4. Continue in Running for activation/job progress.")
                    .arg(static_cast<int>(launchedIds.size()))
                    .arg(qs(graph.name))
                    .arg(rtcMin)
                    .arg(rtcMax);
            setContext(
                { QStringLiteral("setup"), QStringLiteral("launch"), launchedIds.front(), qs(graph.name) },
                QStringLiteral("Workflow launched"),
                message,
                {},
                soasimqt2::gui::ContextDrawerMode::Expanded);
        }
    });
    launchLayout->addWidget(launchButton);
    launchLayout->addStretch();
    workbenchLayout->addWidget(launchPanel, 2);

    canvasLayout()->addWidget(workbench);

    auto* footer = new QFrame(this);
    footer->setObjectName("workspaceCardGrid");
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(0, 0, 0, 0);
    footerLayout->setSpacing(8);
    auto* advancedLabel = new QLabel(QStringLiteral("Advanced:"), footer);
    advancedLabel->setObjectName("sectionDescription");
    footerLayout->addWidget(advancedLabel);
    const QVector<std::pair<QString, std::function<void()>>> actions{
        { QStringLiteral("Authoring Libraries"), actions_.openAuthoring },
        { QStringLiteral("Artifacts"), actions_.openArtifacts },
        { QStringLiteral("DTM Editor"), actions_.openDtmEditor },
        { QStringLiteral("Environment"), actions_.openSettings },
        { QStringLiteral("Battle Settings"), actions_.openBattleSettings },
    };
    for (const auto& action : actions) {
        auto* button = new QPushButton(action.first, footer);
        button->setObjectName("jobsSecondaryButton");
        QObject::connect(button, &QPushButton::clicked, footer, [callback = action.second]() {
            if (callback) {
                callback();
            }
        });
        footerLayout->addWidget(button);
    }
    footerLayout->addStretch();
    canvasLayout()->addWidget(footer);
    canvasLayout()->addStretch();
}
