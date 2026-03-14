#pragma once
#include "imgui.h"

namespace ImGui { void BeginDisabled(bool disabled); void EndDisabled(); }

enum class GuiPane {
    JobBuilder, Workers, JobSets, Jobs, Debugger, BattleRunSettings, Artifacts, Triggers, ExplorerRuns, SeedProbe, TasMovies, Settings
};

static inline const char* items[] = {
        "Programs","Workers","Job Sets","Jobs","Debugger","BattleRunSettings","Artifacts","Triggers","Explorer Runs","Seed Probe","TAS Movies","Settings"
};

struct GuiLeftNav {
    static void Draw();
    static GuiPane GetActive();
    static void SetActive(GuiPane p);
    static void SetDebuggerHookActive(bool active);
};
