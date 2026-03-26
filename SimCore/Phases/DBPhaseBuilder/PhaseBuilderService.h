#pragma once
#include "../../Utils/IniDoc.h"
#include "../../DB/ProgramKindsRepo.h"
#include "../../DB/DBCore/ObjectStore.h"
#include "../../DB/SeedProbeRepo.h"
#include "../../DB/DeltaSeedRepo.h"
#include "../../DB/BattlePlanRepo.h"
#include "../../DB/SavestateRepo.h"
#include "../../DB/ExplorerSettingsRepo.h"
#include "../../DB/ExplorerSettingsPlanLinkRepo.h"
#include "../../DB/ExplorerSettingsPredicateRepo.h"
#include "PhaseBuilderPreview.h"
#include "PhaseBuilderSchemas.h"
#include <string>
#include <vector>

namespace simcore::db::phasebuilder {

    struct ValidationError { std::string field; std::string message; };

    class PhaseBuilderService {
    public:
        static IniDoc DefaultsFor(int program_kind);

        static std::vector<ValidationError> Validate(int program_kind, const IniDoc& ini);

        static DbResult<PhasePreview> Preview(int program_kind, const IniDoc& ini);

    private:
        static DbResult<TasMoviePreview>    PreviewTas(const IniDoc& ini);
        static DbResult<SeedProbePreview>   PreviewSeedProbe(const IniDoc& ini);
        static DbResult<ExplorerRunPreview> PreviewExplorer(const IniDoc& ini);
    };

} // namespace simcore::db::phasebuilder
