#include "Win32Dialogs.h"
#include <windows.h>
#include <commdlg.h>

namespace Win32Dialogs 
{
    std::optional<std::filesystem::path> SaveFileDialog(const wchar_t* title, const wchar_t* default_name) {
        wchar_t fname[MAX_PATH]{ 0 };
        if (default_name) wcsncpy_s(fname, default_name, _TRUNCATE);

        wchar_t filter[] = L"All Files\0*.*\0\0";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;
        ofn.lpstrFilter = filter;
        ofn.lpstrFile = fname;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        ofn.lpstrTitle = title ? title : L"Save As";
        if (GetSaveFileNameW(&ofn)) {
            return std::filesystem::path(fname);
        }
        return std::nullopt;
    }
}
