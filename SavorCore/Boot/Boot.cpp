#include "Boot.h"

#include <chrono>
#include <fstream>
#include <utility>

namespace simboot {

    namespace {
        constexpr std::string_view kPreparationMarkerName =
            "session-preparation-v2.marker";

        [[nodiscard]] std::string CanonicalPathString(
            const std::filesystem::path& path,
            std::error_code& error) {
            const auto canonical = std::filesystem::weakly_canonical(path, error);
            return error ? std::string{} : canonical.generic_string();
        }

        [[nodiscard]] SessionFilesystemPreparationResult PreparationFailure(
            const SessionFilesystemPreparationRequest& request,
            bool retryable,
            std::string error,
            std::chrono::steady_clock::time_point started) {
            return {
                .ok = false,
                .retryable = retryable,
                .preparation_id = request.preparation_id,
                .user_directory = request.worker_root / "User",
                .marker_path = SessionFilesystemPreparer::MarkerPath(
                    request.worker_root / "User"),
                .elapsed_ms = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started).count()),
                .error = std::move(error),
            };
        }
    }

    std::filesystem::path SessionFilesystemPreparer::MarkerPath(
        const std::filesystem::path& user_directory) {
        return user_directory.parent_path() / kPreparationMarkerName;
    }

    SessionFilesystemPreparationResult SessionFilesystemPreparer::Prepare(
        const SessionFilesystemPreparationRequest& request) {
        const auto started = std::chrono::steady_clock::now();
        if (request.preparation_id.empty() || request.process_generation == 0
            || request.worker_root.empty() || request.dolphin_qt_base.empty()) {
            return PreparationFailure(
                request, false, "session preparation identity is incomplete", started);
        }

        std::error_code error;
        const auto base = std::filesystem::weakly_canonical(
            request.dolphin_qt_base, error);
        if (error || !std::filesystem::is_directory(base, error)
            || !std::filesystem::is_regular_file(base / "portable.txt", error)
            || !std::filesystem::is_directory(base / "Sys", error)) {
            return PreparationFailure(
                request, false,
                "Dolphin base must contain portable.txt and Sys", started);
        }

        std::filesystem::create_directories(request.worker_root, error);
        if (error) {
            return PreparationFailure(
                request, true,
                "worker root creation failed: " + error.message(), started);
        }
        const auto user_directory = request.worker_root / "User";
        const auto marker_path = MarkerPath(user_directory);
        if (request.cancelled && request.cancelled()) {
            return PreparationFailure(
                request, true, "session preparation canceled", started);
        }

        std::filesystem::remove(marker_path, error);
        if (error) {
            return PreparationFailure(
                request, true,
                "session marker removal failed path=" + marker_path.string()
                    + ": " + error.message(),
                started);
        }
        std::filesystem::remove_all(user_directory, error);
        if (error) {
            return PreparationFailure(
                request, true,
                "session User removal failed path=" + user_directory.string()
                    + ": " + error.message(),
                started);
        }
        if (request.cancelled && request.cancelled()) {
            return PreparationFailure(
                request, true, "session preparation canceled", started);
        }
        std::filesystem::create_directories(user_directory, error);
        if (error) {
            return PreparationFailure(
                request, true,
                "session User creation failed path=" + user_directory.string()
                    + ": " + error.message(),
                started);
        }

        std::error_code path_error;
        const std::string target_path = CanonicalPathString(
            user_directory, path_error);
        if (path_error) {
            std::filesystem::remove_all(user_directory, error);
            return PreparationFailure(
                request, false,
                "session User path could not be canonicalized path="
                    + user_directory.string(),
                started);
        }
        {
            std::ofstream marker(marker_path, std::ios::binary | std::ios::trunc);
            marker << "version=2\n"
                   << "preparation_id=" << request.preparation_id << '\n'
                   << "process_generation=" << request.process_generation << '\n'
                   << "target=" << target_path << '\n';
            if (!marker) {
                marker.close();
                std::filesystem::remove(marker_path, error);
                std::filesystem::remove_all(user_directory, error);
                return PreparationFailure(
                    request, true,
                    "session marker creation failed path=" + marker_path.string(),
                    started);
            }
        }

        SessionFilesystemPreparationResult result{
            .preparation_id = request.preparation_id,
            .user_directory = user_directory,
            .marker_path = marker_path,
            .directory_count = 1,
        };
        result.ok = true;
        result.retryable = true;
        result.elapsed_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count());
        return result;
    }

    bool SessionFilesystemPreparer::Validate(
        const std::filesystem::path& user_directory,
        std::string_view preparation_id,
        std::uint64_t process_generation,
        std::string* error_out) {
        std::ifstream marker(MarkerPath(user_directory), std::ios::binary);
        std::string version;
        std::string id;
        std::string generation;
        std::string target;
        if (!std::getline(marker, version) || !std::getline(marker, id)
            || !std::getline(marker, generation) || !std::getline(marker, target)) {
            if (error_out)
                *error_out = "prepared session marker is missing or malformed";
            return false;
        }
        std::error_code error;
        const auto expected_target =
            std::filesystem::weakly_canonical(user_directory, error).generic_string();
        const bool valid = !error
            && version == "version=2"
            && id == "preparation_id=" + std::string(preparation_id)
            && generation == "process_generation=" + std::to_string(process_generation)
            && target == "target=" + expected_target
            && std::filesystem::is_directory(user_directory, error);
        if (!valid && error_out)
            *error_out = "prepared session marker identity does not match OpenSession";
        return valid;
    }

    static inline bool is_dir(const std::filesystem::path& p) {
        return !p.empty() && std::filesystem::exists(p) && std::filesystem::is_directory(p);
    }

    bool BootDolphinWrapper(savor::DolphinWrapper& dw, const BootOptions& opts, std::string* error_out)
    {
        std::string err;

        // 0) Validate inputs up-front
        if (!is_dir(opts.user_dir)) {
            if (error_out) *error_out =
                "Prepared user_dir does not exist: " + opts.user_dir.string();
            return false;
        }
        if (!is_dir(opts.dolphin_qt_base)) {
            if (error_out) *error_out = "Invalid DolphinQt base: " + opts.dolphin_qt_base.string();
            return false;
        }
        if (!std::filesystem::exists(opts.dolphin_qt_base / "portable.txt")) {
            if (error_out) *error_out = "DolphinQt base is not portable (portable.txt missing): " + opts.dolphin_qt_base.string();
            return false;
        }
        if (!is_dir(opts.dolphin_qt_base / "Sys")) {
            if (error_out) *error_out = "DolphinQt base must contain Sys/: " + opts.dolphin_qt_base.string();
            return false;
        }
        if (!SessionFilesystemPreparer::Validate(
                opts.user_dir,
                opts.session_filesystem_preparation_id,
                opts.process_generation,
                &err)) {
            if (error_out) *error_out =
                "Prepared session filesystem validation failed: " + err;
            return false;
        }

        // 1) Build wrapper and point it at our isolated User dir
        dw.SetVisualMode(opts.visual);
        dw.SetRenderWidgetHandle(opts.render_widget_handle);
        if (!dw.SetUserDirectory(opts.user_dir)) {
            if (error_out) *error_out = "SetUserDirectory() failed for: " + opts.user_dir.string();
            return false;
        }

        // 2) Require & remember the portable DolphinQt base
        if (!dw.SetDolphinQtBaseDir(opts.dolphin_qt_base, &err)) {
            if (error_out) *error_out = "SetDolphinQtBaseDir failed: " + err;
            return false;
        }

        return true;
    }

} // namespace savor
