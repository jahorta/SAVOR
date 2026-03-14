#pragma once
#include <chrono>

namespace simcore {
    namespace db {

        struct RetryPolicy {
            int max_attempts{ 3 };
            std::chrono::milliseconds initial_backoff{ 10 };
            double backoff_multiplier{ 2.0 };
            std::chrono::milliseconds max_backoff{ 200 };
            bool enabled() const { return max_attempts > 1; }
        };

    }
} // namespace simcore::db
