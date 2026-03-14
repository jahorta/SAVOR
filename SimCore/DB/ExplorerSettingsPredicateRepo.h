#pragma once
#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include "DBCore/DbService.h"
#include <future>
#include <vector>
#include <cstdint>

namespace simcore {
    namespace db {

        struct SettingsPredicateRow { int64_t settings_id; int32_t ordinal; int64_t predicate_id; };

        struct ExplorerSettingsPredicateRepo {
            // Async
            static std::future<DbResult<void>> ReplaceAllAsync(int64_t settings_id, const std::vector<SettingsPredicateRow>& rows, RetryPolicy rp = {});
            static std::future<DbResult<std::vector<SettingsPredicateRow>>> ListAsync(int64_t settings_id, RetryPolicy rp = {});
            static std::future<DbResult<void>> AppendAsync(int64_t settings_id, int64_t predicate_id, RetryPolicy rp = {});
            static std::future<DbResult<void>> RemoveAsync(int64_t settings_id, int32_t ordinal, RetryPolicy rp = {});
            static std::future<DbResult<void>> ClearAsync(int64_t settings_id, RetryPolicy rp = {});

            // Blocking conveniences
            static inline DbResult<void> ReplaceAll(int64_t settings_id, const std::vector<SettingsPredicateRow>& rows) { return ReplaceAllAsync(settings_id, rows).get(); }
            static inline DbResult<std::vector<SettingsPredicateRow>> List(int64_t settings_id) { return ListAsync(settings_id).get(); }
            static inline DbResult<void> Append(int64_t settings_id, int64_t predicate_id) { return AppendAsync(settings_id, predicate_id).get(); }
            static inline DbResult<void> Remove(int64_t settings_id, int32_t ordinal) { return RemoveAsync(settings_id, ordinal).get(); }
            static inline DbResult<void> Clear(int64_t settings_id) { return ClearAsync(settings_id).get(); }
        };

    } // namespace db
} // namespace simcore
