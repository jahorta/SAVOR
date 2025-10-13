#pragma once
#include <string>
#include <vector>
#include <future>
#include <optional>
#include <cstdint>

#include "Utils/IniDoc.h"
#include "DB/DBCore/DbResult.h"
#include "Phases/DBPhaseBuilder/PhaseBuilderPreview.h"

namespace ImGui { struct PlotConfig; }

namespace simcore::db {
    struct ProgramKindKV;
}

namespace simcore::db::phasebuilder {
    struct PhasePreview;
}

class PhaseBuilderPane {
public:
    static void Draw();

private:
    static void ensureKindsLoaded();
    static void ensureDefaults();
    static void drawKindPicker();
    static void drawSeedProbeForm();
    static void drawTasMovieForm();
    static void drawExplorerRunForm();
    static void drawValidation();
    static void startPreviewAsync();
    static void drawPreview();
    static void startSubmitAsync();
    static void drawSubmit();
};
