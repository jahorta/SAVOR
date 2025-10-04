#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "../../DB/DBCore/DbResult.h"
#include "../../DB/PredicateSpecRepo.h"
#include "../../DB/ExplorerSettingsPlanLinkRepo.h"
#include "../../DB/BattleRunGroupRepo.h"
#include "../../DB/DeltaSeedRepo.h"
#include "../../DB/SavestateRepo.h"
#include "../../Runner/Breakpoints/Predicate.h"
#include "../../Core/Input/SoaBattle/ActionPlanSerializer.h"

namespace simcore {
    namespace phase {

        struct BRPlanSpec {
            std::string name;
            soa::battle::actions::BattlePath path;  // raw domain plan
        };

        struct BROptions {
            uint32_t run_ms{ 100000 };
            uint32_t vi_stall_ms{ 2000 };
            std::optional<int32_t> priority{};
            std::optional<int32_t> max_attempts{};
        };

        class BattleRunnerCoordinator {
        public:
            static simcore::db::DbResult<int64_t> EnsureSettingsWithPredicatesAndPlans(
                const std::string& settings_name,
                const std::string& settings_desc,
                const std::vector<simcore::pred::Spec>& predicates,
                const std::vector<BRPlanSpec>& plans);

            // NEW: Create a BattleRunGroup for (settings_id, seed_probe_id), then for each
            // (plan_id in settings) x (unique delta seed in probe) create an ExplorerRun
            // and queue exactly one job in its own JobSet (expected_total=1).
            static simcore::db::DbResult<int64_t> CreateGroupAndQueueRuns(
                int64_t settings_id,
                int64_t seed_probe_id,
                const BROptions& opt,
                std::optional<std::string> group_name = std::nullopt,
                std::optional<std::string> group_desc = std::nullopt);

            static simcore::db::DbResult<std::string> PollProgress_JobSet(int64_t job_set_id);
            static simcore::db::DbResult<std::string> RetrieveResults_JobSet(int64_t job_set_id);

        private:
            static simcore::db::DbResult<std::vector<int64_t>> StagePlans(int64_t settings_id, const std::vector<BRPlanSpec>& plans);
        };

    }
} // namespace
