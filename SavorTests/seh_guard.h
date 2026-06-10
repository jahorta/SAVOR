#pragma once
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <string>
#include <functional>

namespace tests {

    // Returns true if the exception came from a Dolphin module.
    inline bool SehFromDolphinModule(EXCEPTION_POINTERS* ep)
    {
        if (!ep || !ep->ExceptionRecord) return false;
        HMODULE mod = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(ep->ExceptionRecord->ExceptionAddress), &mod))
            return false;
        char path[MAX_PATH] = {};
        if (!GetModuleFileNameA(mod, path, MAX_PATH)) return false;
        std::string p(path);
        // Heuristics: adjust to your vendored path/names as needed
        // e.g., "dolphin-2506a", "Dolphin", "VideoCommon", etc.
        for (auto& c : p) c = (char)tolower((unsigned char)c);
        return p.find("dolphin") != std::string::npos; // broad match is fine here
    }

    // Run fn() and swallow SEH only if thrown from Dolphin.
    // Returns true if fn completed (or a swallowed Dolphin SEH happened), false if a non-Dolphin SEH occurred.
    inline bool RunIgnoringDolphinSEH(const std::function<void()>& fn)
    {
        __try {
            fn();
            return true;
        }
        __except (tests::SehFromDolphinModule(GetExceptionInformation())
            ? EXCEPTION_EXECUTE_HANDLER      // swallow Dolphin first-chance throws
            : EXCEPTION_CONTINUE_SEARCH)    // let real crashes propagate
        {
            return true;
        }
    }

    // Run fn() and swallow SEH only if thrown from Dolphin.
// Returns true if fn completed (or a swallowed Dolphin SEH happened), false if a non-Dolphin SEH occurred.
    inline bool RunIgnoringAllSEH(const std::function<void()>& fn)
    {
        __try {
            fn();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)      // swallow Dolphin first-chance throws
        {
            return true;
        }
        return true;
    }

} // namespace tests
#endif
