#pragma once

#include <optional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <QtCore/QPointer>
#include <QtWidgets/QWidget>

#include "Authoring/IAuthoringDb.h"
#include "Execution/Workflow/WorkflowComposition.h"
#include "GUI/Common/StatusToast.h"
#include "GUI/Refresh/AsyncRefreshPipeline.h"
#include "DB/SavorDbServiceResult.h"
#include "DB/WorkflowReferenceSelectorProvider.h"
#include "DB/SavorDbWorkflowService.h"

class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
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
    void preselectStandaloneInput(const QString& unit_kind, const QString& input_key, qint64 ref_id);

signals:
    void statusToastRequested(StatusToast toast);

private:
    struct ExternalInputRow {
        QString node_key;
        QString node_name;
        QString input_key;
        QString display_name;
        QString data_kind;
        QString ref_kind;
        bool required = true;
        bool satisfied_by_edge = false;
        QString edge_evidence;
        QString presentation_family_key;
    };
    struct AuthoredRefOption {
        QString label;
        QString description;
        std::string ref_kind;
        std::int64_t ref_id = 0;
    };
    struct ExternalInputDraft {
        QString refKind;
        QString refId;
        QString memberUnitKind;
    };
    struct LauncherDraft {
        QString rtcLow;
        QString rtcHigh;
        int seedSamplesPerAxis = 5;
        int battleFakeMin = 0;
        int battleFakeMax = 0;
        QString continuationMode;
        bool continueAfterVictory = false;
        qint64 authoredRefId = 0;
        std::map<QString, ExternalInputDraft> externalInputs;
    };
    using WorkflowUnitDefinition = savor::db::execution::workflow::WorkflowUnitDefinition;
    using StandaloneLaunchEntry = savorqt::db::WorkflowStandaloneLaunchEntry;
    using WorkflowGraphListResult = savorqt::db::ServiceResult<std::vector<savor::db::WorkflowGraphSnapshot>>;
    using StandaloneLaunchEntryListResult = savorqt::db::ServiceResult<std::vector<StandaloneLaunchEntry>>;
    using ReferenceOptions = std::map<QString, std::vector<savorqt::db::WorkflowReferenceOption>>;
    using ReferenceOptionsResult = savorqt::db::ServiceResult<ReferenceOptions>;

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
    void populateExternalInputsForStandaloneEntry(const StandaloneLaunchEntry& entry);
    void handleStandaloneSourceSelectionChanged(int row);
    void updateStandaloneArgumentControls();
    void applyExternalInputs(std::vector<ExternalInputRow> rows);
    void refreshExternalInputOptions();
    void refreshAuthoredRefsForStandaloneUnit(const WorkflowUnitDefinition& unit);
    std::optional<savor::db::WorkflowGraphSnapshot> selectedGraph() const;
    const WorkflowUnitDefinition* selectedUnit() const;
    const StandaloneLaunchEntry* selectedStandaloneEntry() const;
    bool standaloneModeActive() const;
    QString currentDraftKey() const;
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    static QString externalInputKey(const ExternalInputRow& input);
    static QString graphLaunchShapeSignature(const savor::db::WorkflowGraphSnapshot& graph);
    static QString unitLaunchShapeSignature(const WorkflowUnitDefinition& unit);
    static QString standaloneEntryShapeSignature(const StandaloneLaunchEntry& entry);
    static QString graphListText(const savor::db::WorkflowGraphSnapshot& graph);
    static QString nodeDisplayName(const savor::db::WorkflowGraphSnapshot& graph, const std::string& node_key);
    static QString unitListText(const WorkflowUnitDefinition& unit);
    static std::string standaloneNodeKey(const WorkflowUnitDefinition& unit);
    static std::vector<QString> argumentNodeKeys(const savor::db::WorkflowGraphSnapshot& graph, std::string_view argument_key);

    QTabWidget* launchModeTabs_ = nullptr;
    QListWidget* graphList_ = nullptr;
    QListWidget* unitList_ = nullptr;
    QLabel* authoredRefLabel_ = nullptr;
    QComboBox* authoredRefCombo_ = nullptr;
    QLabel* rtcRangeLabel_ = nullptr;
    QFrame* rtcRangePanel_ = nullptr;
    QLineEdit* rtcLowEdit_ = nullptr;
    QLineEdit* rtcHighEdit_ = nullptr;
    QLabel* seedSamplesLabel_ = nullptr;
    QSpinBox* seedSamplesSpin_ = nullptr;
    QLabel* battleFakeRangeLabel_ = nullptr;
    QFrame* battleFakeRangePanel_ = nullptr;
    QSpinBox* battleFakeMinSpin_ = nullptr;
    QSpinBox* battleFakeMaxSpin_ = nullptr;
    QLabel* continuationLabel_ = nullptr;
    QComboBox* continuationCombo_ = nullptr;
    QCheckBox* continueAfterVictoryCheck_ = nullptr;
    QTableWidget* externalInputsTable_ = nullptr;
    QLabel* graphDetailLabel_ = nullptr;
    QLabel* launchStatusLabel_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* editGraphsButton_ = nullptr;
    QPushButton* launchButton_ = nullptr;

    QPointer<WorkflowGraphEditorWindow> workflowGraphEditor_;
    savorqt::gui::AsyncRefreshPipeline<int, WorkflowGraphListResult>* graphRefreshPipeline_ = nullptr;
    savorqt::gui::AsyncRefreshPipeline<int, StandaloneLaunchEntryListResult>* unitRefreshPipeline_ = nullptr;
    savorqt::gui::AsyncRefreshPipeline<std::vector<ExternalInputRow>, ReferenceOptionsResult>* referenceRefreshPipeline_ = nullptr;
    std::map<QString, LauncherDraft> launchDrafts_;
    QString renderedTargetKey_;
    QString renderedTargetShape_;
    std::vector<savor::db::WorkflowGraphSnapshot> workflowGraphs_;
    std::vector<StandaloneLaunchEntry> standaloneLaunchEntries_;
    std::vector<AuthoredRefOption> authoredRefOptions_;
    std::vector<ExternalInputRow> externalInputs_;
    std::vector<ExternalInputRow> currentExternalInputRows_;
    QString pendingUnitKind_;
    QString activeStandaloneMemberUnitKind_;
};
