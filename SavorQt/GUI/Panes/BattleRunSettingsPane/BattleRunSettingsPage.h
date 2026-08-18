#pragma once

#include <vector>

#include <QtCore/QPointer>
#include <QtWidgets/QWidget>

#include "Authoring/IAuthoringDb.h"
#include "GUI/Common/StatusToast.h"

class BattlePlanEditorWindow;
class QLabel;
class QListWidget;
class QPushButton;
class SeedProbeSpecEditorWindow;

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
    void openSeedProbeSpecEditor();
    void openBattlePlanEditor();
    void editSelectedBattlePlan();
    void duplicateSelectedBattlePlan();
    void refreshAuthoringLists();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    QPushButton* newSeedProbeSpecButton_ = nullptr;
    QPushButton* newBattlePlanButton_ = nullptr;
    QPushButton* editBattlePlanButton_ = nullptr;
    QPushButton* duplicateBattlePlanButton_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QListWidget* battlePlanList_ = nullptr;
    QLabel* libraryStatusLabel_ = nullptr;
    std::vector<savor::db::BattlePlanSnapshot> battlePlans_;
    QPointer<SeedProbeSpecEditorWindow> seedProbeSpecEditor_;
    QPointer<BattlePlanEditorWindow> battlePlanEditor_;
};
