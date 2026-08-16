#include "DolphinWrapperBackend.h"

#include "../../Boot/Boot.h"
#include "../../Core/DolphinWrapper.h"
#include "../../Tas/DtmFile.h"
#include "../../Utils/Hash.h"

#include "Common/Buffer.h"
#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/Movie.h"
#include "Core/PowerPC/BreakPoints.h"

#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace savor::runtime {
namespace {

[[nodiscard]] std::uint32_t ClampUnsignedTimeout(std::chrono::milliseconds timeout) noexcept
{
    const auto count = timeout.count();
    if (count <= 0)
        return 1;
    return static_cast<std::uint32_t>(std::min<std::int64_t>(
        count,
        std::numeric_limits<std::uint32_t>::max()));
}

[[nodiscard]] bool IsRegularFile(const std::filesystem::path& path) noexcept
{
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

[[nodiscard]] MovieBackendResult MovieFailure(
    std::string message,
    GuestIntegrity integrity = GuestIntegrity::Preserved)
{
    return MovieBackendResult::Failure(
        std::move(message),
        integrity);
}

[[nodiscard]] bool ReadBinaryFile(
    const std::filesystem::path& path,
    std::vector<std::uint8_t>& bytes)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return false;
    const std::streamsize size = input.tellg();
    if (size < 0)
        return false;
    bytes.resize(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    return size == 0 ||
        static_cast<bool>(input.read(
            reinterpret_cast<char*>(bytes.data()),
            size));
}

[[nodiscard]] MovieBackendResult MaterializeExactMovieHistory(
    const MovieCheckpointMetadata& movie,
    const std::filesystem::path& runtime_root,
    std::filesystem::path& output)
{
    if (runtime_root.empty() || movie.dtm_bytes.empty() ||
        movie.dtm_sha256.size() != 64)
    {
        return MovieFailure(
            "Movie restore requires exact embedded DTM bytes, hash, and runtime root");
    }
    const std::string embedded_hash = hash::sha256(
        movie.dtm_bytes.data(),
        movie.dtm_bytes.size());
    if (embedded_hash != movie.dtm_sha256)
    {
        return MovieFailure(
            "Movie restore DTM bytes do not match the recorded SHA-256");
    }

    std::error_code error;
    const std::filesystem::path directory =
        runtime_root / "movie-restore";
    std::filesystem::create_directories(directory, error);
    if (error)
    {
        return MovieFailure(
            "Movie restore staging directory could not be created: " +
            error.message());
    }
    output = directory /
        ("checkpoint-" + movie.dtm_sha256 + ".dtm");
    if (IsRegularFile(output))
    {
        try
        {
            if (hash::sha256_of_file(output.string()) ==
                movie.dtm_sha256)
            {
                return MovieBackendResult::Success();
            }
        }
        catch (...)
        {
        }
        std::filesystem::remove(output, error);
        error.clear();
    }

    const std::filesystem::path staging =
        std::filesystem::path(output.string() + ".stage");
    std::filesystem::remove(staging, error);
    error.clear();
    {
        std::ofstream stream(
            staging,
            std::ios::binary | std::ios::trunc);
        if (!stream)
            return MovieFailure("Movie restore DTM staging file could not be created");
        stream.write(
            reinterpret_cast<const char*>(movie.dtm_bytes.data()),
            static_cast<std::streamsize>(movie.dtm_bytes.size()));
        stream.flush();
        if (!stream.good())
        {
            stream.close();
            std::filesystem::remove(staging, error);
            return MovieFailure("Movie restore DTM staging write failed");
        }
    }
    try
    {
        if (hash::sha256_of_file(staging.string()) !=
            movie.dtm_sha256)
        {
            std::filesystem::remove(staging, error);
            return MovieFailure("Movie restore staged DTM failed hash verification");
        }
    }
    catch (const std::exception& ex)
    {
        std::filesystem::remove(staging, error);
        return MovieFailure(ex.what());
    }
    std::filesystem::rename(staging, output, error);
    if (error)
    {
        std::filesystem::remove(staging, error);
        return MovieFailure(
            "Movie restore DTM could not be published: " +
            error.message());
    }

    savor::tas::DtmFile dtm;
    if (!dtm.load(output.string()) ||
        dtm.validate().has_error() ||
        dtm.compute_sha256() != movie.dtm_sha256)
    {
        std::filesystem::remove(output, error);
        return MovieFailure(
            "Movie restore staged DTM did not validate");
    }
    return MovieBackendResult::Success();
}

[[nodiscard]] PhysicalStopPointPlan ReadPhysicalPlan(Core::System& system)
{
    PhysicalStopPointPlan result;
    const auto& power_pc = system.GetPowerPC();
    for (const auto& breakpoint : power_pc.GetBreakPoints().GetBreakPoints())
    {
        result.pcs.push_back(PhysicalPcStop{breakpoint.address});
    }
    for (const auto& check : power_pc.GetMemChecks().GetMemChecks())
    {
        result.memory.push_back(PhysicalMemoryStop{
            check.start_address,
            check.end_address,
            check.is_break_on_read,
            check.is_break_on_write});
    }
    std::ranges::sort(result.pcs, {}, &PhysicalPcStop::pc);
    std::ranges::sort(result.memory, [](const auto& lhs, const auto& rhs) {
        if (lhs.start != rhs.start)
            return lhs.start < rhs.start;
        return lhs.end < rhs.end;
    });
    return result;
}

[[nodiscard]] bool PhysicalObjectsHaveManagerShape(
    Core::System& system,
    const PhysicalStopPointPlan& expected,
    std::string* error)
{
    const auto& power_pc = system.GetPowerPC();
    const auto& breakpoints = power_pc.GetBreakPoints().GetBreakPoints();
    if (breakpoints.size() != expected.pcs.size())
    {
        if (error)
            *error = "unexpected regular Dolphin breakpoint count";
        return false;
    }
    for (const auto& wanted : expected.pcs)
    {
        const auto* actual = power_pc.GetBreakPoints().GetRegularBreakpoint(wanted.pc);
        if (!actual || !actual->is_enabled ||
            actual->break_on_hit ||
            actual->log_on_hit || actual->condition.has_value())
        {
            if (error)
            {
                std::ostringstream out;
                out << "unmanaged or malformed Dolphin breakpoint at 0x"
                    << std::hex << wanted.pc;
                *error = out.str();
            }
            return false;
        }
    }

    const auto& checks = power_pc.GetMemChecks().GetMemChecks();
    if (checks.size() != expected.memory.size())
    {
        if (error)
            *error = "unexpected regular Dolphin memcheck count";
        return false;
    }
    for (const auto& wanted : expected.memory)
    {
        const auto found = std::ranges::find_if(checks, [&](const TMemCheck& check) {
            return check.start_address == wanted.start &&
                check.end_address == wanted.end;
        });
        if (found == checks.end() || !found->is_enabled ||
            found->is_ranged != (wanted.start != wanted.end) ||
            found->is_break_on_read != wanted.read ||
            found->is_break_on_write != wanted.write ||
            found->break_on_hit || found->log_on_hit ||
            found->condition.has_value())
        {
            if (error)
            {
                std::ostringstream out;
                out << "unmanaged or malformed Dolphin memcheck at 0x"
                    << std::hex << wanted.start;
                *error = out.str();
            }
            return false;
        }
    }
    return true;
}

void ReplacePhysicalPlan(
    Core::System& system,
    const PhysicalStopPointPlan& previous,
    const PhysicalStopPointPlan& next)
{
    if (!next.pcs.empty() || !next.memory.empty())
    {
        Config::SetCurrent(
            Config::MAIN_ENABLE_DEBUGGING,
            true);
    }
    auto& power_pc = system.GetPowerPC();
    auto& breakpoints = power_pc.GetBreakPoints();
    auto& memchecks = power_pc.GetMemChecks();

    for (const auto& pc : previous.pcs)
        (void)breakpoints.Remove(pc.pc);
    for (const auto& memory : previous.memory)
        (void)memchecks.Remove(memory.start, false);

    for (const auto& pc : next.pcs)
    {
        TBreakPoint breakpoint;
        breakpoint.address = pc.pc;
        breakpoint.is_enabled = true;
        breakpoint.log_on_hit = false;
        breakpoint.break_on_hit = false;
        breakpoints.Add(std::move(breakpoint));
    }
    for (const auto& memory : next.memory)
    {
        TMemCheck check;
        check.start_address = memory.start;
        check.end_address = memory.end;
        check.is_enabled = true;
        check.is_ranged = memory.start != memory.end;
        check.is_break_on_read = memory.read;
        check.is_break_on_write = memory.write;
        check.log_on_hit = false;
        check.break_on_hit = false;
        memchecks.Add(std::move(check), false);
    }
    memchecks.Update();
}

} // namespace

struct DolphinWrapperBackend::Impl
{
    struct PauseConfirmation
    {
        std::atomic<std::uint64_t> requested{0};
        std::atomic<std::uint64_t> acknowledged{0};
    };

    // Core::SetState(Paused) is Dolphin's host-side synchronization primitive:
    // it waits until the CPU has left RunLoop. It cannot run on WorkerRuntime's
    // actor because that would turn RequestPause into a blocking operation.
    // Keep one backend-owned helper alive for the open session instead. The
    // helper is always joined before its Core::System is destroyed.
    struct PauseSynchronizer
    {
        ~PauseSynchronizer()
        {
            StopAndJoin();
        }

        PauseSynchronizer() = default;
        PauseSynchronizer(const PauseSynchronizer&) = delete;
        PauseSynchronizer& operator=(const PauseSynchronizer&) = delete;

        [[nodiscard]] bool Start(
            Core::System& next_system,
            std::shared_ptr<PauseConfirmation> next_confirmation,
            std::string* error)
        {
            StopAndJoin();
            {
                std::lock_guard lock(mutex);
                system = &next_system;
                confirmation = std::move(next_confirmation);
                pending_generation = 0;
                stopping = false;
                failed = false;
                failure.clear();
            }
            try
            {
                worker = std::thread([this] { Run(); });
                return true;
            }
            catch (const std::exception& ex)
            {
                std::lock_guard lock(mutex);
                system = nullptr;
                confirmation.reset();
                stopping = true;
                if (error)
                    *error = ex.what();
                return false;
            }
            catch (...)
            {
                std::lock_guard lock(mutex);
                system = nullptr;
                confirmation.reset();
                stopping = true;
                if (error)
                    *error = "unknown pause-helper startup failure";
                return false;
            }
        }

        [[nodiscard]] bool Request(std::uint64_t generation) noexcept
        {
            {
                std::lock_guard lock(mutex);
                if (!worker.joinable() || stopping || !system ||
                    !confirmation || failed)
                {
                    return false;
                }
                pending_generation =
                    std::max(pending_generation, generation);
            }
            wake.notify_one();
            return true;
        }

        [[nodiscard]] bool Failure(std::string* diagnostic) const
        {
            std::lock_guard lock(mutex);
            if (diagnostic)
                *diagnostic = failure;
            return failed;
        }

        void StopAndJoin() noexcept
        {
            {
                std::lock_guard lock(mutex);
                stopping = true;
            }
            wake.notify_one();
            if (worker.joinable())
                worker.join();
            std::lock_guard lock(mutex);
            system = nullptr;
            confirmation.reset();
            pending_generation = 0;
        }

    private:
        static void Acknowledge(
            const std::shared_ptr<PauseConfirmation>& target,
            std::uint64_t generation) noexcept
        {
            std::uint64_t acknowledged =
                target->acknowledged.load(std::memory_order_acquire);
            while (acknowledged < generation &&
                !target->acknowledged.compare_exchange_weak(
                    acknowledged,
                    generation,
                    std::memory_order_release,
                    std::memory_order_acquire))
            {
            }
        }

        void Run() noexcept
        {
            for (;;)
            {
                Core::System* target_system = nullptr;
                std::shared_ptr<PauseConfirmation> target_confirmation;
                std::uint64_t generation = 0;
                {
                    std::unique_lock lock(mutex);
                    wake.wait(lock, [this] {
                        return stopping || pending_generation != 0;
                    });
                    // Finish a pause already accepted by Request(), even when
                    // shutdown has begun, before releasing the Core::System.
                    if (pending_generation == 0 && stopping)
                        return;
                    target_system = system;
                    target_confirmation = confirmation;
                    generation = std::exchange(pending_generation, 0);
                }

                try
                {
                    Core::SetState(
                        *target_system,
                        Core::State::Paused);
                    if (Core::GetState(*target_system) !=
                        Core::State::Paused)
                    {
                        std::lock_guard lock(mutex);
                        failed = true;
                        failure =
                            "Dolphin did not enter Paused state after synchronized pause";
                    }
                    else
                    {
                        // SetState(Paused) returns only after CPUManager has
                        // observed m_state_cpu_thread_active == false.
                        Acknowledge(target_confirmation, generation);
                    }
                }
                catch (const std::exception& ex)
                {
                    std::lock_guard lock(mutex);
                    failed = true;
                    failure =
                        std::string("Dolphin synchronized pause threw: ") +
                        ex.what();
                }
                catch (...)
                {
                    std::lock_guard lock(mutex);
                    failed = true;
                    failure = "Dolphin synchronized pause threw";
                }

                std::lock_guard lock(mutex);
                if (failed)
                {
                    pending_generation = 0;
                    return;
                }
                if (stopping && pending_generation == 0)
                    return;
            }
        }

        mutable std::mutex mutex;
        std::condition_variable wake;
        std::thread worker;
        Core::System* system = nullptr;
        std::shared_ptr<PauseConfirmation> confirmation;
        std::uint64_t pending_generation = 0;
        bool stopping = true;
        bool failed = false;
        std::string failure;
    };

    std::unique_ptr<DolphinWrapper> wrapper;
    BackendOpenOptions last_open_options;
    bool has_open_options = false;
    bool open = false;
    savor::probe::INativeStopSink* native_sink = nullptr;
    PhysicalStopPointPlan owned_physical_plan;
    PhysicalPlanGeneration physical_generation;
    DolphinBackendCpuCore cpu_core =
        DolphinBackendCpuCore::ProductionDefault;
    std::shared_ptr<PauseConfirmation> pause_confirmation =
        std::make_shared<PauseConfirmation>();
    PauseSynchronizer pause_synchronizer;
    mutable std::uint32_t last_confirmed_pc = 0;
    int state_callback_handle = -1;
    std::optional<std::filesystem::path> prepared_movie_path;
    std::optional<std::filesystem::path> prepared_movie_savestate;
    std::optional<std::string> prepared_movie_sha256;
    bool prepared_movie_core_started = false;
    std::optional<std::string> active_movie_sha256;
    bool pause_at_playback_end_owned = false;
    bool pause_at_playback_end_had_current_run_value = false;
    bool pause_at_playback_end_previous_value = false;
    std::optional<SavestateMovieRestoreContext> prepared_movie_replacement;
    std::optional<std::string> prepared_movie_replacement_sha256;
    bool prepared_movie_started_for_replacement = false;
    std::vector<std::filesystem::path> owned_movie_restore_paths;
    ArtifactCompatibilityToken compatibility;
    std::uint64_t movie_checkpoint_sequence = 1;
    std::uint64_t savestate_file_capture_sequence = 1;

    void DetachStateCallback() noexcept
    {
        if (state_callback_handle >= 0)
            (void)Core::RemoveOnStateChangedCallback(
                &state_callback_handle);
    }

    void ResetPauseConfirmation(bool confirmed)
    {
        pause_confirmation = std::make_shared<PauseConfirmation>();
        if (!confirmed)
            pause_confirmation->requested.store(1);
        last_confirmed_pc = 0;
    }

    void InvalidatePauseConfirmation()
    {
        const std::uint64_t generation =
            pause_confirmation->requested.fetch_add(
                1,
                std::memory_order_acq_rel) +
            1;
        if (generation == 0)
            pause_confirmation->requested.store(1, std::memory_order_release);
    }

    [[nodiscard]] bool PauseConfirmed() const noexcept
    {
        return pause_confirmation->acknowledged.load(
                   std::memory_order_acquire) >=
            pause_confirmation->requested.load(
                std::memory_order_acquire);
    }

    void ConfirmPause() noexcept
    {
        pause_confirmation->acknowledged.store(
            pause_confirmation->requested.load(
                std::memory_order_acquire),
            std::memory_order_release);
    }

    [[nodiscard]] BackendResult StartPauseInfrastructure()
    {
        if (!wrapper || !wrapper->system())
        {
            return BackendResult::Failure(
                BackendErrorCode::InvalidState,
                "Dolphin pause infrastructure requires a live wrapper");
        }
        ResetPauseConfirmation(true);
        const auto confirmation = pause_confirmation;
        Core::System* const system = wrapper->system();
        state_callback_handle = Core::AddOnStateChangedCallback(
            [confirmation, system](Core::State state) {
                if (state != Core::State::Paused)
                    return;
                auto acknowledge = [confirmation] {
                    const std::uint64_t generation =
                        confirmation->requested.load(
                            std::memory_order_acquire);
                    std::uint64_t acknowledged =
                        confirmation->acknowledged.load(
                            std::memory_order_acquire);
                    while (acknowledged < generation &&
                        !confirmation->acknowledged.compare_exchange_weak(
                            acknowledged,
                            generation,
                            std::memory_order_release,
                            std::memory_order_acquire))
                    {
                    }
                };
                if (Core::IsCPUThread())
                    system->GetCPU().AddCPUThreadJob(std::move(acknowledge));
                else
                    acknowledge();
            });

        std::string error;
        if (!pause_synchronizer.Start(
                *system, confirmation, &error))
        {
            DetachStateCallback();
            ResetPauseConfirmation(false);
            return BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                error.empty()
                    ? "Failed to start Dolphin pause synchronizer"
                    : "Failed to start Dolphin pause synchronizer: " + error,
                BackendIntegrity::Unknown);
        }
        return BackendResult::Success();
    }

    [[nodiscard]] BackendResult RequireOpen() const
    {
        if (!open || !wrapper)
        {
            return BackendResult::Failure(
                BackendErrorCode::InvalidState,
                "Dolphin backend is not open");
        }
        return BackendResult::Success();
    }
};

DolphinWrapperBackend::DolphinWrapperBackend(
    DolphinBackendCpuCore cpu_core)
    : impl_(std::make_unique<Impl>())
{
    impl_->cpu_core = cpu_core;
}

DolphinWrapperBackend::~DolphinWrapperBackend()
{
    if (impl_)
        (void)Close();
}

BackendResult DolphinWrapperBackend::Open(const BackendOpenOptions& options)
{
    if (impl_->open)
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidState,
            "Dolphin backend is already open");
    }
    if (options.user_directory.empty())
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidArgument,
            "Dolphin user directory is required");
    }
    if (options.dolphin_base_directory.empty())
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidArgument,
            "Dolphin base directory is required");
    }
    if (options.iso_path.empty() || !IsRegularFile(options.iso_path))
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidArgument,
            "A readable game ISO path is required");
    }
    auto wrapper = std::make_unique<DolphinWrapper>();

    simboot::BootOptions boot_options;
    boot_options.user_dir = options.user_directory;
    boot_options.dolphin_qt_base = options.dolphin_base_directory;
    boot_options.force_resync_from_base = options.force_resync_from_base;
    boot_options.visual = options.visual;
    boot_options.render_widget_handle =
        reinterpret_cast<void*>(options.render_window_handle);
    boot_options.save_config_on_success = false;

    std::string error;
    if (!simboot::BootDolphinWrapper(*wrapper, boot_options, &error))
    {
        return BackendResult::Failure(
            BackendErrorCode::BootFailed,
            error.empty() ? "Dolphin boot failed" : std::move(error));
    }

    if (impl_->cpu_core == DolphinBackendCpuCore::Jit64)
        Config::SetCurrent(Config::MAIN_CPU_CORE, PowerPC::CPUCore::JIT64);

    if (!wrapper->loadGame(
            options.iso_path.string(),
            true))
    {
        if (wrapper->system()->GetMovie().IsMovieActive())
            wrapper->system()->GetMovie().EndPlayInput(false);
        wrapper.reset();
        return BackendResult::Failure(
            BackendErrorCode::GameLoadFailed,
            "Dolphin failed to load the requested game",
            BackendIntegrity::Unknown);
    }

    wrapper->ConfigurePortsStandardPadP1();

    impl_->wrapper = std::move(wrapper);
    if (BackendResult pause = impl_->StartPauseInfrastructure(); !pause.ok)
    {
        impl_->wrapper.reset();
        return pause;
    }
    impl_->last_open_options = options;
    impl_->has_open_options = true;
    impl_->open = true;
    impl_->active_movie_sha256.reset();
    try
    {
        const auto disc = impl_->wrapper->getDiscInfo();
        impl_->compatibility = {
            .game_id = disc ? disc->game_id : std::string{},
            .iso_sha256 =
                hash::sha256_of_file(options.iso_path.string()),
            .emulator_build = "dolphin-2506a",
            .runtime_revision = "worker-runtime-slice4",
        };
    }
    catch (const std::exception& ex)
    {
        (void)Close();
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            std::string("Failed to establish state compatibility: ") +
                ex.what(),
            BackendIntegrity::Unknown);
    }
    return BackendResult::Success();
}

MovieBackendResult DolphinWrapperBackend::StopCoreForPreparedReadOnlyMovie()
{
    if (!impl_->open || !impl_->wrapper || !impl_->has_open_options)
    {
        return MovieBackendResult::Failure(
            "Dolphin movie preparation requires an open wrapper session");
    }
    if (!impl_->prepared_movie_path || !impl_->prepared_movie_sha256)
    {
        return MovieBackendResult::Failure(
            "Dolphin core stop requires one prepared read-only movie");
    }

    try
    {
        auto& system = *impl_->wrapper->system();
        Core::CPUThreadGuard guard(system);
        std::string error;
        if (!PhysicalObjectsHaveManagerShape(
                system,
                impl_->owned_physical_plan,
                &error))
        {
            return MovieBackendResult::Failure(
                error.empty()
                    ? "Dolphin core stop found unmanaged physical stop points"
                    : std::move(error),
                GuestIntegrity::Unknown);
        }
    }
    catch (const std::exception& ex)
    {
        return MovieBackendResult::Failure(
            std::string("failed validating physical stop points before core stop: ") +
                ex.what(),
            GuestIntegrity::Unknown);
    }
    catch (...)
    {
        return MovieBackendResult::Failure(
            "failed validating physical stop points before core stop",
            GuestIntegrity::Unknown);
    }

    impl_->pause_synchronizer.StopAndJoin();
    impl_->DetachStateCallback();
    impl_->active_movie_sha256.reset();
    std::string error;
    bool stopped = false;
    try
    {
        stopped = impl_->wrapper->stopCoreForReadOnlyMovie(&error);
    }
    catch (const std::exception& ex)
    {
        error = std::string(
            "Dolphin guest-core stop raised an exception: ") +
            ex.what();
    }
    catch (...)
    {
        error = "Dolphin guest-core stop raised an unknown exception";
    }
    if (!stopped)
    {
        impl_->ResetPauseConfirmation(false);
        return MovieBackendResult::Failure(
            error.empty()
                ? "Dolphin failed to stop its guest core for movie playback"
                : std::move(error),
            GuestIntegrity::Unknown);
    }
    impl_->ResetPauseConfirmation(false);
    return MovieBackendResult::Success();
}

MovieBackendResult
DolphinWrapperBackend::StartPreparedReadOnlyMovieCorePaused()
{
    if (!impl_->open || !impl_->wrapper || !impl_->has_open_options)
    {
        return MovieBackendResult::Failure(
            "Dolphin movie start requires an open wrapper session");
    }
    if (!impl_->prepared_movie_path || !impl_->prepared_movie_sha256)
    {
        return MovieBackendResult::Failure(
            "Dolphin movie start requires one prepared read-only movie");
    }
    if (impl_->prepared_movie_core_started)
    {
        return MovieBackendResult::Failure(
            "Dolphin prepared movie core has already been started");
    }

    std::optional<std::string> discovered_startup;
    std::string error;
    bool started = false;
    try
    {
        started = impl_->wrapper->startReadOnlyMovieFromStoppedCore(
            impl_->prepared_movie_path->string(),
            discovered_startup,
            &error);
    }
    catch (const std::exception& ex)
    {
        error = std::string(
            "Dolphin prepared-movie start raised an exception: ") +
            ex.what();
    }
    catch (...)
    {
        error = "Dolphin prepared-movie start raised an unknown exception";
    }
    if (!started)
    {
        impl_->ResetPauseConfirmation(false);
        return MovieBackendResult::Failure(
            error.empty()
                ? "Dolphin failed to start the prepared read-only movie"
                : std::move(error),
            GuestIntegrity::Unknown);
    }
    const bool startup_matches =
        impl_->prepared_movie_savestate.has_value() ==
            discovered_startup.has_value() &&
        (!discovered_startup ||
         *discovered_startup ==
             impl_->prepared_movie_savestate->string());
    if (!startup_matches)
    {
        (void)StopMovie();
        impl_->ResetPauseConfirmation(false);
        return MovieBackendResult::Failure(
            "Dolphin movie startup-state discovery disagreed with the prepared artifact baseline",
            GuestIntegrity::Unknown);
    }
    if (BackendResult pause = impl_->StartPauseInfrastructure(); !pause.ok)
    {
        return MovieBackendResult::Failure(
            pause.message.empty()
                ? "Dolphin pause infrastructure did not restart"
                : std::move(pause.message),
            GuestIntegrity::Unknown);
    }
    impl_->active_movie_sha256 = impl_->prepared_movie_sha256;
    impl_->prepared_movie_core_started = true;
    return MovieBackendResult::Success();
}

MovieBackendResult
DolphinWrapperBackend::ActivatePreparedReadOnlyMoviePlayback()
{
    if (!impl_->open || !impl_->wrapper || !impl_->has_open_options ||
        !impl_->prepared_movie_path || !impl_->prepared_movie_sha256 ||
        !impl_->prepared_movie_core_started)
    {
        return MovieBackendResult::Failure(
            "Dolphin playback activation requires one prepared paused movie core");
    }
    const BackendExecutionSnapshot execution = QueryExecutionSnapshot();
    if (!execution.result.ok ||
        execution.core_state != BackendCoreState::Paused ||
        !execution.pause_confirmed)
    {
        return MovieBackendResult::Failure(
            execution.result.message.empty()
                ? "Dolphin prepared movie core is not authoritatively paused"
                : execution.result.message,
            execution.result.integrity == BackendIntegrity::Unknown
                ? GuestIntegrity::Unknown
                : GuestIntegrity::Preserved);
    }
    const MovieBackendObservation movie = ObserveMovieWhilePaused();
    if (!movie.result.ok || !movie.playing || movie.recording ||
        !movie.read_only)
    {
        return MovieBackendResult::Failure(
            "Dolphin prepared movie is not active in read-only mode",
            GuestIntegrity::Unknown);
    }
    impl_->prepared_movie_path.reset();
    impl_->prepared_movie_savestate.reset();
    impl_->prepared_movie_sha256.reset();
    impl_->prepared_movie_core_started = false;
    return MovieBackendResult::Success();
}

MovieBackendResult
DolphinWrapperBackend::DiscardPreparedReadOnlyMovie() noexcept
{
    if (impl_->prepared_movie_core_started)
    {
        return MovieBackendResult::Failure(
            "A started prepared movie core must be stopped, not discarded",
            GuestIntegrity::Unknown);
    }
    impl_->prepared_movie_path.reset();
    impl_->prepared_movie_savestate.reset();
    impl_->prepared_movie_sha256.reset();
    return MovieBackendResult::Success();
}

BackendResult DolphinWrapperBackend::Close()
{
    const MovieBackendResult pause_at_end =
        ReleasePauseAtPlaybackEnd();
    impl_->pause_synchronizer.StopAndJoin();
    if (!impl_->wrapper)
    {
        impl_->DetachStateCallback();
        if (impl_->native_sink)
        {
            (void)savor::probe::UnbindNativeStopSink(*impl_->native_sink);
            impl_->native_sink = nullptr;
        }
        savor::probe::UninstallNativeStopHooks();
        impl_->owned_physical_plan = {};
        impl_->physical_generation = {};
        impl_->open = false;
        impl_->active_movie_sha256.reset();
        impl_->prepared_movie_path.reset();
        impl_->prepared_movie_savestate.reset();
        impl_->prepared_movie_sha256.reset();
        impl_->prepared_movie_core_started = false;
        impl_->prepared_movie_replacement.reset();
        impl_->prepared_movie_replacement_sha256.reset();
        impl_->prepared_movie_started_for_replacement = false;
        for (const auto& path : impl_->owned_movie_restore_paths)
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            std::filesystem::remove(
                std::filesystem::path(path.string() + ".stage"),
                ignored);
        }
        impl_->owned_movie_restore_paths.clear();
        impl_->ResetPauseConfirmation(false);
        if (!pause_at_end.ok)
        {
            return BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                pause_at_end.message.empty()
                    ? "Dolphin pause-at-playback-end configuration was not restored"
                    : pause_at_end.message,
                BackendIntegrity::Unknown);
        }
        return BackendResult::Success();
    }

    try
    {
        bool cleanup_ok = pause_at_end.ok &&
            impl_->owned_physical_plan.pcs.empty() &&
            impl_->owned_physical_plan.memory.empty();
        std::string cleanup_message;
        if (!pause_at_end.ok)
        {
            cleanup_message = pause_at_end.message.empty()
                ? "Dolphin pause-at-playback-end configuration was not restored"
                : pause_at_end.message;
        }
        if (!impl_->owned_physical_plan.pcs.empty() ||
            !impl_->owned_physical_plan.memory.empty())
        {
            if (!cleanup_message.empty())
                cleanup_message += "; ";
            cleanup_message +=
                "PhysicalStopPointManager did not remove all owned sites before backend close";
        }
        if (impl_->native_sink)
        {
            cleanup_ok =
                savor::probe::UnbindNativeStopSink(*impl_->native_sink) &&
                cleanup_ok;
            impl_->native_sink = nullptr;
        }
        savor::probe::UninstallNativeStopHooks();
        impl_->DetachStateCallback();
        impl_->wrapper.reset();
        impl_->open = false;
        impl_->active_movie_sha256.reset();
        impl_->prepared_movie_path.reset();
        impl_->prepared_movie_savestate.reset();
        impl_->prepared_movie_sha256.reset();
        impl_->prepared_movie_core_started = false;
        impl_->prepared_movie_replacement.reset();
        impl_->prepared_movie_replacement_sha256.reset();
        impl_->prepared_movie_started_for_replacement = false;
        for (const auto& path : impl_->owned_movie_restore_paths)
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            std::filesystem::remove(
                std::filesystem::path(path.string() + ".stage"),
                ignored);
        }
        impl_->owned_movie_restore_paths.clear();
        impl_->ResetPauseConfirmation(false);
        impl_->owned_physical_plan = {};
        impl_->physical_generation = {};
        if (!cleanup_ok)
        {
            return BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                cleanup_message.empty()
                    ? "Dolphin stop-point cleanup could not be proven"
                    : std::move(cleanup_message),
                BackendIntegrity::Unknown);
        }
        return BackendResult::Success();
    }
    catch (const std::exception& ex)
    {
        impl_->open = false;
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            std::string("Dolphin shutdown failed: ") + ex.what(),
            BackendIntegrity::Unknown);
    }
    catch (...)
    {
        impl_->open = false;
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "Dolphin shutdown failed",
            BackendIntegrity::Unknown);
    }
}

BackendCoreState DolphinWrapperBackend::QueryCoreState() const noexcept
{
    if (!impl_->open || !impl_->wrapper)
        return BackendCoreState::Closed;
    if (impl_->wrapper->isEmulationPaused())
        return BackendCoreState::Paused;
    if (impl_->wrapper->isRunning())
        return BackendCoreState::Running;
    return BackendCoreState::Stopped;
}

BackendHealthReport DolphinWrapperBackend::CheckHealth() const
{
    const BackendCoreState state = QueryCoreState();
    if (state == BackendCoreState::Closed)
        return {false, state, "Dolphin backend is closed"};
    if (state == BackendCoreState::Unknown)
        return {false, state, "Dolphin core state is unknown"};
    if (state == BackendCoreState::Stopped)
        return {false, state, "Dolphin core is stopped"};
    return {true, state, {}};
}

ArtifactCompatibilityToken
DolphinWrapperBackend::SavestateCompatibility() const
{
    return impl_->compatibility;
}

BackendExecutionCapabilityMask
DolphinWrapperBackend::Capabilities() const noexcept
{
    return BackendExecutionCapability::Pause |
        BackendExecutionCapability::Resume |
        BackendExecutionCapability::FrameStep |
        BackendExecutionCapability::ViObservation |
        BackendExecutionCapability::ThrottleControl;
}

BackendExecutionSnapshot
DolphinWrapperBackend::QueryExecutionSnapshot() const
{
    BackendExecutionSnapshot snapshot;
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
    {
        snapshot.result = std::move(open);
        return snapshot;
    }
    std::string pause_failure;
    if (impl_->pause_synchronizer.Failure(&pause_failure))
    {
        snapshot.result = BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            pause_failure.empty()
                ? "Dolphin pause synchronizer failed"
                : std::move(pause_failure),
            BackendIntegrity::Unknown);
        return snapshot;
    }
    snapshot.result = BackendResult::Success();
    snapshot.core_state = QueryCoreState();
    snapshot.vi_count = impl_->wrapper->getViFieldCountApprox();
    snapshot.pause_confirmed =
        snapshot.core_state == BackendCoreState::Paused &&
        impl_->PauseConfirmed();
    if (snapshot.pause_confirmed)
    {
        impl_->last_confirmed_pc = impl_->wrapper->getPC();
    }
    snapshot.pc = impl_->last_confirmed_pc;
    snapshot.throttle_disabled =
        Core::GetIsThrottlerTempDisabled();
    return snapshot;
}

BackendResult DolphinWrapperBackend::RequestPause()
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    if (QueryCoreState() == BackendCoreState::Paused &&
        impl_->PauseConfirmed())
    {
        return BackendResult::Success();
    }

    const auto confirmation = impl_->pause_confirmation;
    const std::uint64_t generation =
        confirmation->requested.fetch_add(
            1,
            std::memory_order_acq_rel) +
        1;
    if (!impl_->pause_synchronizer.Request(generation))
    {
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "Dolphin pause synchronizer is unavailable",
            BackendIntegrity::Unknown);
    }
    return BackendResult::Success();
}

BackendResult DolphinWrapperBackend::Resume()
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    impl_->InvalidatePauseConfirmation();
    Core::SetState(*impl_->wrapper->system(), Core::State::Running);
    return Core::IsRunning(*impl_->wrapper->system())
        ? BackendResult::Success()
        : BackendResult::Failure(
              BackendErrorCode::OperationFailed,
              "Dolphin failed to resume emulation");
}

BackendResult DolphinWrapperBackend::BeginFrameStep()
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    if (QueryCoreState() != BackendCoreState::Paused ||
        !impl_->PauseConfirmed())
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidState,
            "Dolphin must be authoritatively paused before beginning a frame step");
    }
    impl_->InvalidatePauseConfirmation();
    Core::DoFrameStep(*impl_->wrapper->system());
    return BackendResult::Success();
}

BackendResult DolphinWrapperBackend::SetThrottleDisabled(bool disabled)
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    Core::SetIsThrottlerTempDisabled(disabled);
    return BackendResult::Success();
}

BackendResult DolphinWrapperBackend::RestoreStateFile(const std::filesystem::path& path)
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    if (path.empty() || !IsRegularFile(path))
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidArgument,
            "A readable savestate path is required");
    }
    if (impl_->wrapper->loadSavestate(path.string()))
    {
        if (QueryCoreState() == BackendCoreState::Paused)
            impl_->ConfirmPause();
        else
            impl_->InvalidatePauseConfirmation();
        return BackendResult::Success();
    }
    return BackendResult::Failure(
        BackendErrorCode::OperationFailed,
        "Dolphin failed to restore the savestate",
        BackendIntegrity::Unknown);
}

BackendResult DolphinWrapperBackend::SaveStateFile(const std::filesystem::path& path)
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    if (path.empty())
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidArgument,
            "A savestate output path is required");
    }
    if (impl_->wrapper->saveSavestateBlocking(path.string()))
        return BackendResult::Success();
    return BackendResult::Failure(
        BackendErrorCode::OperationFailed,
        "Dolphin failed to save the savestate");
}

BackendBufferResult DolphinWrapperBackend::SaveStateFileBytes()
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return {std::move(open), {}};
    if (impl_->last_open_options.runtime_root.empty() ||
        impl_->savestate_file_capture_sequence == 0)
    {
        return {
            BackendResult::Failure(
                BackendErrorCode::InvalidState,
                "Native savestate-file capture requires a private runtime root and available sequence"),
            {}};
    }

    std::error_code error;
    const std::filesystem::path directory =
        impl_->last_open_options.runtime_root / "savestate-capture";
    std::filesystem::create_directories(directory, error);
    if (error)
    {
        return {
            BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                "Native savestate capture directory could not be created: " +
                    error.message()),
            {}};
    }

    const std::uint64_t sequence =
        impl_->savestate_file_capture_sequence++;
    const std::filesystem::path staging = directory /
        ("capture-" + std::to_string(sequence) + ".sav");
    const std::filesystem::path staging_movie =
        std::filesystem::path(staging.string() + ".dtm");
    std::filesystem::remove(staging, error);
    error.clear();
    std::filesystem::remove(staging_movie, error);
    if (error)
    {
        return {
            BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                "Native savestate capture staging sidecar could not be cleared: " +
                    error.message()),
            {}};
    }

    if (!impl_->wrapper->saveSavestateBlocking(staging.string()))
    {
        std::filesystem::remove(staging, error);
        error.clear();
        std::filesystem::remove(staging_movie, error);
        return {
            BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                "Dolphin failed to serialize the native savestate file"),
            {}};
    }

    std::vector<std::uint8_t> bytes;
    const bool read = ReadBinaryFile(staging, bytes);
    std::filesystem::remove(staging, error);
    const std::error_code state_cleanup_error = error;
    error.clear();
    std::filesystem::remove(staging_movie, error);
    const std::error_code movie_cleanup_error = error;
    if (!read || bytes.empty())
    {
        return {
            BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                "Completed native savestate file could not be read back"),
            {}};
    }
    if (state_cleanup_error || movie_cleanup_error)
    {
        return {
            BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                "Native savestate capture staging files could not be removed: " +
                    (state_cleanup_error
                        ? state_cleanup_error.message()
                        : movie_cleanup_error.message())),
            {}};
    }
    return {BackendResult::Success(), std::move(bytes)};
}

BackendBufferResult DolphinWrapperBackend::SaveStateBuffer()
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return {std::move(open), {}};

    // State::SaveToBuffer normally queues its work back to Dolphin's CPU
    // thread.  A workset baseline is captured while the core is already
    // authoritatively paused, and a concurrent pause transition can leave
    // that queued callback without another CPU-thread wake.  Exclude the CPU
    // here so SaveToBuffer recognizes this caller as the temporary CPU thread
    // and serializes the state inline.
    Core::CPUThreadGuard guard(*impl_->wrapper->system());
    Common::UniqueBuffer<u8> buffer;
    if (!impl_->wrapper->saveStateToBuffer(buffer))
    {
        return {
            BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                "Dolphin failed to save state to a buffer"),
            {}};
    }

    std::vector<std::uint8_t> bytes(buffer.size());
    if (!bytes.empty())
        std::memcpy(bytes.data(), buffer.data(), bytes.size());
    return {BackendResult::Success(), std::move(bytes)};
}

BackendResult DolphinWrapperBackend::RestoreStateBuffer(
    const std::vector<std::uint8_t>& bytes)
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    if (bytes.empty())
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidArgument,
            "A non-empty state buffer is required");
    }

    Common::UniqueBuffer<u8> buffer(bytes.size());
    std::memcpy(buffer.data(), bytes.data(), bytes.size());
    // Match buffer capture's CPU exclusion.  State::LoadFromBuffer otherwise
    // uses the same paused-core CPU-thread job handoff.
    Core::CPUThreadGuard guard(*impl_->wrapper->system());
    if (impl_->wrapper->loadStateFromBuffer(buffer))
    {
        if (QueryCoreState() == BackendCoreState::Paused)
            impl_->ConfirmPause();
        else
            impl_->InvalidatePauseConfirmation();
        return BackendResult::Success();
    }
    return BackendResult::Failure(
        BackendErrorCode::OperationFailed,
        "Dolphin failed to restore state from a buffer",
        BackendIntegrity::Unknown);
}

BackendResult DolphinWrapperBackend::CaptureScreenshot(
    const std::filesystem::path& path,
    std::chrono::milliseconds timeout)
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    if (path.empty())
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidArgument,
            "A screenshot output path is required");
    }
    if (impl_->wrapper->saveScreenshotBlocking(
            path.string(),
            ClampUnsignedTimeout(timeout)))
    {
        return BackendResult::Success();
    }
    return BackendResult::Failure(
        BackendErrorCode::Timeout,
        "Dolphin failed to capture a screenshot before the deadline");
}

IPhysicalStopPointBackendPort* DolphinWrapperBackend::PhysicalStopPoints() noexcept
{
    return this;
}

IExecutionBackendPort* DolphinWrapperBackend::Execution() noexcept
{
    return this;
}

IInputBackendPort* DolphinWrapperBackend::Input() noexcept
{
    return this;
}

IGuestMemoryBackendPort* DolphinWrapperBackend::GuestMemory() noexcept
{
    return this;
}

IHitTimeGuestMemoryBackendPort*
DolphinWrapperBackend::HitTimeGuestMemory() noexcept
{
    return this;
}

IScreenshotBackendPort* DolphinWrapperBackend::Screenshots() noexcept
{
    return this;
}

IMovieBackendPort* DolphinWrapperBackend::Movies() noexcept
{
    return this;
}

ICaptureBackendPort* DolphinWrapperBackend::Captures() noexcept
{
    return this;
}

MoviePlaybackPrepareResult
DolphinWrapperBackend::PrepareReadOnlyPlaybackForRestart(
    const std::filesystem::path& dtm_path)
{
    if (dtm_path.empty() || !IsRegularFile(dtm_path))
    {
        return {
            MovieFailure("A readable DTM path is required"),
            std::nullopt};
    }
    if (impl_->prepared_movie_path.has_value())
    {
        return {
            MovieFailure("Another movie is already prepared"),
            std::nullopt};
    }

    savor::tas::DtmFile dtm;
    if (!dtm.load(dtm_path.string()) ||
        dtm.validate().has_error())
    {
        return {
            MovieFailure("The DTM could not be validated"),
            std::nullopt};
    }
    const bool starts_from_state =
        dtm.info().starts_from_savestate;
    std::optional<std::filesystem::path> state;
    if (starts_from_state)
    {
        state = std::filesystem::path(dtm_path.string() + ".sav");
        if (!IsRegularFile(*state))
        {
            return {
                MovieFailure(
                    "The DTM requires a readable <dtm>.sav companion"),
                std::nullopt};
        }
    }
    impl_->prepared_movie_path = dtm_path;
    impl_->prepared_movie_savestate = state;
    impl_->prepared_movie_sha256 = dtm.compute_sha256();
    impl_->prepared_movie_core_started = false;
    return {MovieBackendResult::Success(), std::move(state)};
}

MovieBackendResult DolphinWrapperBackend::StopMovie() noexcept
{
    try
    {
        impl_->prepared_movie_path.reset();
        impl_->prepared_movie_savestate.reset();
        impl_->prepared_movie_sha256.reset();
        impl_->prepared_movie_core_started = false;
        impl_->active_movie_sha256.reset();
        if (!impl_->wrapper)
            return MovieBackendResult::Success();
        auto& movie = impl_->wrapper->system()->GetMovie();
        if (movie.IsMovieActive())
            movie.EndPlayInput(false);
        movie.SetReadOnly(true);
        return MovieBackendResult::Success();
    }
    catch (const std::exception& ex)
    {
        return MovieFailure(ex.what(), GuestIntegrity::Unknown);
    }
    catch (...)
    {
        return MovieFailure(
            "Dolphin movie shutdown threw",
            GuestIntegrity::Unknown);
    }
}

MovieBackendResult DolphinWrapperBackend::BeginRecording()
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return MovieFailure(std::move(open.message));
    auto& movie = impl_->wrapper->system()->GetMovie();
    if (movie.IsMovieActive())
        return MovieFailure("A Dolphin movie is already active");
    Movie::ControllerTypeArray controllers{};
    controllers[0] = Movie::ControllerType::GC;
    Movie::WiimoteEnabledArray wiimotes{};
    movie.SetReadOnly(false);
    if (!movie.BeginRecordingInput(controllers, wiimotes))
    {
        movie.SetReadOnly(true);
        return MovieFailure("Dolphin rejected movie recording");
    }
    impl_->active_movie_sha256.reset();
    return MovieBackendResult::Success();
}

MovieRecordingFinalizeResult
DolphinWrapperBackend::FinalizeRecording(
    const std::filesystem::path& dtm_path)
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return {MovieFailure(std::move(open.message)), std::nullopt};
    auto& movie = impl_->wrapper->system()->GetMovie();
    if (!movie.IsRecordingInput())
    {
        return {
            MovieFailure("No Dolphin recording is active"),
            std::nullopt};
    }
    try
    {
        movie.SaveRecording(dtm_path.string());
        if (!IsRegularFile(dtm_path))
        {
            return {
                MovieFailure("Dolphin did not publish the recording"),
                std::nullopt};
        }
        const std::filesystem::path state(
            dtm_path.string() + ".sav");
        std::optional<std::filesystem::path> starting_state;
        if (IsRegularFile(state))
            starting_state = state;
        movie.EndPlayInput(false);
        movie.SetReadOnly(true);
        impl_->active_movie_sha256.reset();
        return {
            MovieBackendResult::Success(),
            std::move(starting_state)};
    }
    catch (const std::exception& ex)
    {
        return {
            MovieFailure(ex.what(), GuestIntegrity::Unknown),
            std::nullopt};
    }
    catch (...)
    {
        return {
            MovieFailure(
                "Dolphin recording finalization threw",
                GuestIntegrity::Unknown),
            std::nullopt};
    }
}

MovieBackendResult DolphinWrapperBackend::CancelRecording() noexcept
{
    return StopMovie();
}

MovieBackendObservation
DolphinWrapperBackend::ObserveMovieWhilePaused() const
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
    {
        return {
            .result = MovieFailure(
                open.message.empty()
                    ? "Dolphin movie observation requires an open backend"
                    : std::move(open.message),
                open.integrity == BackendIntegrity::Unknown
                    ? GuestIntegrity::Unknown
                    : GuestIntegrity::Preserved),
        };
    }
    Core::System* const system = impl_->wrapper->system();
    if (!system)
    {
        return {
            .result = MovieFailure(
                "Dolphin movie observation requires a live core",
                GuestIntegrity::Unknown),
        };
    }
    const BackendExecutionSnapshot execution = QueryExecutionSnapshot();
    if (!execution.result.ok ||
        execution.core_state != BackendCoreState::Paused ||
        !execution.pause_confirmed)
    {
        return {
            .result = MovieFailure(
                execution.result.message.empty()
                    ? "Dolphin movie inspection requires an authoritatively paused core"
                    : execution.result.message,
                execution.result.integrity == BackendIntegrity::Unknown
                    ? GuestIntegrity::Unknown
                    : GuestIntegrity::Preserved),
        };
    }

    try
    {
        // Pause confirmation proves the CPU owner is quiescent. Reading these
        // ordinary MovieManager fields is therefore coherent without taking
        // CPUThreadGuard or otherwise changing guest execution.
        const auto& movie = system->GetMovie();
        return {
            .result = MovieBackendResult::Success(),
            .playing = movie.IsPlayingInput(),
            .recording = movie.IsRecordingInput(),
            .read_only = movie.IsReadOnly(),
            .current_frame = movie.GetCurrentFrame(),
            .current_input_count = movie.GetCurrentInputCount(),
        };
    }
    catch (const std::exception& ex)
    {
        return {
            .result = MovieFailure(
                std::string("Dolphin movie observation threw: ") + ex.what(),
                GuestIntegrity::Unknown),
        };
    }
    catch (...)
    {
        return {
            .result = MovieFailure(
                "Dolphin movie observation threw",
                GuestIntegrity::Unknown),
        };
    }
}

MovieBackendResult DolphinWrapperBackend::AcquirePauseAtPlaybackEnd()
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return MovieFailure(std::move(open.message));
    if (impl_->pause_at_playback_end_owned)
        return MovieFailure("Pause-at-playback-end override is already owned");
    try
    {
        const auto layer = Config::GetLayer(Config::LayerType::CurrentRun);
        const auto& setting = Config::MAIN_MOVIE_PAUSE_MOVIE;
        impl_->pause_at_playback_end_had_current_run_value =
            layer->Exists(setting.GetLocation());
        if (impl_->pause_at_playback_end_had_current_run_value)
        {
            impl_->pause_at_playback_end_previous_value =
                layer->Get(setting);
        }
        // Mark ownership before mutating CurrentRun so every failure path can
        // restore the exact prior layer entry through the normal release
        // operation.
        impl_->pause_at_playback_end_owned = true;
        Config::SetCurrent(setting, true);
        if (!Config::Get(setting))
        {
            const MovieBackendResult restored =
                ReleasePauseAtPlaybackEnd();
            std::string message =
                "Dolphin did not enable pause at playback end";
            if (!restored.ok)
            {
                message += "; prior CurrentRun configuration could not be restored";
                if (!restored.message.empty())
                    message += ": " + restored.message;
            }
            return MovieFailure(
                std::move(message),
                GuestIntegrity::Unknown);
        }
        return MovieBackendResult::Success();
    }
    catch (const std::exception& ex)
    {
        (void)ReleasePauseAtPlaybackEnd();
        return MovieFailure(
            std::string("Dolphin pause-at-playback-end setup threw: ") +
                ex.what(),
            GuestIntegrity::Unknown);
    }
    catch (...)
    {
        (void)ReleasePauseAtPlaybackEnd();
        return MovieFailure(
            "Dolphin pause-at-playback-end setup threw",
            GuestIntegrity::Unknown);
    }
}

MovieBackendResult
DolphinWrapperBackend::ReleasePauseAtPlaybackEnd() noexcept
{
    if (!impl_->pause_at_playback_end_owned)
        return MovieBackendResult::Success();
    try
    {
        const auto& setting = Config::MAIN_MOVIE_PAUSE_MOVIE;
        if (impl_->pause_at_playback_end_had_current_run_value)
        {
            Config::SetCurrent(
                setting,
                impl_->pause_at_playback_end_previous_value);
        }
        else
        {
            Config::DeleteKey(
                Config::LayerType::CurrentRun,
                setting);
        }
        impl_->pause_at_playback_end_owned = false;
        impl_->pause_at_playback_end_had_current_run_value = false;
        impl_->pause_at_playback_end_previous_value = false;
        return MovieBackendResult::Success();
    }
    catch (const std::exception& ex)
    {
        return MovieFailure(
            std::string("Dolphin pause-at-playback-end restoration threw: ") +
                ex.what(),
            GuestIntegrity::Unknown);
    }
    catch (...)
    {
        return MovieFailure(
            "Dolphin pause-at-playback-end restoration threw",
            GuestIntegrity::Unknown);
    }
}

MovieCheckpointBackendResult
DolphinWrapperBackend::CaptureRecordingCheckpoint()
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return {MovieFailure(std::move(open.message)), {}};
    auto& movie = impl_->wrapper->system()->GetMovie();
    if (!movie.IsRecordingInput())
    {
        return {
            MovieFailure("No Dolphin recording is active"),
            {}};
    }

    try
    {
        const auto directory =
            impl_->last_open_options.runtime_root /
            "movie-checkpoints";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error)
        {
            return {
                MovieFailure(
                    "Movie checkpoint directory could not be created"),
                {}};
        }
        const auto path = directory /
            ("recording-" +
             std::to_string(impl_->movie_checkpoint_sequence++) +
             ".dtm");
        movie.SaveRecording(path.string());

        savor::tas::DtmFile dtm;
        if (!dtm.load(path.string()) ||
            dtm.validate().has_error())
        {
            std::filesystem::remove(path, error);
            return {
                MovieFailure(
                    "Dolphin recording checkpoint could not be validated"),
                {}};
        }
        MovieCheckpointMetadata checkpoint;
        checkpoint.mode = MovieCheckpointMode::Recording;
        checkpoint.dtm_sha256 = dtm.compute_sha256();
        const auto info = dtm.info();
        checkpoint.game_id.assign(
            info.game_id.data(),
            info.game_id.size());
        checkpoint.dtm_bytes = dtm.bytes();
        checkpoint.current_frame = movie.GetCurrentFrame();
        checkpoint.current_input_count =
            movie.GetCurrentInputCount();
        checkpoint.starts_from_savestate =
            info.starts_from_savestate;
        std::filesystem::remove(path, error);
        std::filesystem::remove(
            std::filesystem::path(path.string() + ".sav"),
            error);
        return {
            MovieBackendResult::Success(),
            std::move(checkpoint)};
    }
    catch (const std::exception& ex)
    {
        return {
            MovieFailure(ex.what(), GuestIntegrity::Unknown),
            {}};
    }
    catch (...)
    {
        return {
            MovieFailure(
                "Dolphin recording checkpoint capture threw",
                GuestIntegrity::Unknown),
            {}};
    }
}

MovieBackendResult DolphinWrapperBackend::PrepareSavestateRestore(
    const SavestateMovieRestoreContext& context)
{
    if (impl_->prepared_movie_replacement.has_value())
        return MovieFailure("A movie state replacement is already prepared");
    impl_->prepared_movie_replacement = context;
    impl_->prepared_movie_started_for_replacement = false;
    impl_->prepared_movie_replacement_sha256.reset();

    if (!context.movie.has_value() ||
        !impl_->wrapper)
    {
        return MovieBackendResult::Success();
    }

    const MovieBackendObservation current = ObserveMovieWhilePaused();
    if (!current.result.ok)
    {
        impl_->prepared_movie_replacement.reset();
        return current.result;
    }
    if (context.movie->mode ==
        MovieCheckpointMode::ReadOnlyPlayback)
    {
        std::filesystem::path exact_dtm;
        MovieBackendResult materialized =
            MaterializeExactMovieHistory(
                *context.movie,
                impl_->last_open_options.runtime_root,
                exact_dtm);
        if (!materialized.ok)
        {
            impl_->prepared_movie_replacement.reset();
            return materialized;
        }
        if (std::ranges::find(
                impl_->owned_movie_restore_paths,
                exact_dtm) ==
            impl_->owned_movie_restore_paths.end())
        {
            impl_->owned_movie_restore_paths.push_back(exact_dtm);
        }

        if (current.playing && !current.recording && current.read_only)
        {
            if (!impl_->active_movie_sha256.has_value() ||
                *impl_->active_movie_sha256 !=
                    context.movie->dtm_sha256)
            {
                impl_->prepared_movie_replacement.reset();
                return MovieFailure(
                    "Active Dolphin movie identity does not match the state checkpoint");
            }
            impl_->prepared_movie_replacement_sha256 =
                context.movie->dtm_sha256;
            return MovieBackendResult::Success();
        }
        if (current.playing || current.recording)
        {
            impl_->prepared_movie_replacement.reset();
            return MovieFailure(
                "Read-only movie state cannot replace a different active movie mode");
        }
        std::optional<std::string> ignored;
        auto& movie = impl_->wrapper->system()->GetMovie();
        movie.SetReadOnly(true);
        if (!movie.PlayInput(
                exact_dtm.string(),
                &ignored))
        {
            impl_->prepared_movie_replacement.reset();
            return MovieFailure(
                "Dolphin could not stage movie history for restore");
        }
        impl_->prepared_movie_started_for_replacement = true;
        impl_->prepared_movie_replacement_sha256 =
            context.movie->dtm_sha256;
    }
    else if (
        context.movie->mode == MovieCheckpointMode::Recording &&
        (!current.recording || current.playing || current.read_only))
    {
        impl_->prepared_movie_replacement.reset();
        return MovieFailure(
            "Recording checkpoint restore requires the active same-session recording");
    }
    return MovieBackendResult::Success();
}

MovieBackendResult DolphinWrapperBackend::CommitSavestateRestore(
    const SavestateMovieRestoreContext& context)
{
    if (!impl_->prepared_movie_replacement.has_value())
        return MovieFailure("Movie replacement was not prepared");
    if (!context.movie.has_value())
    {
        impl_->active_movie_sha256.reset();
        impl_->prepared_movie_replacement.reset();
        impl_->prepared_movie_replacement_sha256.reset();
        impl_->prepared_movie_started_for_replacement = false;
        return MovieBackendResult::Success();
    }
    const bool expect_recording =
        context.movie->mode == MovieCheckpointMode::Recording;
    if ((!expect_recording &&
         (!impl_->prepared_movie_replacement_sha256.has_value() ||
          *impl_->prepared_movie_replacement_sha256 !=
              context.movie->dtm_sha256)))
    {
        return MovieFailure(
            "Dolphin movie identity did not reconcile with the restored state",
            GuestIntegrity::Unknown);
    }
    // MovieService performs the one authoritative paused reconciliation after
    // this commit. Keeping physical state and cursor classification there
    // avoids a competing second inspection in the backend commit path.
    if (!expect_recording)
    {
        impl_->active_movie_sha256 =
            context.movie->dtm_sha256;
    }
    else
    {
        impl_->active_movie_sha256.reset();
    }
    impl_->prepared_movie_replacement.reset();
    impl_->prepared_movie_replacement_sha256.reset();
    impl_->prepared_movie_started_for_replacement = false;
    return MovieBackendResult::Success();
}

MovieBackendResult DolphinWrapperBackend::RollbackSavestateRestore(
    const SavestateMovieRestoreContext&) noexcept
{
    try
    {
        if (impl_->prepared_movie_started_for_replacement &&
            impl_->wrapper)
        {
            auto& movie = impl_->wrapper->system()->GetMovie();
            if (movie.IsMovieActive())
                movie.EndPlayInput(false);
            movie.SetReadOnly(true);
            impl_->active_movie_sha256.reset();
        }
        impl_->prepared_movie_replacement.reset();
        impl_->prepared_movie_replacement_sha256.reset();
        impl_->prepared_movie_started_for_replacement = false;
        return MovieBackendResult::Success();
    }
    catch (...)
    {
        return MovieFailure(
            "Movie replacement rollback was not proven",
            GuestIntegrity::Unknown);
    }
}

std::unique_ptr<ICaptureProfileAdapter>
DolphinWrapperBackend::CreateCaptureProfileAdapter(
    const ProbeRouterAdapterConfig& config,
    std::string* error_out)
{
    if (!impl_->open || !impl_->wrapper)
    {
        if (error_out)
            *error_out = "Dolphin backend is not open";
        return {};
    }
    try
    {
        return std::make_unique<ProbeCaptureProfileAdapter>(
            *impl_->wrapper->system(),
            config);
    }
    catch (const std::exception& ex)
    {
        if (error_out)
            *error_out = ex.what();
        return {};
    }
}

bool DolphinWrapperBackend::IsAvailable(std::uint8_t port) const noexcept
{
    return port == 0 && impl_->open && impl_->wrapper &&
        impl_->wrapper->isInputReady();
}

BackendInputPublication DolphinWrapperBackend::Publish(
    std::uint8_t port,
    const savor::GCInputFrame& frame)
{
    if (!IsAvailable(port))
    {
        return {
            BackendResult::Failure(
                BackendErrorCode::Unavailable,
                "standard controller port is unavailable"),
            0};
    }
    const std::uint64_t publication_epoch =
        impl_->wrapper->publishInputEpoch(frame);
    if (publication_epoch == 0)
    {
        return {
            BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                "Dolphin did not accept the input publication"),
            0};
    }
    return {BackendResult::Success(), publication_epoch};
}

BackendInputPoll DolphinWrapperBackend::QueryPoll(
    std::uint8_t port) const
{
    if (!IsAvailable(port))
    {
        return {
            BackendResult::Failure(
                BackendErrorCode::Unavailable,
                "standard controller port is unavailable")};
    }
    const auto receipt = impl_->wrapper->getInputPollReceipt();
    return {
        .result = BackendResult::Success(),
        .publication_epoch = receipt.epoch,
        .callback_count = receipt.callback_count,
        .a_control_callback_count = receipt.a_control_callback_count,
        .frame = receipt.frame};
}

bool DolphinWrapperBackend::IsPaused() const noexcept
{
    return QueryCoreState() == BackendCoreState::Paused;
}

GuestBytesResult DolphinWrapperBackend::Read(
    std::uint32_t address,
    std::size_t size) const
{
    if (!IsPaused())
    {
        return {
            BackendResult::Failure(
                BackendErrorCode::InvalidState,
                "guest-memory access requires a paused core"),
            {}};
    }
    if (size == 0 || size > 1024 * 1024)
    {
        return {
            BackendResult::Failure(
                BackendErrorCode::InvalidArgument,
                "guest-memory read size is invalid"),
            {}};
    }
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t index = 0; index < size; ++index)
    {
        if (!impl_->wrapper->readU8(
                address + static_cast<std::uint32_t>(index),
                bytes[index]))
        {
            return {
                BackendResult::Failure(
                    BackendErrorCode::OperationFailed,
                    "Dolphin guest-memory read failed"),
                {}};
        }
    }
    return {BackendResult::Success(), std::move(bytes)};
}

MovieBackendResult
DolphinWrapperBackend::BranchReadOnlyPlaybackToRecording()
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return MovieFailure(std::move(open.message));
    auto& movie = impl_->wrapper->system()->GetMovie();
    if (!movie.IsPlayingInput() || !movie.IsReadOnly())
        return MovieFailure(
            "A read-only Dolphin playback session is required");
    try
    {
        movie.SetReadOnly(false);
        movie.EndPlayInput(true);
        if (!movie.IsRecordingInput() || movie.IsReadOnly())
            return MovieFailure(
                "Dolphin did not enter writable rerecording mode",
                GuestIntegrity::Unknown);
        impl_->active_movie_sha256.reset();
        return MovieBackendResult::Success();
    }
    catch (const std::exception& ex)
    {
        return MovieFailure(ex.what(), GuestIntegrity::Unknown);
    }
    catch (...)
    {
        return MovieFailure(
            "Dolphin playback-to-recording branch threw",
            GuestIntegrity::Unknown);
    }
}

HitTimeGuestReadReceipt DolphinWrapperBackend::ReadHitTimeBytes(
    std::uint32_t address,
    std::span<std::uint8_t> destination) const noexcept
{
    HitTimeGuestReadReceipt receipt{
        false,
        HitTimeGuestReadError::BackendUnavailable,
        address,
        destination.size()};
    if (destination.empty() ||
        destination.size() > std::numeric_limits<std::uint32_t>::max() ||
        address > std::numeric_limits<std::uint32_t>::max() -
            static_cast<std::uint32_t>(destination.size() - 1))
    {
        receipt.error = HitTimeGuestReadError::InvalidArgument;
        return receipt;
    }
    Core::System* system =
        impl_ && impl_->wrapper ? impl_->wrapper->system() : nullptr;
    if (!system)
        return receipt;
    auto& memory = system->GetMemory();
    const auto* source =
        memory.GetPointerForRange(address, destination.size());
    if (!source)
    {
        receipt.error = HitTimeGuestReadError::UnmappedRange;
        return receipt;
    }
    std::memcpy(destination.data(), source, destination.size());
    receipt.ok = true;
    receipt.error = HitTimeGuestReadError::None;
    return receipt;
}

BackendResult DolphinWrapperBackend::Write(
    std::uint32_t address,
    const std::vector<std::uint8_t>& bytes)
{
    if (!IsPaused())
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidState,
            "guest-memory mutation requires a paused core");
    }
    if (bytes.empty() || bytes.size() > 1024 * 1024)
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidArgument,
            "guest-memory write size is invalid");
    }
    Core::System* system = impl_->wrapper->system();
    if (!system)
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidState,
            "Dolphin system is unavailable");
    }
    auto& memory = system->GetMemory();
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        memory.Write_U8(
            bytes[index],
            address + static_cast<std::uint32_t>(index));
    }
    return BackendResult::Success();
}

BackendResult DolphinWrapperBackend::InvalidateExecutableRange(
    std::uint32_t address,
    std::size_t size)
{
    if (!IsPaused())
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidState,
            "executable invalidation requires a paused core");
    }
    if (size == 0)
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidArgument,
            "executable invalidation range is empty");
    }
    Core::System* system = impl_->wrapper->system();
    if (!system)
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidState,
            "Dolphin system is unavailable");
    }
    auto& power_pc = system->GetPowerPC();
    for (std::size_t offset = 0; offset < size; offset += 4)
    {
        power_pc.ScheduleInvalidateCacheThreadSafe(
            address + static_cast<std::uint32_t>(offset));
    }
    return BackendResult::Success();
}

BackendResult DolphinWrapperBackend::Capture(
    const std::filesystem::path& path,
    std::chrono::milliseconds timeout)
{
    return CaptureScreenshot(path, timeout);
}

PhysicalStopBackendReceipt DolphinWrapperBackend::BindNativeStopSink(
    savor::probe::INativeStopSink& sink)
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
    {
        return {
            false,
            PhysicalStopIntegrity::Preserved,
            impl_->physical_generation,
            impl_->owned_physical_plan,
            std::move(open.message)};
    }
    if (impl_->native_sink == &sink)
    {
        return {
            true,
            PhysicalStopIntegrity::Preserved,
            impl_->physical_generation,
            impl_->owned_physical_plan,
            {}};
    }
    if (impl_->native_sink)
    {
        return {
            false,
            PhysicalStopIntegrity::Preserved,
            impl_->physical_generation,
            impl_->owned_physical_plan,
            "another native stop sink is already bound"};
    }

    std::string error;
    if (!savor::probe::BindNativeStopSink(sink, &error))
    {
        return {
            false,
            PhysicalStopIntegrity::Preserved,
            impl_->physical_generation,
            impl_->owned_physical_plan,
            error.empty() ? "failed binding native Dolphin stop sink" : std::move(error)};
    }
    if (!savor::probe::InstallNativeStopHooks(&error))
    {
        (void)savor::probe::UnbindNativeStopSink(sink);
        return {
            false,
            PhysicalStopIntegrity::Preserved,
            impl_->physical_generation,
            impl_->owned_physical_plan,
            error.empty() ? "failed installing native Dolphin stop hooks" : std::move(error)};
    }
    impl_->native_sink = &sink;
    return {
        true,
        PhysicalStopIntegrity::Preserved,
        impl_->physical_generation,
        impl_->owned_physical_plan,
        {}};
}

PhysicalStopBackendReceipt DolphinWrapperBackend::UnbindNativeStopSink(
    savor::probe::INativeStopSink& sink)
{
    if (!impl_->native_sink)
    {
        return {
            true,
            PhysicalStopIntegrity::Preserved,
            impl_->physical_generation,
            impl_->owned_physical_plan,
            {}};
    }
    if (impl_->native_sink != &sink)
    {
        return {
            false,
            PhysicalStopIntegrity::Preserved,
            impl_->physical_generation,
            impl_->owned_physical_plan,
            "native stop sink ownership mismatch"};
    }
    if (!savor::probe::UnbindNativeStopSink(sink))
    {
        return {
            false,
            PhysicalStopIntegrity::Unknown,
            impl_->physical_generation,
            impl_->owned_physical_plan,
            "native stop sink could not be detached"};
    }
    impl_->native_sink = nullptr;
    savor::probe::UninstallNativeStopHooks();
    return {
        true,
        PhysicalStopIntegrity::Preserved,
        impl_->physical_generation,
        impl_->owned_physical_plan,
        {}};
}

PhysicalStopBackendReceipt DolphinWrapperBackend::QueryPhysicalStopPoints() const
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
    {
        return {
            false,
            PhysicalStopIntegrity::Preserved,
            impl_->physical_generation,
            {},
            std::move(open.message)};
    }
    Core::CPUThreadGuard guard(*impl_->wrapper->system());
    auto actual = ReadPhysicalPlan(*impl_->wrapper->system());
    std::string error;
    const bool valid = PhysicalObjectsHaveManagerShape(
        *impl_->wrapper->system(),
        impl_->owned_physical_plan,
        &error);
    return {
        valid,
        valid ? PhysicalStopIntegrity::Preserved : PhysicalStopIntegrity::Unknown,
        impl_->physical_generation,
        std::move(actual),
        std::move(error)};
}

PhysicalStopBackendReceipt DolphinWrapperBackend::ApplyExactPhysicalStopPlan(
    const PhysicalStopPointPlan& plan,
    PhysicalPlanGeneration generation,
    const std::function<void()>& commit_while_cpu_excluded)
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
    {
        return {
            false,
            PhysicalStopIntegrity::Preserved,
            impl_->physical_generation,
            {},
            std::move(open.message)};
    }

    auto& system = *impl_->wrapper->system();
    Core::CPUThreadGuard guard(system);
    std::string error;
    if (!PhysicalObjectsHaveManagerShape(
            system,
            impl_->owned_physical_plan,
            &error))
    {
        return {
            false,
            PhysicalStopIntegrity::Unknown,
            impl_->physical_generation,
            ReadPhysicalPlan(system),
            std::move(error)};
    }

    const PhysicalStopPointPlan previous = impl_->owned_physical_plan;
    try
    {
        ReplacePhysicalPlan(
            system,
            previous,
            plan);
        if (!PhysicalObjectsHaveManagerShape(
                system,
                plan,
                &error))
            throw std::runtime_error(error.empty()
                ? "Dolphin rejected the exact physical stop plan"
                : error);
        commit_while_cpu_excluded();
        impl_->owned_physical_plan = plan;
        impl_->physical_generation = generation;
        return {
            true,
            PhysicalStopIntegrity::Preserved,
            generation,
            plan,
            {}};
    }
    catch (const std::exception& ex)
    {
        try
        {
            ReplacePhysicalPlan(
                system,
                ReadPhysicalPlan(system),
                previous);
            if (!PhysicalObjectsHaveManagerShape(
                    system,
                    previous,
                    &error))
                throw std::runtime_error(error);
        }
        catch (...)
        {
            return {
                false,
                PhysicalStopIntegrity::Unknown,
                impl_->physical_generation,
                ReadPhysicalPlan(system),
                std::string("physical stop plan failed and rollback was unproven: ") +
                    ex.what()};
        }
        return {
            false,
            PhysicalStopIntegrity::Preserved,
            impl_->physical_generation,
            previous,
            std::string("physical stop plan was rolled back: ") + ex.what()};
    }
    catch (...)
    {
        try
        {
            ReplacePhysicalPlan(
                system,
                ReadPhysicalPlan(system),
                previous);
        }
        catch (...)
        {
            return {
                false,
                PhysicalStopIntegrity::Unknown,
                impl_->physical_generation,
                ReadPhysicalPlan(system),
                "physical stop plan failed and rollback was unproven"};
        }
        return {
            false,
            PhysicalStopIntegrity::Preserved,
            impl_->physical_generation,
            previous,
            "physical stop plan was rolled back"};
    }
}

PhysicalStopBackendReceipt
DolphinWrapperBackend::PublishExactPhysicalStopPlanUnchanged(
    const PhysicalStopPointPlan& expected_plan,
    PhysicalPlanGeneration generation,
    const std::function<void()>& commit_while_cpu_excluded)
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
    {
        return {
            false,
            PhysicalStopIntegrity::Preserved,
            impl_->physical_generation,
            {},
            std::move(open.message)};
    }

    auto& system = *impl_->wrapper->system();
    Core::CPUThreadGuard guard(system);
    std::string error;
    if (generation != impl_->physical_generation ||
        expected_plan != impl_->owned_physical_plan ||
        !PhysicalObjectsHaveManagerShape(
            system,
            expected_plan,
            &error))
    {
        return {
            false,
            PhysicalStopIntegrity::Unknown,
            impl_->physical_generation,
            ReadPhysicalPlan(system),
            error.empty()
                ? "unchanged dispatch publication did not match the "
                  "manager-owned physical plan"
                : std::move(error)};
    }

    try
    {
        commit_while_cpu_excluded();
    }
    catch (const std::exception& ex)
    {
        return {
            false,
            PhysicalStopIntegrity::Preserved,
            impl_->physical_generation,
            expected_plan,
            std::string(
                "dispatch publication failed while the physical plan "
                "remained unchanged: ") +
                ex.what()};
    }
    catch (...)
    {
        return {
            false,
            PhysicalStopIntegrity::Preserved,
            impl_->physical_generation,
            expected_plan,
            "dispatch publication failed while the physical plan remained "
            "unchanged"};
    }

    return {
        true,
        PhysicalStopIntegrity::Preserved,
        impl_->physical_generation,
        expected_plan,
        {}};
}

PhysicalStopBackendReceipt DolphinWrapperBackend::RevalidatePhysicalStopPlan(
    const PhysicalStopPointPlan& plan,
    PhysicalPlanGeneration generation,
    const std::function<void()>& commit_while_cpu_excluded)
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
    {
        return {
            false,
            PhysicalStopIntegrity::Preserved,
            impl_->physical_generation,
            {},
            std::move(open.message)};
    }
    auto& system = *impl_->wrapper->system();
    Core::CPUThreadGuard guard(system);
    std::string error;
    if (plan != impl_->owned_physical_plan ||
        !PhysicalObjectsHaveManagerShape(
            system,
            plan,
            &error))
    {
        return {
            false,
            PhysicalStopIntegrity::Unknown,
            impl_->physical_generation,
            ReadPhysicalPlan(system),
            error.empty()
                ? "JIT revalidation did not match the manager-owned physical plan"
                : std::move(error)};
    }
    try
    {
        commit_while_cpu_excluded();
    }
    catch (const std::exception& ex)
    {
        return {
            false,
            PhysicalStopIntegrity::Preserved,
            impl_->physical_generation,
            plan,
            std::string("dispatch publication failed during JIT revalidation: ") + ex.what()};
    }
    catch (...)
    {
        return {
            false,
            PhysicalStopIntegrity::Preserved,
            impl_->physical_generation,
            plan,
            "dispatch publication failed during JIT revalidation"};
    }
    impl_->physical_generation = generation;
    return {
        true,
        PhysicalStopIntegrity::Preserved,
        generation,
        plan,
        {}};
}

PhysicalStopBackendReceipt DolphinWrapperBackend::ClearOwnedPhysicalStopPoints(
    PhysicalPlanGeneration generation,
    const std::function<void()>& commit_while_cpu_excluded)
{
    return ApplyExactPhysicalStopPlan({}, generation, commit_while_cpu_excluded);
}

std::unique_ptr<IDolphinBackend> MakeDolphinWrapperBackend()
{
    return std::make_unique<DolphinWrapperBackend>();
}

} // namespace savor::runtime
