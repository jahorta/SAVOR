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
        std::string name;
        std::string fingerprint;
        int32_t     num_turns{};
        int64_t     created_at{};
    };

    struct BattlePlanRepo {
        static std::future<DbResult<int64_t>> EnsureAsync(std::string name, std::string fingerprint, int32_t num_turns, RetryPolicy rp = {});
        static std::future<DbResult<BattlePlanRow>> GetAsync(int64_t plan_id, RetryPolicy rp = {});
        static std::future<DbResult<void>> DeleteAsync( int64_t plan_id, RetryPolicy rp = {} );
        static std::future<DbResult<std::optional<int64_t>>> FindByFingerprintAsync(const std::string& fingerprint, RetryPolicy rp = {});

        static inline DbResult<int64_t> Ensure(std::string name, std::string fingerprint, int32_t num_turns) {
            return EnsureAsync(std::move(name), std::move(fingerprint), num_turns).get();
        }
        static inline DbResult<BattlePlanRow> Get(int64_t plan_id) { return GetAsync(plan_id).get(); }
        static inline DbResult<void> Delete(int64_t plan_id) { return DeleteAsync(plan_id).get(); }
        static inline DbResult<std::optional<int64_t>> FindByFingerprint(const std::string& fp) { return FindByFingerprintAsync(fp).get(); }
    };

} // namespace simcore::db
