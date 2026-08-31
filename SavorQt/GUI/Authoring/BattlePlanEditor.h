#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"
#include "Authoring/IAuthoringDb.h"
#include "DB/SavorDbServiceResult.h"
#include "GUI/Refresh/DatabaseProjectionController.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPoint;
class QPushButton;
class QSpinBox;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
class BattlePlanActionPresetEditorWindow;

class BattlePlanEditor final : public QWidget
{
public:
    explicit BattlePlanEditor(QWidget* parent = nullptr);

    void setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback);
    void setSavedCallback(std::function<void()> callback);
    void loadSnapshot(const savor::db::BattlePlanSnapshot& snapshot, bool duplicate);

    struct ActionDraft {
        std::int64_t action_preset_id = 0;
        int actor_slot = 0;
    };

    struct TurnDraft {
        int turn_index = 1;
        int player_combatants = 1;
        std::optional<std::int64_t> predicate_group_revision_id;
        std::vector<ActionDraft> actions;
    };

    struct PlanTreeActionRow {
        int slotIndex = 0;
        int actionIndex = -1;
        QString summary;
    };

    struct PlanTreeTurnRow {
        int turnIndex = 0;
        QString label;
        QString summary;
        std::vector<PlanTreeActionRow> actions;
    };

private:
    void createWidgets();
    void populateActionLibrary();
    void configurePredicateLibraryRefresh();
    void requestPredicateLibraryRefresh();
    void applyPredicateGroupSelection();
    void syncPredicateSelectorToTurn();
    void showPredicateGroupDetail(std::int64_t revisionId);
    void rebuildPlanTree();
    void applyCombatantCountToAllTurns(int combatantCount);
    void addActionFromLibrarySelection();
    void assignPresetToSelection(std::int64_t presetId);
    void addActionToSelectedTurn(std::int64_t presetId, int turnIndex = -1, int slotIndex = -1);
    void assignActionPreset(std::int64_t presetId, int turnIndex, int actionIndex, int slotIndex);
    void clearSelectedSlot();
    void openActionPresetEditorForSelection();
    void openActionPresetEditorForAction();
    void openNewActionPresetEditor();
    void duplicateSelectedAction();
    void removeSelectedNode();
    void moveSelectedAction(int delta);
    void showPlanContextMenu(const QPoint& position);
    void ensureTurnCount(int count);
    void saveBattlePlan();
    void markDirty();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);
    TurnDraft* selectedTurn();
    ActionDraft* selectedAction();
    const TurnDraft* selectedTurn() const;
    const ActionDraft* selectedAction() const;
    int selectedSlotIndex() const;
    const savor::db::BattlePlanActionPresetSnapshot* actionPresetById(std::int64_t presetId) const;
    int findActionIndexBySlot(const TurnDraft& turn, int slotIndex) const;
    void normalizeTurnSlots(TurnDraft& turn) const;
    bool hasValidSlotAssignments(const TurnDraft& turn) const;
    QTreeWidgetItem* selectedTreeItem() const;
    int selectedTurnIndex() const;
    int selectedActionIndex() const;
    static bool isActionItem(const QTreeWidgetItem* item);
    static bool isTurnItem(const QTreeWidgetItem* item);
    std::int64_t selectedPresetIdFromLibrary() const;
    std::int64_t selectedPresetIdFromAction() const;
    void openPresetEditor(std::int64_t presetId, bool duplicate);

    std::function<void(const QString&, StatusToast::Severity)> statusCallback_;
    std::function<void()> savedCallback_;
    bool dirty_ = false;
    bool rebuildingTree_ = false;
    QLineEdit* nameEdit_ = nullptr;
    QPlainTextEdit* descriptionEdit_ = nullptr;
    QSpinBox* turnCountSpin_ = nullptr;
    QListWidget* actionLibraryList_ = nullptr;
    QTreeWidget* planTree_ = nullptr;
    QSpinBox* combatantCountSpin_ = nullptr;
    QComboBox* predicateGroupCombo_ = nullptr;
    QLineEdit* predicateSearchEdit_ = nullptr;
    QCheckBox* includeDraftPredicatesCheck_ = nullptr;
    QListWidget* predicateLibraryList_ = nullptr;
    QLabel* predicateDetailLabel_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    std::vector<savor::db::BattlePlanActionPresetSnapshot> actionPresets_;
    std::vector<savor::db::PredicateGroupRevisionSummary> predicateGroups_;
    std::vector<TurnDraft> turns_;
    std::vector<PlanTreeTurnRow> currentPlanTreeRows_;
    QPointer<BattlePlanActionPresetEditorWindow> presetEditor_;
    struct PredicateRefreshRequest {
        std::optional<std::string> revision_state;
        std::string search_text;
    };
    using PredicatePageResult =
        savorqt::db::ServiceResult<savor::db::PredicateRevisionPageV2<savor::db::PredicateGroupRevisionSummary>>;
    struct PredicateRefreshData {
        PredicatePageResult library;
        PredicatePageResult published;
    };
    savorqt::gui::DatabaseProjectionController<PredicateRefreshRequest, PredicateRefreshData>*
        predicateRefreshPipeline_ = nullptr;
    using PredicateDetailData = savorqt::db::ServiceResult<savor::db::PredicateGroupRevisionSnapshot>;
    savorqt::gui::DatabaseProjectionController<std::int64_t, PredicateDetailData>*
        predicateDetailPipeline_ = nullptr;
};
