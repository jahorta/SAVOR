#pragma once

#include <vector>

#include <QtCore/QPointer>
#include <QtWidgets/QWidget>

#include "Authoring/IAuthoringDb.h"
#include "GUI/Common/StatusToast.h"

class QLabel;
class QListWidget;
class QPushButton;
class WorkflowGraphEditorWindow;

class JobBuilderPage final : public QWidget
{
    Q_OBJECT

public:
    explicit JobBuilderPage(QWidget* parent = nullptr);

signals:
    void statusToastRequested(StatusToast toast);

private:
    void createWidgets();
    void openWorkflowGraphEditor();
    void editSelectedWorkflowGraph();
    void duplicateSelectedWorkflowGraph();
    void refreshWorkflowGraphs();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    QPushButton* newGraphButton_ = nullptr;
    QPushButton* editGraphButton_ = nullptr;
    QPushButton* duplicateGraphButton_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QListWidget* graphList_ = nullptr;
    QLabel* graphStatusLabel_ = nullptr;
    std::vector<simcore::db::WorkflowGraphSnapshot> workflowGraphs_;
    QPointer<WorkflowGraphEditorWindow> workflowGraphEditor_;
};
