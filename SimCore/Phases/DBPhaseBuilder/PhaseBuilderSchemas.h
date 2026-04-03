#pragma once
#include "../../Utils/IniDoc.h"
#include "../../DB/ProgramKindsRepo.h"
#include "../../DB/ProgramDB/IProgramDBCodec.h"
#include "../../DB/ProgramDB/SeedProbeDBCodec.h"
#include "../../DB/ProgramDB/TasMovieDBCodec.h"
#include "../../DB/ProgramDB/TasFrameDetectorDBCodec.h"
#include "../../DB/ProgramDB/ExplorerRunDBCodec.h"

namespace simcore::db::phasebuilder {

    struct SchemaComposer {
        static IniDoc DefaultIniFor(int program_kind);
    private:
        static IniDoc DefaultSeedProbe();
        static IniDoc DefaultTasMovie();
        static IniDoc DefaultExplorerRun();
    };

} // namespace simcore::db::phasebuilder
