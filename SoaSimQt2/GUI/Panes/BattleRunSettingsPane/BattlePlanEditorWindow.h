#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"
#include "Authoring/IAuthoringDb.h"

class QCloseEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTreeWidget;
class QTreeWidgetItem;

class BattlePlanEditorWindow final : public QWidget
{
public:
    explicit BattlePlanEditorWindow(QWidget* parent = nullptr, bool embeddedInContainer = false);

    void setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback);
    void setSavedCallback(std::function<void()> callback);
    void loadSnapshot(const simcore::db::BattlePlanSnapshot& snapshot, bool duplicate);

    struct ActionDraft {
        std::int64_t action_preset_id = 0;
        int actor_slot = 0;
    };

    struct TurnDraft {
        int turn_index = 1;
        int player_combatants = 1;
        std::vector<ActionDraft> actions;
    };

private:
    void closeEvent(QCloseEvent* event) override;
    void createWidgets();
    void populateActionLibrary();
    void rebuildPlanTree();
    void refreshSelectionPanel();
    void syncSelectionPanelToAction();
    void addActionFromLibrarySelection();
    void assignPresetToSelection(std::int64_t presetId);
    void addActionToSelectedTurn(std::int64_t presetId, int turnIndex = -1, int slotIndex = -1);
    void assignActionPreset(std::int64_t presetId, int turnIndex, int actionIndex, int slotIndex);
    void duplicateSelectedAction();
    void removeSelectedNode();
    void moveSelectedAction(int delta);
    void ensureTurnCount(int count);
    void saveBattlePlan();
    void markDirty();
    bool confirmDiscardIfDirty();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);
    TurnDraft* selectedTurn();
    ActionDraft* selectedAction();
    const TurnDraft* selectedTurn() const;
    const ActionDraft* selectedAction() const;
    int selectedSlotIndex() const;
    const simcore::db::BattlePlanActionPresetSnapshot* actionPresetById(std::int64_t presetId) const;
    int findActionIndexBySlot(const TurnDraft& turn, int slotIndex) const;
    void ensureTurnActionSlots(TurnDraft& turn) const;
    void normalizeActionOrder(TurnDraft& turn) const;
    void removeActionsOutsideSlotRange(TurnDraft& turn, int maxSlots) const;
    QTreeWidgetItem* selectedTreeItem() const;
    int selectedTurnIndex() const;
    int selectedActionIndex() const;
    static bool isActionItem(const QTreeWidgetItem* item);
    static bool isTurnItem(const QTreeWidgetItem* item);

    std::function<void(const QString&, StatusToast::Severity)> statusCallback_;
    std::function<void()> savedCallback_;
    bool dirty_ = false;
    bool rebuildingTree_ = false;
    bool refreshingSelection_ = false;
    QLineEdit* nameEdit_ = nullptr;
    QSpinBox* turnCountSpin_ = nullptr;
    QListWidget* actionLibraryList_ = nullptr;
    QTreeWidget* planTree_ = nullptr;
    QLabel* selectionLabel_ = nullptr;
    QLabel* presetSummaryLabel_ = nullptr;
    QLabel* presetDetailLabel_ = nullptr;
    QLabel* slotLabel_ = nullptr;
    QSpinBox* combatantCountSpin_ = nullptr;
    QSpinBox* actorSlotSpin_ = nullptr;
    QPushButton* addActionButton_ = nullptr;
    QPushButton* duplicateActionButton_ = nullptr;
    QPushButton* removeNodeButton_ = nullptr;
    QPushButton* moveUpButton_ = nullptr;
    QPushButton* moveDownButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    std::vector<simcore::db::BattlePlanActionPresetSnapshot> actionPresets_;
    std::vector<TurnDraft> turns_;
};
