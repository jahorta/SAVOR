#include "NativeStopHooks.h"
#include "NativeHookSemantics.h"
#include "BreakpointDiagnostics.h"

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>

#include <MinHook.h>

#include "Core/HW/CPU.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace savor::probe {
namespace {

using CheckBreakpointsFromJitFn =
    decltype(&PowerPC::CheckAndHandleBreakPointsFromJIT);
using MemcheckActionFn = bool(__fastcall*)(
    TMemCheck*, Core::System&, std::uint64_t, std::uint32_t, bool, std::size_t, std::uint32_t);

CheckBreakpointsFromJitFn s_check_breakpoints_from_jit = nullptr;
MemcheckActionFn s_memcheck_action = nullptr;

std::mutex s_hook_mutex;
std::atomic<bool> s_hooks_installed{ false };
std::atomic<INativeStopSink*> s_stop_sink{ nullptr };
std::atomic<std::uint32_t> s_active_sink_dispatches{ 0 };
std::atomic_flag s_sink_binding_lock = ATOMIC_FLAG_INIT;
thread_local bool s_in_native_stop_hook = false;
thread_local std::uint32_t s_sink_callback_depth = 0;

template <typename Function, typename Member>
Function member_function_address(Member member)
{
    static_assert(std::is_member_function_pointer_v<Member>);
    static_assert(std::is_pointer_v<Function>);
    static_assert(sizeof(Function) == sizeof(Member),
        "SavorProbe requires the supported x64 single-inheritance member-pointer ABI");
    Function function = nullptr;
    std::memcpy(&function, &member, sizeof(function));
    return function;
}

template <typename Function>
void* hook_address(Function function)
{
    static_assert(std::is_pointer_v<Function>);
    void* address = nullptr;
    static_assert(sizeof(address) == sizeof(function));
    std::memcpy(&address, &function, sizeof(address));
    return address;
}

class HookDispatchScope {
public:
    HookDispatchScope() : outermost_(!s_in_native_stop_hook)
    {
        if (outermost_)
            s_in_native_stop_hook = true;
    }

    ~HookDispatchScope()
    {
        if (outermost_)
            s_in_native_stop_hook = false;
    }

    bool outermost() const { return outermost_; }

private:
    bool outermost_ = false;
};

class SinkBindingLock {
public:
    SinkBindingLock() noexcept
    {
        while (s_sink_binding_lock.test_and_set(std::memory_order_acquire))
            std::this_thread::yield();
    }

    ~SinkBindingLock()
    {
        s_sink_binding_lock.clear(std::memory_order_release);
    }

    SinkBindingLock(const SinkBindingLock&) = delete;
    SinkBindingLock& operator=(const SinkBindingLock&) = delete;
};

class ActiveSinkDispatch {
public:
    ActiveSinkDispatch() noexcept
    {
        s_active_sink_dispatches.fetch_add(1, std::memory_order_seq_cst);
    }

    ~ActiveSinkDispatch()
    {
        s_active_sink_dispatches.fetch_sub(1, std::memory_order_seq_cst);
    }

    ActiveSinkDispatch(const ActiveSinkDispatch&) = delete;
    ActiveSinkDispatch& operator=(const ActiveSinkDispatch&) = delete;
};

class SinkCallbackScope {
public:
    SinkCallbackScope() noexcept
    {
        ++s_sink_callback_depth;
    }

    ~SinkCallbackScope()
    {
        --s_sink_callback_depth;
    }

    SinkCallbackScope(const SinkCallbackScope&) = delete;
    SinkCallbackScope& operator=(const SinkCallbackScope&) = delete;
};

void check_breakpoints_from_jit_hook(PowerPC::PowerPCManager& power_pc)
{
    HookDispatchScope scope;
    NativeStopDecision sink_decision;
    if (scope.outermost()) {
        sink_decision = DispatchBoundNativePcStop(NativePcStop{
            NativeStopOrigin::Jit,
            power_pc.GetPPCState().pc,
            &power_pc,
        });
    }
    const bool sink_control = NativeStopRequiresBreak(sink_decision);
    s_check_breakpoints_from_jit(power_pc);
    auto& cpu = Core::System::GetInstance().GetCPU();
    const bool dolphin_control = cpu.IsStepping();
    if (sink_control && !dolphin_control)
    {
        BreakpointDiagnosticCauseScope cause(
            BreakpointDiagnosticCause::StopRouter,
            sink_decision.routed_sequence);
        cpu.Break();
    }
}

bool __fastcall memcheck_action_hook(
    TMemCheck* memcheck,
    Core::System& system,
    std::uint64_t value,
    std::uint32_t address,
    bool write,
    std::size_t size,
    std::uint32_t pc)
{
    HookDispatchScope scope;
    const NativeMemcheckEvent event{ value, address, write, size, pc };
    return dispatch_memcheck_action_once(
        event,
        [&](const NativeMemcheckEvent& current) {
            return s_memcheck_action(
                memcheck,
                system,
                current.value,
                current.address,
                current.write,
                current.size,
                current.pc);
        },
        [&](const NativeMemcheckEvent& current) {
            if (!scope.outermost())
                return false;
            return NativeStopRequiresBreak(DispatchBoundNativeMemoryStop(NativeMemoryStop{
                NativeStopOrigin::Memcheck,
                current.pc,
                current.address,
                static_cast<std::uint32_t>(current.size),
                current.value,
                current.write,
                true,
                &system,
            }));
        });
}

struct HookSpec {
    void* target = nullptr;
    void* detour = nullptr;
    void** original = nullptr;
    const char* name = nullptr;
};

std::array<HookSpec, 2> hook_specs()
{
    return {{
        { hook_address(&PowerPC::CheckAndHandleBreakPointsFromJIT),
            hook_address(&check_breakpoints_from_jit_hook),
            reinterpret_cast<void**>(&s_check_breakpoints_from_jit),
            "PowerPC::CheckAndHandleBreakPointsFromJIT" },
        { hook_address(member_function_address<MemcheckActionFn>(&TMemCheck::Action)),
            hook_address(&memcheck_action_hook), reinterpret_cast<void**>(&s_memcheck_action),
            "TMemCheck::Action" },
    }};
}

struct HookSpecSet
{
    std::array<HookSpec, 2> hooks{};
    std::size_t count = 0;
};

HookSpecSet unique_hook_specs()
{
    const auto candidates = hook_specs();
    HookSpecSet unique;
    for (const auto& candidate : candidates) {
        bool already_present = false;
        for (std::size_t index = 0; index < unique.count; ++index) {
            if (unique.hooks[index].target == candidate.target) {
                already_present = true;
                break;
            }
        }
        if (!already_present) {
            unique.hooks[unique.count] = candidate;
            ++unique.count;
        }
    }
    return unique;
}

std::string minhook_error(const char* action, const HookSpec* hook, MH_STATUS status)
{
    std::string message = action;
    if (hook && hook->name) {
        message += " ";
        message += hook->name;
    }
    message += ": ";
    message += MH_StatusToString(status);
    return message;
}

void set_error_noexcept(std::string* error_out, const char* message) noexcept
{
    if (!error_out)
        return;
    try {
        *error_out = message;
    } catch (...) {
    }
}

} // namespace

bool InstallNativeStopHooks(std::string* error_out)
{
    std::scoped_lock lock(s_hook_mutex);
    if (s_hooks_installed.load(std::memory_order_acquire))
        return true;

    auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        if (error_out) *error_out = minhook_error("MH_Initialize failed", nullptr, status);
        return false;
    }

    const auto hooks = unique_hook_specs();
    std::size_t created = 0;
    for (; created < hooks.count; ++created) {
        const auto& hook = hooks.hooks[created];
        status = MH_CreateHook(hook.target, hook.detour, hook.original);
        if (status != MH_OK)
            break;
    }
    if (status == MH_OK) {
        for (std::size_t index = 0; index < hooks.count; ++index) {
            const auto& hook = hooks.hooks[index];
            status = MH_QueueEnableHook(hook.target);
            if (status != MH_OK)
                break;
        }
    }
    if (status == MH_OK)
        status = MH_ApplyQueued();

    if (status != MH_OK) {
        if (error_out) {
            const HookSpec* failed =
                created < hooks.count ? &hooks.hooks[created] : nullptr;
            *error_out = minhook_error("failed installing native hook", failed, status);
        }
        for (std::size_t index = 0; index < created; ++index)
            MH_QueueDisableHook(hooks.hooks[index].target);
        MH_ApplyQueued();
        for (std::size_t i = 0; i < created; ++i)
            MH_RemoveHook(hooks.hooks[i].target);
        return false;
    }

    if (!InstallBreakpointDiagnostics(error_out))
    {
        for (std::size_t index = 0; index < hooks.count; ++index)
            MH_QueueDisableHook(hooks.hooks[index].target);
        MH_ApplyQueued();
        for (std::size_t index = 0; index < hooks.count; ++index)
            MH_RemoveHook(hooks.hooks[index].target);
        MH_Uninitialize();
        return false;
    }
    s_hooks_installed.store(true, std::memory_order_release);
    return true;
}

void UninstallNativeStopHooks() noexcept
{
    std::scoped_lock lock(s_hook_mutex);
    if (!s_hooks_installed.exchange(false, std::memory_order_acq_rel))
        return;
    UninstallBreakpointDiagnostics();
    const auto hooks = unique_hook_specs();
    for (std::size_t index = 0; index < hooks.count; ++index)
        MH_QueueDisableHook(hooks.hooks[index].target);
    MH_ApplyQueued();
    for (std::size_t index = 0; index < hooks.count; ++index)
        MH_RemoveHook(hooks.hooks[index].target);
    MH_Uninitialize();
}

bool NativeStopHooksInstalled() noexcept
{
    return s_hooks_installed.load(std::memory_order_acquire);
}

bool BindNativeStopSink(INativeStopSink& sink, std::string* error_out) noexcept
{
    if (s_sink_callback_depth != 0) {
        set_error_noexcept(error_out, "native stop sink binding is not allowed from a sink callback");
        return false;
    }

    SinkBindingLock lock;
    auto* const current = s_stop_sink.load(std::memory_order_seq_cst);
    if (current == &sink)
        return true;
    if (current != nullptr) {
        set_error_noexcept(error_out, "a native stop sink is already bound");
        return false;
    }
    s_stop_sink.store(&sink, std::memory_order_seq_cst);
    return true;
}

bool UnbindNativeStopSink(INativeStopSink& sink) noexcept
{
    if (s_sink_callback_depth != 0)
        return false;

    SinkBindingLock lock;
    auto* expected = &sink;
    if (!s_stop_sink.compare_exchange_strong(
            expected,
            nullptr,
            std::memory_order_seq_cst,
            std::memory_order_seq_cst)) {
        return false;
    }

    while (s_active_sink_dispatches.load(std::memory_order_seq_cst) != 0)
        std::this_thread::yield();
    return true;
}

INativeStopSink* BoundNativeStopSink() noexcept
{
    return s_stop_sink.load(std::memory_order_seq_cst);
}

NativeStopDecision DispatchBoundNativePcStop(const NativePcStop& stop) noexcept
{
    ActiveSinkDispatch dispatch;
    auto* const sink = s_stop_sink.load(std::memory_order_seq_cst);
    if (!sink)
        return {};

    SinkCallbackScope callback;
    return sink->OnPcStop(stop);
}

NativeStopDecision DispatchBoundNativeMemoryStop(const NativeMemoryStop& stop) noexcept
{
    ActiveSinkDispatch dispatch;
    auto* const sink = s_stop_sink.load(std::memory_order_seq_cst);
    if (!sink)
        return {};

    SinkCallbackScope callback;
    return sink->OnMemoryStop(stop);
}

} // namespace savor::probe
