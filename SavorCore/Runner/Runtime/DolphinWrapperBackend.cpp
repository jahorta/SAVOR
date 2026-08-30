#include "DolphinWrapperBackend.h"

#include "../../Boot/Boot.h"
#include "../../Core/DolphinWrapper.h"
#include "../../Tas/DtmFile.h"
#include "../../Utils/Hash.h"
#include "../../Utils/Log.h"

#include "Common/Buffer.h"
#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/Movie.h"
#include "Core/PowerPC/BreakPoints.h"
#include "VideoCommon/OnScreenDisplay.h"

#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
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

std::atomic<std::uint64_t> g_backend_transition_id{1};
std::atomic<std::uint64_t> g_movie_snapshot_id{1};

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
    // The Dolphin host actor owns at most one movable control task. A completed
    // task remains authoritative until ExecutionControlCore consumes it; no
    // later task can replace or supersede it.
    struct ControlActuator
    {
        ControlActuator() = default;
        ControlActuator(const ControlActuator&) = delete;
        ControlActuator& operator=(const ControlActuator&) = delete;

        [[nodiscard]] bool Start(
            Core::System& target_system,
            std::atomic<bool>& target_paused_quiescent,
            std::string* error)
        {
            if (system != nullptr ||
                state != BackendExecutionSnapshot::ControlTaskState::Stopping)
            {
                if (error)
                    *error = "Dolphin control actuator is already started";
                return false;
            }

            system = &target_system;
            paused_quiescent = &target_paused_quiescent;
            state = BackendExecutionSnapshot::ControlTaskState::Idle;
            task.reset();
            completion.reset();
            return true;
        }

        [[nodiscard]] BackendResult Submit(BackendControlTask submitted)
        {
            if (system == nullptr ||
                state == BackendExecutionSnapshot::ControlTaskState::Stopping)
            {
                return BackendResult::Failure(
                    BackendErrorCode::InvalidState,
                    "Dolphin control actuator is unavailable");
            }
            if (state != BackendExecutionSnapshot::ControlTaskState::Idle ||
                task || completion)
            {
                return BackendResult::Failure(
                    BackendErrorCode::InvalidState,
                    "Dolphin control actuator already owns a task");
            }

            task.emplace(std::move(submitted));
            state = BackendExecutionSnapshot::ControlTaskState::Pending;
            return BackendResult::Success();
        }

        [[nodiscard]] BackendResult Pump()
        {
            if (system == nullptr || paused_quiescent == nullptr ||
                state == BackendExecutionSnapshot::ControlTaskState::Stopping)
            {
                return BackendResult::Failure(
                    BackendErrorCode::InvalidState,
                    "Dolphin control actuator is unavailable");
            }
            if (state != BackendExecutionSnapshot::ControlTaskState::Pending)
                return BackendResult::Success();
            if (!task || completion)
            {
                return BackendResult::Failure(
                    BackendErrorCode::InvalidState,
                    "Pending Dolphin control actuator state has no unique task");
            }

            BackendControlTask owned_task = std::move(*task);
            task.reset();
            state = BackendExecutionSnapshot::ControlTaskState::Running;
            SCLOGDX(
                SC_TAGS("dolphin.control", "dolphin.actor_task.begin"),
                "kind=%u thread=%llu",
                static_cast<unsigned>(owned_task.kind),
                static_cast<unsigned long long>(
                    std::hash<std::thread::id>{}(std::this_thread::get_id())));
            completion.emplace(Execute(owned_task));
            state = BackendExecutionSnapshot::ControlTaskState::Completed;
            SCLOGDX(
                SC_TAGS("dolphin.control", "dolphin.actor_task.end"),
                "kind=%u ok=%d thread=%llu",
                static_cast<unsigned>(owned_task.kind),
                completion->result.ok ? 1 : 0,
                static_cast<unsigned long long>(
                    std::hash<std::thread::id>{}(std::this_thread::get_id())));
            return BackendResult::Success();
        }

        [[nodiscard]] std::optional<BackendControlCompletion> TakeCompletion()
        {
            if (state != BackendExecutionSnapshot::ControlTaskState::Completed ||
                !completion)
            {
                return std::nullopt;
            }

            std::optional<BackendControlCompletion> taken =
                std::move(completion);
            completion.reset();
            state = BackendExecutionSnapshot::ControlTaskState::Idle;
            return taken;
        }

        [[nodiscard]] BackendExecutionSnapshot::ControlTaskState State() const
        {
            return state;
        }

        [[nodiscard]] BackendResult Stop() noexcept
        {
            BackendResult result = BackendResult::Success();
            if (state == BackendExecutionSnapshot::ControlTaskState::Running)
            {
                result = BackendResult::Failure(
                    BackendErrorCode::InvalidState,
                    "Dolphin shutdown encountered a running actor control task");
            }
            else if (state == BackendExecutionSnapshot::ControlTaskState::Completed ||
                     completion)
            {
                result = BackendResult::Failure(
                    BackendErrorCode::InvalidState,
                    "Dolphin shutdown encountered an unconsumed control completion");
            }
            else if (state == BackendExecutionSnapshot::ControlTaskState::Pending)
            {
                SCLOGWX(
                    SC_TAGS("dolphin.control", "dolphin.actor_task.cancel"),
                    "reason=backend_shutdown");
            }
            system = nullptr;
            paused_quiescent = nullptr;
            task.reset();
            completion.reset();
            state = BackendExecutionSnapshot::ControlTaskState::Stopping;
            return result;
        }

    private:
        [[nodiscard]] BackendControlCompletion Execute(
            const BackendControlTask& owned_task) noexcept
        {
            BackendControlCompletion completed;
            completed.kind = owned_task.kind;
            try
            {
                switch (owned_task.kind)
                {
                case BackendControlTaskKind::Pause:
                    paused_quiescent->store(false, std::memory_order_release);
                    Core::SetState(*system, Core::State::Paused);
                    break;
                case BackendControlTaskKind::Resume:
                    paused_quiescent->store(false, std::memory_order_release);
                    Core::SetState(*system, Core::State::Running);
                    break;
                case BackendControlTaskKind::FrameStep:
                    paused_quiescent->store(false, std::memory_order_release);
                    Core::DoFrameStep(*system);
                    break;
                case BackendControlTaskKind::SynchronizePaused:
                    SCLOGDX(
                        SC_TAGS("dolphin.control", "dolphin.synchronize_paused.begin"),
                        "thread=%llu",
                        static_cast<unsigned long long>(
                            std::hash<std::thread::id>{}(
                                std::this_thread::get_id())));
                    if (system->GetCPU().GetState() != CPU::State::Stepping)
                    {
                        completed.result = BackendResult::Failure(
                            BackendErrorCode::InvalidState,
                            "Dolphin is not paused for quiescence synchronization");
                        return completed;
                    }
                    system->GetCPU().PauseAndLock(true, false, false);
                    system->GetCPU().PauseAndLock(false, false, false);
                    if (system->GetCPU().GetState() != CPU::State::Stepping)
                    {
                        completed.result = BackendResult::Failure(
                            BackendErrorCode::OperationFailed,
                            "Dolphin left the paused state while synchronizing quiescence");
                        return completed;
                    }
                    paused_quiescent->store(true, std::memory_order_release);
                    SCLOGDX(
                        SC_TAGS("dolphin.control", "dolphin.synchronize_paused.end"),
                        "ok=1 thread=%llu",
                        static_cast<unsigned long long>(
                            std::hash<std::thread::id>{}(
                                std::this_thread::get_id())));
                    break;
                }

                completed.result = BackendResult::Success();
                return completed;
            }
            catch (const std::exception& ex)
            {
                completed.result = BackendResult::Failure(
                    BackendErrorCode::OperationFailed,
                    ex.what());
            }
            catch (...)
            {
                completed.result = BackendResult::Failure(
                    BackendErrorCode::OperationFailed,
                    "Dolphin control actuator raised an unknown exception");
            }
            return completed;
        }

        Core::System* system = nullptr;
        std::atomic<bool>* paused_quiescent = nullptr;
        BackendExecutionSnapshot::ControlTaskState state =
            BackendExecutionSnapshot::ControlTaskState::Stopping;
        std::optional<BackendControlTask> task;
        std::optional<BackendControlCompletion> completion;
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
    std::atomic<bool> paused_quiescent{false};
    ControlActuator control_actuator;
    std::thread::id host_thread;
    bool host_thread_declared = false;
    mutable std::uint32_t last_confirmed_pc = 0;    std::optional<std::filesystem::path> prepared_movie_path;
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

    void ResetControlState(bool quiescent) noexcept
    {
        paused_quiescent.store(quiescent, std::memory_order_release);
        last_confirmed_pc = 0;
    }

    [[nodiscard]] BackendResult StartControlActuator()
    {
        if (!wrapper || !wrapper->system())
        {
            return BackendResult::Failure(
                BackendErrorCode::InvalidState,
                "Dolphin control actuator requires a live wrapper");
        }
        ResetControlState(true);
        std::string error;
        if (!control_actuator.Start(
                *wrapper->system(), paused_quiescent, &error))
        {
            ResetControlState(false);
            return BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                error.empty()
                    ? "Failed to start Dolphin control actuator"
                    : std::move(error),
                BackendIntegrity::Unknown);
        }
        return BackendResult::Success();
    }
    [[nodiscard]] BackendResult BindHostThread()
    {
        if (host_thread_declared)
        {
            return BackendResult::Failure(
                BackendErrorCode::InvalidState,
                "Dolphin host thread is already bound");
        }
        if (Core::IsHostThread())
        {
            return BackendResult::Failure(
                BackendErrorCode::InvalidState,
                "Current thread already has an unowned Dolphin host registration");
        }
        host_thread = std::this_thread::get_id();
        Core::DeclareAsHostThread();
        host_thread_declared = true;
        SCLOGDX(
            SC_TAGS("dolphin.host", "dolphin.host.bind"),
            "thread=%llu",
            static_cast<unsigned long long>(
                std::hash<std::thread::id>{}(host_thread)));
        return BackendResult::Success();
    }
    [[nodiscard]] BackendResult RequireHostThread() const
    {
        if (!host_thread_declared || !Core::IsHostThread())
        {
            return BackendResult::Failure(
                BackendErrorCode::InvalidState,
                "Dolphin backend owner is not a declared host thread");
        }
        if (host_thread != std::this_thread::get_id())
        {
            return BackendResult::Failure(
                BackendErrorCode::InvalidState,
                "Dolphin backend was accessed from a non-owner thread");
        }
        return BackendResult::Success();
    }
    [[nodiscard]] BackendResult ReleaseHostThread() noexcept
    {
        if (!host_thread_declared)
            return BackendResult::Success();
        if (host_thread != std::this_thread::get_id() || !Core::IsHostThread())
        {
            return BackendResult::Failure(
                BackendErrorCode::InvalidState,
                "Dolphin host thread cannot be released by a non-owner");
        }
        SCLOGDX(
            SC_TAGS("dolphin.host", "dolphin.host.release"),
            "thread=%llu",
            static_cast<unsigned long long>(
                std::hash<std::thread::id>{}(host_thread)));
        Core::UndeclareAsHostThread();
        host_thread = {};
        host_thread_declared = false;
        return BackendResult::Success();
    }
    [[nodiscard]] BackendResult RequireOpen() const
    {
        if (BackendResult owner = RequireHostThread(); !owner.ok)
            return owner;
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
    if (impl_ && (!impl_->host_thread_declared ||
                  impl_->host_thread == std::this_thread::get_id()))
        (void)Close();
    else if (impl_)
        SCLOGEX(
            SC_TAGS("dolphin.host", "dolphin.host.invariant"),
            "destructor refused non-owner Dolphin shutdown");
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
    if (BackendResult owner = impl_->BindHostThread(); !owner.ok)
        return owner;
    struct HostOpenRollback
    {
        Impl& impl;
        bool committed = false;
        ~HostOpenRollback()
        {
            if (!committed)
                (void)impl.ReleaseHostThread();
        }
    } host_rollback{*impl_};
    auto wrapper = std::make_unique<DolphinWrapper>();

    simboot::BootOptions boot_options;
    boot_options.user_dir = options.user_directory;
    boot_options.dolphin_qt_base = options.dolphin_base_directory;
    boot_options.session_filesystem_preparation_id =
        options.session_filesystem_preparation_id;
    boot_options.process_generation = options.process_generation;
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

    if (options.visual)
        Config::SetCurrent(Config::MAIN_OSD_MESSAGES, true);

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

    Core::SetIsThrottlerTempDisabled(true);

    wrapper->ConfigurePortsStandardPadP1();

    impl_->wrapper = std::move(wrapper);
    if (BackendResult pause = impl_->StartControlActuator(); !pause.ok)
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
    host_rollback.committed = true;
    return BackendResult::Success();
}

IVisualMessageBackendPort*
DolphinWrapperBackend::VisualMessages() noexcept
{
    return impl_ && impl_->open && impl_->has_open_options &&
            impl_->last_open_options.visual
        ? this
        : nullptr;
}

bool DolphinWrapperBackend::IsAvailable() const noexcept
{
    return impl_ && impl_->open && impl_->wrapper &&
        impl_->has_open_options && impl_->last_open_options.visual;
}

BackendResult DolphinWrapperBackend::ReplaceMessage(
    const VisualMessageSlot slot,
    std::string message)
{
    if (!IsAvailable())
    {
        return BackendResult::Failure(
            BackendErrorCode::Unavailable,
            "Visual message backend is unavailable");
    }

    constexpr std::uint32_t CurrentPhaseDurationMs =
        24u * 60u * 60u * 1000u;
    switch (slot)
    {
    case VisualMessageSlot::CurrentPhase:
        OSD::AddTypedMessage(
            static_cast<OSD::MessageType>(-1000),
            std::move(message),
            CurrentPhaseDurationMs,
            OSD::Color::CYAN);
        return BackendResult::Success();
    }
    return BackendResult::Failure(
        BackendErrorCode::InvalidArgument,
        "Visual message slot is unknown");
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

    if (BackendResult actuator = impl_->control_actuator.Stop(); !actuator.ok)
    {
        return MovieBackendResult::Failure(
            actuator.message.empty()
                ? "Dolphin control actuator was not drained before core stop"
                : actuator.message,
            GuestIntegrity::Unknown);
    }
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
        impl_->ResetControlState(false);
        return MovieBackendResult::Failure(
            error.empty()
                ? "Dolphin failed to stop its guest core for movie playback"
                : std::move(error),
            GuestIntegrity::Unknown);
    }
    impl_->ResetControlState(false);
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
        impl_->ResetControlState(false);
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
        impl_->ResetControlState(false);
        return MovieBackendResult::Failure(
            "Dolphin movie startup-state discovery disagreed with the prepared artifact baseline",
            GuestIntegrity::Unknown);
    }
    if (BackendResult pause = impl_->StartControlActuator(); !pause.ok)
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
        !execution.paused_quiescent)
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
    if (impl_->host_thread_declared)
    {
        if (BackendResult owner = impl_->RequireHostThread(); !owner.ok)
            return owner;
    }
    struct HostCloseRelease
    {
        Impl& impl;
        ~HostCloseRelease()
        {
            const BackendResult released = impl.ReleaseHostThread();
            if (!released.ok)
            {
                SCLOGEX(
                    SC_TAGS("dolphin.host", "dolphin.host.invariant"),
                    "release_failed=%s",
                    released.message.c_str());
            }
        }
    } host_release{*impl_};
    const MovieBackendResult pause_at_end =
        ReleasePauseAtPlaybackEnd();
    const BackendResult actuator_stop = impl_->control_actuator.Stop();
    if (!impl_->wrapper)
    {
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
        impl_->ResetControlState(false);
        if (!pause_at_end.ok || !actuator_stop.ok)
        {
            return BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                !actuator_stop.ok
                    ? actuator_stop.message
                    : pause_at_end.message.empty()
                    ? "Dolphin pause-at-playback-end configuration was not restored"
                    : pause_at_end.message,
                BackendIntegrity::Unknown);
        }
        return BackendResult::Success();
    }

    try
    {
        bool cleanup_ok = pause_at_end.ok && actuator_stop.ok &&
            impl_->owned_physical_plan.pcs.empty() &&
            impl_->owned_physical_plan.memory.empty();
        std::string cleanup_message;
        if (!actuator_stop.ok)
            cleanup_message = actuator_stop.message;
        if (!pause_at_end.ok)
        {
            if (!cleanup_message.empty())
                cleanup_message += "; ";
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
        impl_->ResetControlState(false);
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
    snapshot.result = BackendResult::Success();
    snapshot.core_state = QueryCoreState();
    snapshot.vi_count = impl_->wrapper->getViFieldCountApprox();
    snapshot.paused_quiescent =
        snapshot.core_state == BackendCoreState::Paused &&
        impl_->paused_quiescent.load(std::memory_order_acquire);
    snapshot.control_task_state = impl_->control_actuator.State();
    if (snapshot.paused_quiescent)
    {
        impl_->last_confirmed_pc = impl_->wrapper->getPC();
    }
    snapshot.pc = impl_->last_confirmed_pc;
    snapshot.throttle_disabled =
        Core::GetIsThrottlerTempDisabled();
    return snapshot;
}

BackendResult DolphinWrapperBackend::SubmitControlTask(BackendControlTask task)
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;

    const BackendCoreState state_before = QueryCoreState();
    SCLOGDX(
        SC_TAGS("dolphin.control", "dolphin.task.submit"),
        "kind=%u state_before=%u actuator_state=%u thread=%llu",
        static_cast<unsigned>(task.kind),
        static_cast<unsigned>(state_before),
        static_cast<unsigned>(impl_->control_actuator.State()),
        static_cast<unsigned long long>(
            std::hash<std::thread::id>{}(std::this_thread::get_id())));
    return impl_->control_actuator.Submit(std::move(task));
}

BackendResult DolphinWrapperBackend::PumpControlTask()
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    SCLOGDX(
        SC_TAGS("dolphin.control", "dolphin.actor_pump"),
        "actuator_state=%u thread=%llu",
        static_cast<unsigned>(impl_->control_actuator.State()),
        static_cast<unsigned long long>(
            std::hash<std::thread::id>{}(std::this_thread::get_id())));
    Core::HostDispatchJobs(*impl_->wrapper->system());
    return impl_->control_actuator.Pump();
}

std::optional<BackendControlCompletion>
DolphinWrapperBackend::TakeControlCompletion()
{
    std::optional<BackendControlCompletion> completion =
        impl_->control_actuator.TakeCompletion();
    if (!completion)
        return std::nullopt;

    const BackendExecutionSnapshot observed = QueryExecutionSnapshot();
    completion->resulting_core_state = observed.core_state;
    completion->pc = observed.pc;
    completion->vi_count = observed.vi_count;
    SCLOGDX(
        SC_TAGS("dolphin.control", "dolphin.task.complete"),
        "kind=%u ok=%d state_after=%u pc=0x%08x vi=%llu thread=%llu",
        static_cast<unsigned>(completion->kind),
        completion->result.ok ? 1 : 0,
        static_cast<unsigned>(completion->resulting_core_state),
        completion->pc,
        static_cast<unsigned long long>(completion->vi_count),
        static_cast<unsigned long long>(
            std::hash<std::thread::id>{}(std::this_thread::get_id())));
    return completion;
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
            impl_->ResetControlState(true);
        else
            impl_->ResetControlState(false);
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
            impl_->ResetControlState(true);
        else
            impl_->ResetControlState(false);
        return BackendResult::Success();
    }
    return BackendResult::Failure(
        BackendErrorCode::OperationFailed,
        "Dolphin failed to restore state from a buffer",
        BackendIntegrity::Unknown);
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
    const auto snapshot_id = g_movie_snapshot_id.fetch_add(1);
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
        !execution.paused_quiescent)
    {
        SCLOGWX(
            SC_TAGS("dolphin.movie_snapshot", "dolphin.invariant"),
            "snapshot_transaction=%llu operation=inspect_movie pre_state=%u paused_quiescent=%d reason=%s post_state=%u failed_invariant=authoritative_pause",
            static_cast<unsigned long long>(snapshot_id),
            static_cast<unsigned>(execution.core_state),
            execution.paused_quiescent ? 1 : 0,
            execution.result.message.empty()
                ? "state"
                : execution.result.message.c_str(),
            static_cast<unsigned>(QueryCoreState()));
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
        SCLOGDX(
            SC_TAGS("dolphin.movie_snapshot", "dolphin.transition"),
            "snapshot_transaction=%llu operation=inspect_movie pre_state=%u inspection=movie_fields post_state=%u",
            static_cast<unsigned long long>(snapshot_id),
            static_cast<unsigned>(execution.core_state),
            static_cast<unsigned>(QueryCoreState()));
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

std::string DolphinWrapperBackend::DescribePhysicalStopPoints(
    std::uint32_t observed_pc) const
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return "dolphin_physical_state_unavailable=" + open.message;

    Core::System& system = *impl_->wrapper->system();
    Core::CPUThreadGuard guard(system);
    auto& power_pc = system.GetPowerPC();
    auto& cpu = system.GetCPU();
    const auto& state = power_pc.GetPPCState();
    const auto& breakpoints = power_pc.GetBreakPoints();
    const auto& memchecks = power_pc.GetMemChecks();

    const auto& regular = breakpoints.GetBreakPoints();
    const auto& memory = memchecks.GetMemChecks();

    std::string shape_error;
    const bool manager_shape = PhysicalObjectsHaveManagerShape(
        system,
        impl_->owned_physical_plan,
        &shape_error);
    const TBreakPoint* effective = breakpoints.GetBreakpoint(observed_pc);
    const TBreakPoint* regular_at_pc =
        breakpoints.GetRegularBreakpoint(observed_pc);
    const bool temporary_at_pc = effective != nullptr &&
        effective != regular_at_pc;

    std::ostringstream out;
    out << "dolphin_cpu pc=0x" << std::hex << std::setw(8)
        << std::setfill('0') << state.pc
        << " npc=0x" << std::setw(8) << state.npc
        << " lr=0x" << std::setw(8) << LR(state)
        << " ctr=0x" << std::setw(8) << CTR(state)
        << std::dec << " core_mode=" << static_cast<unsigned>(power_pc.GetMode())
        << " cpu_stepping=" << (cpu.IsStepping() ? 1 : 0)
        << " observed_pc_effective_breakpoint="
        << (effective == nullptr ? "none" : temporary_at_pc ? "temporary" : "regular")
        << '\n';
    out << "native_hooks installed="
        << (savor::probe::NativeStopHooksInstalled() ? 1 : 0)
        << " sink_bound=" << (savor::probe::BoundNativeStopSink() != nullptr ? 1 : 0)
        << " expected_sink_bound=" << (impl_->native_sink != nullptr ? 1 : 0)
        << '\n';
    out << "dolphin_regular_breakpoints count=" << regular.size();
    for (const TBreakPoint& breakpoint : regular)
    {
        out << "\n  pc=0x" << std::hex << std::setw(8) << std::setfill('0')
            << breakpoint.address << std::dec
            << " enabled=" << (breakpoint.is_enabled ? 1 : 0)
            << " break_on_hit=" << (breakpoint.break_on_hit ? 1 : 0)
            << " log_on_hit=" << (breakpoint.log_on_hit ? 1 : 0)
            << " condition=" << (breakpoint.condition.has_value() ? 1 : 0);
    }
    out << "\ndolphin_memchecks count=" << memory.size();
    for (const TMemCheck& check : memory)
    {
        out << "\n  range=0x" << std::hex << std::setw(8)
            << std::setfill('0') << check.start_address
            << "-0x" << std::setw(8) << check.end_address << std::dec
            << " enabled=" << (check.is_enabled ? 1 : 0)
            << " read=" << (check.is_break_on_read ? 1 : 0)
            << " write=" << (check.is_break_on_write ? 1 : 0)
            << " break_on_hit=" << (check.break_on_hit ? 1 : 0)
            << " log_on_hit=" << (check.log_on_hit ? 1 : 0)
            << " condition=" << (check.condition.has_value() ? 1 : 0);
    }
    out << "\nmanager_shape_valid=" << (manager_shape ? 1 : 0)
        << " manager_generation=" << impl_->physical_generation.value()
        << " manager_shape_error="
        << (shape_error.empty() ? "<none>" : shape_error);
    return out.str();
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
