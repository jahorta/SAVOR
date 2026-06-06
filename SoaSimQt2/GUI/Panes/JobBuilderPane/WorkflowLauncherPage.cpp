#include "WorkflowLauncherPage.h"

#include "DB/SimCoreDbAuthoringService.h"
#include "DB/SimCoreDbWorkflowService.h"

#include <QtCore/QDateTime>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QVBoxLayout>

#include <set>

WorkflowLauncherPage::WorkflowLauncherPage(QWidget* parent)
    : QWidget(parent)
{
    createWidgets();
    refreshWorkflowGraphs();
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
    launchButton_ = new QPushButton(QStringLiteral("Launch Instance"), toolbar);
    refreshButton_->setObjectName("jobsSecondaryButton");
    launchButton_->setObjectName("jobsPrimaryButton");
    toolbarLayout->addWidget(refreshButton_);
    toolbarLayout->addStretch();
    toolbarLayout->addWidget(launchButton_);
    rootLayout->addWidget(toolbar);

    auto* body = new QFrame(this);
    body->setObjectName("jobsSurfacePanel");
    auto* bodyLayout = new QGridLayout(body);
    bodyLayout->setContentsMargins(18, 18, 18, 18);
    bodyLayout->setHorizontalSpacing(12);
    bodyLayout->setVerticalSpacing(10);

    auto* graphTitle = new QLabel(QStringLiteral("Workflow Graphs"), body);
    graphTitle->setObjectName("panelTitle");
    graphList_ = new QListWidget(body);
    graphList_->setSelectionMode(QAbstractItemView::SingleSelection);
    bodyLayout->addWidget(graphTitle, 0, 0);
    bodyLayout->addWidget(graphList_, 1, 0, 4, 1);

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
    form->addRow(QStringLiteral("Root scope kind"), rootScopeKindEdit_);
    form->addRow(QStringLiteral("Root scope id"), rootScopeIdEdit_);
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

    connect(refreshButton_, &QPushButton::clicked, this, &WorkflowLauncherPage::refreshWorkflowGraphs);
    connect(launchButton_, &QPushButton::clicked, this, &WorkflowLauncherPage::launchSelectedGraph);
    connect(graphList_, &QListWidget::currentRowChanged, this, &WorkflowLauncherPage::handleGraphSelectionChanged);
}

void WorkflowLauncherPage::refreshWorkflowGraphs()
{
    const auto result = soasimqt2::db::SimCoreDbAuthoringService::ListWorkflowGraphs();
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }

    workflowGraphs_ = result.value;
    graphList_->clear();
    for (const auto& graph : workflowGraphs_) {
        auto* item = new QListWidgetItem(graphListText(graph), graphList_);
        item->setData(Qt::UserRole, static_cast<qint64>(graph.workflow_graph_revision_id));
    }
    if (!workflowGraphs_.empty()) {
        graphList_->setCurrentRow(0);
    } else {
        handleGraphSelectionChanged();
    }
}

void WorkflowLauncherPage::handleGraphSelectionChanged()
{
    const auto graph = selectedGraph();
    if (!graph.has_value()) {
        externalInputs_.clear();
        externalInputsTable_->setRowCount(0);
        graphDetailLabel_->setText(QStringLiteral("No workflow graph selected."));
        launchStatusLabel_->setText(QString());
        launchButton_->setEnabled(false);
        return;
    }

    populateExternalInputs(*graph);
    graphDetailLabel_->setText(QStringLiteral("%1 nodes, %2 edges, revision %3")
        .arg(static_cast<int>(graph->nodes.size()))
        .arg(static_cast<int>(graph->edges.size()))
        .arg(static_cast<qint64>(graph->workflow_graph_revision_id)));
    launchStatusLabel_->setText(externalInputs_.empty()
        ? QStringLiteral("All required inputs are supplied by graph edges.")
        : QStringLiteral("Provide one external reference for each required input below."));
    launchButton_->setEnabled(true);
}

void WorkflowLauncherPage::launchSelectedGraph()
{
    const auto graph = selectedGraph();
    if (!graph.has_value()) {
        postStatusMessage(QStringLiteral("Select a workflow graph to launch."), StatusToast::Severity::Warn);
        return;
    }

    soasimqt2::db::WorkflowGraphStartRequest request{};
    request.workflow_graph_revision_id = graph->workflow_graph_revision_id;
    request.root_scope_kind = rootScopeKindEdit_->text().trimmed().isEmpty()
        ? "manual"
        : rootScopeKindEdit_->text().trimmed().toStdString();

    if (!rootScopeIdEdit_->text().trimmed().isEmpty()) {
        bool ok = false;
        const auto rootScopeId = rootScopeIdEdit_->text().trimmed().toLongLong(&ok, 0);
        if (!ok || rootScopeId <= 0) {
            postStatusMessage(QStringLiteral("Root scope id must be a positive number."), StatusToast::Severity::Warn);
            return;
        }
        request.root_scope_id = rootScopeId;
    }

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
        request.input_bindings.push_back(soasimqt2::db::WorkflowGraphInputBindingDraft{
            .node_key = input.node_key.toStdString(),
            .input_key = input.input_key.toStdString(),
            .data_kind = input.data_kind.toStdString(),
            .ref_kind = refKindText.toStdString(),
            .ref_id = refId,
            .source_kind = "external",
        });
    }

    launchButton_->setEnabled(false);
    const auto result = soasimqt2::db::SimCoreDbWorkflowService::StartWorkflowGraphRevision(request);
    launchButton_->setEnabled(true);
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }

    launchStatusLabel_->setText(QStringLiteral("Launched workflow instance %1.").arg(static_cast<qint64>(result.value)));
    postStatusMessage(QStringLiteral("Launched workflow instance %1.").arg(static_cast<qint64>(result.value)), StatusToast::Severity::Info);
}

void WorkflowLauncherPage::populateExternalInputs(const simcore::db::WorkflowGraphSnapshot& graph)
{
    std::set<QString> suppliedByEdge;
    for (const auto& edge : graph.edges) {
        suppliedByEdge.emplace(QString::fromStdString(edge.to_node_key + "\n" + edge.input_key));
    }

    externalInputs_.clear();
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
            externalInputs_.push_back(ExternalInputRow{
                .node_key = QString::fromStdString(node.node_key),
                .node_name = nodeDisplayName(graph, node.node_key),
                .input_key = QString::fromStdString(input.input_key),
                .display_name = QString::fromStdString(input.display_name),
                .data_kind = dataKind,
                .default_ref_kind = defaultRefKindForDataKind(dataKind),
            });
        }
    }

    externalInputsTable_->setRowCount(0);
    for (const auto& input : externalInputs_) {
        const int row = externalInputsTable_->rowCount();
        externalInputsTable_->insertRow(row);
        externalInputsTable_->setItem(row, 0, new QTableWidgetItem(input.node_name));
        externalInputsTable_->setItem(row, 1, new QTableWidgetItem(input.display_name.isEmpty() ? input.input_key : input.display_name));
        externalInputsTable_->setItem(row, 2, new QTableWidgetItem(input.data_kind));
        externalInputsTable_->setItem(row, 3, new QTableWidgetItem(input.default_ref_kind));
        externalInputsTable_->setItem(row, 4, new QTableWidgetItem(QString()));
        for (int column = 0; column < 3; ++column) {
            externalInputsTable_->item(row, column)->setFlags(externalInputsTable_->item(row, column)->flags() & ~Qt::ItemIsEditable);
        }
    }
}

std::optional<simcore::db::WorkflowGraphSnapshot> WorkflowLauncherPage::selectedGraph() const
{
    const int row = graphList_ != nullptr ? graphList_->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(workflowGraphs_.size())) {
        return std::nullopt;
    }
    return workflowGraphs_[static_cast<std::size_t>(row)];
}

void WorkflowLauncherPage::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    if (text.isEmpty()) {
        return;
    }
    emit statusToastRequested(StatusToast{ severity, text, {}, 1, QDateTime{}, 4000 });
}

QString WorkflowLauncherPage::graphListText(const simcore::db::WorkflowGraphSnapshot& graph)
{
    return QStringLiteral("#%1 r%2  %3\n%4 nodes, %5 edges")
        .arg(static_cast<qint64>(graph.workflow_graph_id))
        .arg(graph.graph_version)
        .arg(QString::fromStdString(graph.name))
        .arg(static_cast<int>(graph.nodes.size()))
        .arg(static_cast<int>(graph.edges.size()));
}

QString WorkflowLauncherPage::nodeDisplayName(
    const simcore::db::WorkflowGraphSnapshot& graph,
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
        return QStringLiteral("sp_probe_run");
    }
    if (data_kind == QStringLiteral("analysis.battle_followup_id")) {
        return QStringLiteral("analysis.battle_followup");
    }
    return data_kind;
}
