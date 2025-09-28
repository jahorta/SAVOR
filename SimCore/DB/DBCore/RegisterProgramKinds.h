#pragma once
#include "DbResult.h"
#include "DbRetryPolicy.h"
#include "DbService.h"
#include <future>

namespace simcore {
    namespace db {

        // Inserts default program kinds based on Wire.h enum values.
        // Safe to call repeatedly; uses INSERT OR IGNORE.

        // Async API
        std::future<DbResult<void>> RegisterProgramKindsAsync(RetryPolicy rp = {});

        // Blocking convenience
        inline DbResult<void> RegisterProgramKinds() {
            return RegisterProgramKindsAsync().get();
        }

    } // namespace db
} // namespace simcore
