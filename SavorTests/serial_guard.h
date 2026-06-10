#pragma once
#pragma once
#include <mutex>

namespace tests {


    // Use this to serialize any test that touches ViGEm/XInput/Dolphin input.
    inline std::mutex& vigemMutex() { static std::mutex m; return m; }

    struct SerialGuard {
        std::unique_lock<std::mutex> lk;
        SerialGuard() : lk(vigemMutex()) {}
    };

} // namespace tests