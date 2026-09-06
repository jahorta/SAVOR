#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "../Core/DolphinWrapper.h"

namespace simboot {

    struct SessionFilesystemPreparationRequest {
        std::size_t worker_id = 0;
        std::uint64_t process_generation = 0;
        std::string preparation_id;
        std::filesystem::path dolphin_qt_base;
        std::filesystem::path worker_root;
        std::function<bool()> cancelled;
    };

    struct SessionFilesystemPreparationResult {
        bool ok = false;
        bool retryable = true;
        std::string preparation_id;
        std::filesystem::path user_directory;
        std::filesystem::path marker_path;
        std::uint64_t file_count = 0;
        std::uint64_t directory_count = 0;
        std::uint64_t byte_count = 0;
        std::uint64_t elapsed_ms = 0;
        std::string error;
    };

    class SessionFilesystemPreparer final {
    public:
        [[nodiscard]] static SessionFilesystemPreparationResult Prepare(
            const SessionFilesystemPreparationRequest& request);
        [[nodiscard]] static bool Validate(
            const std::filesystem::path& user_directory,
            std::string_view preparation_id,
            std::uint64_t process_generation,
            std::string* error_out = nullptr);
        [[nodiscard]] static std::filesystem::path MarkerPath(
            const std::filesystem::path& user_directory);
    };

    // SavorCore library bootstrap entry points live in this module (Boot/Boot.h + Boot/Boot.cpp).
    // Projects should call these helpers rather than relying on SavorCore.cpp, which is only a
    // Visual Studio static-library template placeholder.

    // What to boot with.
    struct BootOptions {
        std::filesystem::path user_dir;          // isolated User/ for this simulator
        std::filesystem::path dolphin_qt_base;   // MUST be portable (contains portable.txt)
        std::string session_filesystem_preparation_id;
        std::uint64_t process_generation = 0;
        bool visual = false;                     // request boot with a render-surface connection
        void* render_widget_handle = nullptr;    // Qt render widget native handle (HWND on Windows)
    };

    // Boot using explicit paths.
    bool BootDolphinWrapper(savor::DolphinWrapper& dw, const BootOptions& opts, std::string* error_out = nullptr);

} // namespace savor
