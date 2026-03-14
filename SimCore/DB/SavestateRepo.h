#pragma once
#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include "DBCore/DbService.h"
#include "Querying/IdRepoListDTO.h"
#include "Querying/Paging.h"
#include "Querying/PagedQuery.h"
#include <string>
#include <cstdint>
#include <optional>
#include <future>

namespace simcore {
    namespace db {

        enum SavestateType : uint8_t {
            BATTLE = 0,
            UNKNOWN = 0xFF
        };

        struct SavestateRow {
            int64_t id{};
            SavestateType savestate_type{};
            std::string note;
            int64_t object_ref_id{};
            bool complete{ false };
        };

        class SavestateRepo {
        public:
            static std::future<DbResult<int64_t>> PlanAsync(int savestate_type, std::string note, RetryPolicy rp = {});
            static std::future<DbResult<void>>    FinalizeAsync(int64_t id, int64_t object_ref_id, RetryPolicy rp = {});
            static std::future<DbResult<std::optional<SavestateRow>>> GetAsync(int64_t id, RetryPolicy rp = {});
            static std::future<DbResult<std::optional<SavestateRow>>> GetByProbeIdAsync(int64_t probe_id, RetryPolicy rp = {});
            static std::future<DbResult<Page<SavestateLite>>> ListPagedAsync(const PagedQuery<>& q, const std::string& search, RetryPolicy rp = {});
            static std::future<DbResult<void>> DeleteAsync(int64_t id, RetryPolicy rp = {});

            static inline DbResult<int64_t> Plan(int savestate_type, std::string note) { return PlanAsync(savestate_type, std::move(note)).get(); }
            static inline DbResult<void>    Finalize(int64_t id, int64_t object_ref_id) { return FinalizeAsync(id, object_ref_id).get(); }
            static inline DbResult<std::optional<SavestateRow>> Get(int64_t id) { return GetAsync(id).get(); }
            static inline DbResult<std::optional<SavestateRow>> GetByProbeId(int64_t probe_id) { return GetByProbeIdAsync(probe_id).get(); }
            static inline DbResult<Page<SavestateLite>> ListPaged(const PagedQuery<>& q, const std::string& s) { return ListPagedAsync(q, s).get(); }
            static inline DbResult<void> Delete(int64_t id) { return DeleteAsync(id).get(); }
        };

    } // namespace db
} // namespace simcore
