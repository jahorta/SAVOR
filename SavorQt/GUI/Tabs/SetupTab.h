#pragma once

#include "GUI/Common/StatusToast.h"
#include "GUI/Workspace/WorkspaceWidgets.h"

#include <functional>

class CoordinatorController;
namespace savor::db { struct WorkflowGraphSnapshot; }
struct StatusToast;

class SetupTab final : public savorqt::gui::WorkspacePageShell
{
    Q_OBJECT

public:
    struct Actions {
        std::function<void()> openLauncher;
        std::function<void()> openAuthoring;
        std::function<void()> openGraphEditor;
        std::function<void(const savor::db::WorkflowGraphSnapshot&, bool)> openGraphEditorSnapshot;
        std::function<void()> openSettings;
        std::function<void()> openIsoSettings;
        std::function<void()> openDolphinSettings;
        std::function<void()> openArtifacts;
        std::function<void()> openDtmEditor;
        std::function<void()> openBattleSettings;
        std::function<void(const StatusToast&)> postStatusToast;
    };

    explicit SetupTab(CoordinatorController* coordinatorController, Actions actions, QWidget* parent = nullptr);

signals:
	void statusToastRequested(StatusToast toast);

private:
    void build();

    CoordinatorController* coordinatorController_ = nullptr;
    Actions actions_;
};
