#pragma once
#include "../../Utils/IniDoc.h"
#include "../../DB/ProgramKindsRepo.h"
#include "../../DB/ProgramDB/IProgramDBCodec.h"
#include "../../DB/DBCore/ObjectStore.h"
#include "../../DB/SeedProbeRepo.h"
#include "../../DB/DeltaSeedRepo.h"
#include "../../DB/BattlePlanRepo.h"
#include "../../DB/SavestateRepo.h"
#include "../../DB/ExplorerSettingsRepo.h"
#include "../../DB/ExplorerSettingsPlanLinkRepo.h"
#include "../../DB/ExplorerSettingsPredicateRepo.h"
#include "../../DB/Scheduling/JobSetsRepo.h"
#include "PhaseBuilderPreview.h"
#include "PhaseBuilderSchemas.h"
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace simcore::db::phasebuilder {

    struct ValidationError { std::string field; std::string message; };

    struct SubmitResult {
        int64_t job_set_id{};
        int program_kind{};
    };

    class PhaseBuilderService {
    public:
        static IniDoc DefaultsFor(int program_kind);

        static std::vector<ValidationError> Validate(int program_kind, const IniDoc& ini);

        static DbResult<PhasePreview> Preview(int program_kind, const IniDoc& ini);

        static DbResult<SubmitResult> Submit(
            const std::string& purpose,
            int program_kind,
            const IniDoc& ini,
            std::optional<std::string> meta_text = {},
            std::optional<int64_t> expected_total_override = {}
        );

    private:
        static DbResult<TasMoviePreview>    PreviewTas(const IniDoc& ini);
        static DbResult<SeedProbePreview>   PreviewSeedProbe(const IniDoc& ini);
        static DbResult<ExplorerRunPreview> PreviewExplorer(const IniDoc& ini);
    };

} // namespace simcore::db::phasebuilder
