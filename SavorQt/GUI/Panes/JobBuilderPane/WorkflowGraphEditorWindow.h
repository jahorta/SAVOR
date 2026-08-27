#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Authoring/IAuthoringDb.h"
#include "Execution/Workflow/WorkflowComposition.h"
#include "GUI/Common/StatusToast.h"
#include "GUI/Widgets/PersistentToolWindow.h"

class QCloseEvent;
class QLabel;
class QCheckBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

class WorkflowGraphEditorWindow final : public PersistentToolWindow
{
public:
    explicit WorkflowGraphEditorWindow(QWidget* parent = nullptr);

    void setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback);
    void setSavedCallback(std::function<void()> callback);
    void loadSnapshot(const savor::db::WorkflowGraphSnapshot& snapshot, bool duplicate);

private:
    using WorkflowUnitDefinition = savor::db::execution::workflow::WorkflowUnitDefinition;
    using WorkflowCompositionNode = savor::db::execution::workflow::WorkflowCompositionNode;
    using WorkflowUnitOutputBinding = savor::db::execution::workflow::WorkflowUnitOutputBinding;
    struct AuthoredRefOption {
        QString label;
        QString description;
        std::string ref_kind;
        std::int64_t ref_id = 0;
    };

    void closeEvent(QCloseEvent* event) override;
    void createWidgets();
    void loadUnits();
    void refreshWorkflowGraphs();
    void newGraph();
    void loadSelectedGraph(bool duplicate);
    void setSelectedGraphHidden(bool hidden);
    void addSelectedUnit();
    void removeSelectedNode();
    void clearComposition();
    void saveGraph();
    void rebuildBindings();
    void refreshUnitList();
    void refreshCompositionList();
    void refreshNodeSettings();
    void loadAuthoredRefOptionsForSelectedNode();
    void applySelectedAuthoredRef();
    void clearSelectedAuthoredRef();
    void refreshPreview();
    void markDirty();
    bool confirmDiscardIfDirty();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    const WorkflowUnitDefinition* findUnit(const std::string& unit_kind) const;
    QString describeUnit(const WorkflowUnitDefinition& unit) const;
    QString describeNode(const WorkflowCompositionNode& node) const;
    std::optional<std::string> requiredAuthoredRefKindForUnit(const std::string& unit_kind) const;

    std::function<void(const QString&, StatusToast::Severity)> statusCallback_;
    std::function<void()> savedCallback_;
    std::vector<savor::db::WorkflowGraphSnapshot> workflowGraphs_;
    std::vector<WorkflowUnitDefinition> units_;
    std::vector<WorkflowCompositionNode> nodes_;
    std::vector<WorkflowUnitOutputBinding> outputBindings_;
    std::vector<savor::db::SaveWorkflowGraphEdgeCommand> controlDependencies_;
    std::vector<AuthoredRefOption> authoredRefOptions_;
    std::unordered_map<std::string, std::pair<std::optional<std::string>, std::optional<std::int64_t>>> authoredRefsByNode_;
    std::optional<std::int64_t> workflowGraphId_;
    std::optional<std::int64_t> parentRevisionId_;
    std::string executionShape_ = "WORKFLOW";
    std::string expansionKind_;
    int nextNodeOrdinal_ = 1;
    bool dirty_ = false;

    QListWidget* graphList_ = nullptr;
    QLabel* graphStatusLabel_ = nullptr;
    QCheckBox* showHiddenGraphsCheck_ = nullptr;
    QPushButton* newGraphButton_ = nullptr;
    QPushButton* editGraphButton_ = nullptr;
    QPushButton* duplicateGraphButton_ = nullptr;
    QPushButton* hideGraphButton_ = nullptr;
    QPushButton* unhideGraphButton_ = nullptr;
    QPushButton* refreshGraphsButton_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QPlainTextEdit* descriptionEdit_ = nullptr;
    QListWidget* unitList_ = nullptr;
    QListWidget* compositionList_ = nullptr;
    QLabel* nodeSettingsLabel_ = nullptr;
    QListWidget* authoredRefList_ = nullptr;
    QPushButton* refreshRefsButton_ = nullptr;
    QPushButton* assignRefButton_ = nullptr;
    QPushButton* clearRefButton_ = nullptr;
    QPushButton* addUnitButton_ = nullptr;
    QPushButton* removeNodeButton_ = nullptr;
    QPushButton* clearButton_ = nullptr;
    QPushButton* saveGraphButton_ = nullptr;
    QPlainTextEdit* previewText_ = nullptr;
};
