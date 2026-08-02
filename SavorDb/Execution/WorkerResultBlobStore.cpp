#include "WorkerResultBlobStore.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <exception>
#include <fstream>
#include <limits>
#include <span>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

#include "../../SavorCore/Utils/Hash.h"

namespace savor::db::execution {
namespace {

constexpr std::string_view kWorkerResultDirectory = "worker_results";
constexpr std::string_view kWorkerResultSuffix = ".wrms-terminal-v1";
constexpr std::string_view kStagingSuffix = ".staging";

bool Fail(std::string message, std::string* error_out) {
    if (error_out != nullptr) {
        *error_out = std::move(message);
    }
    return false;
}

std::string Utf8Path(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    std::string result;
    result.reserve(encoded.size());
    for (const auto character : encoded) {
        result.push_back(static_cast<char>(character));
    }
    return result;
}

std::string PathContext(const std::filesystem::path& path) {
    return "path=\"" + Utf8Path(path) + "\", path_length="
        + std::to_string(path.native().size());
}

std::string FilesystemFailure(
    std::string_view operation,
    const std::filesystem::path& path,
    const std::error_code& error) {
    return std::string(operation) + " failed [" + PathContext(path)
        + ", os_error=" + error.category().name() + ":"
        + std::to_string(error.value()) + " (" + error.message() + ")]";
}

std::string StreamFailure(
    std::string_view operation,
    const std::filesystem::path& path,
    int saved_errno
#ifdef _WIN32
    ,
    unsigned long saved_win32_error
#endif
) {
    std::string detail = std::string(operation) + " failed ["
        + PathContext(path);
    if (saved_errno != 0) {
        const std::error_code error(saved_errno, std::generic_category());
        detail += ", errno=" + std::to_string(saved_errno) + " ("
            + error.message() + ")";
    } else {
        detail += ", errno=0";
    }
#ifdef _WIN32
    if (saved_win32_error != ERROR_SUCCESS) {
        const std::error_code error(
            static_cast<int>(saved_win32_error),
            std::system_category());
        detail += ", win32_error=" + std::to_string(saved_win32_error)
            + " (" + error.message() + ")";
    } else {
        detail += ", win32_error=0";
    }
#endif
    detail += "]";
    return detail;
}

std::string MakeFilename(std::string_view sha256) {
    return std::string(sha256) + std::string(kWorkerResultSuffix);
}

std::string StageIdentity(const WorkerResultBlobStageRequest& request) {
    return " [workset_id=" + std::to_string(request.workset_id)
        + ", dispatch_attempt_id="
        + std::to_string(request.dispatch_attempt_id)
        + ", job_id=" + std::to_string(request.job_id)
        + ", terminal_id=" + std::to_string(request.terminal_id) + "]";
}

bool MakeIoPath(
    const std::filesystem::path& ordinary_path,
    std::filesystem::path* io_path_out,
    std::string* error_out) {
    if (io_path_out == nullptr) {
        return Fail("worker result I/O path output is required", error_out);
    }

    std::error_code error;
    auto absolute_path = std::filesystem::absolute(ordinary_path, error);
    if (error) {
        return Fail(
            FilesystemFailure(
                "resolving absolute worker result path",
                ordinary_path,
                error),
            error_out);
    }
    absolute_path = absolute_path.lexically_normal();

#ifdef _WIN32
    const auto native = absolute_path.native();
    if (native.rfind(LR"(\\?\)", 0) == 0) {
        *io_path_out = absolute_path;
        return true;
    }
    if (native.rfind(LR"(\\)", 0) == 0) {
        *io_path_out = std::filesystem::path(
            std::wstring(LR"(\\?\UNC\)") + native.substr(2));
        return true;
    }
    if (native.size() < 3 || native[1] != L':'
        || (native[2] != L'\\' && native[2] != L'/')) {
        return Fail(
            "worker result path did not resolve to a drive-qualified "
            "absolute Windows path ["
                + PathContext(absolute_path) + "]",
            error_out);
    }
    *io_path_out =
        std::filesystem::path(std::wstring(LR"(\\?\)") + native);
#else
    *io_path_out = std::move(absolute_path);
#endif
    return true;
}

bool WriteBytes(
    const std::filesystem::path& path,
    std::span<const std::uint8_t> bytes,
    std::string_view description,
    std::string* error_out) {
    errno = 0;
#ifdef _WIN32
    ::SetLastError(ERROR_SUCCESS);
#endif
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        const int saved_errno = errno;
#ifdef _WIN32
        const auto saved_win32_error = ::GetLastError();
        return Fail(
            StreamFailure(
                "opening " + std::string(description),
                path,
                saved_errno,
                saved_win32_error),
            error_out);
#else
        return Fail(
            StreamFailure(
                "opening " + std::string(description),
                path,
                saved_errno),
            error_out);
#endif
    }

    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream) {
        const int saved_errno = errno;
#ifdef _WIN32
        const auto saved_win32_error = ::GetLastError();
        return Fail(
            StreamFailure(
                "writing and flushing " + std::string(description),
                path,
                saved_errno,
                saved_win32_error),
            error_out);
#else
        return Fail(
            StreamFailure(
                "writing and flushing " + std::string(description),
                path,
                saved_errno),
            error_out);
#endif
    }

    stream.close();
    if (!stream) {
        const int saved_errno = errno;
#ifdef _WIN32
        const auto saved_win32_error = ::GetLastError();
        return Fail(
            StreamFailure(
                "closing " + std::string(description),
                path,
                saved_errno,
                saved_win32_error),
            error_out);
#else
        return Fail(
            StreamFailure(
                "closing " + std::string(description),
                path,
                saved_errno),
            error_out);
#endif
    }
    return true;
}

bool ReadBytes(
    const std::filesystem::path& path,
    std::size_t size,
    std::string_view description,
    std::vector<std::uint8_t>* bytes_out,
    std::string* error_out) {
    errno = 0;
#ifdef _WIN32
    ::SetLastError(ERROR_SUCCESS);
#endif
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        const int saved_errno = errno;
#ifdef _WIN32
        const auto saved_win32_error = ::GetLastError();
        return Fail(
            StreamFailure(
                "opening " + std::string(description),
                path,
                saved_errno,
                saved_win32_error),
            error_out);
#else
        return Fail(
            StreamFailure(
                "opening " + std::string(description),
                path,
                saved_errno),
            error_out);
#endif
    }

    std::vector<std::uint8_t> bytes(size);
    stream.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!stream && !bytes.empty()) {
        const int saved_errno = errno;
#ifdef _WIN32
        const auto saved_win32_error = ::GetLastError();
        return Fail(
            StreamFailure(
                "reading " + std::string(description),
                path,
                saved_errno,
                saved_win32_error),
            error_out);
#else
        return Fail(
            StreamFailure(
                "reading " + std::string(description),
                path,
                saved_errno),
            error_out);
#endif
    }

    *bytes_out = std::move(bytes);
    return true;
}

void AppendCleanupFailure(
    std::string* message,
    std::string_view operation,
    const std::filesystem::path& path,
    const std::error_code& error) {
    if (message == nullptr || !error) {
        return;
    }
    *message += "; cleanup: "
        + FilesystemFailure(operation, path, error);
}

} // namespace

WorkerResultBlobStore::WorkerResultBlobStore(
    std::filesystem::path object_store_root)
    : object_store_root_(std::move(object_store_root))
    , worker_results_root_(object_store_root_ / kWorkerResultDirectory) {
}

const std::filesystem::path& WorkerResultBlobStore::Root() const noexcept {
    return worker_results_root_;
}

bool WorkerResultBlobStore::ValidateReady(std::string* error_out) const {
    std::lock_guard stage_lock(stage_mutex_);

    std::filesystem::path worker_results_io_root;
    if (!ResolvePrivatePath(
            kWorkerResultDirectory,
            &worker_results_io_root,
            error_out)) {
        return false;
    }

    std::error_code error;
    std::filesystem::create_directories(worker_results_io_root, error);
    if (error) {
        return Fail(
            FilesystemFailure(
                "creating worker result directory during readiness validation",
                worker_results_io_root,
                error),
            error_out);
    }

    static std::atomic<std::uint64_t> probe_counter = 0;
    std::filesystem::path final_path;
    std::filesystem::path staging_path;
    std::vector<std::uint8_t> payload;
    std::string expected_sha256;
    bool selected_unused_path = false;
    for (std::uint32_t attempt = 0; attempt < 8; ++attempt) {
        std::ostringstream probe;
        probe << "savor-worker-result-readiness-v1|"
              << std::chrono::steady_clock::now().time_since_epoch().count()
              << "|" << probe_counter.fetch_add(1)
              << "|" << std::hash<std::thread::id>{}(
                     std::this_thread::get_id())
              << "|" << reinterpret_cast<std::uintptr_t>(this)
              << "|" << attempt;
        const auto probe_text = probe.str();
        payload.assign(probe_text.begin(), probe_text.end());
        expected_sha256 = ::hash::sha256(payload.data(), payload.size());
        if (expected_sha256.size() != 64) {
            return Fail(
                "failed hashing worker result readiness payload",
                error_out);
        }

        const auto relative =
            std::filesystem::path(kWorkerResultDirectory)
            / MakeFilename(expected_sha256);
        if (!ResolvePrivatePath(
                relative.generic_string(),
                &final_path,
                error_out)) {
            return false;
        }
        staging_path = final_path;
        staging_path.concat(kStagingSuffix);

        error.clear();
        const bool final_exists = std::filesystem::exists(final_path, error);
        if (error) {
            return Fail(
                FilesystemFailure(
                    "inspecting worker result readiness destination",
                    final_path,
                    error),
                error_out);
        }
        error.clear();
        const bool staging_exists =
            std::filesystem::exists(staging_path, error);
        if (error) {
            return Fail(
                FilesystemFailure(
                    "inspecting worker result readiness staging path",
                    staging_path,
                    error),
                error_out);
        }
        if (!final_exists && !staging_exists) {
            selected_unused_path = true;
            break;
        }
    }
    if (!selected_unused_path) {
        return Fail(
            "could not allocate an unused content-addressed worker result "
            "readiness path",
            error_out);
    }

    bool published = false;
    const auto fail_probe =
        [&](std::string message) {
            std::error_code cleanup_error;
            std::filesystem::remove(staging_path, cleanup_error);
            AppendCleanupFailure(
                &message,
                "removing worker result readiness staging file",
                staging_path,
                cleanup_error);
            if (published) {
                cleanup_error.clear();
                std::filesystem::remove(final_path, cleanup_error);
                AppendCleanupFailure(
                    &message,
                    "removing published worker result readiness file",
                    final_path,
                    cleanup_error);
            }
            return Fail(std::move(message), error_out);
        };

    std::string operation_error;
    if (!WriteBytes(
            staging_path,
            payload,
            "worker result readiness staging file",
            &operation_error)) {
        return fail_probe(std::move(operation_error));
    }

    error.clear();
    std::filesystem::rename(staging_path, final_path, error);
    if (error) {
        return fail_probe(
            FilesystemFailure(
                "publishing worker result readiness blob",
                final_path,
                error));
    }
    published = true;

    error.clear();
    const auto published_size = std::filesystem::file_size(final_path, error);
    if (error) {
        return fail_probe(
            FilesystemFailure(
                "reading worker result readiness blob size",
                final_path,
                error));
    }
    if (published_size != payload.size()) {
        return fail_probe(
            "worker result readiness blob size verification failed ["
            + PathContext(final_path) + ", expected_size="
            + std::to_string(payload.size()) + ", actual_size="
            + std::to_string(published_size) + "]");
    }

    std::vector<std::uint8_t> read_payload;
    if (!ReadBytes(
            final_path,
            payload.size(),
            "worker result readiness blob",
            &read_payload,
            &operation_error)) {
        return fail_probe(std::move(operation_error));
    }
    if (read_payload != payload
        || ::hash::sha256(read_payload.data(), read_payload.size())
            != expected_sha256) {
        return fail_probe(
            "worker result readiness blob content verification failed ["
            + PathContext(final_path) + "]");
    }

    error.clear();
    const bool removed = std::filesystem::remove(final_path, error);
    if (error) {
        return fail_probe(
            FilesystemFailure(
                "deleting worker result readiness blob",
                final_path,
                error));
    }
    if (!removed) {
        published = false;
        return Fail(
            "deleting worker result readiness blob did not remove the "
            "published file ["
                + PathContext(final_path) + "]",
            error_out);
    }
    published = false;

    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

bool WorkerResultBlobStore::Stage(
    const WorkerResultBlobStageRequest& request,
    WorkerResultBlobReference* reference_out,
    std::string* error_out) const {
    if (reference_out == nullptr) {
        return Fail("worker result blob reference output is required", error_out);
    }
    if (request.workset_id <= 0 || request.dispatch_attempt_id <= 0
        || request.job_id <= 0 || request.terminal_id == 0) {
        return Fail("worker result blob identity is invalid", error_out);
    }
    if (request.envelope.empty()
        || request.envelope.size()
            > static_cast<std::size_t>(
                (std::numeric_limits<std::int64_t>::max)())) {
        return Fail(
            "worker result terminal envelope is empty or too large",
            error_out);
    }

    const auto sha256 =
        ::hash::sha256(request.envelope.data(), request.envelope.size());
    if (sha256.size() != 64) {
        return Fail("failed hashing worker result terminal envelope", error_out);
    }

    const std::filesystem::path relative =
        std::filesystem::path(kWorkerResultDirectory) / MakeFilename(sha256);
    const auto relative_key = relative.generic_string();
    const auto staging_key =
        relative_key + std::string(kStagingSuffix);
    std::filesystem::path final_path;
    if (!ResolvePrivatePath(relative_key, &final_path, error_out)) {
        return false;
    }
    auto staging_path = final_path;
    staging_path.concat(kStagingSuffix);

    PinPath(relative_key);
    PinPath(staging_key);
    const auto fail_pinned =
        [&](std::string message) {
            UnpinPath(staging_key);
            UnpinPath(relative_key);
            return Fail(
                std::move(message) + StageIdentity(request),
                error_out);
        };

    std::lock_guard stage_lock(stage_mutex_);

    std::filesystem::path worker_results_io_root;
    std::string operation_error;
    if (!ResolvePrivatePath(
            kWorkerResultDirectory,
            &worker_results_io_root,
            &operation_error)) {
        return fail_pinned(std::move(operation_error));
    }

    std::error_code error;
    std::filesystem::create_directories(worker_results_io_root, error);
    if (error) {
        return fail_pinned(
            FilesystemFailure(
                "creating worker result blob directory",
                worker_results_io_root,
                error));
    }

    error.clear();
    const bool final_exists = std::filesystem::exists(final_path, error);
    if (error) {
        return fail_pinned(
            FilesystemFailure(
                "inspecting existing worker result blob",
                final_path,
                error));
    }
    if (final_exists) {
        error.clear();
        const auto existing_size =
            std::filesystem::file_size(final_path, error);
        if (error) {
            return fail_pinned(
                FilesystemFailure(
                    "reading existing worker result blob size",
                    final_path,
                    error));
        }
        if (existing_size != request.envelope.size()) {
            return fail_pinned(
                "content-addressed worker result blob contains a different "
                "size ["
                + PathContext(final_path) + ", expected_size="
                + std::to_string(request.envelope.size())
                + ", actual_size=" + std::to_string(existing_size) + "]");
        }

        std::vector<std::uint8_t> existing_bytes;
        if (!ReadBytes(
                final_path,
                request.envelope.size(),
                "existing worker result blob",
                &existing_bytes,
                &operation_error)) {
            return fail_pinned(std::move(operation_error));
        }
        if (::hash::sha256(existing_bytes.data(), existing_bytes.size())
            != sha256) {
            return fail_pinned(
                "content-addressed worker result blob contains different "
                "bytes ["
                + PathContext(final_path) + "]");
        }
    } else {
        if (!WriteBytes(
                staging_path,
                request.envelope,
                "worker result staging file",
                &operation_error)) {
            error.clear();
            std::filesystem::remove(staging_path, error);
            if (error) {
                operation_error += "; cleanup: "
                    + FilesystemFailure(
                        "removing failed worker result staging file",
                        staging_path,
                        error);
            }
            return fail_pinned(std::move(operation_error));
        }

        error.clear();
        std::filesystem::rename(staging_path, final_path, error);
        if (error) {
            auto message = FilesystemFailure(
                "publishing worker result blob",
                final_path,
                error);
            error.clear();
            std::filesystem::remove(staging_path, error);
            AppendCleanupFailure(
                &message,
                "removing unpublished worker result staging file",
                staging_path,
                error);
            return fail_pinned(std::move(message));
        }
    }
    UnpinPath(staging_key);

    *reference_out = WorkerResultBlobReference{
        .relative_path = relative_key,
        .sha256 = sha256,
        .size_bytes = static_cast<std::int64_t>(request.envelope.size()),
        .format = kWorkerTerminalEnvelopeFormatV1,
    };
    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

bool WorkerResultBlobStore::Read(
    const WorkerResultBlobReference& reference,
    std::vector<std::uint8_t>* envelope_out,
    std::string* error_out) const {
    if (envelope_out == nullptr) {
        return Fail("worker result blob output is required", error_out);
    }
    if (reference.size_bytes < 0) {
        return Fail("worker result blob size is invalid", error_out);
    }
    std::filesystem::path path;
    if (!ResolvePrivatePath(reference.relative_path, &path, error_out)) {
        return false;
    }

    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return Fail(
            FilesystemFailure(
                "reading worker result blob size",
                path,
                error),
            error_out);
    }
    if (size
            > static_cast<std::uintmax_t>(
                (std::numeric_limits<std::size_t>::max)())
        || size
            > static_cast<std::uintmax_t>(
                (std::numeric_limits<std::int64_t>::max)())
        || static_cast<std::int64_t>(size) != reference.size_bytes) {
        return Fail(
            "worker result blob size verification failed ["
                + PathContext(path) + ", expected_size="
                + std::to_string(reference.size_bytes)
                + ", actual_size=" + std::to_string(size) + "]",
            error_out);
    }

    std::vector<std::uint8_t> bytes;
    if (!ReadBytes(
            path,
            static_cast<std::size_t>(size),
            "worker result blob",
            &bytes,
            error_out)) {
        return false;
    }
    if (::hash::sha256(bytes.data(), bytes.size()) != reference.sha256) {
        return Fail(
            "worker result blob hash verification failed ["
                + PathContext(path) + "]",
            error_out);
    }

    *envelope_out = std::move(bytes);
    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

bool WorkerResultBlobStore::Exists(
    std::string_view relative_path,
    bool* exists_out,
    std::string* error_out) const {
    if (exists_out == nullptr) {
        return Fail("worker result blob existence output is required", error_out);
    }
    std::filesystem::path path;
    if (!ResolvePrivatePath(relative_path, &path, error_out)) {
        return false;
    }
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        return Fail(
            FilesystemFailure(
                "checking worker result blob existence", path, error),
            error_out);
    }
    *exists_out = exists;
    if (error_out != nullptr) error_out->clear();
    return true;
}

bool WorkerResultBlobStore::Remove(
    std::string_view relative_path,
    std::string* error_out) const {
    std::filesystem::path path;
    if (!ResolvePrivatePath(relative_path, &path, error_out)) {
        return false;
    }
    const auto normalized =
        std::filesystem::path(relative_path).lexically_normal().generic_string();
    std::lock_guard lock(pin_mutex_);
    if (pinned_relative_paths_.contains(normalized)) {
        return Fail(
            "worker result blob is still pinned by its publisher",
            error_out);
    }
    std::error_code error;
    const bool removed = std::filesystem::remove(path, error);
    if (error) {
        return Fail(
            FilesystemFailure(
                "deleting worker result blob",
                path,
                error),
            error_out);
    }
    (void)removed; // Missing is idempotent cleanup success.
    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

void WorkerResultBlobStore::Unpin(std::string_view relative_path) const {
    UnpinPath(
        std::filesystem::path(relative_path)
            .lexically_normal()
            .generic_string());
}

void WorkerResultBlobStore::UnpinAll() const {
    std::lock_guard lock(pin_mutex_);
    pinned_relative_paths_.clear();
}

bool WorkerResultBlobStore::RemoveIfUnpinnedAndUntracked(
    std::string_view relative_path,
    const WorkerResultBlobOwnershipProbe& ownership_probe,
    bool* removed_out,
    std::string* error_out) const {
    if (removed_out == nullptr || !ownership_probe) {
        return Fail(
            "worker result removal output and ownership probe are required",
            error_out);
    }
    *removed_out = false;
    std::filesystem::path path;
    if (!ResolvePrivatePath(relative_path, &path, error_out)) {
        return false;
    }
    const auto normalized =
        std::filesystem::path(relative_path).lexically_normal().generic_string();
    std::lock_guard lock(pin_mutex_);
    if (pinned_relative_paths_.contains(normalized)) {
        if (error_out != nullptr) {
            error_out->clear();
        }
        return true;
    }

    bool tracked = false;
    try {
        if (!ownership_probe(normalized, &tracked, error_out)) {
            return false;
        }
    } catch (const std::exception& exception) {
        return Fail(
            "worker result ownership probe failed: "
                + std::string(exception.what()),
            error_out);
    } catch (...) {
        return Fail("worker result ownership probe failed", error_out);
    }
    if (tracked) {
        if (error_out != nullptr) {
            error_out->clear();
        }
        return true;
    }

    std::error_code error;
    *removed_out = std::filesystem::remove(path, error);
    if (error) {
        return Fail(
            FilesystemFailure(
                "deleting orphan worker result blob",
                path,
                error),
            error_out);
    }
    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

bool WorkerResultBlobStore::ListFilesOlderThan(
    std::chrono::milliseconds minimum_age,
    std::vector<std::string>* relative_paths_out,
    std::string* error_out) const {
    if (relative_paths_out == nullptr
        || minimum_age < std::chrono::milliseconds::zero()) {
        return Fail("invalid worker result orphan scan request", error_out);
    }
    relative_paths_out->clear();

    std::filesystem::path worker_results_io_root;
    if (!ResolvePrivatePath(
            kWorkerResultDirectory,
            &worker_results_io_root,
            error_out)) {
        return false;
    }
    std::filesystem::path object_store_io_root;
    if (!MakeIoPath(
            object_store_root_,
            &object_store_io_root,
            error_out)) {
        return false;
    }

    std::error_code error;
    const bool root_exists =
        std::filesystem::exists(worker_results_io_root, error);
    if (error) {
        return Fail(
            FilesystemFailure(
                "inspecting worker result blob directory",
                worker_results_io_root,
                error),
            error_out);
    }
    if (!root_exists) {
        if (error_out != nullptr) {
            error_out->clear();
        }
        return true;
    }

    const auto cutoff =
        std::filesystem::file_time_type::clock::now() - minimum_age;
    std::filesystem::recursive_directory_iterator cursor(
        worker_results_io_root,
        std::filesystem::directory_options::none,
        error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
        return Fail(
            FilesystemFailure(
                "opening worker result blob directory",
                worker_results_io_root,
                error),
            error_out);
    }
    while (cursor != end) {
        const auto entry_path = cursor->path();
        const auto status = cursor->symlink_status(error);
        if (error) {
            return Fail(
                FilesystemFailure(
                    "inspecting worker result blob entry",
                    entry_path,
                    error),
                error_out);
        }
        if (std::filesystem::is_symlink(status)) {
            // Directory symlinks are not followed without
            // follow_directory_symlink; file symlinks are never candidates.
        } else if (std::filesystem::is_regular_file(status)) {
            const auto modified = cursor->last_write_time(error);
            if (error) {
                return Fail(
                    FilesystemFailure(
                        "reading worker result blob timestamp",
                        entry_path,
                        error),
                    error_out);
            }
            if (modified <= cutoff) {
                const auto relative =
                    entry_path.lexically_relative(object_store_io_root);
                const auto relative_key =
                    relative.lexically_normal().generic_string();
                std::filesystem::path checked_path;
                if (!ResolvePrivatePath(
                        relative_key,
                        &checked_path,
                        error_out)) {
                    return false;
                }
                relative_paths_out->push_back(relative_key);
            }
        }
        cursor.increment(error);
        if (error) {
            return Fail(
                FilesystemFailure(
                    "enumerating worker result blob directory",
                    worker_results_io_root,
                    error),
                error_out);
        }
    }
    std::sort(relative_paths_out->begin(), relative_paths_out->end());
    relative_paths_out->erase(
        std::unique(
            relative_paths_out->begin(),
            relative_paths_out->end()),
        relative_paths_out->end());
    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

bool WorkerResultBlobStore::ResolvePrivatePath(
    std::string_view relative_path,
    std::filesystem::path* path_out,
    std::string* error_out) const {
    if (path_out == nullptr || relative_path.empty()) {
        return Fail("worker result relative path is required", error_out);
    }
    const std::filesystem::path relative(relative_path);
    if (relative.is_absolute() || relative.has_root_name()
        || relative.has_root_directory()) {
        return Fail("worker result path must be relative", error_out);
    }

    const auto normalized = relative.lexically_normal();
    auto iterator = normalized.begin();
    if (iterator == normalized.end()
        || iterator->generic_string() != kWorkerResultDirectory) {
        return Fail(
            "worker result path is outside the private worker_results store",
            error_out);
    }
    for (const auto& component : normalized) {
        if (component == "..") {
            return Fail("worker result path contains traversal", error_out);
        }
    }

    return MakeIoPath(
        object_store_root_ / normalized,
        path_out,
        error_out);
}

void WorkerResultBlobStore::PinPath(std::string relative_path) const {
    std::lock_guard lock(pin_mutex_);
    ++pinned_relative_paths_[std::move(relative_path)];
}

void WorkerResultBlobStore::UnpinPath(
    std::string_view relative_path) const {
    std::lock_guard lock(pin_mutex_);
    const auto found =
        pinned_relative_paths_.find(std::string(relative_path));
    if (found == pinned_relative_paths_.end()) {
        return;
    }
    if (found->second > 1) {
        --found->second;
        return;
    }
    pinned_relative_paths_.erase(found);
}

} // namespace savor::db::execution
