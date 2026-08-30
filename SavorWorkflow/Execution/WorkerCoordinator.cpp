#include "WorkerCoordinator.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <algorithm>
#include <charconv>
#include <exception>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

#include "Utils/Hash.h"
#include "Utils/Log.h"
#include "Utils/ModulePath.h"

namespace savor::runner::parallel::savordb {
namespace {

std::atomic<std::uint64_t> g_worker_coordinator_generation{0};

std::uint64_t NextProcessGeneration() noexcept {
    auto generation =
        g_worker_coordinator_generation.fetch_add(
            1,
            std::memory_order_relaxed)
        + 1;
    if (generation == 0) {
        generation =
            g_worker_coordinator_generation.fetch_add(
                1,
                std::memory_order_relaxed)
            + 1;
    }
    return generation;
}

bool IsActiveStartupPhase(WorkerStartupPhase phase) noexcept {
    return phase == WorkerStartupPhase::PreparingFilesystem
        || phase == WorkerStartupPhase::Launching
        || phase == WorkerStartupPhase::OpeningSession;
}

bool IsStartupPhase(WorkerStartupPhase phase) noexcept {
    return phase != WorkerStartupPhase::None
        && phase != WorkerStartupPhase::Failed;
}

bool IsTerminalWorksetState(savor::wrms::WorksetStateCode state) noexcept {
    return state == savor::wrms::WorksetStateCode::Completed
        || state == savor::wrms::WorksetStateCode::Cancelled
        || state == savor::wrms::WorksetStateCode::Failed;
}

bool RuntimeAcceptsWorksets(
    const savor::ProcessWorkerSnapshot& snapshot) noexcept {
    // Injected preflight tests may not own a live ProcessWorker transport.
    if (!snapshot.runtime_contract_received) return true;
    return snapshot.session_open
        && snapshot.worker_state != savor::runtime::WorkerState::Tainted
        && snapshot.worker_state != savor::runtime::WorkerState::Stopping
        && snapshot.worker_state != savor::runtime::WorkerState::Stopped
        && snapshot.session_disposition !=
            savor::runtime::SessionDisposition::Tainted
        && snapshot.session_disposition !=
            savor::runtime::SessionDisposition::Closed;
}

WorkerCommandDisposition MapProcessWorkerCommandDisposition(
    savor::ProcessWorkerCommandDisposition disposition) noexcept {
    using ProcessDisposition = savor::ProcessWorkerCommandDisposition;
    switch (disposition) {
    case ProcessDisposition::Accepted:
        return WorkerCommandDisposition::Accepted;
    case ProcessDisposition::LocalRejected:
        return WorkerCommandDisposition::LocalRejected;
    case ProcessDisposition::DefiniteRejected:
        return WorkerCommandDisposition::DefiniteRejected;
    case ProcessDisposition::TransportCanceledBeforeWrite:
        return WorkerCommandDisposition::TransportCanceledBeforeWrite;
    case ProcessDisposition::AmbiguousAfterWrite:
        return WorkerCommandDisposition::AmbiguousAfterWrite;
    }
    return WorkerCommandDisposition::LocalRejected;
}

std::string JoinDiagnostic(
    std::string_view prefix,
    std::string_view detail) {
    if (detail.empty()) {
        return std::string(prefix);
    }
    std::string result(prefix);
    result += ": ";
    result += detail;
    return result;
}

bool ValidateHomogeneousPoolConfiguration(
    const WorkerCoordinatorConfig& config,
    std::string* diagnostic_out) {
    if (config.desired_workers > kMaximumWorkerCount) {
        if (diagnostic_out) {
            *diagnostic_out =
                "worker count exceeds the maximum supported count of "
                + std::to_string(kMaximumWorkerCount);
        }
        return false;
    }
    const auto validated =
        savor::runtime::ValidateWorkerRuntimeContractV1(
            config.expected_runtime_contract);
    if (!validated.ok) {
        if (diagnostic_out) {
            *diagnostic_out = JoinDiagnostic(
                "homogeneous worker pool has an invalid expected runtime contract",
                validated.error.message);
        }
        return false;
    }
    if (!savor::runtime::IsNormalWorkerMode(config.worker_mode)) {
        if (diagnostic_out) {
            *diagnostic_out =
                "JobExecutionCoordinator worker pools permit only Headless or Visual mode";
        }
        return false;
    }
    if (diagnostic_out) diagnostic_out->clear();
    return true;
}

bool ValidatePreflight(
    const WorkerCoordinatorConfig& config,
    const WorkerCoordinatorRuntimePreflightResult& preflight,
    std::string* diagnostic_out) {
    auto fail = [&](std::string diagnostic) {
        if (diagnostic_out != nullptr) {
            *diagnostic_out = std::move(diagnostic);
        }
        return false;
    };

    if (!preflight.process_ready) {
        return fail(preflight.error.empty()
            ? "worker was not process-ready"
            : preflight.error);
    }
    if (!preflight.error.empty()) {
        return fail(preflight.error);
    }
    if (!preflight.runtime_contract.has_value()) {
        return fail("worker did not publish a runtime contract");
    }
    const auto contract_validation =
        savor::runtime::ValidateWorkerRuntimeContractV1(
            *preflight.runtime_contract);
    if (!contract_validation.ok) {
        return fail(JoinDiagnostic(
            "worker runtime contract is invalid",
            contract_validation.error.message));
    }
    if (*preflight.runtime_contract != config.expected_runtime_contract) {
        return fail(
            "worker runtime contract does not match the coordinator's expected homogeneous runtime identity");
    }
    if (diagnostic_out != nullptr) {
        diagnostic_out->clear();
    }
    return true;
}

std::int64_t ToTelemetryWorkerId(std::size_t worker_id) noexcept {
    if (worker_id
        > static_cast<std::size_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(worker_id);
}

std::filesystem::path WeaklyCanonicalOrAbsolute(
    const std::filesystem::path& path) {
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(path, error);
    if (!error) {
        return result;
    }
    result = std::filesystem::absolute(path, error);
    return error ? path : result;
}

std::string FileStamp(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return "missing";
    }
    const auto write_time =
        std::filesystem::last_write_time(path, error);
    const auto ticks =
        error ? 0 : write_time.time_since_epoch().count();
    return std::to_string(size) + ":" + std::to_string(ticks);
}

bool CopyTree(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::string* error_out) {
    namespace fs = std::filesystem;
    std::error_code error;
    fs::create_directories(destination, error);
    if (error) {
        if (error_out != nullptr) {
            *error_out =
                "create runtime directory failed: " + error.message();
        }
        return false;
    }

    fs::recursive_directory_iterator iterator(source, error);
    const fs::recursive_directory_iterator end;
    for (; !error && iterator != end; iterator.increment(error)) {
        const auto relative =
            fs::relative(iterator->path(), source, error);
        if (error) {
            break;
        }
        const auto target = destination / relative;
        if (iterator->is_directory(error)) {
            fs::create_directories(target, error);
        } else if (
            !error && iterator->is_regular_file(error)) {
            fs::create_directories(target.parent_path(), error);
            if (!error) {
                fs::copy_file(
                    iterator->path(),
                    target,
                    fs::copy_options::overwrite_existing,
                    error);
            }
        }
    }
    if (error) {
        if (error_out != nullptr) {
            *error_out =
                "copy runtime tree failed: " + error.message();
        }
        return false;
    }
    return true;
}

bool ReadFileToString(
    const std::filesystem::path& path,
    std::string* text_out) {
    if (text_out == nullptr) {
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    std::ostringstream text;
    text << input.rdbuf();
    *text_out = text.str();
    return true;
}

bool WriteStringToFile(
    const std::filesystem::path& path,
    const std::string& text,
    std::string* error_out) {
    std::ofstream output(
        path,
        std::ios::binary | std::ios::trunc);
    if (!output) {
        if (error_out != nullptr) {
            *error_out =
                "open runtime identity file failed: " + path.string();
        }
        return false;
    }
    output << text;
    output.flush();
    if (!output) {
        if (error_out != nullptr) {
            *error_out =
                "write runtime identity file failed: " + path.string();
        }
        return false;
    }
    output.close();
    if (!output) {
        if (error_out != nullptr) {
            *error_out =
                "close runtime identity file failed: " + path.string();
        }
        return false;
    }
    return true;
}

std::optional<std::string> DirectoryTreeStamp(
    const std::filesystem::path& root,
    std::string* error_out) {
    namespace fs = std::filesystem;
    std::error_code error;
    std::vector<std::string> entries;
    fs::recursive_directory_iterator iterator(root, error);
    const fs::recursive_directory_iterator end;
    for (; !error && iterator != end; iterator.increment(error)) {
        std::error_code item_error;
        if (iterator->is_directory(item_error)) {
            continue;
        }
        if (item_error
            || !iterator->is_regular_file(item_error)
            || item_error) {
            if (error_out != nullptr) {
                *error_out =
                    "unsupported or unreadable runtime source entry: "
                    + iterator->path().string();
            }
            return std::nullopt;
        }

        const auto size = iterator->file_size(item_error);
        if (item_error) {
            if (error_out != nullptr) {
                *error_out =
                    "read runtime source file size failed: "
                    + item_error.message();
            }
            return std::nullopt;
        }
        const auto write_time =
            iterator->last_write_time(item_error);
        if (item_error) {
            if (error_out != nullptr) {
                *error_out =
                    "read runtime source timestamp failed: "
                    + item_error.message();
            }
            return std::nullopt;
        }

        std::ostringstream entry;
        entry << iterator->path().lexically_relative(root).generic_string()
              << '|' << size
              << '|' << write_time.time_since_epoch().count();
        entries.push_back(entry.str());
    }
    if (error) {
        if (error_out != nullptr) {
            *error_out =
                "enumerate runtime source tree failed: "
                + error.message();
        }
        return std::nullopt;
    }

    std::sort(entries.begin(), entries.end());
    std::ostringstream fingerprint_input;
    fingerprint_input << "file_count=" << entries.size() << '\n';
    for (const auto& entry : entries) {
        fingerprint_input << entry << '\n';
    }
    const auto text = fingerprint_input.str();
    const auto digest = ::hash::sha256(text.data(), text.size());
    if (digest.empty()) {
        if (error_out != nullptr) {
            *error_out = "hash runtime source tree failed";
        }
        return std::nullopt;
    }
    return digest;
}

struct WorkerRuntimeSourceSnapshot {
    std::filesystem::path worker_executable;
    std::filesystem::path dolphin_base;
    std::filesystem::path sys;
    std::filesystem::path portable;
    std::string worker_executable_stamp;
    std::string sys_tree_stamp;
    std::string portable_stamp;
    std::string fingerprint;
};

std::optional<WorkerRuntimeSourceSnapshot> InspectWorkerRuntimeSource(
    const WorkerCoordinatorConfig& config,
    std::string* error_out) {
    namespace fs = std::filesystem;
    WorkerRuntimeSourceSnapshot source;
    source.worker_executable =
        WeaklyCanonicalOrAbsolute(config.worker_exe_path);
    source.dolphin_base =
        WeaklyCanonicalOrAbsolute(config.dolphin_base_dir);
    source.sys = source.dolphin_base / "Sys";
    source.portable = source.dolphin_base / "portable.txt";

    std::error_code error;
    if (!fs::is_regular_file(source.worker_executable, error)
        || error) {
        if (error_out != nullptr) {
            *error_out =
                "source SavorWorker.exe missing: "
                + source.worker_executable.string();
        }
        return std::nullopt;
    }
    error.clear();
    if (!fs::is_directory(source.sys, error) || error) {
        if (error_out != nullptr) {
            *error_out =
                "Dolphin base Sys is missing: "
                + source.sys.string();
        }
        return std::nullopt;
    }
    error.clear();
    if (!fs::is_regular_file(
            source.sys / "GC" / "dsp_coef.bin",
            error)
        || error) {
        if (error_out != nullptr) {
            *error_out =
                "Dolphin base Sys is incomplete: "
                + source.sys.string();
        }
        return std::nullopt;
    }
    error.clear();
    if (!fs::is_regular_file(source.portable, error) || error) {
        if (error_out != nullptr) {
            *error_out =
                "Dolphin base portable.txt is missing: "
                + source.dolphin_base.string();
        }
        return std::nullopt;
    }

    source.worker_executable_stamp =
        FileStamp(source.worker_executable);
    source.portable_stamp = FileStamp(source.portable);
    const auto sys_tree_stamp =
        DirectoryTreeStamp(source.sys, error_out);
    if (!sys_tree_stamp.has_value()) {
        return std::nullopt;
    }
    source.sys_tree_stamp = *sys_tree_stamp;

    std::ostringstream fingerprint_input;
    fingerprint_input
        << "savor_worker_runtime_layout_version=3\n"
        << "worker_exe=" << source.worker_executable.string() << '\n'
        << "worker_exe_stamp=" << source.worker_executable_stamp << '\n'
        << "dolphin_base=" << source.dolphin_base.string() << '\n'
        << "sys_tree_stamp=" << source.sys_tree_stamp << '\n'
        << "portable_stamp=" << source.portable_stamp << '\n';
    const auto text = fingerprint_input.str();
    source.fingerprint =
        ::hash::sha256(text.data(), text.size());
    if (source.fingerprint.empty()) {
        if (error_out != nullptr) {
            *error_out = "hash worker runtime fingerprint failed";
        }
        return std::nullopt;
    }
    return source;
}

std::string ExpectedRuntimeImageIdentity(
    const WorkerRuntimeSourceSnapshot& source) {
    std::ostringstream identity;
    identity
        << "savor_worker_runtime_image_identity_version=4\n"
        << "layout=shared_immutable\n"
        << "fingerprint=" << source.fingerprint << '\n'
        << "source_worker_exe="
        << source.worker_executable.string() << '\n'
        << "source_worker_exe_stamp="
        << source.worker_executable_stamp << '\n'
        << "dolphin_base_dir=" << source.dolphin_base.string() << '\n'
        << "sys_tree_stamp=" << source.sys_tree_stamp << '\n'
        << "portable_stamp=" << source.portable_stamp << '\n'
        << "exe_materialization=copy\n"
        << "user_template=empty\n";
    return identity.str();
}

bool ValidateRuntimeImage(
    const std::filesystem::path& runtime_root,
    const WorkerRuntimeSourceSnapshot& source,
    std::string* error_out) {
    namespace fs = std::filesystem;
    const auto worker_executable = runtime_root / "SavorWorker.exe";
    const auto runtime_sys = runtime_root / "Sys";
    const auto runtime_user = runtime_root / "User";
    const auto runtime_portable = runtime_root / "portable.txt";
    const auto identity_path =
        runtime_root / "worker-runtime.identity";

    std::string identity;
    if (!ReadFileToString(identity_path, &identity)
        || identity != ExpectedRuntimeImageIdentity(source)) {
        if (error_out != nullptr) {
            *error_out =
                "runtime image identity is missing or does not match its source";
        }
        return false;
    }

    std::error_code error;
    if (!fs::is_regular_file(worker_executable, error) || error) {
        if (error_out != nullptr) {
            *error_out = "runtime worker executable is missing";
        }
        return false;
    }
    error.clear();
    if (!fs::is_regular_file(
            runtime_sys / "GC" / "dsp_coef.bin",
            error)
        || error) {
        if (error_out != nullptr) {
            *error_out = "runtime Sys is missing or incomplete";
        }
        return false;
    }
    error.clear();
    if (!fs::is_regular_file(runtime_portable, error) || error) {
        if (error_out != nullptr) {
            *error_out = "runtime portable.txt is missing";
        }
        return false;
    }
    error.clear();
    if (!fs::is_directory(runtime_user, error) || error) {
        if (error_out != nullptr) {
            *error_out = "runtime User template is missing";
        }
        return false;
    }
    error.clear();
    if (!fs::is_empty(runtime_user, error) || error) {
        if (error_out != nullptr) {
            *error_out = "runtime User template is not empty";
        }
        return false;
    }
    return true;
}

class RuntimeMaterializationLock {
public:
    RuntimeMaterializationLock() = default;
    RuntimeMaterializationLock(const RuntimeMaterializationLock&) = delete;
    RuntimeMaterializationLock& operator=(
        const RuntimeMaterializationLock&) = delete;

    ~RuntimeMaterializationLock() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    bool Acquire(
        const std::filesystem::path& path,
        std::string* error_out) {
        const auto deadline =
            std::chrono::steady_clock::now()
            + std::chrono::minutes(5);
        while (std::chrono::steady_clock::now() < deadline) {
            handle_ = CreateFileW(
                path.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) {
                return true;
            }

            const auto error = GetLastError();
            if (error != ERROR_SHARING_VIOLATION
                && error != ERROR_LOCK_VIOLATION) {
                if (error_out != nullptr) {
                    *error_out =
                        "acquire runtime materialization lock failed: "
                        + std::to_string(error);
                }
                return false;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(25));
        }
        if (error_out != nullptr) {
            *error_out =
                "timed out waiting for runtime materialization lock";
        }
        return false;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

bool EnsureSharedWorkerRuntime(
    const WorkerCoordinatorConfig& config,
    std::filesystem::path* executable_out,
    std::string* error_out) {
    namespace fs = std::filesystem;
    if (error_out != nullptr) {
        error_out->clear();
    }
    const auto source =
        InspectWorkerRuntimeSource(config, error_out);
    if (!source.has_value()) {
        return false;
    }

    const auto configured_cache_root =
        config.worker_binary_runtime_root.empty()
        ? utils::getExecutablePath() / ".worker-runtime"
        : fs::path(config.worker_binary_runtime_root);
    const auto cache_root =
        WeaklyCanonicalOrAbsolute(configured_cache_root);
    const auto fingerprint_root =
        cache_root / source->fingerprint;
    const auto runtime_root = fingerprint_root / "runtime";
    const auto runtime_executable =
        runtime_root / "SavorWorker.exe";
    const auto staging_root =
        fingerprint_root / "runtime.pending";
    const auto lock_path =
        cache_root / (source->fingerprint + ".lock");

    std::string validation_error;
    std::error_code error;
    if (fs::exists(runtime_root, error)
        && !error
        && ValidateRuntimeImage(
            runtime_root,
            *source,
            &validation_error)) {
        if (executable_out != nullptr) {
            *executable_out = runtime_executable;
        }
        return true;
    }
    if (error) {
        if (error_out != nullptr) {
            *error_out =
                "inspect shared worker runtime failed: "
                + error.message();
        }
        return false;
    }

    fs::create_directories(cache_root, error);
    if (error) {
        if (error_out != nullptr) {
            *error_out =
                "create worker runtime cache failed: "
                + error.message();
        }
        return false;
    }

    RuntimeMaterializationLock lock;
    if (!lock.Acquire(lock_path, error_out)) {
        return false;
    }

    validation_error.clear();
    error.clear();
    if (fs::exists(runtime_root, error) && !error) {
        if (ValidateRuntimeImage(
                runtime_root,
                *source,
                &validation_error)) {
            if (executable_out != nullptr) {
                *executable_out = runtime_executable;
            }
            return true;
        }
        if (error_out != nullptr) {
            *error_out =
                "existing shared worker runtime is invalid and was left "
                "untouched: "
                + validation_error;
        }
        return false;
    }
    if (error) {
        if (error_out != nullptr) {
            *error_out =
                "inspect shared worker runtime failed: "
                + error.message();
        }
        return false;
    }

    fs::create_directories(fingerprint_root, error);
    if (error) {
        if (error_out != nullptr) {
            *error_out =
                "create worker runtime fingerprint directory failed: "
                + error.message();
        }
        return false;
    }
    fs::remove_all(staging_root, error);
    if (error) {
        if (error_out != nullptr) {
            *error_out =
                "clear stale pending worker runtime failed: "
                + error.message();
        }
        return false;
    }
    fs::create_directories(staging_root, error);
    if (error) {
        if (error_out != nullptr) {
            *error_out =
                "create pending worker runtime failed: "
                + error.message();
        }
        return false;
    }

    const auto fail_pending = [&](std::string diagnostic) {
        std::error_code cleanup_error;
        fs::remove_all(staging_root, cleanup_error);
        if (error_out != nullptr && error_out->empty()) {
            *error_out = std::move(diagnostic);
        }
        return false;
    };

    fs::copy_file(
        source->worker_executable,
        staging_root / "SavorWorker.exe",
        fs::copy_options::none,
        error);
    if (error) {
        return fail_pending(
            "copy worker executable into shared runtime failed: "
            + error.message());
    }
    if (!CopyTree(
            source->sys,
            staging_root / "Sys",
            error_out)) {
        return fail_pending(
            "copy Sys into shared worker runtime failed");
    }
    fs::create_directories(staging_root / "User", error);
    if (error) {
        return fail_pending(
            "create empty runtime User template failed: "
            + error.message());
    }
    fs::copy_file(
        source->portable,
        staging_root / "portable.txt",
        fs::copy_options::none,
        error);
    if (error) {
        return fail_pending(
            "copy runtime portable.txt failed: " + error.message());
    }

    const auto source_after_copy =
        InspectWorkerRuntimeSource(config, error_out);
    if (!source_after_copy.has_value()
        || source_after_copy->fingerprint != source->fingerprint) {
        return fail_pending(
            "worker runtime source changed while the shared image was "
            "being copied");
    }
    if (!WriteStringToFile(
            staging_root / "worker-runtime.identity",
            ExpectedRuntimeImageIdentity(*source),
            error_out)) {
        return fail_pending(
            "write shared worker runtime identity failed");
    }
    validation_error.clear();
    if (!ValidateRuntimeImage(
            staging_root,
            *source,
            &validation_error)) {
        return fail_pending(
            "pending shared worker runtime validation failed: "
            + validation_error);
    }

    for (int attempt = 0; attempt < 20; ++attempt) {
        error.clear();
        fs::rename(staging_root, runtime_root, error);
        if (!error
            || (error.value() != ERROR_ACCESS_DENIED
                && error.value() != ERROR_SHARING_VIOLATION
                && error.value() != ERROR_LOCK_VIOLATION)
            || attempt == 19) {
            break;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(25));
    }
    if (error) {
        return fail_pending(
            "publish shared worker runtime failed: "
            + error.message());
    }

    validation_error.clear();
    if (!ValidateRuntimeImage(
            runtime_root,
            *source,
            &validation_error)) {
        if (error_out != nullptr) {
            *error_out =
                "published shared worker runtime validation failed: "
                + validation_error;
        }
        return false;
    }
    if (executable_out != nullptr) {
        *executable_out = runtime_executable;
    }
    return true;
}

bool ValidateVisualSurfaceBinding(
    const WorkerVisualSurfaceBinding& surface,
    std::string* error_out) {
    const auto fail = [&](std::string message) {
        if (error_out != nullptr)
            *error_out = std::move(message);
        return false;
    };
    if (surface.worker_id >= kMaximumWorkerCount)
        return fail("visual surface worker ID exceeds the worker limit");
    if (surface.render_widget_handle == 0)
        return fail("visual surface has no native window handle");
    if (surface.surface_generation == 0)
        return fail("visual surface has no generation");
    if (surface.owner_process_id == 0)
        return fail("visual surface has no owner process");
#ifdef _WIN32
    const auto hwnd = reinterpret_cast<HWND>(
        static_cast<std::uintptr_t>(surface.render_widget_handle));
    if (!::IsWindow(hwnd))
        return fail("visual surface native window is invalid");
    DWORD actual_owner = 0;
    (void)::GetWindowThreadProcessId(hwnd, &actual_owner);
    if (actual_owner != surface.owner_process_id)
        return fail("visual surface belongs to a different process");
#endif
    if (error_out != nullptr)
        error_out->clear();
    return true;
}

} // namespace

WorkerCoordinator::WorkerCoordinator(WorkerCoordinatorConfig config)
    : config_(std::move(config))
    , desired_worker_count_(config_.desired_workers) {
}

WorkerCoordinator::~WorkerCoordinator() {
    Stop();
}

WorkerCoordinatorStartResult WorkerCoordinator::Start() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (started_.load(std::memory_order_acquire)) {
        return SnapshotStartResult();
    }

    std::string configuration_error;
    if (!ValidateHomogeneousPoolConfiguration(
            config_,
            &configuration_error)) {
        const WorkerCoordinatorStartResult failed{
            .status = WorkerCoordinatorStartStatus::StartupExhausted,
            .ready_workers = 0,
            .diagnostic = std::move(configuration_error),
        };
        {
            std::lock_guard<std::mutex> result_lock(start_result_mutex_);
            start_result_ = failed;
        }
        return failed;
    }

    stopping_.store(false, std::memory_order_release);
    paused_.store(config_.initially_paused, std::memory_order_release);
    {
        std::lock_guard<std::mutex> preparation_lock(
            runtime_preparation_mutex_);
        prepared_runtime_worker_executable_.reset();
    }
    {
        std::lock_guard<std::mutex> routes_lock(routes_mutex_);
        routes_.clear();
    }
    {
        std::lock_guard<std::mutex> workers_lock(workers_mutex_);
        workers_.clear();
        const auto desired =
            desired_worker_count_.load(std::memory_order_relaxed);
        workers_.reserve(desired);
        for (std::size_t worker_id = 0; worker_id < desired; ++worker_id) {
            workers_.push_back(MakeWorkerSlot(worker_id));
        }
    }

    started_.store(true, std::memory_order_release);
    const auto initial_slots = CopyWorkerSlots();
    if (!initial_slots.empty()) {
        // Establish only the first useful unit of capacity synchronously.
        // The lifecycle loop owns bounded asynchronous startup for the rest
        // of the requested fleet.
        (void)StartWorkerSlot(initial_slots.front());
    }

    RefreshStartResult();
    const auto start_result = SnapshotStartResult();
    lifecycle_thread_ =
        std::thread([this]() { LifecycleLoop(); });
    NotifyAvailabilityChanged();
    return start_result;
}

void WorkerCoordinator::Stop() {
    const auto fleet_stop_started = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!started_.exchange(false, std::memory_order_acq_rel)
        && !lifecycle_thread_.joinable()) {
        return;
    }

    stopping_.store(true, std::memory_order_release);
    lifecycle_lock.unlock();
    if (lifecycle_thread_.joinable()) {
        lifecycle_thread_.join();
    }

    const auto slots = CopyWorkerSlots();
    std::vector<std::thread> startup_threads;
    std::vector<std::shared_ptr<savor::ProcessWorker>> broadcast_workers;
    std::unordered_set<savor::ProcessWorker*> broadcast_worker_identities;
    const auto capture_worker = [&](const WorkerSlotPtr& slot) {
        if (!slot) {
            return;
        }
        std::shared_ptr<savor::ProcessWorker> worker;
        {
            std::lock_guard<std::mutex> slot_lock(slot->mutex);
            worker = slot->worker;
        }
        if (worker
            && broadcast_worker_identities.insert(worker.get()).second) {
            broadcast_workers.push_back(std::move(worker));
        }
    };
    for (const auto& slot : slots) {
        if (!slot) {
            continue;
        }
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        if (slot->startup_thread.joinable()) {
            startup_threads.push_back(
                std::move(slot->startup_thread));
        }
    }
    for (const auto& slot : slots) {
        capture_worker(slot);
    }
    const auto broadcast_started = std::chrono::steady_clock::now();
    for (const auto& worker : broadcast_workers) {
        worker->begin_stop();
    }
    const auto initial_broadcast_count = broadcast_workers.size();
    const auto broadcast_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - broadcast_started);
    SCLOGIX(
        SC_TAGS("worker.shutdown.broadcast"),
        "shutdown broadcast workers=%zu duration_ms=%lld",
        initial_broadcast_count,
        static_cast<long long>(broadcast_elapsed.count()));
    for (auto& startup_thread : startup_threads) {
        if (startup_thread.joinable()) {
            startup_thread.join();
        }
    }
    for (const auto& slot : slots) {
        const auto before = broadcast_workers.size();
        capture_worker(slot);
        if (broadcast_workers.size() != before) {
            broadcast_workers.back()->begin_stop();
        }
    }
    for (const auto& slot : slots) {
        StopWorkerSlot(slot);
    }
    std::size_t graceful_workers = 0;
    std::size_t forced_workers = 0;
    for (const auto& worker : broadcast_workers) {
        const auto stopped = worker->last_stop_snapshot();
        graceful_workers += stopped.graceful ? 1u : 0u;
        forced_workers += stopped.forced ? 1u : 0u;
    }
    {
        std::lock_guard<std::mutex> routes_lock(routes_mutex_);
        routes_.clear();
    }
    {
        std::lock_guard<std::mutex> workers_lock(workers_mutex_);
        workers_.clear();
    }
    {
        std::lock_guard<std::mutex> result_lock(start_result_mutex_);
        start_result_ = {};
    }
    NotifyAvailabilityChanged();

    const auto fleet_stop_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - fleet_stop_started);
    SCLOGIX(
        SC_TAGS("worker.shutdown.complete"),
        "shutdown complete workers=%zu late_workers=%zu graceful=%zu forced=%zu duration_ms=%lld",
        broadcast_workers.size(),
        broadcast_workers.size() - initial_broadcast_count,
        graceful_workers,
        forced_workers,
        static_cast<long long>(fleet_stop_elapsed.count()));

    lifecycle_lock.lock();
    stopping_.store(false, std::memory_order_release);
}

void WorkerCoordinator::SetPaused(bool paused) {
    paused_.store(paused, std::memory_order_release);
    NotifyAvailabilityChanged();
}

bool WorkerCoordinator::IsPaused() const noexcept {
    return paused_.load(std::memory_order_acquire);
}

bool WorkerCoordinator::SetDesiredWorkerCount(
    std::size_t desired_workers,
    std::string* error_out) {
    if (desired_workers > kMaximumWorkerCount) {
        if (error_out != nullptr)
            *error_out = "desired worker count exceeds the maximum of 40";
        return false;
    }
    desired_worker_count_.store(
        desired_workers,
        std::memory_order_release);
    if (error_out != nullptr)
        error_out->clear();
    NotifyAvailabilityChanged();
    return true;
}

std::size_t WorkerCoordinator::DesiredWorkerCount() const noexcept {
    return desired_worker_count_.load(std::memory_order_acquire);
}

bool WorkerCoordinator::IsStarted() const noexcept {
    return started_.load(std::memory_order_acquire)
        && !stopping_.load(std::memory_order_acquire);
}

const savor::runtime::WorkerRuntimeContractV1&
WorkerCoordinator::RuntimeContract() const noexcept {
    return config_.expected_runtime_contract;
}

void WorkerCoordinator::SetCallbacks(
    WorkerCoordinatorCallbacks callbacks) {
    std::lock_guard<std::mutex> callback_lock(callbacks_mutex_);
    callbacks_ = std::move(callbacks);
}

std::vector<ReadyWorkerDispatchSnapshot>
WorkerCoordinator::SnapshotReadyWorkers() const {
    std::vector<ReadyWorkerDispatchSnapshot> snapshots;
    const auto slots = CopyWorkerSlots();
    snapshots.reserve(slots.size());
    for (const auto& slot : slots) {
        if (!slot) {
            continue;
        }
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        if (!slot->ready || slot->runtime_contract_sha256.empty()) {
            continue;
        }
        auto snapshot = SnapshotReadyWorker(*slot);
        if (paused_.load(std::memory_order_acquire)
            || stopping_.load(std::memory_order_acquire)) {
            snapshot.accepting_workset = false;
        }
        snapshots.push_back(std::move(snapshot));
    }
    return snapshots;
}

std::vector<WorkerSnapshot> WorkerCoordinator::SnapshotWorkers() const {
    auto snapshots = worker_status_.GetClusterSnapshot();
    const auto slots = CopyWorkerSlots();
    for (auto& snapshot : snapshots) {
        if (snapshot.worker_id < 0)
            continue;
        const auto index = static_cast<std::size_t>(snapshot.worker_id);
        if (index >= slots.size() || !slots[index])
            continue;
        std::lock_guard<std::mutex> slot_lock(slots[index]->mutex);
        if (slots[index]->id != index)
            continue;
        snapshot.process_generation = slots[index]->process_generation;
        snapshot.log_path = slots[index]->log_path;
        snapshot.startup_phase = slots[index]->startup_phase;
    }
    return snapshots;
}

FleetStartupSnapshot WorkerCoordinator::SnapshotFleetStartup() const {
    FleetStartupSnapshot snapshot{};
    snapshot.desired =
        desired_worker_count_.load(std::memory_order_acquire);
    const auto maximum_attempts =
        std::max<std::uint32_t>(
            1,
            config_.max_worker_start_attempts);
    const auto slots = CopyWorkerSlots();
    snapshot.worker_slots.reserve(slots.size());
    for (const auto& slot : slots) {
        if (!slot) {
            continue;
        }
        FleetStartupSlotSnapshot slot_snapshot{};
        {
            std::lock_guard<std::mutex> slot_lock(slot->mutex);
            slot_snapshot.worker_id = slot->id;
            slot_snapshot.attempt_count =
                slot->observed_start_attempts;
            slot_snapshot.maximum_attempts = maximum_attempts;
            slot_snapshot.ready =
                slot->ready && !slot->runtime_contract_sha256.empty();
            slot_snapshot.startup_phase = slot->startup_phase;
            slot_snapshot.starting = IsStartupPhase(slot->startup_phase);
            slot_snapshot.exhausted =
                !slot_snapshot.ready
                && !slot_snapshot.starting
                && (slot->start_retry_exhausted
                    || slot->start_attempts >= maximum_attempts);
            slot_snapshot.retry_pending =
                !slot_snapshot.ready
                && !slot_snapshot.starting
                && !slot_snapshot.exhausted;
            if (slot_snapshot.exhausted) {
                slot_snapshot.terminal_diagnostic =
                    slot->last_start_error;
            }
        }
        snapshot.ready += slot_snapshot.ready ? 1u : 0u;
        snapshot.starting += slot_snapshot.starting ? 1u : 0u;
        snapshot.retry_pending +=
            slot_snapshot.retry_pending ? 1u : 0u;
        snapshot.exhausted += slot_snapshot.exhausted ? 1u : 0u;
        snapshot.worker_slots.push_back(std::move(slot_snapshot));
    }
    return snapshot;
}

WorkerCoordinatorTelemetry WorkerCoordinator::SnapshotTelemetry() const {
    return {
        .start_attempts = start_attempts_.load(std::memory_order_relaxed),
        .start_failures = start_failures_.load(std::memory_order_relaxed),
        .worker_losses = worker_losses_.load(std::memory_order_relaxed),
        .submit_attempts = submit_attempts_.load(std::memory_order_relaxed),
        .submit_accepted = submit_accepted_.load(std::memory_order_relaxed),
        .submit_ambiguous =
            submit_ambiguous_.load(std::memory_order_relaxed),
        .submit_rejected = submit_rejected_.load(std::memory_order_relaxed),
        .submit_temporary_unavailable =
            submit_temporary_unavailable_.load(std::memory_order_relaxed),
        .submit_stale_generation =
            submit_stale_generation_.load(std::memory_order_relaxed),
        .submit_deterministic_rejection =
            submit_deterministic_rejection_.load(std::memory_order_relaxed),
        .submit_transport_canceled_before_write =
            submit_transport_canceled_before_write_.load(
                std::memory_order_relaxed),
        .item_cancellation_attempts =
            item_cancellation_attempts_.load(std::memory_order_relaxed),
        .item_cancellation_accepted =
            item_cancellation_accepted_.load(std::memory_order_relaxed),
        .terminal_ack_attempts =
            terminal_ack_attempts_.load(std::memory_order_relaxed),
        .terminal_ack_accepted =
            terminal_ack_accepted_.load(std::memory_order_relaxed),
        .terminal_envelopes =
            terminal_envelopes_.load(std::memory_order_relaxed),
        .liveness_probe_attempts =
            liveness_probe_attempts_.load(std::memory_order_relaxed),
        .liveness_probe_failures =
            liveness_probe_failures_.load(std::memory_order_relaxed),
        .liveness_quarantines =
            liveness_quarantines_.load(std::memory_order_relaxed),
    };
}

WorkerCoordinatorStartResult
WorkerCoordinator::SnapshotStartResult() const {
    std::lock_guard<std::mutex> result_lock(start_result_mutex_);
    return start_result_;
}

bool WorkerCoordinator::ConfirmWorksetResidence(
    WorkerExecutionTarget target,
    savor::runtime::WorkerWorksetId expected_workset_id,
    savor::wrms::WorksetResidenceSnapshotV1* snapshot_out,
    std::string* diagnostic_out) {
    const auto slot = GetWorkerSlot(target.worker_id);
    std::shared_ptr<savor::ProcessWorker> worker;
    if (!slot || !expected_workset_id) {
        if (diagnostic_out) {
            *diagnostic_out = "invalid worker residence request";
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        if (!slot->ready
            || slot->process_generation != target.process_generation
            || slot->quarantine_requested
            || !slot->worker) {
            if (diagnostic_out) {
                *diagnostic_out =
                    "worker generation is not ready for residence evidence";
            }
            return false;
        }
        worker = slot->worker;
    }

    ++liveness_probe_attempts_;
    savor::wrms::CommandResultPayload result;
    if (!worker->probe_liveness(
            &result,
            std::max<std::uint32_t>(
                1,
                config_.liveness_probe_timeout_ms))) {
        ++liveness_probe_failures_;
        const auto diagnostic =
            "workset residence liveness probe failed: "
            + worker->last_error();
        QuarantineWorkerGeneration(target, diagnostic);
        if (diagnostic_out) *diagnostic_out = diagnostic;
        return false;
    }

    savor::wrms::WorksetResidenceSnapshotV1 snapshot{};
    const auto decoded = savor::wrms::DecodePayload(
        result.result,
        snapshot);
    if (!decoded
        || !snapshot.has_resident_workset
        || snapshot.workset_id != expected_workset_id.value()) {
        std::ostringstream diagnostic;
        diagnostic << "worker residence evidence mismatch: expected="
                   << expected_workset_id.value() << " observed=";
        if (decoded && snapshot.has_resident_workset) {
            diagnostic << snapshot.workset_id;
        } else {
            diagnostic << "none";
        }
        QuarantineWorkerGeneration(target, diagnostic.str());
        if (snapshot_out) *snapshot_out = snapshot;
        if (diagnostic_out) *diagnostic_out = diagnostic.str();
        return false;
    }
    if (snapshot_out) *snapshot_out = snapshot;
    if (diagnostic_out) diagnostic_out->clear();
    return true;
}

void WorkerCoordinator::QuarantineWorkerGeneration(
    WorkerExecutionTarget target,
    std::string diagnostic) {
    const auto slot = GetWorkerSlot(target.worker_id);
    if (!slot) return;
    bool changed = false;
    {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        if (slot->process_generation == target.process_generation
            && slot->ready
            && !slot->quarantine_requested) {
            slot->ready = false;
            slot->quarantine_requested = true;
            slot->quarantine_diagnostic = std::move(diagnostic);
            changed = true;
        }
    }
    if (changed) {
        ++liveness_quarantines_;
        NotifyAvailabilityChanged();
    }
}

namespace {
std::optional<std::int64_t> ParseTelemetryJobId(
    const std::string& value) {
    std::int64_t parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size() || parsed <= 0) {
        return std::nullopt;
    }
    return parsed;
}
} // namespace

WorkerSubmitResult WorkerCoordinator::SubmitWorksetToWorker(
    WorkerExecutionTarget target,
    const savor::runtime::WorkerWorksetDefinition& workset,
    const savor::runtime::InitialWorksetCancellationSidecarV1&
        initial_cancellations) {
    ++submit_attempts_;
    if (!IsStarted()
        || paused_.load(std::memory_order_acquire)) {
        ++submit_rejected_;
        ++submit_transport_canceled_before_write_;
        return {
            .disposition =
                WorkerSubmitDisposition::CoordinatorNotAccepting,
            .diagnostic =
                "worker coordinator is stopped, stopping, or paused",
        };
    }
    if (!workset.workset_id
        || !workset.execution_key
        || workset.items.empty()
        || !savor::runtime::ValidateInitialWorksetCancellationSidecar(
                workset, initial_cancellations).ok) {
        ++submit_rejected_;
        ++submit_deterministic_rejection_;
        return {
            .disposition = WorkerSubmitDisposition::InvalidWorkset,
            .rejection_code =
                savor::wrms::RejectionCode::InvalidArgument,
            .error_code = "InvalidWorkset",
            .diagnostic =
                "workset identity, execution key, or item population is empty",
        };
    }

    const auto dispatch = DispatchInfoOf(workset);
    const auto slot = GetWorkerSlot(target.worker_id);
    if (!slot) {
        ++submit_rejected_;
        ++submit_stale_generation_;
        return {
            .disposition = WorkerSubmitDisposition::StaleGeneration,
            .worker_id = target.worker_id,
            .process_generation = target.process_generation,
            .diagnostic =
                "the explicitly targeted physical worker slot does not exist",
        };
    }

    const auto workset_id = workset.workset_id.value();
    bool idempotent_retry = false;
    {
        std::lock_guard<std::mutex> routes_lock(routes_mutex_);
        const auto route_it = routes_.find(workset_id);
        if (route_it != routes_.end()) {
            if (route_it->second.worker_id != target.worker_id
                || route_it->second.process_generation
                    != target.process_generation) {
                ++submit_rejected_;
                ++submit_deterministic_rejection_;
                return {
                    .disposition = WorkerSubmitDisposition::DuplicateWorkset,
                    .worker_id = route_it->second.worker_id,
                    .process_generation =
                        route_it->second.process_generation,
                    .rejection_code =
                        savor::wrms::RejectionCode::WorksetAlreadyActive,
                    .error_code = "DuplicateWorksetRoute",
                    .diagnostic =
                        "workset identity is routed to another worker generation",
                };
            }
            idempotent_retry = true;
        }
    }
    std::unique_lock<std::mutex> submission_lock(
        slot->submission_mutex,
        std::try_to_lock);
    if (!submission_lock.owns_lock()) {
        ++submit_rejected_;
        ++submit_temporary_unavailable_;
        return {
            .disposition =
                WorkerSubmitDisposition::TargetTemporarilyUnavailable,
            .worker_id = target.worker_id,
            .process_generation = target.process_generation,
            .diagnostic =
                "the explicitly targeted worker is processing another command",
        };
    }

    std::shared_ptr<savor::ProcessWorker> worker;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        generation = slot->process_generation;
        if (generation != target.process_generation) {
            ++submit_rejected_;
            ++submit_stale_generation_;
            return {
                .disposition = WorkerSubmitDisposition::StaleGeneration,
                .worker_id = slot->id,
                .process_generation = generation,
                .diagnostic =
                    "the explicitly targeted worker generation has been replaced",
            };
        }
        if (!slot->ready
            || slot->quarantine_requested
            || slot->submission_in_progress
            || (slot->active_workset_id.has_value()
                && !(idempotent_retry
                    && *slot->active_workset_id == workset_id))
            || !slot->worker
            || slot->runtime_contract_sha256.empty()
            || !savor::runtime::IsNormalWorkerMode(slot->mode)
            || slot->available_item_credits < dispatch.item_count) {
            ++submit_rejected_;
            ++submit_temporary_unavailable_;
            return {
                .disposition =
                    WorkerSubmitDisposition::TargetTemporarilyUnavailable,
                .worker_id = slot->id,
                .process_generation = generation,
                .diagnostic =
                    "the explicitly targeted worker is not immediately accepting",
            };
        }
        const auto process_snapshot = slot->worker->latest_snapshot();
        if (!RuntimeAcceptsWorksets(process_snapshot)) {
            ++submit_rejected_;
            ++submit_temporary_unavailable_;
            return {
                .disposition =
                    WorkerSubmitDisposition::TargetTemporarilyUnavailable,
                .worker_id = slot->id,
                .process_generation = generation,
                .rejection_code =
                    savor::wrms::RejectionCode::SessionTainted,
                .error_code = "WorkerRuntimeNotReusable",
                .diagnostic = process_snapshot.last_error.empty()
                    ? "the worker runtime is not reusable"
                    : process_snapshot.last_error,
            };
        }
        const auto validation =
            savor::runtime::ValidateWorkerWorksetDefinition(
                workset,
                config_.expected_runtime_contract.limits);
        if (!validation.ok) {
            ++submit_rejected_;
            ++submit_deterministic_rejection_;
            return {
                .disposition = WorkerSubmitDisposition::InvalidWorkset,
                .worker_id = slot->id,
                .process_generation = generation,
                .rejection_code =
                    savor::wrms::RejectionCode::InvalidArgument,
                .error_code = "LocalWorksetValidationFailed",
                .diagnostic = validation.error.message,
            };
        }
        worker = slot->worker;
        slot->submission_in_progress = true;
        slot->submitting_workset_id = workset_id;
    }

    if (!idempotent_retry) {
        WorksetRoute route{
            .worker_id = slot->id,
            .process_generation = generation,
        };
        route.job_ids_by_ordinal.resize(workset.items.size());
        route.program_kind = static_cast<int>(
            workset.phase_invocation.program_package.identity.program_kind);
        for (const auto& item : workset.items) {
            if (item.ordinal < route.job_ids_by_ordinal.size()) {
                route.job_ids_by_ordinal[item.ordinal] =
                    ParseTelemetryJobId(item.correlation.durable_job_id);
            }
        }
        std::lock_guard<std::mutex> routes_lock(routes_mutex_);
        const auto [route_it, inserted] = routes_.emplace(
            workset_id,
            std::move(route));
        (void)route_it;
        if (!inserted) {
            std::lock_guard<std::mutex> slot_lock(slot->mutex);
            slot->submission_in_progress = false;
            slot->submitting_workset_id.reset();
            ++submit_rejected_;
            ++submit_deterministic_rejection_;
            return {
                .disposition = WorkerSubmitDisposition::DuplicateWorkset,
                .worker_id = slot->id,
                .process_generation = generation,
                .rejection_code =
                    savor::wrms::RejectionCode::WorksetAlreadyActive,
                .error_code = "DuplicateWorksetRoute",
                .diagnostic =
                    "workset identity is already routed to a worker",
            };
        }
    }

    const auto outcome = worker->submit_workset_with_outcome(
        workset,
        initial_cancellations,
        0);
    std::optional<savor::runtime::SubmitWorksetResultV1>
        submission_receipt;
    bool submission_receipt_valid = true;
    if (outcome.disposition
        == savor::ProcessWorksetSubmitDisposition::Accepted) {
        savor::wrms::SubmitWorksetResultPayload payload{};
        const auto decoded = savor::wrms::DecodePayload(
            outcome.result.result, payload);
        if (!decoded) {
            submission_receipt_valid = false;
        } else {
            submission_receipt = savor::runtime::SubmitWorksetResultV1{
            .workset_id = savor::runtime::WorkerWorksetId{
                payload.workset_id},
            .sidecar_version = payload.sidecar_version,
            .applied_item_count = payload.applied_item_count,
            .applied_sidecar_sha256 =
                std::move(payload.applied_sidecar_sha256),
            .disposition = payload.already_admitted
                ? savor::runtime::WorksetSubmissionDispositionV1::
                    AlreadyAdmitted
                : savor::runtime::WorksetSubmissionDispositionV1::
                    Admitted,
            };
        }
    }
    bool terminal_state_already_observed = false;
    {
        std::lock_guard<std::mutex> routes_lock(routes_mutex_);
        const auto route_it = routes_.find(workset_id);
        const bool route_is_current =
            route_it != routes_.end()
            && route_it->second.worker_id == slot->id
            && route_it->second.process_generation == generation;
        terminal_state_already_observed =
            !route_is_current || route_it->second.terminal_state_observed;
    }
    {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        if (slot->process_generation == generation) {
            slot->submission_in_progress = false;
            slot->submitting_workset_id.reset();
            if (outcome.disposition
                != savor::ProcessWorksetSubmitDisposition::DefiniteRejected) {
                if (!terminal_state_already_observed) {
                    slot->active_workset_id = workset_id;
                }
                slot->warm_execution_key_sha256 =
                    workset.execution_key.canonical_sha256;
                slot->warm_program_package_sha256 =
                    dispatch.program_package_sha256;
                ++slot->accepted_worksets;
                worker_status_.UpdateState(
                    ToTelemetryWorkerId(slot->id),
                    terminal_state_already_observed
                        ? WorkerStateKind::Idle
                        : WorkerStateKind::Running);
            }
        }
    }

    if (outcome.disposition
        == savor::ProcessWorksetSubmitDisposition::DefiniteRejected) {
        {
            std::lock_guard<std::mutex> routes_lock(routes_mutex_);
            routes_.erase(workset_id);
        }
        ++submit_rejected_;
        if (outcome.result.error_code == "WorksetSubmissionNotWritten") {
            ++submit_transport_canceled_before_write_;
        } else {
            ++submit_deterministic_rejection_;
        }
        NotifyAvailabilityChanged();
        return {
            .disposition = WorkerSubmitDisposition::DefiniteRejected,
            .worker_id = slot->id,
            .process_generation = generation,
            .rejection_code = outcome.result.rejection_code,
            .error_code = outcome.result.error_code,
            .diagnostic = outcome.diagnostic,
        };
    }

    NotifyAvailabilityChanged();
    if (outcome.disposition
        == savor::ProcessWorksetSubmitDisposition::AmbiguousAfterWrite) {
        ++submit_ambiguous_;
        return {
            .disposition = WorkerSubmitDisposition::AmbiguousAfterWrite,
            .worker_id = slot->id,
            .process_generation = generation,
            .diagnostic = outcome.diagnostic,
        };
    }
    if (!submission_receipt_valid) {
        ++submit_ambiguous_;
        return {
            .disposition = WorkerSubmitDisposition::AmbiguousAfterWrite,
            .worker_id = slot->id,
            .process_generation = generation,
            .error_code = "MalformedWorksetSubmissionReceipt",
            .diagnostic =
                "worker accepted SubmitWorkset without a valid typed receipt",
        };
    }
    ++submit_accepted_;
    return {
        .disposition = WorkerSubmitDisposition::Accepted,
        .worker_id = slot->id,
        .process_generation = generation,
        .diagnostic = outcome.diagnostic,
        .submission_receipt = std::move(submission_receipt),
    };
}

WorkerCommandResult WorkerCoordinator::CancelWorksetItem(
    savor::runtime::WorkerWorksetId workset_id,
    savor::runtime::WorkerWorksetItemId item_id,
    std::string reason) {
    ++item_cancellation_attempts_;
    if (!started_.load(std::memory_order_acquire)
        && !stopping_.load(std::memory_order_acquire)) {
        return {
            .disposition =
                WorkerCommandDisposition::CoordinatorStopped,
            .command_kind = WorkerCommandKind::CancelWorksetItem,
            .diagnostic = "worker coordinator is stopped",
        };
    }

    WorksetRoute route;
    {
        std::lock_guard<std::mutex> routes_lock(routes_mutex_);
        const auto route_it = routes_.find(workset_id.value());
        if (route_it == routes_.end()) {
            return {
                .disposition = WorkerCommandDisposition::NotFound,
                .command_kind = WorkerCommandKind::CancelWorksetItem,
                .diagnostic = "workset is not routed to a worker",
            };
        }
        route = route_it->second;
    }
    const auto slot = GetWorkerSlot(route.worker_id);
    if (!slot) {
        return {
            .disposition = WorkerCommandDisposition::StaleRoute,
            .command_kind = WorkerCommandKind::CancelWorksetItem,
            .worker_id = route.worker_id,
            .process_generation = route.process_generation,
            .diagnostic = "routed worker no longer exists",
        };
    }

    std::lock_guard<std::mutex> submission_lock(slot->submission_mutex);
    std::shared_ptr<savor::ProcessWorker> worker;
    {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        if (slot->process_generation != route.process_generation
            || !slot->worker) {
            return {
                .disposition = WorkerCommandDisposition::StaleRoute,
                .command_kind = WorkerCommandKind::CancelWorksetItem,
                .worker_id = route.worker_id,
                .process_generation = route.process_generation,
                .diagnostic = "workset route refers to an old worker process",
            };
        }
        worker = slot->worker;
    }

    auto outcome = worker->cancel_workset_item_with_outcome(
        workset_id, item_id, std::move(reason), 0);
    if (!outcome.accepted()) {
        return {
            .disposition = MapProcessWorkerCommandDisposition(
                outcome.disposition),
            .command_kind = WorkerCommandKind::CancelWorksetItem,
            .worker_id = route.worker_id,
            .process_generation = route.process_generation,
            .request_frame_written = outcome.request_frame_written,
            .correlated_result_received =
                outcome.correlated_result_received,
            .rejection_code = outcome.result.rejection_code,
            .error_code = outcome.result.error_code,
            .diagnostic = outcome.diagnostic,
        };
    }
    ++item_cancellation_accepted_;
    return {
        .disposition = WorkerCommandDisposition::Accepted,
        .command_kind = WorkerCommandKind::CancelWorksetItem,
        .worker_id = route.worker_id,
        .process_generation = route.process_generation,
        .request_frame_written = outcome.request_frame_written,
        .correlated_result_received =
            outcome.correlated_result_received,
        .diagnostic = outcome.result.message,
    };
}

WorkerCommandResult WorkerCoordinator::CancelWorkset(
    savor::runtime::WorkerWorksetId workset_id,
    std::string reason) {
    if (!started_.load(std::memory_order_acquire)
        && !stopping_.load(std::memory_order_acquire)) {
        return {
            .disposition =
                WorkerCommandDisposition::CoordinatorStopped,
            .command_kind = WorkerCommandKind::CancelWorkset,
            .diagnostic = "worker coordinator is stopped",
        };
    }

    WorksetRoute route;
    {
        std::lock_guard<std::mutex> routes_lock(routes_mutex_);
        const auto route_it = routes_.find(workset_id.value());
        if (route_it == routes_.end()) {
            return {
                .disposition = WorkerCommandDisposition::NotFound,
                .command_kind = WorkerCommandKind::CancelWorkset,
                .diagnostic = "workset is not routed to a worker",
            };
        }
        route = route_it->second;
    }
    const auto slot = GetWorkerSlot(route.worker_id);
    if (!slot) {
        return {
            .disposition = WorkerCommandDisposition::StaleRoute,
            .command_kind = WorkerCommandKind::CancelWorkset,
            .worker_id = route.worker_id,
            .process_generation = route.process_generation,
            .diagnostic = "routed worker no longer exists",
        };
    }

    std::lock_guard<std::mutex> submission_lock(slot->submission_mutex);
    std::shared_ptr<savor::ProcessWorker> worker;
    {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        if (slot->process_generation != route.process_generation
            || !slot->worker) {
            return {
                .disposition = WorkerCommandDisposition::StaleRoute,
                .command_kind = WorkerCommandKind::CancelWorkset,
                .worker_id = route.worker_id,
                .process_generation = route.process_generation,
                .diagnostic = "workset route refers to an old worker process",
            };
        }
        worker = slot->worker;
    }

    auto outcome = worker->cancel_workset_with_outcome(
        workset_id, std::move(reason), 0);
    if (!outcome.accepted()) {
        return {
            .disposition = MapProcessWorkerCommandDisposition(
                outcome.disposition),
            .command_kind = WorkerCommandKind::CancelWorkset,
            .worker_id = route.worker_id,
            .process_generation = route.process_generation,
            .request_frame_written = outcome.request_frame_written,
            .correlated_result_received =
                outcome.correlated_result_received,
            .rejection_code = outcome.result.rejection_code,
            .error_code = outcome.result.error_code,
            .diagnostic = outcome.diagnostic,
        };
    }
    return {
        .disposition = WorkerCommandDisposition::Accepted,
        .command_kind = WorkerCommandKind::CancelWorkset,
        .worker_id = route.worker_id,
        .process_generation = route.process_generation,
        .request_frame_written = outcome.request_frame_written,
        .correlated_result_received =
            outcome.correlated_result_received,
        .diagnostic = outcome.result.message,
    };
}

WorkerCommandResult WorkerCoordinator::AcknowledgeTerminal(
    const savor::runtime::WorkerItemTerminalCorrelation& terminal) {
    ++terminal_ack_attempts_;
    if (!started_.load(std::memory_order_acquire)
        && !stopping_.load(std::memory_order_acquire)) {
        return {
            .disposition =
                WorkerCommandDisposition::CoordinatorStopped,
            .command_kind = WorkerCommandKind::AcknowledgeTerminal,
            .diagnostic = "worker coordinator is stopped",
        };
    }

    WorksetRoute route;
    const auto terminal_key = TerminalKey(terminal);
    {
        std::lock_guard<std::mutex> routes_lock(routes_mutex_);
        const auto route_it =
            routes_.find(terminal.workset_id.value());
        if (route_it == routes_.end()
            || !route_it->second.retained_terminals.contains(
                terminal_key)) {
            return {
                .disposition = WorkerCommandDisposition::NotFound,
                .command_kind = WorkerCommandKind::AcknowledgeTerminal,
                .diagnostic =
                    "terminal is not retained by a routed worker",
            };
        }
        route = route_it->second;
    }
    const auto slot = GetWorkerSlot(route.worker_id);
    if (!slot) {
        return {
            .disposition = WorkerCommandDisposition::StaleRoute,
            .command_kind = WorkerCommandKind::AcknowledgeTerminal,
            .worker_id = route.worker_id,
            .process_generation = route.process_generation,
            .diagnostic = "routed worker no longer exists",
        };
    }

    std::lock_guard<std::mutex> submission_lock(slot->submission_mutex);
    std::shared_ptr<savor::ProcessWorker> worker;
    {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        if (slot->process_generation != route.process_generation
            || !slot->worker) {
            return {
                .disposition = WorkerCommandDisposition::StaleRoute,
                .command_kind = WorkerCommandKind::AcknowledgeTerminal,
                .worker_id = route.worker_id,
                .process_generation = route.process_generation,
                .diagnostic = "terminal route refers to an old worker process",
            };
        }
        worker = slot->worker;
    }

    auto outcome = worker->acknowledge_terminal_with_outcome(terminal, 0);
    if (!outcome.accepted()) {
        return {
            .disposition = MapProcessWorkerCommandDisposition(
                outcome.disposition),
            .command_kind = WorkerCommandKind::AcknowledgeTerminal,
            .worker_id = route.worker_id,
            .process_generation = route.process_generation,
            .request_frame_written = outcome.request_frame_written,
            .correlated_result_received =
                outcome.correlated_result_received,
            .rejection_code = outcome.result.rejection_code,
            .error_code = outcome.result.error_code,
            .diagnostic = outcome.diagnostic,
        };
    }

    {
        std::lock_guard<std::mutex> routes_lock(routes_mutex_);
        const auto route_it =
            routes_.find(terminal.workset_id.value());
        if (route_it != routes_.end()
            && route_it->second.worker_id == route.worker_id
            && route_it->second.process_generation
                == route.process_generation) {
            route_it->second.retained_terminals.erase(terminal_key);
        }
    }
    ++terminal_ack_accepted_;
    RemoveCompletedRouteIfPossible(terminal.workset_id.value());
    return {
        .disposition = WorkerCommandDisposition::Accepted,
        .command_kind = WorkerCommandKind::AcknowledgeTerminal,
        .worker_id = route.worker_id,
        .process_generation = route.process_generation,
        .request_frame_written = outcome.request_frame_written,
        .correlated_result_received =
            outcome.correlated_result_received,
        .diagnostic = outcome.result.message,
    };
}

bool WorkerCoordinator::ResizeVisualWorkerPool(
    std::size_t desired_workers,
    std::vector<WorkerVisualSurfaceBinding> surfaces,
    std::string* error_out) {
    if (desired_workers > kMaximumWorkerCount) {
        if (error_out != nullptr)
            *error_out = "desired worker count exceeds the maximum of 40";
        return false;
    }
    std::unordered_map<std::size_t, WorkerVisualSurfaceBinding> requested;
    for (auto& surface : surfaces) {
        std::string validation_error;
        if (!ValidateVisualSurfaceBinding(surface, &validation_error)) {
            if (error_out != nullptr)
                *error_out = std::move(validation_error);
            return false;
        }
        if (!requested.emplace(surface.worker_id, std::move(surface)).second) {
            if (error_out != nullptr)
                *error_out = "duplicate visual surface worker ID";
            return false;
        }
    }
    for (std::size_t worker_id = 0; worker_id < desired_workers; ++worker_id) {
        if (!requested.contains(worker_id)) {
            if (error_out != nullptr)
                *error_out = "visual worker pool is missing required surface for worker "
                    + std::to_string(worker_id);
            return false;
        }
    }
    for (auto& [worker_id, surface] : requested) {
        std::string surface_error;
        if (!SetWorkerVisualSurface(std::move(surface), &surface_error)) {
            if (error_out != nullptr)
                *error_out = std::move(surface_error);
            return false;
        }
    }
    return SetDesiredWorkerCount(desired_workers, error_out);
}

bool WorkerCoordinator::SetWorkerVisualSurface(
    WorkerVisualSurfaceBinding surface,
    std::string* error_out) {
    if (!ValidateVisualSurfaceBinding(surface, error_out))
        return false;
    const std::size_t worker_id = surface.worker_id;
    WorkerSlotPtr slot;
    {
        std::lock_guard<std::mutex> workers_lock(workers_mutex_);
        const auto existing = visual_surfaces_.find(worker_id);
        if (existing != visual_surfaces_.end() &&
            existing->second.render_widget_handle ==
                surface.render_widget_handle &&
            existing->second.surface_generation ==
                surface.surface_generation &&
            existing->second.owner_process_id == surface.owner_process_id) {
            if (error_out != nullptr)
                error_out->clear();
            return true;
        }
        visual_surfaces_[worker_id] = surface;
        if (worker_id < workers_.size()) {
            slot = workers_[worker_id];
        }
    }
    if (slot) {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        const bool active_surface_changed =
            slot->visual_render_widget_handle != 0 &&
            (slot->visual_render_widget_handle !=
                 surface.render_widget_handle ||
             slot->visual_surface_generation !=
                 surface.surface_generation);
        slot->visual_render_widget_handle = surface.render_widget_handle;
        slot->visual_surface_generation = surface.surface_generation;
        slot->visual_owner_process_id = surface.owner_process_id;
        slot->visual_host_events_pipe_name =
            std::move(surface.host_events_pipe_name);
        if (active_surface_changed) {
            slot->quarantine_requested = true;
            slot->quarantine_diagnostic =
                "visual worker surface was replaced";
        }
    }
    if (error_out != nullptr)
        error_out->clear();
    NotifyAvailabilityChanged();
    return true;
}

void WorkerCoordinator::InvalidateWorkerVisualSurface(
    std::size_t worker_id,
    std::uint64_t surface_generation) {
    WorkerSlotPtr slot;
    {
        std::lock_guard<std::mutex> workers_lock(workers_mutex_);
        const auto found = visual_surfaces_.find(worker_id);
        if (found == visual_surfaces_.end() ||
            found->second.surface_generation != surface_generation) {
            return;
        }
        visual_surfaces_.erase(found);
        if (worker_id < workers_.size())
            slot = workers_[worker_id];
    }
    if (slot) {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        if (slot->visual_surface_generation != surface_generation)
            return;
        slot->visual_render_widget_handle = 0;
        slot->visual_surface_generation = 0;
        slot->visual_owner_process_id = 0;
        slot->quarantine_requested = true;
        slot->quarantine_diagnostic =
            "visual worker surface was invalidated";
    }
    NotifyAvailabilityChanged();
}

WorkerWorksetDispatchInfo WorkerCoordinator::DispatchInfoOf(
    const savor::runtime::WorkerWorksetDefinition& workset) {
    return {
        .execution_key_sha256 =
            workset.execution_key.canonical_sha256,
        .program_package_sha256 =
            workset.execution_key.program_package_sha256,
        .baseline_sha256 =
            workset.execution_key.baseline.sha256,
        .item_count = static_cast<std::uint32_t>(
            std::min<std::size_t>(
                workset.items.size(),
                std::numeric_limits<std::uint32_t>::max())),
        .encoded_size_bytes = workset.encoded_size_bytes,
    };
}

WorkerCoordinator::WorkerSlotPtr WorkerCoordinator::MakeWorkerSlot(
    std::size_t worker_id) {
    auto slot = std::make_shared<WorkerSlot>();
    slot->id = worker_id;
    slot->process_generation = NextProcessGeneration();
    slot->worker = std::make_shared<savor::ProcessWorker>();
    const auto surface_it = visual_surfaces_.find(worker_id);
    if (surface_it != visual_surfaces_.end()) {
        slot->visual_render_widget_handle =
            surface_it->second.render_widget_handle;
        slot->visual_surface_generation =
            surface_it->second.surface_generation;
        slot->visual_owner_process_id =
            surface_it->second.owner_process_id;
        slot->visual_host_events_pipe_name =
            surface_it->second.host_events_pipe_name;
    }
    ConfigureWorkerCallbacks(slot);
    worker_status_.RegisterWorker(
        ToTelemetryWorkerId(worker_id),
        "localhost",
        0,
        "worker-coordinator");
    worker_status_.UpdateState(
        ToTelemetryWorkerId(worker_id),
        WorkerStateKind::Spawning);
    return slot;
}

WorkerCoordinator::WorkerSlotPtr WorkerCoordinator::GetWorkerSlot(
    std::size_t worker_id) const {
    std::lock_guard<std::mutex> workers_lock(workers_mutex_);
    if (worker_id >= workers_.size()) {
        return {};
    }
    return workers_[worker_id];
}

std::vector<WorkerCoordinator::WorkerSlotPtr>
WorkerCoordinator::CopyWorkerSlots() const {
    std::lock_guard<std::mutex> workers_lock(workers_mutex_);
    return workers_;
}

void WorkerCoordinator::ConfigureWorkerCallbacks(
    const WorkerSlotPtr& slot) {
    if (!slot) {
        return;
    }
    std::shared_ptr<savor::ProcessWorker> worker;
    std::size_t worker_id = 0;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        worker = slot->worker;
        worker_id = slot->id;
        generation = slot->process_generation;
    }
    if (!worker) {
        return;
    }
    worker->set_workset_state_callback(
        [this, worker_id, generation](
            const savor::wrms::WorksetStatePayload& payload) {
            HandleWorksetState(worker_id, generation, payload);
        });
    worker->set_workset_item_started_callback(
        [this, worker_id, generation](
            const savor::wrms::WorksetItemStartedPayload& payload) {
            HandleItemStarted(worker_id, generation, payload);
        });
    worker->set_invocation_progress_callback(
        [this, worker_id, generation](
            const savor::wrms::InvocationProgressPayload& payload) {
            HandleItemProgress(worker_id, generation, payload);
        });
    worker->set_workset_item_terminal_callback(
        [this, worker_id, generation](
            const savor::wrms::WorksetItemTerminalPayload& payload) {
            HandleItemTerminal(worker_id, generation, payload);
        });
    worker->set_workset_credits_callback(
        [this, worker_id, generation](
            const savor::wrms::WorksetCreditsPayload& payload) {
            HandleCredits(worker_id, generation, payload);
        });
    worker->set_workset_summary_callback(
        [this, worker_id, generation](
            const savor::wrms::WorksetSummaryPayload& payload) {
            HandleWorksetSummary(worker_id, generation, payload);
        });
    worker->set_session_event_callback(
        [this, worker_id, generation](
            const savor::wrms::SessionEventPayload& payload) {
            HandleSessionEvent(worker_id, generation, payload);
        });
}

void WorkerCoordinator::HandleSessionEvent(
    std::size_t worker_id,
    std::uint64_t process_generation,
    const savor::wrms::SessionEventPayload& payload) {
    if (payload.worker_state != savor::wrms::WorkerStateCode::Tainted
        && payload.session_disposition !=
            savor::wrms::SessionDispositionCode::Tainted) {
        return;
    }

    const auto slot = GetWorkerSlot(worker_id);
    if (!slot) return;
    {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        if (!slot->ready
            || slot->process_generation != process_generation) {
            return;
        }
        slot->quarantine_requested = true;
        slot->available_item_credits = 0;
        slot->quarantine_diagnostic = payload.message.empty()
            ? "worker runtime reported a tainted session"
            : "worker runtime reported a tainted session: "
                + payload.message;
        worker_status_.UpdateState(
            ToTelemetryWorkerId(slot->id),
            WorkerStateKind::Dead);
        worker_status_.RecordError(
            ToTelemetryWorkerId(slot->id),
            slot->quarantine_diagnostic);
    }
    NotifyAvailabilityChanged();
}

WorkerCoordinatorRuntimePreflightResult
WorkerCoordinator::PreflightWorkerSlot(const WorkerSlotPtr& slot) {
    if (!slot) {
        return {
            .error = "worker slot is unavailable",
        };
    }

    std::size_t worker_id = 0;
    std::shared_ptr<savor::ProcessWorker> worker;
    std::uint64_t render_widget_handle = 0;
    std::uint64_t process_generation = 0;
    std::string preparation_id;
    std::filesystem::path prepared_user_directory;
    {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        worker_id = slot->id;
        worker = slot->worker;
        render_widget_handle = slot->visual_render_widget_handle;
        process_generation = slot->process_generation;
        preparation_id = slot->preparation_id;
        prepared_user_directory = slot->prepared_user_directory;
    }
    if (!worker) {
        return {
            .error = "worker process object is unavailable",
        };
    }
    if (config_.require_managed_visual_surfaces &&
        render_widget_handle == 0) {
        return {
            .error = "managed visual worker has no valid render surface",
        };
    }
    if (config_.worker_runtime_preflight) {
        {
            std::lock_guard<std::mutex> slot_lock(slot->mutex);
            if (slot->process_generation == process_generation
                && slot->startup_phase == WorkerStartupPhase::Launching) {
                slot->startup_phase = WorkerStartupPhase::OpeningSession;
            }
        }
        try {
            return config_.worker_runtime_preflight(
                worker_id,
                config_,
                worker);
        } catch (const std::exception& ex) {
            return {
                .error = std::string("worker preflight threw: ") + ex.what(),
            };
        } catch (...) {
            return {
                .error = "worker preflight threw an unknown exception",
            };
        }
    }

    std::filesystem::path executable;
    std::string error;
    if (!PrepareRuntimeSlot(worker_id, &executable, &error)) {
        return {
            .error = JoinDiagnostic(
                "worker runtime preparation failed",
                error),
        };
    }
    const auto worker_root =
        std::filesystem::path(config_.worker_dir_root)
        / ("worker-" + std::to_string(worker_id));
    std::error_code filesystem_error;
    const auto log_directory = worker_root / "logs";
    std::filesystem::create_directories(log_directory, filesystem_error);
    if (filesystem_error) {
        return {
            .error = JoinDiagnostic(
                "worker directory creation failed",
                filesystem_error.message()),
        };
    }
    const auto utc_launch_ticks = static_cast<std::uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
    const auto log_path = log_directory /
        ("worker-" + std::to_string(worker_id) + "-" +
         std::to_string(utc_launch_ticks) + ".log");
    {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        if (slot->worker != worker ||
            slot->process_generation != process_generation) {
            return {.error = "worker slot changed before launch"};
        }
        slot->utc_launch_ticks = utc_launch_ticks;
        slot->log_path = log_path.string();
    }
    if (!worker->launch_and_negotiate(
            savor::ProcessLaunchOptions{
                .worker_id = worker_id,
                .exe_path = executable.string(),
                .log_file_path = log_path.string(),
                .process_generation = process_generation,
                .utc_launch_ticks = utc_launch_ticks,
                .hello_timeout_ms = config_.worker_start_timeout_ms,
                .breakpoint_diagnostics = config_.breakpoint_diagnostics,
            },
            &error)) {
        return {
            .error = error.empty()
                ? "worker launch or WRMS negotiation failed"
                : error,
        };
    }

    const auto runtime_contract = worker->runtime_contract();
    if (!runtime_contract.has_value()) {
        return {
            .process_ready = true,
            .error = "worker did not publish a runtime contract",
        };
    }

    WorkerCoordinatorRuntimePreflightResult contract_preflight{
        .process_ready = true,
        .runtime_contract = runtime_contract,
    };
    std::string contract_error;
    if (!ValidatePreflight(config_, contract_preflight, &contract_error)) {
        return {
            .process_ready = true,
            .runtime_contract = runtime_contract,
            .retryable = false,
            .error = contract_error,
        };
    }

    {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        if (slot->process_generation != process_generation
            || slot->startup_phase != WorkerStartupPhase::Launching) {
            return {
                .process_ready = true,
                .runtime_contract = runtime_contract,
                .error = "worker slot changed before OpenSession",
            };
        }
        slot->startup_phase = WorkerStartupPhase::OpeningSession;
    }

    savor::wrms::OpenSessionResultPayload open_result;
    if (!worker->open_session(
            savor::ProcessOpenSessionOptions{
                .runtime_root = executable.parent_path().string(),
                .user_directory = prepared_user_directory.string(),
                .iso_path = config_.iso_path,
                .worker_mode = config_.worker_mode,
                .render_widget_handle = render_widget_handle,
                .runtime_artifact_root =
                    config_.runtime_artifact_root,
                .session_filesystem_preparation_id = preparation_id,
                .process_generation = process_generation,
            },
            &open_result,
            &error,
            config_.worker_start_timeout_ms)) {
        return {
            .process_ready = true,
            .runtime_contract = runtime_contract,
            .error = error.empty()
                ? "worker OpenSession failed"
                : error,
        };
    }
    return {
        .process_ready = true,
        .runtime_contract = runtime_contract,
    };
}

bool WorkerCoordinator::PrepareRuntimeSlot(
    std::size_t worker_id,
    std::filesystem::path* executable_out,
    std::string* error_out) const {
    if (config_.runtime_slot_preparer) {
        return config_.runtime_slot_preparer(
            worker_id,
            config_,
            executable_out,
            error_out);
    }

    std::lock_guard<std::mutex> preparation_lock(
        runtime_preparation_mutex_);
    std::error_code error;
    if (prepared_runtime_worker_executable_.has_value()
        && std::filesystem::is_regular_file(
            *prepared_runtime_worker_executable_,
            error)
        && !error) {
        if (executable_out != nullptr) {
            *executable_out =
                *prepared_runtime_worker_executable_;
        }
        if (error_out != nullptr) {
            error_out->clear();
        }
        return true;
    }

    prepared_runtime_worker_executable_.reset();
    std::filesystem::path executable;
    if (!EnsureSharedWorkerRuntime(
            config_,
            &executable,
            error_out)) {
        return false;
    }
    prepared_runtime_worker_executable_ = executable;
    if (executable_out != nullptr) {
        *executable_out = std::move(executable);
    }
    return true;
}

simboot::SessionFilesystemPreparationResult
WorkerCoordinator::PrepareSessionFilesystem(
    const WorkerSlotPtr& slot,
    std::uint32_t attempt) const {
    std::size_t worker_id = 0;
    std::uint64_t process_generation = 0;
    {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        worker_id = slot->id;
        process_generation = slot->process_generation;
    }

    const auto preparation_id =
        "worker-" + std::to_string(worker_id)
        + "-generation-" + std::to_string(process_generation)
        + "-attempt-" + std::to_string(attempt);
    const auto worker_root =
        std::filesystem::path(config_.worker_dir_root)
        / ("worker-" + std::to_string(worker_id));
    const simboot::SessionFilesystemPreparationRequest request{
        .worker_id = worker_id,
        .process_generation = process_generation,
        .preparation_id = preparation_id,
        .dolphin_qt_base = config_.dolphin_base_dir,
        .worker_root = worker_root,
        .cancelled = [this, weak_slot = std::weak_ptr<WorkerSlot>(slot),
                      process_generation]() {
            if (stopping_.load(std::memory_order_acquire)) {
                return true;
            }
            const auto current = weak_slot.lock();
            if (!current) {
                return true;
            }
            std::lock_guard<std::mutex> slot_lock(current->mutex);
            return current->process_generation != process_generation;
        },
    };

    simboot::SessionFilesystemPreparationResult result;
    if (config_.session_filesystem_preparer) {
        result = config_.session_filesystem_preparer(request);
    } else if (config_.worker_runtime_preflight) {
        // The injected preflight is a complete test seam and does not launch
        // the production worker against a filesystem.
        result = {
            .ok = true,
            .preparation_id = preparation_id,
            .user_directory = worker_root / "User",
            .marker_path = simboot::SessionFilesystemPreparer::MarkerPath(
                worker_root / "User"),
        };
    } else {
        result = simboot::SessionFilesystemPreparer::Prepare(request);
    }

    if (result.ok
        && (result.preparation_id != preparation_id
            || result.user_directory != worker_root / "User")) {
        result.ok = false;
        result.retryable = false;
        result.error =
            "session filesystem preparation returned mismatched identity";
    }
    return result;
}

bool WorkerCoordinator::StartSessionFilesystemPreparation(
    const WorkerSlotPtr& slot) {
    std::uint32_t attempt = 0;
    if (!BeginSessionFilesystemPreparation(slot, &attempt)) {
        return false;
    }
    CompleteSessionFilesystemPreparation(slot, attempt);
    std::lock_guard<std::mutex> slot_lock(slot->mutex);
    return slot->startup_phase == WorkerStartupPhase::WaitingToOpen;
}

bool WorkerCoordinator::StartSessionFilesystemPreparationAsync(
    const WorkerSlotPtr& slot) {
    std::uint32_t attempt = 0;
    if (!BeginSessionFilesystemPreparation(slot, &attempt)) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        slot->startup_thread = std::thread(
            [this, slot, attempt]() {
                CompleteSessionFilesystemPreparation(slot, attempt);
            });
    } catch (const std::exception& exception) {
        {
            std::lock_guard<std::mutex> slot_lock(slot->mutex);
            slot->startup_phase = WorkerStartupPhase::Failed;
            slot->last_start_error =
                std::string("create filesystem preparation thread failed: ")
                + exception.what();
            slot->start_retry_exhausted =
                slot->start_attempts
                >= std::max<std::uint32_t>(
                    1,
                    config_.max_worker_start_attempts);
        }
        ++start_failures_;
        RefreshStartResult();
        NotifyAvailabilityChanged();
        return false;
    }
    return true;
}

bool WorkerCoordinator::BeginSessionFilesystemPreparation(
    const WorkerSlotPtr& slot,
    std::uint32_t* attempt_out) {
    if (!slot || attempt_out == nullptr
        || stopping_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        const auto maximum_attempts = std::max<std::uint32_t>(
            1,
            config_.max_worker_start_attempts);
        if (slot->ready
            || slot->startup_phase != WorkerStartupPhase::PendingFilesystem
            || slot->startup_thread.joinable()
            || slot->start_retry_exhausted
            || slot->start_attempts >= maximum_attempts) {
            return false;
        }
        slot->startup_phase = WorkerStartupPhase::PreparingFilesystem;
        *attempt_out = ++slot->start_attempts;
        ++slot->observed_start_attempts;
        worker_status_.UpdateState(
            ToTelemetryWorkerId(slot->id),
            WorkerStateKind::Spawning);
        worker_status_.RecordHeartbeat(ToTelemetryWorkerId(slot->id));
    }
    ++start_attempts_;
    return true;
}

void WorkerCoordinator::CompleteSessionFilesystemPreparation(
    const WorkerSlotPtr& slot,
    std::uint32_t attempt) {
    auto result = PrepareSessionFilesystem(slot, attempt);
    const bool prepared = result.ok
        && !stopping_.load(std::memory_order_acquire);
    std::size_t worker_id = 0;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        worker_id = slot->id;
        generation = slot->process_generation;
        if (slot->start_attempts != attempt
            || slot->startup_phase
                != WorkerStartupPhase::PreparingFilesystem) {
            return;
        }
        if (prepared) {
            slot->preparation_id = result.preparation_id;
            slot->prepared_user_directory = result.user_directory;
            slot->startup_phase = WorkerStartupPhase::WaitingToOpen;
            slot->last_start_error.clear();
        } else {
            ++start_failures_;
            slot->startup_phase = WorkerStartupPhase::Failed;
            slot->preparation_id.clear();
            slot->prepared_user_directory.clear();
            slot->last_start_error = result.error.empty()
                ? "session filesystem preparation failed"
                : result.error;
            if (stopping_.load(std::memory_order_acquire)
                && result.error.empty()) {
                slot->last_start_error =
                    "session filesystem preparation canceled";
            }
            slot->start_retry_exhausted =
                !result.retryable
                || attempt >= std::max<std::uint32_t>(
                    1,
                    config_.max_worker_start_attempts);
            if (!slot->start_retry_exhausted) {
                slot->next_start_after =
                    std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(
                        config_.worker_start_retry_backoff_ms);
            }
            worker_status_.UpdateState(
                ToTelemetryWorkerId(slot->id),
                WorkerStateKind::Dead);
            worker_status_.RecordError(
                ToTelemetryWorkerId(slot->id),
                slot->last_start_error);
        }
    }

    if (prepared) {
        SCLOGIX(
            SC_TAGS("worker.startup.filesystem"),
            "prepared worker=%zu generation=%llu files=%llu directories=%llu bytes=%llu duration_ms=%llu source=%s target=%s preparation_id=%s",
            worker_id,
            static_cast<unsigned long long>(generation),
            static_cast<unsigned long long>(result.file_count),
            static_cast<unsigned long long>(result.directory_count),
            static_cast<unsigned long long>(result.byte_count),
            static_cast<unsigned long long>(result.elapsed_ms),
            config_.dolphin_base_dir.c_str(),
            result.user_directory.string().c_str(),
            result.preparation_id.c_str());
    } else {
        SCLOGWX(
            SC_TAGS("worker.startup.filesystem"),
            "preparation failed worker=%zu generation=%llu duration_ms=%llu error=%s",
            worker_id,
            static_cast<unsigned long long>(generation),
            static_cast<unsigned long long>(result.elapsed_ms),
            result.error.c_str());
    }
    RefreshStartResult(prepared ? std::string{} : result.error);
    NotifyAvailabilityChanged();
}

bool WorkerCoordinator::StartWorkerSlot(const WorkerSlotPtr& slot) {
    if (!StartSessionFilesystemPreparation(slot)) {
        return false;
    }
    std::uint32_t attempt = 0;
    if (!BeginWorkerSlotStart(slot, &attempt)) {
        return false;
    }
    CompleteWorkerSlotStart(slot, attempt);
    std::lock_guard<std::mutex> slot_lock(slot->mutex);
    return slot->ready;
}

bool WorkerCoordinator::StartWorkerSlotAsync(
    const WorkerSlotPtr& slot) {
    std::uint32_t attempt = 0;
    if (!BeginWorkerSlotStart(slot, &attempt)) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        slot->startup_thread = std::thread(
            [this, slot, attempt]() {
                CompleteWorkerSlotStart(slot, attempt);
            });
    } catch (const std::exception& exception) {
        {
            std::lock_guard<std::mutex> slot_lock(slot->mutex);
            slot->startup_phase = WorkerStartupPhase::Failed;
            slot->last_start_error =
                std::string("create worker startup thread failed: ")
                + exception.what();
            slot->start_retry_exhausted =
                slot->start_attempts
                >= std::max<std::uint32_t>(
                    1,
                    config_.max_worker_start_attempts);
            if (!slot->start_retry_exhausted) {
                slot->next_start_after =
                    std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(
                        config_.worker_start_retry_backoff_ms);
            }
        }
        ++start_failures_;
        RefreshStartResult();
        NotifyAvailabilityChanged();
        return false;
    }
    return true;
}

bool WorkerCoordinator::BeginWorkerSlotStart(
    const WorkerSlotPtr& slot,
    std::uint32_t* attempt_out) {
    if (!slot || attempt_out == nullptr
        || stopping_.load(std::memory_order_acquire)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        const auto maximum_attempts =
            std::max<std::uint32_t>(
                1,
                config_.max_worker_start_attempts);
        if (slot->ready
            || slot->startup_phase != WorkerStartupPhase::WaitingToOpen
            || slot->startup_thread.joinable()
            || slot->start_retry_exhausted
            || slot->start_attempts >= maximum_attempts) {
            return false;
        }
        slot->startup_phase = WorkerStartupPhase::Launching;
        *attempt_out = slot->start_attempts;
        worker_status_.UpdateState(
            ToTelemetryWorkerId(slot->id),
            WorkerStateKind::Spawning);
        worker_status_.RecordHeartbeat(
            ToTelemetryWorkerId(slot->id));
    }
    return true;
}

void WorkerCoordinator::CompleteWorkerSlotStart(
    const WorkerSlotPtr& slot,
    std::uint32_t attempt) {
    auto preflight = PreflightWorkerSlot(slot);
    if (preflight.runtime_contract.has_value()
        && *preflight.runtime_contract
            != config_.expected_runtime_contract) {
        preflight.retryable = false;
    }
    std::string validation_error;
    const bool preflight_ready =
        ValidatePreflight(config_, preflight, &validation_error);
    const bool ready =
        preflight_ready
        && !stopping_.load(std::memory_order_acquire);

    std::shared_ptr<savor::ProcessWorker> rejected_worker;
    {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        if (slot->start_attempts != attempt
            || (slot->startup_phase != WorkerStartupPhase::Launching
                && slot->startup_phase
                    != WorkerStartupPhase::OpeningSession)) {
            return;
        }
        if (ready) {
            slot->startup_phase = WorkerStartupPhase::None;
            slot->ready = true;
            slot->mode = config_.worker_mode;
            slot->runtime_contract_sha256 =
                preflight.runtime_contract->canonical_sha256;
            slot->available_item_credits =
                preflight.runtime_contract->limits.maximum_item_credits;
            if (slot->worker) {
                const auto process_snapshot =
                    slot->worker->latest_snapshot();
                if (process_snapshot.available_item_credits != 0) {
                    slot->available_item_credits =
                        process_snapshot.available_item_credits;
                }
            }
            slot->last_start_error.clear();
            slot->next_start_after = {};
            slot->start_attempts = 0;
            slot->start_retry_exhausted = false;
            slot->next_liveness_probe =
                std::chrono::steady_clock::now()
                + std::chrono::milliseconds(
                    config_.liveness_probe_interval_ms);
            slot->consecutive_liveness_failures = 0;
            slot->quarantine_requested = false;
            slot->quarantine_diagnostic.clear();
            const int pid = slot->worker
                ? static_cast<int>(slot->worker->GetPid())
                : 0;
            worker_status_.RegisterWorker(
                ToTelemetryWorkerId(slot->id),
                "localhost",
                pid,
                "worker-coordinator");
            worker_status_.UpdateState(
                ToTelemetryWorkerId(slot->id),
                WorkerStateKind::Idle);
            worker_status_.RecordHeartbeat(
                ToTelemetryWorkerId(slot->id));
        } else {
            ++start_failures_;
            slot->startup_phase = WorkerStartupPhase::Failed;
            slot->ready = false;
            slot->runtime_contract_sha256.clear();
            slot->available_item_credits = 0;
            slot->last_start_error =
                validation_error.empty()
                ? preflight.error
                : validation_error;
            if (slot->last_start_error.empty()
                && stopping_.load(std::memory_order_acquire)) {
                slot->last_start_error = "worker startup canceled";
            }
            slot->start_retry_exhausted =
                !preflight.retryable
                || attempt
                    >= std::max<std::uint32_t>(
                        1,
                        config_.max_worker_start_attempts);
            if (!slot->start_retry_exhausted) {
                slot->next_start_after =
                    std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(
                        config_.worker_start_retry_backoff_ms);
            }
            rejected_worker = slot->worker;
            worker_status_.UpdateState(
                ToTelemetryWorkerId(slot->id),
                WorkerStateKind::Dead);
            worker_status_.RecordError(
                ToTelemetryWorkerId(slot->id),
                slot->last_start_error);
        }
    }
    RefreshStartResult(
        ready ? std::string{} : validation_error);
    NotifyAvailabilityChanged();
    if (rejected_worker) {
        if (stopping_.load(std::memory_order_acquire)) {
            rejected_worker->begin_stop();
        } else {
            rejected_worker->stop();
        }
    }
}

void WorkerCoordinator::StopWorkerSlot(const WorkerSlotPtr& slot) {
    if (!slot) {
        return;
    }

    std::shared_ptr<savor::ProcessWorker> worker;
    std::size_t worker_id = 0;
    std::uint64_t generation = 0;
    std::uint16_t protocol_version = 0;
    {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        worker_id = slot->id;
        generation = slot->process_generation;
        slot->ready = false;
        slot->startup_phase = WorkerStartupPhase::None;
        slot->preparation_id.clear();
        slot->prepared_user_directory.clear();
        slot->submission_in_progress = false;
        slot->quarantine_requested = false;
        slot->quarantine_diagnostic.clear();
        slot->submitting_workset_id.reset();
        protocol_version = savor::wrms::ProtocolVersion;
        worker = slot->worker;
    }

    if (worker) {
        // Closing transport is what cancels an unbounded ordinary command.
        // Never wait for that command's serialization mutex first.
        worker->stop();
    }
    {
        std::lock_guard<std::mutex> submission_lock(
            slot->submission_mutex);
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        if (slot->process_generation == generation) {
            slot->active_workset_id.reset();
            slot->runtime_contract_sha256.clear();
            slot->available_item_credits = 0;
            slot->worker.reset();
        }
    }
    std::vector<std::uint64_t> removed_worksets;
    RemoveRoutesForWorker(
        worker_id,
        generation,
        &removed_worksets);
    if (!removed_worksets.empty()) {
        NotifyWorkerUnavailable(WorkerUnavailableEvent{
            .source = {
                .worker_id = worker_id,
                .process_generation = generation,
                .wrms_protocol_version = protocol_version,
            },
            .affected_workset_ids = std::move(removed_worksets),
            .diagnostic =
                "worker stopped before all routed worksets completed",
        });
    }
    worker_status_.UpdateState(
        ToTelemetryWorkerId(worker_id),
        WorkerStateKind::Dead);
    worker_status_.UnregisterWorker(
        ToTelemetryWorkerId(worker_id));
}

void WorkerCoordinator::ResetWorkerSlot(const WorkerSlotPtr& slot) {
    if (!slot) {
        return;
    }

    std::shared_ptr<savor::ProcessWorker> old_worker;
    std::size_t worker_id = 0;
    {
        std::lock_guard<std::mutex> submission_lock(
            slot->submission_mutex);
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        worker_id = slot->id;
        old_worker = slot->worker;
        slot->ready = false;
        slot->startup_phase = WorkerStartupPhase::WaitingForProcessExit;
        slot->submission_in_progress = false;
        slot->quarantine_requested = false;
        slot->quarantine_diagnostic.clear();
        slot->submitting_workset_id.reset();
        slot->active_workset_id.reset();
        slot->runtime_contract_sha256.clear();
        slot->available_item_credits = 0;
    }

    if (old_worker) {
        old_worker->stop();
        if (!old_worker->confirm_process_exit()) {
            worker_status_.UpdateState(
                ToTelemetryWorkerId(worker_id),
                WorkerStateKind::Stopping);
            worker_status_.RecordHeartbeat(
                ToTelemetryWorkerId(worker_id));
            return;
        }
    }

    {
        std::lock_guard<std::mutex> submission_lock(
            slot->submission_mutex);
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        if (slot->worker != old_worker
            || slot->startup_phase
                != WorkerStartupPhase::WaitingForProcessExit) {
            return;
        }
        slot->process_generation = NextProcessGeneration();
        slot->worker = std::make_shared<savor::ProcessWorker>();
        slot->startup_phase = WorkerStartupPhase::PendingFilesystem;
        slot->preparation_id.clear();
        slot->prepared_user_directory.clear();
        slot->next_liveness_probe = {};
        slot->consecutive_liveness_failures = 0;
        slot->warm_execution_key_sha256.reset();
        slot->warm_program_package_sha256.reset();
    }
    ConfigureWorkerCallbacks(slot);
}

void WorkerCoordinator::LifecycleLoop() {
    while (!stopping_.load(std::memory_order_acquire)) {
        ReconcileWorkerPool();
        const auto sleep_ms =
            config_.controller_sleep_ms == 0
            ? 5
            : config_.controller_sleep_ms;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(sleep_ms));
        if (stopping_.load(std::memory_order_acquire)) {
            break;
        }
        DetectLostWorkers();
        ProbeWorkerLiveness();
    }
}

void WorkerCoordinator::ProbeWorkerLiveness() {
    if (config_.liveness_probe_interval_ms == 0
        || config_.liveness_probe_failure_threshold == 0
        || stopping_.load(std::memory_order_acquire)) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    for (const auto& slot : CopyWorkerSlots()) {
        if (!slot) {
            continue;
        }

        std::shared_ptr<savor::ProcessWorker> worker;
        std::uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> slot_lock(slot->mutex);
            if (!slot->ready
                || slot->quarantine_requested
                || !slot->worker
                || slot->next_liveness_probe > now) {
                continue;
            }
            worker = slot->worker;
            generation = slot->process_generation;
            slot->next_liveness_probe =
                now + std::chrono::milliseconds(
                    config_.liveness_probe_interval_ms);
        }

        {
            std::lock_guard<std::mutex> slot_lock(slot->mutex);
            if (!slot->ready
                || slot->process_generation != generation
                || slot->worker != worker
                || slot->quarantine_requested) {
                continue;
            }
        }

        ++liveness_probe_attempts_;
        savor::wrms::CommandResultPayload result;
        const bool responsive =
            worker->probe_liveness(
                &result,
                std::max<std::uint32_t>(
                    1,
                    config_.liveness_probe_timeout_ms));
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        if (!slot->ready
            || slot->process_generation != generation
            || slot->worker != worker) {
            continue;
        }
        if (responsive) {
            slot->consecutive_liveness_failures = 0;
            worker_status_.RecordHeartbeat(
                ToTelemetryWorkerId(slot->id));
            continue;
        }

        ++liveness_probe_failures_;
        ++slot->consecutive_liveness_failures;
        if (slot->consecutive_liveness_failures
            < config_.liveness_probe_failure_threshold) {
            continue;
        }
        slot->quarantine_requested = true;
        slot->quarantine_diagnostic =
            "worker control transport failed "
            + std::to_string(
                slot->consecutive_liveness_failures)
            + " consecutive liveness probes: "
            + worker->last_error();
        ++liveness_quarantines_;
    }
}

void WorkerCoordinator::ReconcileWorkerPool() {
#ifdef _WIN32
    if (config_.require_managed_visual_surfaces) {
        const auto observed_slots = CopyWorkerSlots();
        for (const auto& slot : observed_slots) {
            if (!slot)
                continue;
            std::uint64_t handle = 0;
            std::uint64_t generation = 0;
            std::uint32_t expected_owner = 0;
            {
                std::lock_guard<std::mutex> slot_lock(slot->mutex);
                handle = slot->visual_render_widget_handle;
                generation = slot->visual_surface_generation;
                expected_owner = slot->visual_owner_process_id;
            }
            if (handle == 0 || generation == 0)
                continue;
            const auto hwnd = reinterpret_cast<HWND>(
                static_cast<std::uintptr_t>(handle));
            DWORD actual_owner = 0;
            const bool valid = ::IsWindow(hwnd) != FALSE &&
                ::GetWindowThreadProcessId(hwnd, &actual_owner) != 0 &&
                actual_owner == expected_owner;
            if (!valid)
                InvalidateWorkerVisualSurface(slot->id, generation);
        }
    }
#endif
    const auto desired =
        desired_worker_count_.load(std::memory_order_acquire);
    std::vector<WorkerSlotPtr> removed_slots;
    bool pool_shape_changed = false;
    {
        std::lock_guard<std::mutex> workers_lock(workers_mutex_);
        while (workers_.size() < desired) {
            workers_.push_back(MakeWorkerSlot(workers_.size()));
            pool_shape_changed = true;
        }
        while (workers_.size() > desired) {
            const auto slot = workers_.back();
            bool removable = false;
            if (slot) {
                std::lock_guard<std::mutex> slot_lock(slot->mutex);
                removable = !IsActiveStartupPhase(slot->startup_phase)
                    && !slot->startup_thread.joinable()
                    && !slot->submission_in_progress
                    && !slot->active_workset_id.has_value();
            }
            if (!removable) {
                break;
            }

            bool has_routes = false;
            {
                std::lock_guard<std::mutex> routes_lock(routes_mutex_);
                has_routes = std::any_of(
                    routes_.begin(),
                    routes_.end(),
                    [&](const auto& route) {
                        return route.second.worker_id == slot->id
                            && route.second.process_generation
                                == slot->process_generation;
                    });
            }
            if (has_routes) {
                break;
            }
            removed_slots.push_back(slot);
            workers_.pop_back();
            pool_shape_changed = true;
        }
    }
    for (const auto& slot : removed_slots) {
        StopWorkerSlot(slot);
    }
    if (pool_shape_changed) {
        RefreshStartResult();
        NotifyAvailabilityChanged();
    }

    const auto slots = CopyWorkerSlots();
    std::vector<std::thread> completed_startup_threads;
    for (const auto& slot : slots) {
        if (!slot) {
            continue;
        }
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        if (!IsActiveStartupPhase(slot->startup_phase)
            && slot->startup_thread.joinable()) {
            completed_startup_threads.push_back(
                std::move(slot->startup_thread));
        }
    }
    for (auto& startup_thread : completed_startup_threads) {
        if (startup_thread.joinable()) {
            startup_thread.join();
        }
    }

    const auto maximum_open_sessions = paused_.load(std::memory_order_acquire)
        ? std::max<std::uint32_t>(
            1,
            config_.max_concurrent_worker_starts_when_paused)
        : 1u;
    const auto now = std::chrono::steady_clock::now();

    for (const auto& slot : slots) {
        if (!slot || stopping_.load(std::memory_order_acquire)) {
            break;
        }
        bool waiting_for_process_exit = false;
        bool requires_reset = false;
        {
            std::lock_guard<std::mutex> slot_lock(slot->mutex);
            waiting_for_process_exit =
                slot->startup_phase
                    == WorkerStartupPhase::WaitingForProcessExit;
            requires_reset =
                slot->startup_phase == WorkerStartupPhase::Failed
                && !slot->startup_thread.joinable()
                && !slot->start_retry_exhausted
                && now >= slot->next_start_after
                && slot->start_attempts
                    < std::max<std::uint32_t>(
                        1,
                        config_.max_worker_start_attempts);
        }
        if (waiting_for_process_exit || requires_reset) {
            ResetWorkerSlot(slot);
        }
    }

    std::uint32_t active_open_sessions = 0;
    bool preparation_active = false;
    for (const auto& slot : slots) {
        if (!slot) {
            continue;
        }
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        preparation_active = preparation_active
            || slot->startup_phase
                == WorkerStartupPhase::PreparingFilesystem;
        if (slot->startup_phase == WorkerStartupPhase::Launching
            || slot->startup_phase
                == WorkerStartupPhase::OpeningSession) {
            ++active_open_sessions;
        }
    }

    for (const auto& slot : slots) {
        if (active_open_sessions >= maximum_open_sessions
            || stopping_.load(std::memory_order_acquire)) {
            break;
        }
        bool waiting_to_open = false;
        {
            std::lock_guard<std::mutex> slot_lock(slot->mutex);
            waiting_to_open =
                slot->startup_phase == WorkerStartupPhase::WaitingToOpen
                && !slot->startup_thread.joinable();
        }
        if (waiting_to_open && StartWorkerSlotAsync(slot)) {
            ++active_open_sessions;
        }
    }

    if (!preparation_active
        && !stopping_.load(std::memory_order_acquire)) {
        for (const auto& slot : slots) {
            if (!slot) {
                continue;
            }
            bool pending_preparation = false;
            {
                std::lock_guard<std::mutex> slot_lock(slot->mutex);
                pending_preparation =
                    slot->startup_phase
                        == WorkerStartupPhase::PendingFilesystem
                    && !slot->startup_thread.joinable()
                    && !slot->start_retry_exhausted
                    && now >= slot->next_start_after
                    && slot->start_attempts
                        < std::max<std::uint32_t>(
                            1,
                            config_.max_worker_start_attempts);
            }
            if (pending_preparation) {
                (void)StartSessionFilesystemPreparationAsync(slot);
                break;
            }
        }
    }
}

void WorkerCoordinator::DetectLostWorkers() {
    const auto slots = CopyWorkerSlots();
    for (const auto& slot : slots) {
        if (!slot) {
            continue;
        }

        bool lost = false;
        std::size_t worker_id = 0;
        std::uint64_t generation = 0;
        std::uint16_t protocol_version = 0;
        std::shared_ptr<savor::ProcessWorker> worker;
        std::string loss_diagnostic;
        {
            std::lock_guard<std::mutex> slot_lock(slot->mutex);
            worker_id = slot->id;
            generation = slot->process_generation;
            worker = slot->worker;
            // An injected preflight is a deterministic unit-test seam: it
            // supplies the admitted contract without launching a transport.
            // Production slots always take the live-process branch.
            const bool requires_live_transport =
                !static_cast<bool>(config_.worker_runtime_preflight);
            lost = slot->quarantine_requested
                || (slot->ready
                    && (!worker || (requires_live_transport
                        && !worker->is_running())));
            if (!lost) {
                continue;
            }
            loss_diagnostic =
                slot->quarantine_diagnostic.empty()
                ? "worker process became unavailable"
                : slot->quarantine_diagnostic;
            protocol_version = savor::wrms::ProtocolVersion;
            slot->ready = false;
            slot->submission_in_progress = false;
            slot->submitting_workset_id.reset();
            slot->next_start_after =
                std::chrono::steady_clock::now()
                + std::chrono::milliseconds(
                    config_.worker_start_retry_backoff_ms);
        }

        // Drain any already-received authoritative callbacks while the old
        // generation and its routes are still recognizable. Anything left
        // routed after ProcessWorker::stop() is factual lost-work evidence.
        if (worker) {
            worker->stop();
        }
        std::vector<std::uint64_t> affected_worksets;
        RemoveRoutesForWorker(
            worker_id,
            generation,
            &affected_worksets);
        ++worker_losses_;
        worker_status_.UpdateState(
            ToTelemetryWorkerId(worker_id),
            WorkerStateKind::Dead);
        worker_status_.RecordError(
            ToTelemetryWorkerId(worker_id),
            loss_diagnostic);
        NotifyWorkerUnavailable(WorkerUnavailableEvent{
            .source = {
                .worker_id = worker_id,
                .process_generation = generation,
                .wrms_protocol_version = protocol_version,
            },
            .affected_workset_ids = std::move(affected_worksets),
            .diagnostic = loss_diagnostic,
        });
        ResetWorkerSlot(slot);
        RefreshStartResult(loss_diagnostic);
        NotifyAvailabilityChanged();
    }
}

ReadyWorkerDispatchSnapshot
WorkerCoordinator::SnapshotReadyWorker(const WorkerSlot& slot) const {
    const bool runtime_accepting = !slot.worker
        || RuntimeAcceptsWorksets(slot.worker->latest_snapshot());
    ReadyWorkerDispatchSnapshot snapshot{
        .worker_id = slot.id,
        .process_generation = slot.process_generation,
        .mode = slot.mode,
        .runtime_contract_sha256 = slot.runtime_contract_sha256,
        .available_item_credits = slot.available_item_credits,
        .accepting_workset =
            runtime_accepting
            && !slot.quarantine_requested
            && !slot.submission_in_progress
            && !slot.active_workset_id.has_value()
            && slot.available_item_credits != 0,
        .resident_workset_id = slot.active_workset_id,
        .warm_execution_key_sha256 =
            slot.warm_execution_key_sha256,
        .warm_program_package_sha256 =
            slot.warm_program_package_sha256,
    };
    return snapshot;
}

WorkerCoordinatorEventContext WorkerCoordinator::EventContext(
    const WorkerSlot& slot) const {
    return {
        .worker_id = slot.id,
        .process_generation = slot.process_generation,
        .wrms_protocol_version = savor::wrms::ProtocolVersion,
    };
}

void WorkerCoordinator::HandleWorksetState(
    std::size_t worker_id,
    std::uint64_t process_generation,
    const savor::wrms::WorksetStatePayload& payload) {
    const auto context =
        CurrentEventContext(worker_id, process_generation);
    if (!context.has_value()) {
        return;
    }

    const auto slot = GetWorkerSlot(worker_id);
    if (!slot) {
        return;
    }
    {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        if (slot->process_generation != process_generation) {
            return;
        }
        if (IsTerminalWorksetState(payload.state)) {
            if ((slot->active_workset_id.has_value()
                    && *slot->active_workset_id
                        == payload.workset_id)
                || (slot->submitting_workset_id.has_value()
                    && *slot->submitting_workset_id
                        == payload.workset_id)) {
                slot->active_workset_id.reset();
                ++slot->completed_worksets;
            }
            worker_status_.UpdateState(
                ToTelemetryWorkerId(worker_id),
                WorkerStateKind::Idle);
            worker_status_.SetCurrentJob(
                ToTelemetryWorkerId(worker_id),
                std::nullopt,
                std::nullopt);
        } else if (
            payload.state == savor::wrms::WorksetStateCode::Admitted
            || payload.state
                == savor::wrms::WorksetStateCode::Initializing
            || payload.state == savor::wrms::WorksetStateCode::Ready
            || payload.state == savor::wrms::WorksetStateCode::Running
            || payload.state == savor::wrms::WorksetStateCode::ResettingItem
            || payload.state == savor::wrms::WorksetStateCode::Draining) {
            slot->active_workset_id = payload.workset_id;
            worker_status_.UpdateState(
                ToTelemetryWorkerId(worker_id),
                WorkerStateKind::Running);
        }
        worker_status_.RecordHeartbeat(
            ToTelemetryWorkerId(worker_id));
    }
    if (IsTerminalWorksetState(payload.state)) {
        std::lock_guard<std::mutex> routes_lock(routes_mutex_);
        const auto route_it = routes_.find(payload.workset_id);
        if (route_it != routes_.end()
            && route_it->second.worker_id == worker_id
            && route_it->second.process_generation
                == process_generation) {
            route_it->second.terminal_state_observed = true;
        }
    }

    std::function<void(
        const WorkerCoordinatorEventContext&,
        const savor::wrms::WorksetStatePayload&)> callback;
    {
        std::lock_guard<std::mutex> callback_lock(callbacks_mutex_);
        callback = callbacks_.workset_state;
    }
    if (callback) {
        callback(*context, payload);
    }
    if (IsTerminalWorksetState(payload.state)) {
        RemoveCompletedRouteIfPossible(payload.workset_id);
        NotifyAvailabilityChanged();
    }
}

void WorkerCoordinator::HandleItemStarted(
    std::size_t worker_id,
    std::uint64_t process_generation,
    const savor::wrms::WorksetItemStartedPayload& payload) {
    const auto context =
        CurrentEventContext(worker_id, process_generation);
    if (!context.has_value()) {
        return;
    }
    std::optional<std::int64_t> job_id;
    std::optional<int> program_kind;
    {
        std::lock_guard<std::mutex> routes_lock(routes_mutex_);
        const auto route = routes_.find(payload.workset_id);
        if (route != routes_.end() &&
            route->second.worker_id == worker_id &&
            route->second.process_generation == process_generation &&
            payload.item_ordinal <
                route->second.job_ids_by_ordinal.size()) {
            job_id = route->second.job_ids_by_ordinal[
                payload.item_ordinal];
            program_kind = route->second.program_kind;
        }
    }
    worker_status_.UpdateState(
        ToTelemetryWorkerId(worker_id),
        WorkerStateKind::Running);
    worker_status_.SetCurrentJob(
        ToTelemetryWorkerId(worker_id), job_id, program_kind);
    worker_status_.RecordHeartbeat(ToTelemetryWorkerId(worker_id));

    std::function<void(
        const WorkerCoordinatorEventContext&,
        const savor::wrms::WorksetItemStartedPayload&)> callback;
    {
        std::lock_guard<std::mutex> callback_lock(callbacks_mutex_);
        callback = callbacks_.item_started;
    }
    if (callback) {
        callback(*context, payload);
    }
}

void WorkerCoordinator::HandleItemProgress(
    std::size_t worker_id,
    std::uint64_t process_generation,
    const savor::wrms::InvocationProgressPayload& payload) {
    const auto context =
        CurrentEventContext(worker_id, process_generation);
    if (!context.has_value()) {
        return;
    }
    worker_status_.RecordHeartbeat(ToTelemetryWorkerId(worker_id));
    worker_status_.RecordProgress(
        ToTelemetryWorkerId(worker_id),
        payload.display_text.empty()
            ? "workset progress ordinal "
                + std::to_string(payload.ordinal)
            : payload.display_text,
        ParseTelemetryJobId(payload.durable_job_id));

    std::function<void(
        const WorkerCoordinatorEventContext&,
        const savor::wrms::InvocationProgressPayload&)> callback;
    {
        std::lock_guard<std::mutex> callback_lock(callbacks_mutex_);
        callback = callbacks_.item_progress;
    }
    if (callback) {
        callback(*context, payload);
    }
}

void WorkerCoordinator::HandleItemTerminal(
    std::size_t worker_id,
    std::uint64_t process_generation,
    const savor::wrms::WorksetItemTerminalPayload& payload) {
    const auto context =
        CurrentEventContext(worker_id, process_generation);
    if (!context.has_value()) {
        return;
    }
    std::optional<std::int64_t> terminal_job_id;
    {
        std::lock_guard<std::mutex> routes_lock(routes_mutex_);
        const auto route_it = routes_.find(payload.workset_id);
        if (route_it == routes_.end()
            || route_it->second.worker_id != worker_id
            || route_it->second.process_generation
                != process_generation) {
            return;
        }
        route_it->second.retained_terminals.insert(
            TerminalKey(payload));
        if (payload.item_ordinal <
            route_it->second.job_ids_by_ordinal.size()) {
            terminal_job_id = route_it->second.job_ids_by_ordinal[
                payload.item_ordinal];
        }
    }
    ++terminal_envelopes_;
    worker_status_.RecordHeartbeat(ToTelemetryWorkerId(worker_id));
    if (terminal_job_id) {
        worker_status_.ClearCurrentJobIf(
            ToTelemetryWorkerId(worker_id), *terminal_job_id);
    }

    std::function<void(const WorkerTerminalEnvelope&)> callback;
    {
        std::lock_guard<std::mutex> callback_lock(callbacks_mutex_);
        callback = callbacks_.item_terminal;
    }
    if (callback) {
        callback(WorkerTerminalEnvelope{
            .wrms_protocol_version =
                context->wrms_protocol_version,
            .worker_id =
                static_cast<std::uint64_t>(context->worker_id),
            .process_generation =
                context->process_generation,
            .cancellation_reason = payload.cancellation_reason,
            .terminal = payload,
        });
    }
}

void WorkerCoordinator::HandleCredits(
    std::size_t worker_id,
    std::uint64_t process_generation,
    const savor::wrms::WorksetCreditsPayload& payload) {
    const auto context =
        CurrentEventContext(worker_id, process_generation);
    if (!context.has_value()) {
        return;
    }
    const auto slot = GetWorkerSlot(worker_id);
    if (!slot) {
        return;
    }
    {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        if (slot->process_generation != process_generation) {
            return;
        }
        if (slot->quarantine_requested) {
            slot->available_item_credits = 0;
            return;
        }
        slot->available_item_credits =
            payload.available_item_credits;
    }
    worker_status_.RecordHeartbeat(ToTelemetryWorkerId(worker_id));

    std::function<void(
        const WorkerCoordinatorEventContext&,
        const savor::wrms::WorksetCreditsPayload&)> callback;
    {
        std::lock_guard<std::mutex> callback_lock(callbacks_mutex_);
        callback = callbacks_.credits;
    }
    if (callback) {
        callback(*context, payload);
    }
    NotifyAvailabilityChanged();
}

void WorkerCoordinator::HandleWorksetSummary(
    std::size_t worker_id,
    std::uint64_t process_generation,
    const savor::wrms::WorksetSummaryPayload& payload) {
    const auto context =
        CurrentEventContext(worker_id, process_generation);
    if (!context.has_value()) {
        return;
    }
    {
        std::lock_guard<std::mutex> routes_lock(routes_mutex_);
        const auto route_it = routes_.find(payload.workset_id);
        if (route_it != routes_.end()
            && route_it->second.worker_id == worker_id
            && route_it->second.process_generation
                == process_generation) {
            route_it->second.summary_observed = true;
        }
    }
    worker_status_.RecordHeartbeat(ToTelemetryWorkerId(worker_id));

    std::function<void(
        const WorkerCoordinatorEventContext&,
        const savor::wrms::WorksetSummaryPayload&)> callback;
    {
        std::lock_guard<std::mutex> callback_lock(callbacks_mutex_);
        callback = callbacks_.workset_summary;
    }
    if (callback) {
        callback(*context, payload);
    }
    RemoveCompletedRouteIfPossible(payload.workset_id);
}

std::optional<WorkerCoordinatorEventContext>
WorkerCoordinator::CurrentEventContext(
    std::size_t worker_id,
    std::uint64_t process_generation) const {
    const auto slot = GetWorkerSlot(worker_id);
    if (!slot) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> slot_lock(slot->mutex);
    if (slot->process_generation != process_generation
        || slot->runtime_contract_sha256.empty()) {
        return std::nullopt;
    }
    return EventContext(*slot);
}

void WorkerCoordinator::NotifyAvailabilityChanged() const {
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> callback_lock(callbacks_mutex_);
        callback = callbacks_.availability_changed;
    }
    if (callback) {
        callback();
    }
}

void WorkerCoordinator::NotifyWorkerUnavailable(
    WorkerUnavailableEvent event) const {
    std::function<void(const WorkerUnavailableEvent&)> callback;
    {
        std::lock_guard<std::mutex> callback_lock(callbacks_mutex_);
        callback = callbacks_.worker_unavailable;
    }
    if (callback) {
        callback(event);
    }
}

void WorkerCoordinator::RefreshStartResult(std::string diagnostic) {
    if (!started_.load(std::memory_order_acquire)) {
        return;
    }
    const auto fleet = SnapshotFleetStartup();
    const auto ready_workers = fleet.ready;
    const auto desired = fleet.desired;
    const bool startup_exhausted =
        desired != 0
        && ready_workers == 0
        && fleet.exhausted == desired;
    std::string exhaustion_diagnostic;
    if (startup_exhausted) {
        for (const auto& slot : fleet.worker_slots) {
            if (exhaustion_diagnostic.empty()
                && !slot.terminal_diagnostic.empty()) {
                exhaustion_diagnostic =
                    slot.terminal_diagnostic;
            }
        }
    }
    if (startup_exhausted) {
        diagnostic =
            "all worker startup attempts were exhausted"
            + (exhaustion_diagnostic.empty()
                ? std::string{}
                : ": " + exhaustion_diagnostic);
    } else if (diagnostic.empty()
        && ready_workers == 0 && desired != 0) {
        diagnostic =
            "worker coordinator is still attempting to start a worker";
    }
    std::lock_guard<std::mutex> result_lock(start_result_mutex_);
    start_result_ = {
        .status = startup_exhausted
            ? WorkerCoordinatorStartStatus::StartupExhausted
            : ready_workers == 0
                ? WorkerCoordinatorStartStatus::
                    StartedWithoutReadyWorkers
                : WorkerCoordinatorStartStatus::Started,
        .ready_workers = ready_workers,
        .diagnostic = std::move(diagnostic),
    };
}

void WorkerCoordinator::RemoveCompletedRouteIfPossible(
    std::uint64_t workset_id) {
    std::lock_guard<std::mutex> routes_lock(routes_mutex_);
    const auto route_it = routes_.find(workset_id);
    if (route_it == routes_.end()) {
        return;
    }
    if (route_it->second.terminal_state_observed
        && route_it->second.summary_observed
        && route_it->second.retained_terminals.empty()) {
        routes_.erase(route_it);
    }
}

void WorkerCoordinator::RemoveRoutesForWorker(
    std::size_t worker_id,
    std::uint64_t process_generation,
    std::vector<std::uint64_t>* removed_worksets) {
    std::lock_guard<std::mutex> routes_lock(routes_mutex_);
    for (auto route_it = routes_.begin(); route_it != routes_.end();) {
        if (route_it->second.worker_id == worker_id
            && route_it->second.process_generation
                == process_generation) {
            if (removed_worksets != nullptr) {
                removed_worksets->push_back(route_it->first);
            }
            route_it = routes_.erase(route_it);
        } else {
            ++route_it;
        }
    }
}

std::string WorkerCoordinator::TerminalKey(
    const savor::runtime::WorkerItemTerminalCorrelation& terminal) {
    std::ostringstream key;
    key << terminal.workset_id.value() << ':'
        << terminal.item_id.value() << ':'
        << terminal.item_ordinal << ':'
        << terminal.invocation_id.value() << ':'
        << terminal.attempt_id.value() << ':'
        << terminal.terminal_id.value() << ':'
        << terminal.terminal_order.value();
    return key.str();
}

std::string WorkerCoordinator::TerminalKey(
    const savor::wrms::WorksetItemTerminalPayload& terminal) {
    std::ostringstream key;
    key << terminal.workset_id << ':'
        << terminal.item_id << ':'
        << terminal.item_ordinal << ':'
        << terminal.invocation_id << ':'
        << terminal.attempt_id << ':'
        << terminal.terminal_id << ':'
        << terminal.terminal_order;
    return key.str();
}

} // namespace savor::runner::parallel::savordb
