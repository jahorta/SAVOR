#pragma once

#include <vector>

#include <QtCore/QPointer>
#include <QtWidgets/QWidget>

#include "Authoring/IAuthoringDb.h"
#include "GUI/Common/StatusToast.h"

class BattlePlanEditorWindow;
class BattleRunSpecEditorWindow;
class ExplorerSettingsEditorWindow;
class QLabel;
class QListWidget;
class PredicateSpecEditorWindow;
class PredicateSetEditorWindow;
class QPushButton;
class SeedProbeSpecEditorWindow;
class TasSpecEditorWindow;
class BattleChainSpecEditorWindow;

class BattleRunSettingsPage final : public QWidget
{
    Q_OBJECT

public:
    explicit BattleRunSettingsPage(QWidget* parent = nullptr);
    ~BattleRunSettingsPage() override = default;

signals:
    void statusToastRequested(StatusToast toast);

private:
    void createWidgets();
    void openPredicateEditor();
    void openSeedProbeSpecEditor();
    void openTasSpecEditor();
    void openBattleRunSpecEditor();
    void openPredicateSetEditor();
    void openExplorerSettingsEditor();
    void openBattleChainSpecEditor();
    void editSelectedPredicate();
    void duplicateSelectedPredicate();
    void deleteSelectedPredicate();
    void openBattlePlanEditor();
    void editSelectedBattlePlan();
    void duplicateSelectedBattlePlan();
    void refreshAuthoringLists();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    QPushButton* newPredicateButton_ = nullptr;
    QPushButton* newSeedProbeSpecButton_ = nullptr;
    QPushButton* newTasSpecButton_ = nullptr;
    QPushButton* newBattleRunSpecButton_ = nullptr;
    QPushButton* newPredicateSetButton_ = nullptr;
    QPushButton* newExplorerSettingsButton_ = nullptr;
    QPushButton* newBattleChainSpecButton_ = nullptr;
    QPushButton* editPredicateButton_ = nullptr;
    QPushButton* duplicatePredicateButton_ = nullptr;
    QPushButton* deletePredicateButton_ = nullptr;
    QPushButton* newBattlePlanButton_ = nullptr;
    QPushButton* editBattlePlanButton_ = nullptr;
    QPushButton* duplicateBattlePlanButton_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QListWidget* predicateList_ = nullptr;
    QListWidget* battlePlanList_ = nullptr;
    QLabel* libraryStatusLabel_ = nullptr;
    std::vector<savor::db::PredicateSpecSnapshot> predicates_;
    std::vector<savor::db::BattlePlanSnapshot> battlePlans_;
    QPointer<PredicateSpecEditorWindow> predicateEditor_;
    QPointer<SeedProbeSpecEditorWindow> seedProbeSpecEditor_;
    QPointer<TasSpecEditorWindow> tasSpecEditor_;
    QPointer<BattleRunSpecEditorWindow> battleRunSpecEditor_;
    QPointer<PredicateSetEditorWindow> predicateSetEditor_;
    QPointer<ExplorerSettingsEditorWindow> explorerSettingsEditor_;
    QPointer<BattleChainSpecEditorWindow> battleChainSpecEditor_;
    QPointer<BattlePlanEditorWindow> battlePlanEditor_;
};
