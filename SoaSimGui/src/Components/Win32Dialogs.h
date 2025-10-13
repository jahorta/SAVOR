#pragma once
#include <optional>
#include <filesystem>
#include <string>

namespace Win32Dialogs {
    std::optional<std::filesystem::path> SaveFileDialog(const wchar_t* title, const wchar_t* default_name);
}
