#include "GUI/Tabs/SetupTab.h"

#include "DB/SimCoreDbArtifactService.h"
#include "DB/SimCoreDbAuthoringService.h"
#include "DB/SimCoreDbWorkflowService.h"
#include "GUI/Panes/CoordinatorPane/CoordinatorController.h"
#include "SimCoreDbRuntime.h"

#include <QtCore/QSize>
#include <QtGui/QIcon>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <memory>
#include <optional>
#include <vector>

namespace {

struct InputBindingEditor {
    QString nodeKey;
    QString inputKey;
    QString dataKind;
    QLineEdit* refKindEdit = nullptr;
    QLineEdit* refIdEdit = nullptr;
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
    if (dataKind.contains(QStringLiteral("seed"), Qt::CaseInsensitive)) {
        return QStringLiteral("sp_probe_run");
    }
    return dataKind.isEmpty() ? QStringLiteral("artifact") : dataKind;
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

QPushButton* createRowButton(const QString& primary, const QString& secondary, QWidget* parent)
{
    auto* button = new QPushButton(parent);
    button->setObjectName("jobsSecondaryButton");
    button->setMinimumHeight(42);
    button->setText(primary + QStringLiteral("\n") + secondary);
    return button;
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

QString workflowChainText(const simcore::db::WorkflowGraphSnapshot& graph)
{
    QStringList parts;
    for (const auto& node : graph.nodes) {
        const QString display = node.display_name.empty() ? qs(node.unit_kind) : qs(node.display_name);
        parts << display;
    }
    return parts.isEmpty() ? QStringLiteral("(no nodes)") : parts.join(QStringLiteral(" -> "));
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

    auto* createWorkflowPanel = createSectionPanel(QStringLiteral("Create workflow"), workbench);
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
    createActions->addStretch();
    createLayout->addLayout(createActions);

    auto* existingLabel = new QLabel(QStringLiteral("Existing workflows"), createWorkflowPanel);
    existingLabel->setObjectName("sectionDescription");
    createLayout->addWidget(existingLabel);
    if (!graphs.ok || graphs.value.empty()) {
        auto* empty = new QLabel(graphs.ok ? QStringLiteral("No workflow graphs found.") : qs(graphs.error.message), createWorkflowPanel);
        empty->setObjectName("sectionDescription");
        empty->setWordWrap(true);
        createLayout->addWidget(empty);
    } else {
        int shown = 0;
        for (const auto& graph : graphs.value) {
            if (++shown > 12) {
                break;
            }
            auto* row = createRowButton(
                qs(graph.name),
                QStringLiteral("Graph  |  %1 nodes  |  %2")
                    .arg(static_cast<int>(graph.nodes.size()))
                    .arg(graph.hidden ? QStringLiteral("hidden") : QStringLiteral("visible")),
                createWorkflowPanel);
            row->setContextMenuPolicy(Qt::CustomContextMenu);
            QObject::connect(row, &QPushButton::clicked, createWorkflowPanel, [this, graph]() {
                setContext(
                    { QStringLiteral("setup"), QStringLiteral("workflow_graph"), graph.workflow_graph_id, qs(graph.name) },
                    QStringLiteral("Workflow details"),
                    workflowDetailText(graph),
                    QVector<std::pair<QString, std::function<void()>>>{
                        { QStringLiteral("Edit workflow"), [this, graph]() {
                            if (actions_.openGraphEditorSnapshot) {
                                actions_.openGraphEditorSnapshot(graph, false);
                            }
                        } },
                        { QStringLiteral("Duplicate"), [this, graph]() {
                            if (actions_.openGraphEditorSnapshot) {
                                actions_.openGraphEditorSnapshot(graph, true);
                            }
                        } },
                        { QStringLiteral("Launch with this workflow"), actions_.openLauncher },
                        { QStringLiteral("Open authoring library"), actions_.openAuthoring },
                    },
                    soasimqt2::gui::ContextDrawerMode::Expanded);
            });
            QObject::connect(row, &QPushButton::customContextMenuRequested, createWorkflowPanel, [this, row, graph](const QPoint& pos) {
                QMenu menu(row);
                menu.addAction(QStringLiteral("Edit"), row, [this, graph]() {
                    setContext(
                        { QStringLiteral("setup"), QStringLiteral("workflow_graph"), graph.workflow_graph_id, qs(graph.name) },
                        QStringLiteral("Edit workflow revision"),
                        QStringLiteral("Create a new revision for %1 in the workflow editor.").arg(qs(graph.name)),
                        QVector<std::pair<QString, std::function<void()>>>{
                            { QStringLiteral("Open workflow editor"), [this, graph]() {
                                if (actions_.openGraphEditorSnapshot) {
                                    actions_.openGraphEditorSnapshot(graph, false);
                                }
                            } },
                        },
                        soasimqt2::gui::ContextDrawerMode::Expanded);
                    if (actions_.openGraphEditorSnapshot) {
                        actions_.openGraphEditorSnapshot(graph, false);
                    }
                });
                menu.addAction(QStringLiteral("Duplicate"), row, [this, graph]() {
                    setContext(
                        { QStringLiteral("setup"), QStringLiteral("workflow_graph"), graph.workflow_graph_id, qs(graph.name) },
                        QStringLiteral("Duplicate workflow"),
                        QStringLiteral("Duplicate %1 from the workflow editor.").arg(qs(graph.name)),
                        QVector<std::pair<QString, std::function<void()>>>{
                            { QStringLiteral("Open workflow editor"), [this, graph]() {
                                if (actions_.openGraphEditorSnapshot) {
                                    actions_.openGraphEditorSnapshot(graph, true);
                                }
                            } },
                        },
                        soasimqt2::gui::ContextDrawerMode::Expanded);
                    if (actions_.openGraphEditorSnapshot) {
                        actions_.openGraphEditorSnapshot(graph, true);
                    }
                });
                menu.exec(row->mapToGlobal(pos));
            });
            createLayout->addWidget(row);
        }
    }
    createLayout->addStretch();
    workbenchLayout->addWidget(createWorkflowPanel, 1);

    auto* launchPanel = createSectionPanel(QStringLiteral("Launch workflow"), workbench);
    auto* launchLayout = qobject_cast<QVBoxLayout*>(launchPanel->layout());
    auto* form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(8);

    auto* workflowCombo = new QComboBox(launchPanel);
    if (graphs.ok) {
        for (int i = 0; i < static_cast<int>(graphs.value.size()); ++i) {
            workflowCombo->addItem(qs(graphs.value[i].name), i);
        }
    }
    form->addRow(QStringLiteral("Workflow"), workflowCombo);

    auto* chainPreview = new QLabel(launchPanel);
    chainPreview->setObjectName("sectionDescription");
    chainPreview->setWordWrap(true);
    form->addRow(QStringLiteral("Chain"), chainPreview);

    auto* rtcEdit = new QLineEdit(launchPanel);
    auto* headroomEdit = new QLineEdit(launchPanel);
    auto* samplesEdit = new QLineEdit(launchPanel);
    auto* comboAttemptsEdit = new QLineEdit(launchPanel);
    auto* fakeAttackLowEdit = new QLineEdit(launchPanel);
    auto* fakeAttackHighEdit = new QLineEdit(launchPanel);
    form->addRow(QStringLiteral("rtc"), rtcEdit);
    form->addRow(QStringLiteral("headroom"), headroomEdit);
    form->addRow(QStringLiteral("samples/axis"), samplesEdit);
    form->addRow(QStringLiteral("combo attempts"), comboAttemptsEdit);
    form->addRow(QStringLiteral("fake attack low"), fakeAttackLowEdit);
    form->addRow(QStringLiteral("fake attack high"), fakeAttackHighEdit);
    launchLayout->addLayout(form);

    auto* requiredInputsLabel = new QLabel(QStringLiteral("Required inputs"), launchPanel);
    requiredInputsLabel->setObjectName("sectionDescription");
    launchLayout->addWidget(requiredInputsLabel);
    auto* inputsHost = new QFrame(launchPanel);
    inputsHost->setObjectName("workspaceToolCard");
    auto* inputsLayout = new QFormLayout(inputsHost);
    inputsLayout->setContentsMargins(10, 10, 10, 10);
    inputsLayout->setSpacing(6);
    launchLayout->addWidget(inputsHost);

    auto bindingEditors = std::make_shared<std::vector<InputBindingEditor>>();
    auto refreshLaunchSelection = [graphs, workflowCombo, chainPreview, inputsLayout, inputsHost, bindingEditors]() {
        clearLayout(inputsLayout);
        bindingEditors->clear();
        if (!graphs.ok || workflowCombo->currentIndex() < 0 || workflowCombo->currentIndex() >= workflowCombo->count()) {
            chainPreview->setText(QStringLiteral("(no workflow selected)"));
            return;
        }
        const int graphIndex = workflowCombo->currentData().toInt();
        if (graphIndex < 0 || graphIndex >= static_cast<int>(graphs.value.size())) {
            chainPreview->setText(QStringLiteral("(invalid workflow selection)"));
            return;
        }
        const auto& graph = graphs.value[graphIndex];
        chainPreview->setText(workflowChainText(graph));
        bool hasInput = false;
        for (const auto& node : graph.nodes) {
            for (const auto& input : node.inputs) {
                if (!input.required) {
                    continue;
                }
                hasInput = true;
                auto* rowHost = new QFrame(inputsHost);
                auto* rowLayout = new QHBoxLayout(rowHost);
                rowLayout->setContentsMargins(0, 0, 0, 0);
                rowLayout->setSpacing(6);
                auto* refKindEdit = new QLineEdit(defaultRefKindForDataKind(qs(input.data_kind)), rowHost);
                auto* refIdEdit = new QLineEdit(rowHost);
                refIdEdit->setPlaceholderText(QStringLiteral("ref id"));
                rowLayout->addWidget(refKindEdit, 2);
                rowLayout->addWidget(refIdEdit, 1);
                const QString label = QStringLiteral("%1.%2").arg(qs(node.node_key), qs(input.input_key));
                inputsLayout->addRow(label, rowHost);
                bindingEditors->push_back(InputBindingEditor{
                    qs(node.node_key),
                    qs(input.input_key),
                    qs(input.data_kind),
                    refKindEdit,
                    refIdEdit,
                });
            }
        }
        if (!hasInput) {
            auto* none = new QLabel(QStringLiteral("No external required inputs."), inputsHost);
            none->setObjectName("sectionDescription");
            inputsLayout->addRow(none);
        }
    };
    QObject::connect(workflowCombo, &QComboBox::currentIndexChanged, launchPanel, [refreshLaunchSelection](int) {
        refreshLaunchSelection();
    });
    refreshLaunchSelection();

    auto* launchButton = new QPushButton(QStringLiteral("Launch"), launchPanel);
    launchButton->setObjectName("jobsPrimaryButton");
    QObject::connect(launchButton, &QPushButton::clicked, launchPanel, [=, this]() {
        if (!graphs.ok || workflowCombo->currentIndex() < 0) {
            setContext(
                { QStringLiteral("setup"), QStringLiteral("launch"), 0, QStringLiteral("launch") },
                QStringLiteral("Launch workflow"),
                QStringLiteral("No workflow graph is available to launch."),
                {},
                soasimqt2::gui::ContextDrawerMode::Expanded);
            return;
        }

        const int graphIndex = workflowCombo->currentData().toInt();
        if (graphIndex < 0 || graphIndex >= static_cast<int>(graphs.value.size())) {
            return;
        }
        const auto& graph = graphs.value[graphIndex];
        soasimqt2::db::WorkflowGraphStartRequest request{};
        request.workflow_graph_revision_id = graph.workflow_graph_revision_id;
        request.created_by = "SoaSimQt2.SetupTab";

        for (const auto& editor : *bindingEditors) {
            bool ok = false;
            const auto refId = editor.refIdEdit->text().toLongLong(&ok);
            if (!ok || refId <= 0 || editor.refKindEdit->text().trimmed().isEmpty()) {
                continue;
            }
            request.input_bindings.push_back(soasimqt2::db::WorkflowGraphInputBindingDraft{
                .node_key = editor.nodeKey.toStdString(),
                .input_key = editor.inputKey.toStdString(),
                .data_kind = editor.dataKind.toStdString(),
                .ref_kind = editor.refKindEdit->text().trimmed().toStdString(),
                .ref_id = refId,
                .source_kind = "setup_tab",
            });
        }

        const auto addIntegerArgument = [&request](const char* key, QLineEdit* edit) {
            bool ok = false;
            const auto value = edit->text().trimmed().toLongLong(&ok);
            if (!ok || edit->text().trimmed().isEmpty()) {
                return;
            }
            request.arguments.push_back(soasimqt2::db::WorkflowGraphArgumentDraft{
                .node_key = "",
                .argument_key = key,
                .value_type = "integer",
                .integer_value = value,
                .text_value = std::nullopt,
                .source_kind = "setup_tab",
            });
        };
        addIntegerArgument("rtc", rtcEdit);
        addIntegerArgument("headroom", headroomEdit);
        addIntegerArgument("samples_per_axis", samplesEdit);
        addIntegerArgument("combo_attempts_per_target", comboAttemptsEdit);
        addIntegerArgument("fake_attack_low", fakeAttackLowEdit);
        addIntegerArgument("fake_attack_high", fakeAttackHighEdit);

        const auto result = soasimqt2::db::SimCoreDbWorkflowService::StartWorkflowGraphRevision(request);
        if (result.ok) {
            setContext(
                { QStringLiteral("setup"), QStringLiteral("launch"), result.value, qs(graph.name) },
                QStringLiteral("Workflow launched"),
                QStringLiteral("Started workflow instance #%1 from %2. Continue in Running for activation/job progress.")
                    .arg(result.value)
                    .arg(qs(graph.name)),
                {},
                soasimqt2::gui::ContextDrawerMode::Expanded);
        } else {
            setContext(
                { QStringLiteral("setup"), QStringLiteral("launch"), graph.workflow_graph_id, qs(graph.name) },
                QStringLiteral("Launch blocked"),
                QStringLiteral("%1\n\nCheck required input bindings above, then retry.").arg(qs(result.error.message)),
                QVector<std::pair<QString, std::function<void()>>>{
                    { QStringLiteral("Open full launcher"), actions_.openLauncher },
                },
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
