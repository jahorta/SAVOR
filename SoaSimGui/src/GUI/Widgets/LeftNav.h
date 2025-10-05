#pragma once
#include "imgui.h"

namespace ImGui { void BeginDisabled(bool disabled); void EndDisabled(); }

enum class GuiPane {
    JobSets, Jobs, Workers, Artifacts, Programs, Triggers, ExplorerRuns, SeedProbe, TasMovies
};

struct GuiLeftNav {
    static void Draw();
    static GuiPane GetActive();
    static void SetActive(GuiPane p);
};
