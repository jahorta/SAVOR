#pragma once
#include "../../../DB/DBCore/DbResult.h"
#include "../../../DB/PredicateSpecRepo.h"
#include <cstdint>
#include <string>
#include <vector>
#include "../../../Core/Input/SoaBattle/ActionTypes.h"
#include "../../../Core/Memory/Soa/Battle/BattleContext.h"
#include "../../BattleExplorer.h"


namespace simcore::phases {

    using db::PredicateSpecRow;
    using db::DbResult;

    struct AuthoringPayload {
        // Authoring model already parsed from INI. Resolution of ByEnemyKind must be done before calling.
        simcore::battleexplorer::UI_Config ui;
        std::vector<PredicateSpecRow>      predicates;            // Caller provides a repo-ready row with fingerprints generated (INI-deterministic).
        int32_t                            fake_attack_budget{};
        std::string                        settings_name;
        std::string                        settings_description;
    };

    struct BRSettingsWriter {
        static DbResult<int64_t> EnsureSettingsWithPredicatesAndPlans(
            int64_t savestate_id,
            const soa::battle::ctx::BattleContext& bc,
            const AuthoringPayload& a);

        static DbResult<int64_t> CreateRunGroup(
            int64_t seed_probe_id,
            int64_t savestate_id,
            int64_t settings_id,
            const std::string& name,
            const std::string& description);
    };

} // namespace simcore::phases
