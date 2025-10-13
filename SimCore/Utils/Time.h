#pragma once
#include <chrono>
#include <ctime>
#include <cstdio>

namespace simcore::time_util {

    using namespace std::chrono;

    inline system_clock::time_point to_system(steady_clock::time_point tp) {
        auto now_sys = system_clock::now();
        auto now_steady = steady_clock::now();
        return now_sys + std::chrono::duration_cast<std::chrono::system_clock::duration>(tp - now_steady);
    }

    // Convert steady_clock::time_point to a C string
    inline const char* steady_to_cstr(steady_clock::time_point tp) {
        static char buf[64]; // not thread-safe, reuse buffer

        auto sys_tp = to_system(tp);
        std::time_t t = system_clock::to_time_t(sys_tp);
#if defined(_WIN32)
        std::tm tm{};
        localtime_s(&tm, &t);
#else
        std::tm tm = *std::localtime(&t);
#endif

        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        return buf;
    }

}