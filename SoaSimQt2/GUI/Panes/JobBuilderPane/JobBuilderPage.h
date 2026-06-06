#pragma once

#include <QtCore/QPointer>
#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"

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
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    QPushButton* newGraphButton_ = nullptr;
    QPointer<WorkflowGraphEditorWindow> workflowGraphEditor_;
};
