#pragma once
#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include "DBCore/DbService.h"
#include <future>
#include <vector>
#include <cstdint>

namespace simcore {
    namespace db {

        struct SettingsPlanLinkRow { int64_t settings_id; int32_t ordinal; int64_t plan_id; };

        struct ExplorerSettingsPlanLinkRepo {
            // Async
            static std::future<DbResult<void>> ReplaceAllAsync(int64_t settings_id, const std::vector<int64_t>& plan_ids, RetryPolicy rp = {});
            static std::future<DbResult<std::vector<SettingsPlanLinkRow>>> ListBySettingsAsync(int64_t settings_id, RetryPolicy rp = {});
            static std::future<DbResult<void>> AppendAsync(int64_t settings_id, int64_t plan_id, RetryPolicy rp = {});
            static std::future<DbResult<void>> ClearAsync(int64_t settings_id, RetryPolicy rp = {});
            static std::future<DbResult<void>> RemoveAsync(int64_t settings_id, int32_t ordinal, RetryPolicy rp = {});
            static std::future<DbResult<int>> GetPlanCountAsync(int64_t settings_id, RetryPolicy rp = {});

            // Blocking
            static inline DbResult<void> ReplaceAll(int64_t settings_id, const std::vector<int64_t>& plan_ids) { return ReplaceAllAsync(settings_id, plan_ids).get(); }
            static inline DbResult<std::vector<SettingsPlanLinkRow>> ListBySettings(int64_t settings_id) { return ListBySettingsAsync(settings_id).get(); }
            static inline DbResult<void> Append(int64_t settings_id, int64_t plan_id) { return AppendAsync(settings_id, plan_id).get(); }
            static inline DbResult<void> Clear(int64_t settings_id) { return ClearAsync(settings_id).get(); }
            static inline DbResult<void> Remove(int64_t settings_id, int32_t ordinal) { return RemoveAsync(settings_id, ordinal).get(); }
            static inline DbResult<int> GetPlanCount(int64_t settings_id) { return GetPlanCountAsync(settings_id).get(); }
        };

    }
} // namespace
