#pragma once

#include "GUI/Workspace/WorkspaceWidgets.h"

#include <functional>

class CoordinatorController;
namespace simcore::db { struct WorkflowGraphSnapshot; }

class SetupTab final : public soasimqt2::gui::WorkspacePageShell
{
public:
    struct Actions {
        std::function<void()> openLauncher;
        std::function<void()> openAuthoring;
        std::function<void()> openGraphEditor;
        std::function<void(const simcore::db::WorkflowGraphSnapshot&, bool)> openGraphEditorSnapshot;
        std::function<void()> openSettings;
        std::function<void()> openArtifacts;
        std::function<void()> openDtmEditor;
        std::function<void()> openBattleSettings;
    };

    explicit SetupTab(CoordinatorController* coordinatorController, Actions actions, QWidget* parent = nullptr);

private:
    void build();

    CoordinatorController* coordinatorController_ = nullptr;
    Actions actions_;
};
