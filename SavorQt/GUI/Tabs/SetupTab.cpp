#include "GUI/Tabs/SetupTab.h"

#include "DB/SavorDbArtifactService.h"
#include "DB/SavorDbAuthoringService.h"
#include "DB/SavorDbWorkflowService.h"
#include "GUI/Panes/CoordinatorPane/CoordinatorController.h"
#include "GUI/Refresh/AsyncRefreshPipeline.h"
#include "GUI/Refresh/RowUpdate.h"
#include "SavorDbRuntime.h"

#include <QtCore/QSize>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
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
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <string>
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

struct LaunchInputDraft {
    bool overrideChecked = false;
    QString refKind;
    QString refId;
};

struct LaunchArgumentDraft {
    QString value;
};

struct TasRtcDraft {
    bool rangeChecked = false;
    QString single;
    QString min;
    QString max;
};

struct LaunchDraft {
    std::map<QString, LaunchInputDraft> inputs;
    std::map<QString, LaunchArgumentDraft> arguments;
    std::map<QString, TasRtcDraft> tasRtc;
};

struct WorkflowGraphViewRow {
    QString name;
    QString comboText;
    QString nodeCount;
    QString revision;
    qint64 graphId = -1;
    int graphIndex = -1;
};

struct WorkflowGraphRefreshRequest {
    bool showHidden = false;
    qint64 selectedGraphId = -1;
};

struct WorkflowGraphRefreshData {
    std::vector<savor::db::WorkflowGraphSnapshot> graphs;
    std::vector<WorkflowGraphViewRow> rows;
    int visibleCount = 0;
    int hiddenCount = 0;
    int comboIndexToSelect = -1;
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

QString nodeDisplayName(const savor::db::WorkflowGraphNodeSnapshot& node)
{
    return node.display_name.empty() ? qs(node.unit_kind) : qs(node.display_name);
}

QString inputDisplayName(const savor::db::WorkflowGraphNodeInputSnapshot& input)
{
    return input.display_name.empty() ? qs(input.input_key) : qs(input.display_name);
}

QString outputDisplayName(const savor::db::WorkflowGraphNodeOutputSnapshot& output)
{
    return output.display_name.empty() ? qs(output.output_key) : qs(output.display_name);
}

const savor::db::WorkflowGraphEdgeSnapshot* findIncomingEdge(
    const savor::db::WorkflowGraphSnapshot& graph,
    const std::string& nodeKey,
    const std::string& inputKey)
{
    const auto it = std::find_if(graph.edges.begin(), graph.edges.end(), [&](const auto& edge) {
        return edge.to_node_key == nodeKey && edge.input_key == inputKey;
    });
    return it == graph.edges.end() ? nullptr : &*it;
}

const savor::db::WorkflowGraphNodeSnapshot* findGraphNode(
    const savor::db::WorkflowGraphSnapshot& graph,
    const std::string& nodeKey)
{
    const auto it = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const auto& node) {
        return node.node_key == nodeKey;
    });
    return it == graph.nodes.end() ? nullptr : &*it;
}

const savor::db::WorkflowGraphNodeOutputSnapshot* findGraphOutput(
    const savor::db::WorkflowGraphNodeSnapshot& node,
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

bool coordinatorIsoReady(const CoordinatorController* controller)
{
    if (controller == nullptr) {
        return true;
    }

    const QString isoPath = controller->isoPath().trimmed();
    return !isoPath.isEmpty() && QFileInfo(isoPath).isFile();
}

bool coordinatorDolphinBaseReady(const CoordinatorController* controller)
{
    if (controller == nullptr) {
        return true;
    }

    const QString dolphinBase = controller->dolphinBaseDir().trimmed();
    if (dolphinBase.isEmpty() || !QFileInfo(dolphinBase).isDir()) {
        return false;
    }

    const QDir dolphinDir(dolphinBase);
    return QFileInfo(dolphinDir.filePath(QStringLiteral("portable.txt"))).isFile()
        && QFileInfo(dolphinDir.filePath(QStringLiteral("Sys/GC/dsp_coef.bin"))).isFile();
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

QPushButton* createSetupWarningButton(const QString& text, QWidget* parent)
{
    auto* button = new QPushButton(text, parent);
    button->setObjectName("setupWarningButton");
    return button;
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

void populateWorkflowGraphRow(QTableWidget* table, int row, const WorkflowGraphViewRow& viewRow)
{
    table->setItem(row, 0, createTableItem(viewRow.name, viewRow.graphIndex));
    table->setItem(row, 1, createTableItem(viewRow.nodeCount, viewRow.graphIndex));
    table->setItem(row, 2, createTableItem(viewRow.revision, viewRow.graphIndex));
}

bool workflowGraphRowsEqual(const WorkflowGraphViewRow& lhs, const WorkflowGraphViewRow& rhs)
{
    return lhs.name == rhs.name
        && lhs.comboText == rhs.comboText
        && lhs.nodeCount == rhs.nodeCount
        && lhs.revision == rhs.revision
        && lhs.graphId == rhs.graphId
        && lhs.graphIndex == rhs.graphIndex;
}

QString launchKey(const QString& nodeKey, const QString& childKey)
{
    return nodeKey + QChar(0x1f) + childKey;
}

LaunchDraft captureLaunchDraft(const LaunchEditors& editors)
{
    LaunchDraft draft;
    for (const auto& input : editors.inputs) {
        draft.inputs[launchKey(input.nodeKey, input.inputKey)] = LaunchInputDraft{
            input.overrideCheck != nullptr && input.overrideCheck->isChecked(),
            input.refKindEdit == nullptr ? QString() : input.refKindEdit->text(),
            input.refIdEdit == nullptr ? QString() : input.refIdEdit->text(),
        };
    }
    for (const auto& argument : editors.arguments) {
        draft.arguments[launchKey(argument.nodeKey, argument.argumentKey)] = LaunchArgumentDraft{
            argument.edit == nullptr ? QString() : argument.edit->text(),
        };
    }
    for (const auto& rtc : editors.tasRtc) {
        draft.tasRtc[rtc.nodeKey] = TasRtcDraft{
            rtc.rangeCheck != nullptr && rtc.rangeCheck->isChecked(),
            rtc.singleEdit == nullptr ? QString() : rtc.singleEdit->text(),
            rtc.minEdit == nullptr ? QString() : rtc.minEdit->text(),
            rtc.maxEdit == nullptr ? QString() : rtc.maxEdit->text(),
        };
    }
    return draft;
}

void restoreLaunchDraft(const LaunchDraft& draft, const LaunchEditors& editors)
{
    for (const auto& input : editors.inputs) {
        const auto it = draft.inputs.find(launchKey(input.nodeKey, input.inputKey));
        if (it == draft.inputs.end()) {
            continue;
        }
        if (input.overrideCheck != nullptr) {
            input.overrideCheck->setChecked(it->second.overrideChecked);
        }
        if (input.refKindEdit != nullptr) {
            input.refKindEdit->setText(it->second.refKind);
        }
        if (input.refIdEdit != nullptr) {
            input.refIdEdit->setText(it->second.refId);
        }
    }
    for (const auto& argument : editors.arguments) {
        const auto it = draft.arguments.find(launchKey(argument.nodeKey, argument.argumentKey));
        if (it != draft.arguments.end() && argument.edit != nullptr) {
            argument.edit->setText(it->second.value);
        }
    }
    for (const auto& rtc : editors.tasRtc) {
        const auto it = draft.tasRtc.find(rtc.nodeKey);
        if (it == draft.tasRtc.end()) {
            continue;
        }
        if (rtc.rangeCheck != nullptr) {
            rtc.rangeCheck->setChecked(it->second.rangeChecked);
        }
        if (rtc.singleEdit != nullptr) {
            rtc.singleEdit->setText(it->second.single);
        }
        if (rtc.minEdit != nullptr) {
            rtc.minEdit->setText(it->second.min);
        }
        if (rtc.maxEdit != nullptr) {
            rtc.maxEdit->setText(it->second.max);
        }
    }
}

QString launchShapeSignature(const savor::db::WorkflowGraphSnapshot& graph)
{
    QStringList parts;
    parts.reserve(static_cast<int>(graph.nodes.size()));
    for (const auto& node : graph.nodes) {
        QStringList nodeParts;
        nodeParts << qs(node.node_key) << qs(node.unit_kind);
        const QString unitKind = qs(node.unit_kind);
        if (isTasMovieUnit(unitKind)) {
            nodeParts << QStringLiteral("rtc");
        } else if (isSeedProbeUnit(unitKind)) {
            nodeParts << QStringLiteral("arg:samples_per_axis");
        } else if (isBattleChainUnit(unitKind)) {
            nodeParts << QStringLiteral("arg:fake_attack_min") << QStringLiteral("arg:fake_attack_max");
        }
        for (const auto& input : node.inputs) {
            if (!input.required) {
                continue;
            }
            const auto* incomingEdge = findIncomingEdge(graph, node.node_key, input.input_key);
            nodeParts << QStringLiteral("input:%1:%2:%3")
                .arg(qs(input.input_key), qs(input.data_kind), incomingEdge == nullptr ? QStringLiteral("external") : QStringLiteral("edge"));
        }
        parts << nodeParts.join(QChar(0x1e));
    }
    return parts.join(QChar(0x1d));
}

WorkflowGraphRefreshData prepareWorkflowGraphData(
    const std::vector<savor::db::WorkflowGraphSnapshot>& graphs,
    const WorkflowGraphRefreshRequest& request)
{
    WorkflowGraphRefreshData data;
    data.graphs = graphs;
    data.rows.reserve(graphs.size());

    for (int graphIndex = 0; graphIndex < static_cast<int>(graphs.size()); ++graphIndex) {
        const auto& graph = graphs[static_cast<std::size_t>(graphIndex)];
        if (graph.hidden) {
            ++data.hiddenCount;
            if (!request.showHidden) {
                continue;
            }
        } else {
            ++data.visibleCount;
        }

        const QString name = qs(graph.name);
        const int comboIndex = static_cast<int>(data.rows.size());
        if (graph.workflow_graph_id == request.selectedGraphId) {
            data.comboIndexToSelect = comboIndex;
        }
        data.rows.push_back(WorkflowGraphViewRow{
            name,
            graph.hidden ? name + QStringLiteral(" (hidden)") : name,
            QString::number(static_cast<int>(graph.nodes.size())),
            QStringLiteral("#%1").arg(graph.graph_version),
            graph.workflow_graph_id,
            graphIndex,
        });
    }

    return data;
}

QString describeSpecRef(const std::optional<std::string>& refKind, const std::optional<std::int64_t>& refId)
{
    if (!refKind.has_value() || !refId.has_value() || *refId <= 0) {
        return QStringLiteral("No authored spec reference.");
    }

    const QString kind = qs(*refKind);
    const auto id = *refId;
    if (*refKind == "tas_spec") {
        const auto spec = savorqt::db::SavorDbAuthoringService::GetTasSpec(id);
        if (!spec.ok) {
            return QStringLiteral("TAS spec #%1 unavailable: %2").arg(id).arg(qs(spec.error.message));
        }
        return QStringLiteral("TAS spec: %1 (#%2)\n  base DTM artifact: %3")
            .arg(qs(spec.value.base_name))
            .arg(id)
            .arg(spec.value.base_dtm_artifact_id);
    }
    if (*refKind == "seed_probe_spec") {
        const auto spec = savorqt::db::SavorDbAuthoringService::GetSeedProbeSpec(id);
        if (!spec.ok) {
            return QStringLiteral("Seed probe spec #%1 unavailable: %2").arg(id).arg(qs(spec.error.message));
        }
        return QStringLiteral("Seed probe spec: %1 (#%2)\n  combo attempts: %3\n  value range: %4 to %5")
            .arg(qs(spec.value.name))
            .arg(id)
            .arg(spec.value.combo_attempts_per_target)
            .arg(spec.value.min_value)
            .arg(spec.value.max_value);
    }
    if (*refKind == "battle_run_spec") {
        const auto spec = savorqt::db::SavorDbAuthoringService::GetBattleRunSpec(id);
        if (!spec.ok) {
            return QStringLiteral("Battle run spec #%1 unavailable: %2").arg(id).arg(qs(spec.error.message));
        }
        return QStringLiteral("Battle run spec: %1 (#%2)\n  single-turn runner: %3")
            .arg(qs(spec.value.name))
            .arg(id)
            .arg(spec.value.use_single_turn_runner ? QStringLiteral("yes") : QStringLiteral("no"));
    }
    if (*refKind == "authoring.battle_chain_spec" || *refKind == "battle_chain_spec") {
        const auto spec = savorqt::db::SavorDbAuthoringService::GetBattleChainSpec(id);
        if (!spec.ok) {
            return QStringLiteral("Battle chain spec #%1 unavailable: %2").arg(id).arg(qs(spec.error.message));
        }
        QString text = QStringLiteral("Battle chain spec: %1 (#%2)\n  battle run spec: #%3\n  explorer settings: #%4")
            .arg(qs(spec.value.name))
            .arg(id)
            .arg(spec.value.battle_run_spec_id)
            .arg(spec.value.explorer_settings_id);
        const auto battleRun = savorqt::db::SavorDbAuthoringService::GetBattleRunSpec(spec.value.battle_run_spec_id);
        if (battleRun.ok) {
            text += QStringLiteral("\n  battle run: %1").arg(qs(battleRun.value.name));
        }
        const auto explorer = savorqt::db::SavorDbAuthoringService::GetExplorerSettings(spec.value.explorer_settings_id);
        if (explorer.ok) {
            text += QStringLiteral("\n  explorer: %1").arg(qs(explorer.value.name));
        }
        return text;
    }
    return QStringLiteral("%1 #%2").arg(kind).arg(id);
}

QString workflowDetailText(const savor::db::WorkflowGraphSnapshot& graph)
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
    setPageVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    const bool dbReady = savorqt::SavorDbRuntime::instance().isRunning();
    const bool artifactStorageReady = savorqt::db::SavorDbArtifactService::StorageReady();
    const auto graphs = savorqt::db::SavorDbAuthoringService::ListWorkflowGraphs(100, true);
    const auto units = savorqt::db::SavorDbWorkflowService::ListWorkflowUnits();

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

    auto* isoSetupButton = createSetupWarningButton(QStringLiteral("Set ISO"), statusStrip);
    QObject::connect(isoSetupButton, &QPushButton::clicked, statusStrip, [this]() {
        if (actions_.openIsoSettings) {
            actions_.openIsoSettings();
        } else if (actions_.openSettings) {
            actions_.openSettings();
        }
    });
    statusLayout->addWidget(isoSetupButton);

    auto* dolphinSetupButton = createSetupWarningButton(QStringLiteral("Set Dolphin base"), statusStrip);
    QObject::connect(dolphinSetupButton, &QPushButton::clicked, statusStrip, [this]() {
        if (actions_.openDolphinSettings) {
            actions_.openDolphinSettings();
        } else if (actions_.openSettings) {
            actions_.openSettings();
        }
    });
    statusLayout->addWidget(dolphinSetupButton);

    const auto refreshCoordinatorSetupWarnings = [this, isoSetupButton, dolphinSetupButton]() {
        const bool isoReady = coordinatorIsoReady(coordinatorController_);
        const bool dolphinReady = coordinatorDolphinBaseReady(coordinatorController_);
        const bool isoMissing = coordinatorController_ != nullptr && coordinatorController_->isoPath().trimmed().isEmpty();
        const bool dolphinMissing = coordinatorController_ != nullptr && coordinatorController_->dolphinBaseDir().trimmed().isEmpty();

        isoSetupButton->setText(isoMissing ? QStringLiteral("Set ISO") : QStringLiteral("Fix ISO"));
        dolphinSetupButton->setText(dolphinMissing ? QStringLiteral("Set Dolphin base") : QStringLiteral("Fix Dolphin base"));
        isoSetupButton->setVisible(!isoReady);
        dolphinSetupButton->setVisible(!dolphinReady);
    };
    refreshCoordinatorSetupWarnings();
    if (coordinatorController_ != nullptr) {
        QObject::connect(coordinatorController_, &CoordinatorController::stateChanged, statusStrip, refreshCoordinatorSetupWarnings);
    }

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
    workbench->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* workbenchLayout = new QHBoxLayout(workbench);
    workbenchLayout->setContentsMargins(0, 0, 0, 0);
    workbenchLayout->setSpacing(10);

    auto workflowGraphs = std::make_shared<std::vector<savor::db::WorkflowGraphSnapshot>>(
        graphs.ok ? graphs.value : std::vector<savor::db::WorkflowGraphSnapshot>{});

    auto* createWorkflowPanel = createSectionPanel(QStringLiteral("Workflows"), workbench);
    createWorkflowPanel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
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
    workflowTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
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
    workflowTable->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
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
    launchPanel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto* launchLayout = qobject_cast<QVBoxLayout*>(launchPanel->layout());
    auto* form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(8);

    auto* workflowCombo = new QComboBox(launchPanel);
    form->addRow(QStringLiteral("Workflow"), workflowCombo);
    launchLayout->addLayout(form);

    auto* launchGroupsScroll = new QScrollArea(launchPanel);
    launchGroupsScroll->setWidgetResizable(true);
    launchGroupsScroll->setFrameShape(QFrame::NoFrame);
    launchGroupsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    launchGroupsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    launchGroupsScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* launchGroupsHost = new QFrame(launchGroupsScroll);
    launchGroupsHost->setObjectName("workspaceCardGrid");
    launchGroupsHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* launchGroupsLayout = new QVBoxLayout(launchGroupsHost);
    launchGroupsLayout->setContentsMargins(0, 0, 0, 0);
    launchGroupsLayout->setSpacing(8);
    launchGroupsScroll->setWidget(launchGroupsHost);
    launchLayout->addWidget(launchGroupsScroll, 1);

    auto launchEditors = std::make_shared<LaunchEditors>();
    auto launchDrafts = std::make_shared<std::map<qint64, LaunchDraft>>();
    auto renderedLaunchGraphId = std::make_shared<qint64>(-1);
    auto renderedLaunchShape = std::make_shared<QString>();

    auto selectedWorkflowGraphId = [workflowGraphs, workflowCombo]() -> qint64 {
        const int selectedIndex = workflowCombo->currentIndex() >= 0 ? workflowCombo->currentData().toInt() : -1;
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(workflowGraphs->size())) {
            return (*workflowGraphs)[static_cast<std::size_t>(selectedIndex)].workflow_graph_id;
        }
        return -1;
    };

    auto captureCurrentLaunchDraft = [launchEditors, launchDrafts, renderedLaunchGraphId]() {
        const qint64 graphId = *renderedLaunchGraphId;
        if (graphId > 0) {
            (*launchDrafts)[graphId] = captureLaunchDraft(*launchEditors);
        }
    };

    auto refreshLaunchSelection = [workflowGraphs, workflowCombo, launchGroupsLayout, launchGroupsHost, launchEditors, launchDrafts, renderedLaunchGraphId, renderedLaunchShape, selectedWorkflowGraphId](bool forceRebuild = false) {
        const qint64 graphId = selectedWorkflowGraphId();
        QString shape;
        const int graphIndex = workflowCombo->currentIndex() >= 0 ? workflowCombo->currentData().toInt() : -1;
        if (graphIndex >= 0 && graphIndex < static_cast<int>(workflowGraphs->size())) {
            shape = launchShapeSignature((*workflowGraphs)[static_cast<std::size_t>(graphIndex)]);
        }
        if (!forceRebuild && graphId == *renderedLaunchGraphId && shape == *renderedLaunchShape) {
            return;
        }

        clearLayout(launchGroupsLayout);
        launchEditors->inputs.clear();
        launchEditors->arguments.clear();
        launchEditors->tasRtc.clear();
        *renderedLaunchGraphId = graphId;
        *renderedLaunchShape = shape;
        if (workflowCombo->currentIndex() < 0 || workflowCombo->currentIndex() >= workflowCombo->count()) {
            auto* none = new QLabel(QStringLiteral("No workflow selected."), launchGroupsHost);
            none->setObjectName("sectionDescription");
            launchGroupsLayout->addWidget(none);
            return;
        }
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
            } else if (isSeedProbeUnit(unitKind)) {
                addArgumentRow(QStringLiteral("samples/axis"), QStringLiteral("samples_per_axis"));
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
                    rowLayout->addWidget(overrideCheck);
                    rowLayout->addWidget(edgeLabel, 2);
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
        launchGroupsLayout->addStretch();
        const auto draftIt = launchDrafts->find(graph.workflow_graph_id);
        if (draftIt != launchDrafts->end()) {
            restoreLaunchDraft(draftIt->second, *launchEditors);
        }
    };
    QObject::connect(workflowCombo, &QComboBox::currentIndexChanged, launchPanel, [captureCurrentLaunchDraft, refreshLaunchSelection](int) {
        captureCurrentLaunchDraft();
        refreshLaunchSelection(true);
    });

    auto selectLaunchWorkflow = [workflowCombo](int graphIndex) {
        const int comboIndex = workflowCombo->findData(graphIndex);
        if (comboIndex >= 0) {
            workflowCombo->setCurrentIndex(comboIndex);
        }
    };

    auto workflowRows = std::make_shared<std::vector<WorkflowGraphViewRow>>();
    auto workflowRefreshPipeline =
        new savorqt::gui::AsyncRefreshPipeline<WorkflowGraphRefreshRequest, WorkflowGraphRefreshData>(createWorkflowPanel);
    auto kickWorkflowRefresh = std::make_shared<std::function<void()>>();

    workflowRefreshPipeline->setRequestBuilder([showHiddenWorkflowsCheck, selectedWorkflowGraphId](savorqt::gui::RefreshReason) {
        return WorkflowGraphRefreshRequest{
            showHiddenWorkflowsCheck->isChecked(),
            selectedWorkflowGraphId(),
        };
    });
    workflowRefreshPipeline->setLoadAndPrepare([](WorkflowGraphRefreshRequest request) {
        const auto result = savorqt::db::SavorDbAuthoringService::ListWorkflowGraphs(100, true);
        if (!result.ok) {
            return savorqt::gui::AsyncRefreshResult<WorkflowGraphRefreshData>::Err(qs(result.error.message));
        }
        return savorqt::gui::AsyncRefreshResult<WorkflowGraphRefreshData>::Ok(prepareWorkflowGraphData(result.value, request));
    });
    workflowRefreshPipeline->setApply([=](const WorkflowGraphRefreshData& data, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        captureCurrentLaunchDraft();
        const qint64 previousGraphId = selectedWorkflowGraphId();
        *workflowGraphs = data.graphs;
        {
            QSignalBlocker comboBlocker(workflowCombo);
            workflowCombo->clear();
            for (const auto& viewRow : data.rows) {
                workflowCombo->addItem(viewRow.comboText, viewRow.graphIndex);
            }
            if (workflowCombo->count() > 0) {
                int comboIndexToSelect = data.comboIndexToSelect;
                if (comboIndexToSelect < 0 && previousGraphId > 0) {
                    for (int i = 0; i < static_cast<int>(data.rows.size()); ++i) {
                        if (data.rows[static_cast<std::size_t>(i)].graphId == previousGraphId) {
                            comboIndexToSelect = i;
                            break;
                        }
                    }
                }
                workflowCombo->setCurrentIndex(comboIndexToSelect >= 0 ? comboIndexToSelect : 0);
            }
        }
        savorqt::gui::ApplyTableRowsByKey(
            workflowTable,
            *workflowRows,
            data.rows,
            [](const WorkflowGraphViewRow& row) { return row.graphId; },
            workflowGraphRowsEqual,
            populateWorkflowGraphRow);

        if (data.rows.empty()) {
            workflowSummaryLabel->setText(showHiddenWorkflowsCheck->isChecked()
                ? QStringLiteral("No workflow graphs found.")
                : QStringLiteral("No visible workflow graphs found. Enable Show hidden to include hidden graphs."));
        } else {
            workflowSummaryLabel->setText(QStringLiteral("%1 visible, %2 hidden workflow graphs.")
                .arg(data.visibleCount)
                .arg(data.hiddenCount));
        }
        refreshLaunchSelection();
    });
    workflowRefreshPipeline->setApplyError([workflowSummaryLabel](const QString& error, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        workflowSummaryLabel->setText(error);
    });

    *kickWorkflowRefresh = [workflowRefreshPipeline]() {
        workflowRefreshPipeline->requestRefresh(savorqt::gui::RefreshReason::Manual);
    };

    QObject::connect(showHiddenWorkflowsCheck, &QCheckBox::toggled, createWorkflowPanel, [kickWorkflowRefresh](bool) {
        if (*kickWorkflowRefresh) {
            (*kickWorkflowRefresh)();
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
            savorqt::gui::ContextDrawerMode::Expanded);
    });
    QObject::connect(workflowTable, &QWidget::customContextMenuRequested, createWorkflowPanel, [this, workflowGraphs, workflowTable, workflowSummaryLabel, kickWorkflowRefresh](const QPoint& pos) {
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
        menu.addAction(hide ? QStringLiteral("Hide") : QStringLiteral("Unhide"), workflowTable, [workflowGraphs, graphIndex, hide, workflowSummaryLabel, kickWorkflowRefresh]() {
            const auto graphId = (*workflowGraphs)[static_cast<std::size_t>(graphIndex)].workflow_graph_id;
            const auto result = savorqt::db::SavorDbAuthoringService::SetWorkflowGraphHidden(graphId, hide);
            if (!result.ok) {
                workflowSummaryLabel->setText(qs(result.error.message));
                return;
            }
            (*workflowGraphs)[static_cast<std::size_t>(graphIndex)].hidden = hide;
            if (*kickWorkflowRefresh) {
                (*kickWorkflowRefresh)();
            }
        });
        menu.exec(workflowTable->viewport()->mapToGlobal(pos));
    });
    workflowRefreshPipeline->setRefreshIntervalMs(2000);
    workflowRefreshPipeline->setActive(true);
    workflowRefreshPipeline->requestRefresh(savorqt::gui::RefreshReason::Initial);

    auto* launchButton = new QPushButton(QStringLiteral("Launch"), launchPanel);
    launchButton->setObjectName("jobsPrimaryButton");
    QObject::connect(launchButton, &QPushButton::clicked, launchPanel, [=, this]() {
        if (workflowCombo->currentIndex() < 0) {
            setContext(
                { QStringLiteral("setup"), QStringLiteral("launch"), 0, QStringLiteral("launch") },
                QStringLiteral("Launch workflow"),
                QStringLiteral("No workflow graph is available to launch."),
                {},
                savorqt::gui::ContextDrawerMode::Expanded);
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
                savorqt::gui::ContextDrawerMode::Expanded);
        };

        std::vector<savorqt::db::WorkflowGraphInputBindingDraft> baseBindings;
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
            baseBindings.push_back(savorqt::db::WorkflowGraphInputBindingDraft{
                .node_key = editor.nodeKey.toStdString(),
                .input_key = editor.inputKey.toStdString(),
                .data_kind = editor.dataKind.toStdString(),
                .ref_kind = editor.refKindEdit->text().trimmed().toStdString(),
                .ref_id = refId,
                .source_kind = editor.sourceKind.toStdString(),
            });
        }

        std::vector<savorqt::db::WorkflowGraphArgumentDraft> commonArguments;
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
            commonArguments.push_back(savorqt::db::WorkflowGraphArgumentDraft{
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
                if (!value.hasSingle) {
                    blockLaunch(QStringLiteral("RTC for %1 is required. Enter a single value or enable Range.").arg(rtcEditor.nodeKey));
                    return;
                }
            }
            rtcLaunchValues.push_back(value);
        }

        std::vector<qint64> launchedIds;
        const qint64 launchLow = hasRtcRange ? rtcMin : 0;
        const qint64 launchHigh = hasRtcRange ? rtcMax : 0;
        for (qint64 rtc = launchLow; rtc <= launchHigh; ++rtc) {
            savorqt::db::WorkflowGraphStartRequest request{};
            request.workflow_graph_revision_id = graph.workflow_graph_revision_id;
            request.created_by = "SavorQt.SetupTab";
            request.input_bindings = baseBindings;
            request.arguments = commonArguments;

            for (const auto& rtcValue : rtcLaunchValues) {
                if (hasRtcRange && rtcValue.range) {
                    request.arguments.push_back(savorqt::db::WorkflowGraphArgumentDraft{
                        .node_key = rtcValue.nodeKey.toStdString(),
                        .argument_key = "rtc",
                        .value_type = "integer",
                        .integer_value = rtc,
                        .text_value = std::nullopt,
                        .source_kind = "setup_tab",
                    });
                } else if (rtcValue.hasSingle) {
                    request.arguments.push_back(savorqt::db::WorkflowGraphArgumentDraft{
                        .node_key = rtcValue.nodeKey.toStdString(),
                        .argument_key = "rtc",
                        .value_type = "integer",
                        .integer_value = rtcValue.single,
                        .text_value = std::nullopt,
                        .source_kind = "setup_tab",
                    });
                }
            }

            const auto result = savorqt::db::SavorDbWorkflowService::StartWorkflowGraphRevision(request);
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
                savorqt::gui::ContextDrawerMode::Expanded);
        }
    });
    launchLayout->addWidget(launchButton);
    workbenchLayout->addWidget(launchPanel, 2);

    canvasLayout()->addWidget(workbench, 1);

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
}
