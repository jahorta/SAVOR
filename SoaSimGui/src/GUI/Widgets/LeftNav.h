#pragma once
#include "imgui.h"

namespace ImGui { void BeginDisabled(bool disabled); void EndDisabled(); }

enum class GuiPane {
    JobBuilder, Workers, JobSets, Jobs, BattleRunSettings, Artifacts, Triggers, ExplorerRuns, SeedProbe, TasMovies
};

static inline const char* items[] = {
        "Programs","Workers","Job Sets","Jobs","BattleRunSettings","Artifacts","Triggers","Explorer Runs","Seed Probe","TAS Movies"
};

struct GuiLeftNav {
    static void Draw();
    static GuiPane GetActive();
    static void SetActive(GuiPane p);
};
