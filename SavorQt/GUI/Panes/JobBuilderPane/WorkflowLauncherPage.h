#pragma once

#include <optional>
#include <map>
#include <string>
#include <vector>

#include <QtCore/QPointer>
#include <QtWidgets/QWidget>

#include "Authoring/IAuthoringDb.h"
#include "Execution/Workflow/WorkflowComposition.h"
#include "GUI/Common/StatusToast.h"
#include "GUI/Refresh/AsyncRefreshPipeline.h"
#include "DB/SavorDbServiceResult.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QCheckBox;
class QFrame;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class WorkflowGraphEditorWindow;

class WorkflowLauncherPage final : public QWidget
{
    Q_OBJECT

public:
    explicit WorkflowLauncherPage(QWidget* parent = nullptr);

signals:
    void statusToastRequested(StatusToast toast);

private:
    struct ExternalInputRow {
        QString node_key;
        QString node_name;
        QString input_key;
        QString display_name;
        QString data_kind;
        QString default_ref_kind;
    };
    struct AuthoredRefOption {
        QString label;
        std::string ref_kind;
        std::int64_t ref_id = 0;
    };
    struct ExternalInputDraft {
        QString refKind;
        QString refId;
    };
    struct LauncherDraft {
        QString rootScopeKind;
        QString rootScopeId;
        QString rtcLow;
        QString rtcHigh;
        int seedSamplesPerAxis = 5;
        bool battleFakeOverride = false;
        int battleFakeMin = 0;
        int battleFakeMax = 0;
        qint64 authoredRefId = 0;
        std::map<QString, ExternalInputDraft> externalInputs;
    };
    using WorkflowUnitDefinition = savor::db::execution::workflow::WorkflowUnitDefinition;
    using WorkflowGraphListResult = savorqt::db::ServiceResult<std::vector<savor::db::WorkflowGraphSnapshot>>;
    using WorkflowUnitListResult = savorqt::db::ServiceResult<std::vector<WorkflowUnitDefinition>>;

    void createWidgets();
    void refreshWorkflowGraphs();
    void refreshStandaloneUnits();
    void openWorkflowGraphEditor();
    void handleLaunchModeChanged();
    void handleGraphSelectionChanged();
    void handleStandaloneUnitSelectionChanged();
    void renderCurrentGraphIfNeeded(bool forceRebuild = false);
    void renderCurrentUnitIfNeeded(bool forceRebuild = false);
    void captureLauncherDraft();
    void restoreLauncherDraft();
    void launchSelectedGraph();
    void launchStandaloneUnit();
    void populateExternalInputs(const savor::db::WorkflowGraphSnapshot& graph);
    void populateExternalInputsForUnit(const WorkflowUnitDefinition& unit);
    void applyExternalInputs(std::vector<ExternalInputRow> rows);
    void refreshAuthoredRefsForStandaloneUnit(const WorkflowUnitDefinition& unit);
    std::optional<savor::db::WorkflowGraphSnapshot> selectedGraph() const;
    const WorkflowUnitDefinition* selectedUnit() const;
    bool standaloneModeActive() const;
    QString currentDraftKey() const;
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    static QString externalInputKey(const ExternalInputRow& input);
    static QString graphLaunchShapeSignature(const savor::db::WorkflowGraphSnapshot& graph);
    static QString unitLaunchShapeSignature(const WorkflowUnitDefinition& unit);
    static QString graphListText(const savor::db::WorkflowGraphSnapshot& graph);
    static QString nodeDisplayName(const savor::db::WorkflowGraphSnapshot& graph, const std::string& node_key);
    static QString defaultRefKindForDataKind(const QString& data_kind);
    static QString unitListText(const WorkflowUnitDefinition& unit);
    static std::string standaloneNodeKey(const WorkflowUnitDefinition& unit);
    static std::vector<QString> tasMovieNodeKeys(const savor::db::WorkflowGraphSnapshot& graph);
    static std::vector<QString> seedProbeNodeKeys(const savor::db::WorkflowGraphSnapshot& graph);
    static std::vector<QString> battleChainNodeKeys(const savor::db::WorkflowGraphSnapshot& graph);

    QTabWidget* launchModeTabs_ = nullptr;
    QListWidget* graphList_ = nullptr;
    QListWidget* unitList_ = nullptr;
    QLineEdit* rootScopeKindEdit_ = nullptr;
    QLineEdit* rootScopeIdEdit_ = nullptr;
    QLabel* authoredRefLabel_ = nullptr;
    QComboBox* authoredRefCombo_ = nullptr;
    QLabel* rtcRangeLabel_ = nullptr;
    QFrame* rtcRangePanel_ = nullptr;
    QLineEdit* rtcLowEdit_ = nullptr;
    QLineEdit* rtcHighEdit_ = nullptr;
    QLabel* seedSamplesLabel_ = nullptr;
    QSpinBox* seedSamplesSpin_ = nullptr;
    QCheckBox* battleFakeOverrideCheck_ = nullptr;
    QLabel* battleFakeRangeLabel_ = nullptr;
    QFrame* battleFakeRangePanel_ = nullptr;
    QSpinBox* battleFakeMinSpin_ = nullptr;
    QSpinBox* battleFakeMaxSpin_ = nullptr;
    QTableWidget* externalInputsTable_ = nullptr;
    QLabel* graphDetailLabel_ = nullptr;
    QLabel* launchStatusLabel_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* editGraphsButton_ = nullptr;
    QPushButton* launchButton_ = nullptr;

    QPointer<WorkflowGraphEditorWindow> workflowGraphEditor_;
    savorqt::gui::AsyncRefreshPipeline<int, WorkflowGraphListResult>* graphRefreshPipeline_ = nullptr;
    savorqt::gui::AsyncRefreshPipeline<int, WorkflowUnitListResult>* unitRefreshPipeline_ = nullptr;
    std::map<QString, LauncherDraft> launchDrafts_;
    QString renderedTargetKey_;
    QString renderedTargetShape_;
    std::vector<savor::db::WorkflowGraphSnapshot> workflowGraphs_;
    std::vector<WorkflowUnitDefinition> workflowUnits_;
    std::vector<AuthoredRefOption> authoredRefOptions_;
    std::vector<ExternalInputRow> externalInputs_;
    std::vector<ExternalInputRow> currentExternalInputRows_;
};
