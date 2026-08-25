#pragma once

#include "GUI/Common/StatusToast.h"
#include "GUI/Workspace/WorkspaceWidgets.h"

class CoordinatorController;
class WorkflowLauncherPage;
namespace savor::db { struct WorkflowGraphSnapshot; }
struct StatusToast;

class SetupTab final : public savorqt::gui::WorkspacePageShell
{
    Q_OBJECT

public:
    explicit SetupTab(CoordinatorController* coordinatorController, QWidget* parent = nullptr);
    void showWorkflowLauncher(
        const QString& unitKind,
        const QString& inputKey,
        qint64 refId);

signals:
	void statusToastRequested(StatusToast toast);
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
    WorkflowLauncherPage* workflowLauncherPage_ = nullptr;
};
