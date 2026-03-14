#pragma once
#include <string>
#include <chrono>
#include <atomic>

namespace simcore {
    namespace db {

        class CoordinatorClock {
        public:
            static CoordinatorClock& instance();

            void boot();
            std::string boot_id() const;
            std::string boot_wall_utc_iso() const;
            int64_t mono_now_ns() const;

        private:
            CoordinatorClock() = default;

            std::atomic<bool> booted_{ false };
            std::chrono::steady_clock::time_point mono_zero_{};
            std::string boot_id_;
            std::string wall_utc_;
        };

    } // namespace db
} // namespace simcore
