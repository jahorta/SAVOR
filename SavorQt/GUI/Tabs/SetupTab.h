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
        std::function<void()> openArtifacts;
        std::function<void()> openDtmEditor;
    };

    explicit SetupTab(CoordinatorController* coordinatorController, QWidget* parent = nullptr);

signals:
	void statusToastRequested(StatusToast toast);
	void openLauncherRequested();
	void openAuthoringRequested();
	void openGraphEditorRequested();
	void openGraphEditorSnapshotRequested(const savor::db::WorkflowGraphSnapshot& snapshot, bool isReadOnly);
	void openSettingsRequested();
	void openIsoSettingsRequested();
	void openDolphinSettingsRequested();
	void openArtifactsRequested();
	void openDtmEditorRequested();
	void openBattleSettingsRequested();

private:
    void build();

    CoordinatorController* coordinatorController_ = nullptr;
    Actions actions_;
};
