// SimCore/DB/BattlePlanRepo.h
#pragma once
#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include "DBCore/DbService.h"
#include <string>
#include <vector>
#include <cstdint>
#include <future>
#include <optional>

namespace simcore::db {

    struct BattlePlanRow {
        int64_t     plan_id{};
        int64_t     settings_id{};
        std::string name;
        std::string fingerprint;
        int32_t     num_turns{};
        int64_t     created_at{};
    };

    struct BattlePlanRepo {
        static std::future<DbResult<int64_t>> EnsureAsync(
            int64_t settings_id, std::string name, std::string fingerprint, int32_t num_turns, RetryPolicy rp = {}
        );
        static std::future<DbResult<std::vector<BattlePlanRow>>> ListBySettingsAsync(
            int64_t settings_id, RetryPolicy rp = {}
        );
        static std::future<DbResult<BattlePlanRow>> GetAsync(
            int64_t plan_id, RetryPolicy rp = {}
        );
        static std::future<DbResult<void>> DeleteAsync(
            int64_t plan_id, RetryPolicy rp = {}
        );

        static inline DbResult<int64_t> Ensure(int64_t settings_id, std::string name, std::string fingerprint, int32_t num_turns) {
            return EnsureAsync(settings_id, std::move(name), std::move(fingerprint), num_turns).get();
        }
        static inline DbResult<std::vector<BattlePlanRow>> ListBySettings(int64_t settings_id) {
            return ListBySettingsAsync(settings_id).get();
        }
        static inline DbResult<BattlePlanRow> Get(int64_t plan_id) {
            return GetAsync(plan_id).get();
        }
        static inline DbResult<void> Delete(int64_t plan_id) {
            return DeleteAsync(plan_id).get();
        }
    };

} // namespace simcore::db
