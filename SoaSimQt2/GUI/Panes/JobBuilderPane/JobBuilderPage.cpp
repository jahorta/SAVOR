#include "JobBuilderPage.h"

#include "WorkflowGraphEditorWindow.h"
#include "DB/SimCoreDbAuthoringService.h"

#include <QtCore/QDateTime>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

namespace {

QString workflowGraphText(const simcore::db::WorkflowGraphSnapshot& graph)
{
    return QStringLiteral("#%1 r%2  %3\n%4 nodes, %5 edges")
        .arg(static_cast<qint64>(graph.workflow_graph_id))
        .arg(graph.graph_version)
        .arg(QString::fromStdString(graph.name))
        .arg(static_cast<int>(graph.nodes.size()))
        .arg(static_cast<int>(graph.edges.size()));
}

} // namespace

JobBuilderPage::JobBuilderPage(QWidget* parent)
    : QWidget(parent)
{
    createWidgets();
}

void JobBuilderPage::createWidgets()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(10);

    auto* toolbar = new QFrame(this);
    toolbar->setObjectName("jobsToolbarPanel");
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(12, 10, 12, 10);
    toolbarLayout->setSpacing(10);

    newGraphButton_ = new QPushButton(QStringLiteral("New Workflow Graph"), toolbar);
    editGraphButton_ = new QPushButton(QStringLiteral("Edit Graph"), toolbar);
    duplicateGraphButton_ = new QPushButton(QStringLiteral("Duplicate Graph"), toolbar);
    refreshButton_ = new QPushButton(QStringLiteral("Refresh"), toolbar);
    newGraphButton_->setObjectName("jobsPrimaryButton");
    editGraphButton_->setObjectName("jobsSecondaryButton");
    duplicateGraphButton_->setObjectName("jobsSecondaryButton");
    refreshButton_->setObjectName("jobsSecondaryButton");
    toolbarLayout->addWidget(newGraphButton_);
    toolbarLayout->addWidget(editGraphButton_);
    toolbarLayout->addWidget(duplicateGraphButton_);
    toolbarLayout->addWidget(refreshButton_);
    toolbarLayout->addStretch();
    rootLayout->addWidget(toolbar);

    auto* body = new QFrame(this);
    body->setObjectName("jobsSurfacePanel");
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(18, 18, 18, 18);
    bodyLayout->setSpacing(8);
    auto* title = new QLabel(QStringLiteral("Workflow Graph Authoring"), body);
    title->setObjectName("panelTitle");
    auto* detail = new QLabel(
        QStringLiteral("Reusable workflow graphs are edited in modeless windows. Workflow instance launch and external input selection live in a separate workflow launch surface."),
        body);
    detail->setObjectName("sectionDescription");
    detail->setWordWrap(true);
    bodyLayout->addWidget(title);
    bodyLayout->addWidget(detail);
    graphList_ = new QListWidget(body);
    bodyLayout->addWidget(graphList_, 1);
    graphStatusLabel_ = new QLabel(body);
    graphStatusLabel_->setObjectName("sectionDescription");
    bodyLayout->addWidget(graphStatusLabel_);
    rootLayout->addWidget(body, 1);

    connect(newGraphButton_, &QPushButton::clicked, this, &JobBuilderPage::openWorkflowGraphEditor);
    connect(editGraphButton_, &QPushButton::clicked, this, &JobBuilderPage::editSelectedWorkflowGraph);
    connect(duplicateGraphButton_, &QPushButton::clicked, this, &JobBuilderPage::duplicateSelectedWorkflowGraph);
    connect(refreshButton_, &QPushButton::clicked, this, &JobBuilderPage::refreshWorkflowGraphs);
    connect(graphList_, &QListWidget::itemDoubleClicked, this, [this]() { editSelectedWorkflowGraph(); });

    refreshWorkflowGraphs();
}

void JobBuilderPage::openWorkflowGraphEditor()
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

void JobBuilderPage::editSelectedWorkflowGraph()
{
    const int row = graphList_ != nullptr ? graphList_->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(workflowGraphs_.size())) {
        postStatusMessage(QStringLiteral("Select a workflow graph to edit."), StatusToast::Severity::Warn);
        return;
    }
    openWorkflowGraphEditor();
    if (workflowGraphEditor_) {
        workflowGraphEditor_->loadSnapshot(workflowGraphs_[static_cast<std::size_t>(row)], false);
    }
}

void JobBuilderPage::duplicateSelectedWorkflowGraph()
{
    const int row = graphList_ != nullptr ? graphList_->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(workflowGraphs_.size())) {
        postStatusMessage(QStringLiteral("Select a workflow graph to duplicate."), StatusToast::Severity::Warn);
        return;
    }
    openWorkflowGraphEditor();
    if (workflowGraphEditor_) {
        workflowGraphEditor_->loadSnapshot(workflowGraphs_[static_cast<std::size_t>(row)], true);
    }
}

void JobBuilderPage::refreshWorkflowGraphs()
{
    const auto result = soasimqt2::db::SimCoreDbAuthoringService::ListWorkflowGraphs();
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    workflowGraphs_ = result.value;
    graphList_->clear();
    for (const auto& graph : workflowGraphs_) {
        auto* item = new QListWidgetItem(workflowGraphText(graph), graphList_);
        item->setData(Qt::UserRole, static_cast<qint64>(graph.workflow_graph_id));
    }
    graphStatusLabel_->setText(QStringLiteral("%1 workflow graphs").arg(static_cast<int>(workflowGraphs_.size())));
}

void JobBuilderPage::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    if (text.isEmpty()) {
        return;
    }
    emit statusToastRequested(StatusToast{ severity, text, {}, 1, QDateTime{}, 4000 });
}
