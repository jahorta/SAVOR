#include "DolphinWrapperBackend.h"

#include "../../Boot/Boot.h"
#include "../../Core/DolphinWrapper.h"

#include "Common/Buffer.h"
#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/CPU.h"
#include "Core/PowerPC/BreakPoints.h"

#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <exception>
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
    mutable bool observed_movie_playing = false;
    std::shared_ptr<PauseConfirmation> pause_confirmation =
        std::make_shared<PauseConfirmation>();
    PauseSynchronizer pause_synchronizer;
    mutable std::uint32_t last_confirmed_pc = 0;
    int state_callback_handle = -1;

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
    {
        impl_->pause_synchronizer.StopAndJoin();
        impl_->DetachStateCallback();
    }
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

    if (!wrapper->loadGame(options.iso_path.string(), true))
    {
        wrapper.reset();
        return BackendResult::Failure(
            BackendErrorCode::GameLoadFailed,
            "Dolphin failed to load the requested game",
            BackendIntegrity::Unknown);
    }

    wrapper->ConfigurePortsStandardPadP1();

    impl_->ResetPauseConfirmation(true);
    const auto pause_confirmation = impl_->pause_confirmation;
    Core::System* const system = wrapper->system();
    impl_->state_callback_handle =
        Core::AddOnStateChangedCallback(
            [pause_confirmation, system](Core::State state) {
                if (state != Core::State::Paused)
                    return;
                auto acknowledge = [pause_confirmation] {
                    const std::uint64_t generation =
                        pause_confirmation->requested.load(
                            std::memory_order_acquire);
                    std::uint64_t acknowledged =
                        pause_confirmation->acknowledged.load(
                            std::memory_order_acquire);
                    while (acknowledged < generation &&
                        !pause_confirmation->acknowledged
                             .compare_exchange_weak(
                                 acknowledged,
                                 generation,
                                 std::memory_order_release,
                                 std::memory_order_acquire))
                    {
                    }
                };
                if (Core::IsCPUThread())
                {
                    // The callback runs before the CPU leaves RunLoop.
                    // The queued job executes on the next stepping-loop turn,
                    // after m_state_cpu_thread_active has become false.
                    system->GetCPU().AddCPUThreadJob(
                        std::move(acknowledge));
                }
                else
                {
                    // Host-side Paused notification follows SetStepping(true),
                    // which already synchronizes with CPU idleness.
                    acknowledge();
                }
            });

    std::string pause_helper_error;
    if (!impl_->pause_synchronizer.Start(
            *system,
            pause_confirmation,
            &pause_helper_error))
    {
        impl_->DetachStateCallback();
        wrapper.reset();
        impl_->ResetPauseConfirmation(false);
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            pause_helper_error.empty()
                ? "Failed to start Dolphin pause synchronizer"
                : std::string("Failed to start Dolphin pause synchronizer: ") +
                    pause_helper_error,
            BackendIntegrity::Unknown);
    }

    impl_->wrapper = std::move(wrapper);
    impl_->last_open_options = options;
    impl_->has_open_options = true;
    impl_->open = true;
    impl_->observed_movie_playing = false;
    return BackendResult::Success();
}

BackendResult DolphinWrapperBackend::Reboot()
{
    if (!impl_->has_open_options)
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidState,
            "Dolphin backend has no prior open configuration");
    }

    const BackendOpenOptions options = impl_->last_open_options;
    if (impl_->wrapper)
    {
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
                return BackendResult::Failure(
                    BackendErrorCode::OperationFailed,
                    error.empty()
                        ? "Dolphin reboot found unmanaged physical stop points"
                        : std::move(error),
                    BackendIntegrity::Unknown);
            }
        }
        catch (const std::exception& ex)
        {
            return BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                std::string("failed validating physical stop points for reboot: ") +
                    ex.what(),
                BackendIntegrity::Unknown);
        }
        catch (...)
        {
            return BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                "failed validating physical stop points for reboot",
                BackendIntegrity::Unknown);
        }
    }

    impl_->pause_synchronizer.StopAndJoin();
    impl_->DetachStateCallback();
    impl_->wrapper.reset();
    impl_->open = false;
    impl_->observed_movie_playing = false;
    impl_->owned_physical_plan = {};
    BackendResult result = Open(options);
    if (!result.ok)
        result.integrity = BackendIntegrity::Unknown;
    return result;
}

BackendResult DolphinWrapperBackend::Close()
{
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
        impl_->observed_movie_playing = false;
        impl_->ResetPauseConfirmation(false);
        return BackendResult::Success();
    }

    try
    {
        bool cleanup_ok = impl_->owned_physical_plan.pcs.empty() &&
            impl_->owned_physical_plan.memory.empty();
        std::string cleanup_message = cleanup_ok
            ? std::string{}
            : std::string(
                  "PhysicalStopPointManager did not remove all owned sites "
                  "before backend close");
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
        impl_->observed_movie_playing = false;
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

BackendExecutionCapabilityMask
DolphinWrapperBackend::Capabilities() const noexcept
{
    return BackendExecutionCapability::Pause |
        BackendExecutionCapability::Resume |
        BackendExecutionCapability::FrameStep |
        BackendExecutionCapability::ViObservation |
        BackendExecutionCapability::MovieObservation |
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
    snapshot.movie_input_count =
        impl_->wrapper->getCurrentMovieInputCount();
    const bool movie_playing = impl_->wrapper->isMoviePlaying();
    if (movie_playing)
    {
        impl_->observed_movie_playing = true;
        snapshot.movie_state = BackendMovieState::Playing;
    }
    else if (impl_->observed_movie_playing)
    {
        snapshot.movie_state = BackendMovieState::Ended;
    }
    else
    {
        snapshot.movie_state = BackendMovieState::Inactive;
    }
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

BackendResult DolphinWrapperBackend::BeginExactInstructionStep()
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    return BackendResult::Failure(
        BackendErrorCode::Unavailable,
        "Exact guest-instruction stepping is unavailable in JIT64");
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

BackendBufferResult DolphinWrapperBackend::SaveStateBuffer()
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return {std::move(open), {}};

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
