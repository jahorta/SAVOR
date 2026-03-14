#include "CoordinatorClock.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <ctime>

namespace simcore {
    namespace db {

        static std::string uuid4() {
            std::mt19937_64 rng{ std::random_device{}() ^ (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count() };
            auto gen64 = [&]() { return rng(); };
            uint64_t a = gen64(), b = gen64();
            a = (a & 0xFFFFFFFFFFFF0FFFULL) | (0x4ULL << 12);
            b = (b & 0x3FFFFFFFFFFFFFFFULL) | (0x2ULL << 62);
            std::ostringstream os;
            os << std::hex << std::setfill('0')
                << std::setw(8) << (uint32_t)(a >> 32) << "-"
                << std::setw(4) << (uint32_t)((a >> 16) & 0xFFFF) << "-"
                << std::setw(4) << (uint32_t)(a & 0xFFFF) << "-"
                << std::setw(4) << (uint32_t)(b >> 48) << "-"
                << std::setw(12) << (uint64_t)(b & 0x0000FFFFFFFFFFFFULL);
            return os.str();
        }

        static std::string wall_utc_iso_now() {
            using namespace std::chrono;
            auto tp = system_clock::now();
            std::time_t tt = system_clock::to_time_t(tp);
            std::tm tm{};
#if defined(_WIN32)
            gmtime_s(&tm, &tt);
#else
            gmtime_r(&tt, &tm);
#endif
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
            return std::string(buf);
        }

        CoordinatorClock& CoordinatorClock::instance() {
            static CoordinatorClock inst;
            return inst;
        }

        void CoordinatorClock::boot() {
            if (booted_.exchange(true)) return;
            mono_zero_ = std::chrono::steady_clock::now();
            boot_id_ = uuid4();
            wall_utc_ = wall_utc_iso_now();
        }

        std::string CoordinatorClock::boot_id() const { return boot_id_; }
        std::string CoordinatorClock::boot_wall_utc_iso() const { return wall_utc_; }

        int64_t CoordinatorClock::mono_now_ns() const {
            auto d = std::chrono::steady_clock::now() - mono_zero_;
            return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
        }

    } // namespace db
} // namespace simcore
