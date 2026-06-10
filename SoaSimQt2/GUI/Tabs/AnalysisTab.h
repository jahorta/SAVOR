#pragma once

#include "GUI/Workspace/WorkspaceWidgets.h"

#include <functional>

class AnalysisTab final : public soasimqt2::gui::WorkspacePageShell
{
public:
    struct Actions {
        std::function<void()> openSeedProbe;
        std::function<void()> openExplorerRuns;
        std::function<void()> openArtifacts;
        std::function<void()> openWorkflows;
    };

    explicit AnalysisTab(Actions actions, QWidget* parent = nullptr);

private:
    void build();

    Actions actions_;
};
