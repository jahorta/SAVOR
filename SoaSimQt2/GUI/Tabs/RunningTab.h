#pragma once

#include "GUI/Workspace/WorkspaceWidgets.h"

#include <functional>

class CoordinatorController;

class RunningTab final : public soasimqt2::gui::WorkspacePageShell
{
public:
    struct Actions {
        std::function<void()> openWorkflows;
        std::function<void()> openJobs;
        std::function<void()> openWorkers;
    };

    explicit RunningTab(CoordinatorController* coordinatorController, Actions actions, QWidget* parent = nullptr);

private:
    void build();

    CoordinatorController* coordinatorController_ = nullptr;
    Actions actions_;
};
