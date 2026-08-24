#include "GUI/Tabs/SetupTab.h"

#include "DB/SavorDbArtifactService.h"
#include "DB/SavorDbAuthoringService.h"
#include "DB/SavorDbWorkflowService.h"
#include "GUI/Panes/CoordinatorPane/CoordinatorController.h"
#include "GUI/Panes/JobBuilderPane/WorkflowLauncherPage.h"
#include "GUI/Refresh/AsyncRefreshPipeline.h"
#include "GUI/Refresh/RowUpdate.h"
#include "SavorDbRuntime.h"

#include <QtCore/QSize>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtGui/QIcon>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr int kWorkflowGraphIndexRole = Qt::UserRole + 1;

struct WorkflowGraphViewRow {
    QString name;
    QString nodeCount;
    QString revision;
    qint64 graphId = -1;
    int graphIndex = -1;
};

struct WorkflowGraphRefreshRequest {
    bool showHidden = false;
};

struct WorkflowGraphRefreshData {
    std::vector<savor::db::WorkflowGraphSnapshot> graphs;
    std::vector<WorkflowGraphViewRow> rows;
    int visibleCount = 0;
    int hiddenCount = 0;
};

QString qs(const std::string& value)
{
    return QString::fromStdString(value);
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
        && lhs.nodeCount == rhs.nodeCount
        && lhs.revision == rhs.revision
        && lhs.graphId == rhs.graphId
        && lhs.graphIndex == rhs.graphIndex;
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
        data.rows.push_back(WorkflowGraphViewRow{
            name,
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
    if (*refKind == "authoring.battle_plan") {
        const auto plan = savorqt::db::SavorDbAuthoringService::GetBattlePlan(id);
        if (!plan.ok) {
            return QStringLiteral("Battle Plan #%1 unavailable: %2").arg(id).arg(qs(plan.error.message));
        }
        std::size_t actionCount = 0;
        for (const auto& turn : plan.value.turns) actionCount += turn.actions.size();
        return QStringLiteral("Battle Plan: %1 (#%2)\n  description: %3\n  turns: %4\n  actions: %5\n  fingerprint: %6")
            .arg(qs(plan.value.name))
            .arg(id)
            .arg(qs(plan.value.description))
            .arg(static_cast<int>(plan.value.turns.size()))
            .arg(static_cast<qulonglong>(actionCount))
            .arg(qs(plan.value.fingerprint));
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

} // namespace

SetupTab::SetupTab(CoordinatorController* coordinatorController, QWidget* parent)
    : WorkspacePageShell(
        QStringLiteral("setup"),
        QStringLiteral("Setup"),
        QStringLiteral("Create workflow definitions and instantiate launch-ready workflow groups."),
        parent)
    , coordinatorController_(coordinatorController)
{
    build();
}

void SetupTab::build()
{
    setPageVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    const bool dbReady = savorqt::SavorDbRuntime::instance().isRunning();
    const bool artifactStorageReady = savorqt::db::SavorDbArtifactService::StorageReady();
    const auto graphs = savorqt::db::SavorDbAuthoringService::ListWorkflowGraphs(100, true);
    const auto units = savorqt::db::SavorDbWorkflowService::ListComposableWorkflowUnits();

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
		emit openIsoSettingsRequested();
    });
    statusLayout->addWidget(isoSetupButton);

    auto* dolphinSetupButton = createSetupWarningButton(QStringLiteral("Set Dolphin base"), statusStrip);
    QObject::connect(dolphinSetupButton, &QPushButton::clicked, statusStrip, [this]() {
		emit openDolphinSettingsRequested();
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
            emit openSettingsRequested();
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
        emit openGraphEditorRequested();
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

    auto* sharedLaunchPanel = new WorkflowLauncherPage(workbench);
    connect(sharedLaunchPanel, &WorkflowLauncherPage::statusToastRequested, this, &SetupTab::statusToastRequested);
    workbenchLayout->addWidget(sharedLaunchPanel, 2);

    auto workflowRows = std::make_shared<std::vector<WorkflowGraphViewRow>>();
    auto workflowRefreshPipeline =
        new savorqt::gui::AsyncRefreshPipeline<WorkflowGraphRefreshRequest, WorkflowGraphRefreshData>(createWorkflowPanel);
    auto kickWorkflowRefresh = std::make_shared<std::function<void()>>();

    workflowRefreshPipeline->setRequestBuilder([showHiddenWorkflowsCheck](savorqt::gui::RefreshReason) {
        return WorkflowGraphRefreshRequest{
            showHiddenWorkflowsCheck->isChecked(),
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
        *workflowGraphs = data.graphs;
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
                    emit openGraphEditorSnapshotRequested(graph, false);
                } },
                { QStringLiteral("Duplicate"), [this, graph]() {
                    emit openGraphEditorSnapshotRequested(graph, true);
                } },
                { QStringLiteral("Open authoring library"),[this]() {
                    emit openAuthoringRequested(); 
                } },
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
            emit openGraphEditorSnapshotRequested(graph, false);
        });
        menu.addAction(QStringLiteral("Duplicate"), workflowTable, [this, graph]() {
            emit openGraphEditorSnapshotRequested(graph, true);
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
        { QStringLiteral("Authoring Libraries"), [this](){
            emit openAuthoringRequested();
        } },
        { QStringLiteral("Artifacts"), [this]() {
            emit openArtifactsRequested();
        } },
        { QStringLiteral("DTM Editor"), [this]() {
            emit openDtmEditorRequested();
        } },
        { QStringLiteral("Environment"), [this]() {
            emit openSettingsRequested();
        } },
        { QStringLiteral("Battle Settings"), [this]() {
            emit openBattleSettingsRequested();
        } },
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
