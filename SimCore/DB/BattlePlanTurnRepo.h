#pragma once
#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include "DBCore/DbService.h"
#include <future>
#include <vector>
#include <cstdint>

namespace simcore {
    namespace db {

        // New plan_id keyed readers/writers
        struct BattlePlanTurnByPlanRow { int64_t plan_id; int32_t turn_index; };
        struct TurnActorBindingByPlan { int32_t actor_index; int64_t atom_id; };

        struct BattlePlanTurnRepo {
            static std::future<DbResult<void>> UpsertTurnByPlanAsync(int64_t plan_id, int32_t turn_index, RetryPolicy rp = {});
            static std::future<DbResult<void>> UpsertTurnActorByPlanAsync(int64_t plan_id, int32_t turn_index, int32_t actor_index, int64_t atom_id, RetryPolicy rp = {});
            static std::future<DbResult<void>> ReplaceTurnByPlanAsync(int64_t plan_id, int32_t turn_index, std::vector<TurnActorBindingByPlan> actors, RetryPolicy rp = {});
            static std::future<DbResult<std::vector<BattlePlanTurnByPlanRow>>> LoadTurnsByPlanAsync(int64_t plan_id, RetryPolicy rp = {});
            static std::future<DbResult<std::vector<TurnActorBindingByPlan>>> ListActorsByPlanAsync(int64_t plan_id, int32_t turn_index, RetryPolicy rp = {});

            static inline DbResult<void> UpsertTurnByPlan(int64_t plan_id, int32_t turn_index) {
                return UpsertTurnByPlanAsync(plan_id, turn_index).get();
            }
            static inline DbResult<void> UpsertTurnActorByPlan(int64_t plan_id, int32_t turn_index, int32_t actor_index, int64_t atom_id) {
                return UpsertTurnActorByPlanAsync(plan_id, turn_index, actor_index, atom_id).get();
            }
            static inline DbResult<void> ReplaceTurnByPlan(int64_t plan_id, int32_t turn_index, std::vector<TurnActorBindingByPlan> actors) {
                return ReplaceTurnByPlanAsync(plan_id, turn_index, std::move(actors)).get();
            }
            static inline DbResult<std::vector<BattlePlanTurnByPlanRow>> LoadTurnsByPlan(int64_t plan_id) {
                return LoadTurnsByPlanAsync(plan_id).get();
            }
            static inline DbResult<std::vector<TurnActorBindingByPlan>> ListActorsByPlan(int64_t plan_id, int32_t turn_index) {
                return ListActorsByPlanAsync(plan_id, turn_index).get();
            }
        };

    } // namespace db
} // namespace simcore
