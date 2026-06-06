#include "JobBuilderPage.h"

#include "WorkflowGraphEditorWindow.h"

#include <QtCore/QDateTime>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

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
    newGraphButton_->setObjectName("jobsPrimaryButton");
    toolbarLayout->addWidget(newGraphButton_);
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
    bodyLayout->addStretch();
    rootLayout->addWidget(body, 1);

    connect(newGraphButton_, &QPushButton::clicked, this, &JobBuilderPage::openWorkflowGraphEditor);
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
    connect(editor, &QObject::destroyed, this, [this]() { workflowGraphEditor_.clear(); });
    editor->show();
}

void JobBuilderPage::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    if (text.isEmpty()) {
        return;
    }
    emit statusToastRequested(StatusToast{ severity, text, {}, 1, QDateTime{}, 4000 });
}
