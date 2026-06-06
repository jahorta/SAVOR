#pragma once

#include <functional>
#include <vector>

#include <QtWidgets/QWidget>

#include "Execution/Workflow/WorkflowComposition.h"
#include "GUI/Common/StatusToast.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

class WorkflowGraphEditorWindow final : public QWidget
{
public:
    explicit WorkflowGraphEditorWindow(QWidget* parent = nullptr);

    void setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback);

private:
    using WorkflowUnitDefinition = simcore::db::execution::workflow::WorkflowUnitDefinition;
    using WorkflowCompositionNode = simcore::db::execution::workflow::WorkflowCompositionNode;
    using WorkflowUnitOutputBinding = simcore::db::execution::workflow::WorkflowUnitOutputBinding;

    void createWidgets();
    void loadUnits();
    void addSelectedUnit();
    void removeSelectedNode();
    void clearComposition();
    void saveGraph();
    void rebuildBindings();
    void refreshUnitList();
    void refreshCompositionList();
    void refreshPreview();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    const WorkflowUnitDefinition* findUnit(const std::string& unit_kind) const;
    QString describeUnit(const WorkflowUnitDefinition& unit) const;
    QString describeNode(const WorkflowCompositionNode& node) const;

    std::function<void(const QString&, StatusToast::Severity)> statusCallback_;
    std::vector<WorkflowUnitDefinition> units_;
    std::vector<WorkflowCompositionNode> nodes_;
    std::vector<WorkflowUnitOutputBinding> outputBindings_;
    int nextNodeOrdinal_ = 1;

    QLineEdit* nameEdit_ = nullptr;
    QPlainTextEdit* descriptionEdit_ = nullptr;
    QListWidget* unitList_ = nullptr;
    QListWidget* compositionList_ = nullptr;
    QPushButton* addUnitButton_ = nullptr;
    QPushButton* removeNodeButton_ = nullptr;
    QPushButton* clearButton_ = nullptr;
    QPushButton* saveGraphButton_ = nullptr;
    QPlainTextEdit* previewText_ = nullptr;
};
