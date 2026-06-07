#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"
#include "Authoring/IAuthoringDb.h"

class QCheckBox;
class QCloseEvent;
class QComboBox;
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
    void addActionToSelectedTurn(std::int64_t presetId);
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
    const simcore::db::BattlePlanActionPresetSnapshot* actionPresetById(std::int64_t presetId) const;
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
    QSpinBox* combatantCountSpin_ = nullptr;
    QSpinBox* actorSlotSpin_ = nullptr;
    QComboBox* macroCombo_ = nullptr;
    QComboBox* targetKindCombo_ = nullptr;
    QSpinBox* targetSlotSpin_ = nullptr;
    QSpinBox* targetMaskSpin_ = nullptr;
    QSpinBox* sameAsActorSpin_ = nullptr;
    QCheckBox* itemIdCheck_ = nullptr;
    QSpinBox* itemIdSpin_ = nullptr;
    QPushButton* addActionButton_ = nullptr;
    QPushButton* duplicateActionButton_ = nullptr;
    QPushButton* removeNodeButton_ = nullptr;
    QPushButton* moveUpButton_ = nullptr;
    QPushButton* moveDownButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    std::vector<simcore::db::BattlePlanActionPresetSnapshot> actionPresets_;
    std::vector<TurnDraft> turns_;
};
