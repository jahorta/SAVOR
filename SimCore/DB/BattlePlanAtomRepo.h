#pragma once
#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include "DBCore/DbService.h"
#include <future>
#include <string>
#include <vector>
#include <cstdint>

namespace simcore {
    namespace db {

        struct BattlePlanAtomRow {
            int64_t id{};
            int32_t action_type{};
            int32_t actor_slot{};
            int32_t param_item_id{};
            int32_t target_slot{};
        };

        struct BattlePlanAtomRepo {
            static std::future<DbResult<int64_t>> EnsureAsync(
                int32_t action_type,
                int32_t actor_slot,
                int32_t param_item_id,
                int32_t target_slot,
                RetryPolicy rp = {});

            static std::future<DbResult<BattlePlanAtomRow>> GetAsync(int64_t id, RetryPolicy rp = {});

            static inline DbResult<int64_t> Ensure(
                int32_t action_type,
                int32_t actor_slot,
                int32_t param_item_id,
                int32_t target_slot) {
                return EnsureAsync(action_type, actor_slot, param_item_id, target_slot).get();
            }

            static inline DbResult<BattlePlanAtomRow> Get(int64_t id) {
                return GetAsync(id).get();
            }
        };

    } // namespace db
} // namespace simcore
