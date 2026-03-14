#include "EnsureSys.h"
#include <filesystem>
#include <windows.h>

namespace simcore {
    namespace {
        static std::string exe_dir_utf8() {
            wchar_t buf[MAX_PATH];
            GetModuleFileNameW(nullptr, buf, MAX_PATH);
            std::wstring w(buf);
            auto pos = w.find_last_of(L"\\/");
            if (pos == std::wstring::npos) return ".";
            std::wstring d = w.substr(0, pos);
            int n = WideCharToMultiByte(CP_UTF8, 0, d.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string out(n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, d.c_str(), -1, out.data(), n, nullptr, nullptr);
            if (!out.empty() && out.back() == '\0') out.pop_back();
            return out;
        }

        static bool copy_tree(const std::filesystem::path& src, const std::filesystem::path& dst) {
            std::error_code ec;
            std::filesystem::create_directories(dst, ec);
            for (auto it = std::filesystem::recursive_directory_iterator(src, ec);
                !ec && it != std::filesystem::recursive_directory_iterator(); ++it)
            {
                const auto& p = it->path();
                auto rel = std::filesystem::relative(p, src, ec);
                auto to = dst / rel;
                if (it->is_directory()) {
                    std::filesystem::create_directories(to, ec);
                }
                else if (it->is_regular_file()) {
                    std::filesystem::create_directories(to.parent_path(), ec);
                    ec.clear();
                    std::filesystem::copy_file(p, to,
                        std::filesystem::copy_options::overwrite_existing, ec);
                }
                if (ec) return false;
            }
            return true;
        }
    }

    bool EnsureSysBesideExe(const std::string& dolphin_base_dir)
    {
        namespace fs = std::filesystem;
        const fs::path exe_dir = exe_dir_utf8();
        const fs::path dst = exe_dir / "Sys";
        const fs::path src = fs::path(dolphin_base_dir) / "Sys";

        // already good?
        if (fs::exists(dst / "GC" / "dsp_coef.bin")) return true;

        // simple cross-process guard (optional)
        HANDLE m = CreateMutexW(nullptr, FALSE, L"Global\\SimCore_CopySysOnce");
        WaitForSingleObject(m, INFINITE);

        bool ok = fs::exists(dst / "dsp_coef.bin") || copy_tree(src, dst);

        ReleaseMutex(m);
        CloseHandle(m);
        return ok;
    }

} // namespace simcore
