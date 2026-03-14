#pragma once
#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include "DBCore/DbService.h"
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <future>

namespace simcore::db {

    enum class TargetQuantifier : int32_t { All = 0, Any = 1, First = 2 };

    struct TurnActionPresetRow {
        int64_t     id{};
        std::string name;
        int32_t     macro{};            // soa::battle::actions::BattleAction
        int32_t     target_kind{};      // AnyEnemy / Single / Mask / OneOfMask / SameAsVar (match your UI enum)
        int32_t     item_id{ -1 };        // optional
        int32_t     mask_bits{ 0 };       // for ConcreteMask
        int32_t     single_slot{ -1 };    // for Single
        int32_t     same_as_pc{ -1 };     // for SameAsVar
        int32_t     flags{ 0 };           // reserved
        std::string target_expr_ini;    // INI for ByEnemyKind { enemy_kind_id, quantifier }
        int64_t     created_at{};
        int64_t     updated_at{};
    };

    struct TurnActionPresetLite { int64_t id{}; std::string name; int32_t macro{}; int32_t target_kind{}; };

    struct TurnActionPresetRepo {
        // Async
        static std::future<DbResult<int64_t>> InsertAsync(const TurnActionPresetRow& r, RetryPolicy rp = {});
        static std::future<DbResult<void>>    UpdateAsync(const TurnActionPresetRow& r, RetryPolicy rp = {});
        static std::future<DbResult<void>>    DeleteAsync(int64_t id, RetryPolicy rp = {});
        static std::future<DbResult<TurnActionPresetRow>> GetAsync(int64_t id, RetryPolicy rp = {});
        static std::future<DbResult<std::optional<TurnActionPresetRow>>> FindByNameAsync(const std::string& name, RetryPolicy rp = {});
        static std::future<DbResult<std::vector<TurnActionPresetLite>>> ListLiteAsync(const std::string& search, int32_t limit, RetryPolicy rp = {});

        // Blocking
        static inline DbResult<int64_t> Insert(const TurnActionPresetRow& r) { return InsertAsync(r).get(); }
        static inline DbResult<void>    Update(const TurnActionPresetRow& r) { return UpdateAsync(r).get(); }
        static inline DbResult<void>    Delete(int64_t id) { return DeleteAsync(id).get(); }
        static inline DbResult<TurnActionPresetRow> Get(int64_t id) { return GetAsync(id).get(); }
        static inline DbResult<std::optional<TurnActionPresetRow>> FindByName(const std::string& n) { return FindByNameAsync(n).get(); }
        static inline DbResult<std::vector<TurnActionPresetLite>> ListLite(const std::string& s, int32_t lim) { return ListLiteAsync(s, lim).get(); }
    };

} // namespace simcore::db
