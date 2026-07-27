#include "DolphinWrapperBackend.h"

#include "../../Boot/Boot.h"
#include "../../Core/DolphinWrapper.h"

#include "Common/Buffer.h"
#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/PowerPC/BreakPoints.h"

#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace savor::runtime {
namespace {

[[nodiscard]] int ClampTimeout(std::chrono::milliseconds timeout) noexcept
{
    const auto count = timeout.count();
    if (count <= 0)
        return 1;
    return static_cast<int>(std::min<std::int64_t>(
        count,
        std::numeric_limits<int>::max()));
}

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
    std::unique_ptr<DolphinWrapper> wrapper;
    BackendOpenOptions last_open_options;
    bool has_open_options = false;
    bool open = false;
    savor::probe::INativeStopSink* native_sink = nullptr;
    PhysicalStopPointPlan owned_physical_plan;
    PhysicalPlanGeneration physical_generation;
    DolphinBackendCpuCore cpu_core =
        DolphinBackendCpuCore::ProductionDefault;

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

DolphinWrapperBackend::~DolphinWrapperBackend() = default;

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

    if (!wrapper->loadGame(options.iso_path.string()))
    {
        wrapper.reset();
        return BackendResult::Failure(
            BackendErrorCode::GameLoadFailed,
            "Dolphin failed to load the requested game",
            BackendIntegrity::Unknown);
    }

    wrapper->ConfigurePortsStandardPadP1();

    impl_->wrapper = std::move(wrapper);
    impl_->last_open_options = options;
    impl_->has_open_options = true;
    impl_->open = true;
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

    impl_->wrapper.reset();
    impl_->open = false;
    impl_->owned_physical_plan = {};
    BackendResult result = Open(options);
    if (!result.ok)
        result.integrity = BackendIntegrity::Unknown;
    return result;
}

BackendResult DolphinWrapperBackend::Close()
{
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
        impl_->wrapper.reset();
        impl_->open = false;
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

BackendResult DolphinWrapperBackend::Pause(std::chrono::milliseconds timeout)
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    if (impl_->wrapper->pauseEmulationBlocking(ClampUnsignedTimeout(timeout)))
        return BackendResult::Success();
    return BackendResult::Failure(
        BackendErrorCode::Timeout,
        "Timed out while pausing Dolphin");
}

BackendResult DolphinWrapperBackend::Resume()
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    if (impl_->wrapper->resumeEmulation())
        return BackendResult::Success();
    return BackendResult::Failure(
        BackendErrorCode::OperationFailed,
        "Dolphin failed to resume emulation");
}

BackendResult DolphinWrapperBackend::StepInstruction(std::chrono::milliseconds timeout)
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    if (impl_->wrapper->stepOneOpcodeBlocking(ClampTimeout(timeout)))
        return BackendResult::Success();
    return BackendResult::Failure(
        BackendErrorCode::Timeout,
        "Timed out while stepping one guest instruction",
        BackendIntegrity::Unknown);
}

BackendResult DolphinWrapperBackend::StepFrame(std::chrono::milliseconds timeout)
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    if (impl_->wrapper->stepOneFrameBlocking(ClampTimeout(timeout)))
        return BackendResult::Success();
    return BackendResult::Failure(
        BackendErrorCode::Timeout,
        "Timed out while stepping one guest frame",
        BackendIntegrity::Unknown);
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
        return BackendResult::Success();
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
        return BackendResult::Success();
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
