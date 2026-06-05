#pragma once

#include <vector>

#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"
#include "Execution/Workflow/WorkflowComposition.h"

class QCheckBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

class JobBuilderPage final : public QWidget
{
    Q_OBJECT

public:
    explicit JobBuilderPage(QWidget* parent = nullptr);

signals:
    void statusToastRequested(StatusToast toast);

private:
    using WorkflowUnitDefinition = simcore::db::execution::workflow::WorkflowUnitDefinition;
    using WorkflowCompositionNode = simcore::db::execution::workflow::WorkflowCompositionNode;
    using WorkflowExternalInputBinding = simcore::db::execution::workflow::WorkflowExternalInputBinding;
    using WorkflowUnitOutputBinding = simcore::db::execution::workflow::WorkflowUnitOutputBinding;

    void createWidgets();
    void loadUnits();
    void addSelectedUnit();
    void removeSelectedNode();
    void clearComposition();
    void rebuildBindings();
    void refreshUnitList();
    void refreshCompositionList();
    void refreshPreview();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    const WorkflowUnitDefinition* findUnit(const std::string& unit_kind) const;
    QString describeUnit(const WorkflowUnitDefinition& unit) const;
    QString describeNode(const WorkflowCompositionNode& node) const;
    bool externalInputEnabled(const std::string& data_kind) const;
    qint64 externalInputRefId(const std::string& data_kind) const;

    std::vector<WorkflowUnitDefinition> units_;
    std::vector<WorkflowCompositionNode> nodes_;
    std::vector<WorkflowExternalInputBinding> externalInputs_;
    std::vector<WorkflowUnitOutputBinding> outputBindings_;
    int nextNodeOrdinal_ = 1;

    QListWidget* unitList_ = nullptr;
    QListWidget* compositionList_ = nullptr;
    QCheckBox* externalSavestateCheck_ = nullptr;
    QCheckBox* externalDtmCheck_ = nullptr;
    QCheckBox* externalInputFramesCheck_ = nullptr;
    QPushButton* addUnitButton_ = nullptr;
    QPushButton* removeNodeButton_ = nullptr;
    QPushButton* clearButton_ = nullptr;
    QPlainTextEdit* previewText_ = nullptr;
};
