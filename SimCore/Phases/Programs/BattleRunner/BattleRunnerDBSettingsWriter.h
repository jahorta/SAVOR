#pragma once
#include "../../../DB/DBCore/DbResult.h"
#include "../../../DB/PredicateSpecRepo.h"
#include <cstdint>
#include <string>
#include <vector>
#include "../../../Core/Input/SoaBattle/ActionTypes.h"
#include "../../../Core/Memory/Soa/Battle/BattleContext.h"
#include "../../BattleExplorer.h"

using soa::battle::ctx::BattleContext;

namespace simcore::phases {

    using db::PredicateSpecRow;
    using db::DbResult;

    struct AuthoringPayload {
        // Authoring model already parsed from INI. Resolution of ByEnemyKind must be done before calling.
        battleexplorer::UI_Config ui;
        std::vector<PredicateSpecRow>      predicates;            // Caller provides a repo-ready row with fingerprints generated (INI-deterministic).
        std::string                        settings_name;
        std::string                        settings_description;
    };

    struct BRSettingsWriter {
        static DbResult<int64_t> EnsureSettingsWithPredicatesAndPlans(
            int64_t savestate_id,
            const BattleContext& bc,
            const AuthoringPayload& a);
    };

} // namespace simcore::phases
