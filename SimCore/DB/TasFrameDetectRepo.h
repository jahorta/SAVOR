#pragma once
#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include "DBCore/DbService.h"
#include <future>
#include <cstdint>

namespace simcore::db {

struct TasFrameDetectRepo {
    static std::future<DbResult<int64_t>> InsertAsync(int64_t artifact_id, int64_t dtm_artifact_id, RetryPolicy rp = {});
    static inline DbResult<int64_t> Insert(int64_t artifact_id, int64_t dtm_artifact_id, RetryPolicy rp = {}) {
        return InsertAsync(artifact_id, dtm_artifact_id, rp).get();
    }
};

} // namespace simcore::db
