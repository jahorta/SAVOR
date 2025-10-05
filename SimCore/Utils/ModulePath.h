#pragma once
#include <windows.h>
#include <filesystem>

namespace utils {
    using std::filesystem::path;
    static inline path getExecutablePath() {
        char buffer[MAX_PATH];
        GetModuleFileNameA(NULL, buffer, MAX_PATH);
        path module_path{ buffer };
        return module_path.parent_path();
    }
}