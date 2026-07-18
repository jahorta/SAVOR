#include "ProbeRuntime.h"
#include "NativeHookSemantics.h"

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include <MinHook.h>

#include "Core/HW/CPU.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace savor::probe {
namespace {

using CheckBreakpointsFn = bool(__fastcall*)(PowerPC::PowerPCManager*);
using CheckBreakpointsFromJitFn = void(__fastcall*)(PowerPC::PowerPCManager*);
using MemcheckActionFn = bool(__fastcall*)(
    TMemCheck*, Core::System&, std::uint64_t, std::uint32_t, bool, std::size_t, std::uint32_t);
using CpuBreakFn = void(__fastcall*)(CPU::CPUManager*);

CheckBreakpointsFn s_check_breakpoints = nullptr;
CheckBreakpointsFromJitFn s_check_breakpoints_from_jit = nullptr;
MemcheckActionFn s_memcheck_action = nullptr;
CpuBreakFn s_cpu_break = nullptr;

std::mutex s_hook_mutex;
std::atomic<bool> s_hooks_installed{ false };
thread_local bool s_in_probe_hook = false;

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
    HookDispatchScope() : outermost_(!s_in_probe_hook)
    {
        if (outermost_)
            s_in_probe_hook = true;
    }

    ~HookDispatchScope()
    {
        if (outermost_)
            s_in_probe_hook = false;
    }

    bool outermost() const { return outermost_; }

private:
    bool outermost_ = false;
};

bool __fastcall check_breakpoints_hook(PowerPC::PowerPCManager* power_pc)
{
    HookDispatchScope scope;
    const bool probe_control = scope.outermost()
        && ProbeRuntime::instance().dispatch_pc(*power_pc);
    const bool dolphin_control = s_check_breakpoints(power_pc);
    if (scope.outermost())
        ProbeRuntime::instance().complete_pc_dispatch(probe_control || dolphin_control);
    if (probe_control && !dolphin_control)
        Core::System::GetInstance().GetCPU().Break();
    return probe_control || dolphin_control;
}

void __fastcall check_breakpoints_from_jit_hook(PowerPC::PowerPCManager* power_pc)
{
    HookDispatchScope scope;
    const bool probe_control = scope.outermost()
        && ProbeRuntime::instance().dispatch_pc(*power_pc);
    s_check_breakpoints_from_jit(power_pc);
    auto& cpu = Core::System::GetInstance().GetCPU();
    if (scope.outermost())
        ProbeRuntime::instance().complete_pc_dispatch(probe_control || cpu.IsStepping());
    if (probe_control && !cpu.IsStepping())
        cpu.Break();
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
            return ProbeRuntime::instance().dispatch_memory(
                system,
                current.pc,
                current.address,
                static_cast<std::uint32_t>(current.size),
                current.value,
                current.write,
                true);
        });
}

void __fastcall cpu_break_hook(CPU::CPUManager* cpu)
{
    HookDispatchScope scope;
    if (scope.outermost())
        ProbeRuntime::instance().observe_cpu_break(*cpu);
    s_cpu_break(cpu);
}

struct HookSpec {
    void* target = nullptr;
    void* detour = nullptr;
    void** original = nullptr;
    const char* name = nullptr;
};

std::array<HookSpec, 4> hook_specs()
{
    return { {
        { hook_address(member_function_address<CheckBreakpointsFn>(
              &PowerPC::PowerPCManager::CheckAndHandleBreakPoints)),
            hook_address(&check_breakpoints_hook), reinterpret_cast<void**>(&s_check_breakpoints),
            "PowerPCManager::CheckAndHandleBreakPoints" },
        { hook_address(&PowerPC::CheckAndHandleBreakPointsFromJIT),
            hook_address(&check_breakpoints_from_jit_hook),
            reinterpret_cast<void**>(&s_check_breakpoints_from_jit),
            "PowerPC::CheckAndHandleBreakPointsFromJIT" },
        { hook_address(member_function_address<MemcheckActionFn>(&TMemCheck::Action)),
            hook_address(&memcheck_action_hook), reinterpret_cast<void**>(&s_memcheck_action),
            "TMemCheck::Action" },
        { hook_address(member_function_address<CpuBreakFn>(&CPU::CPUManager::Break)),
            hook_address(&cpu_break_hook), reinterpret_cast<void**>(&s_cpu_break), "CPUManager::Break" },
    } };
}

std::vector<HookSpec> unique_hook_specs()
{
    const auto candidates = hook_specs();
    std::vector<HookSpec> unique;
    unique.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        bool already_present = false;
        for (const auto& installed : unique) {
            if (installed.target == candidate.target) {
                already_present = true;
                break;
            }
        }
        if (!already_present)
            unique.push_back(candidate);
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

} // namespace

bool ProbeRuntime::install_native_hooks(std::string* error_out)
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
    for (; created < hooks.size(); ++created) {
        const auto& hook = hooks[created];
        status = MH_CreateHook(hook.target, hook.detour, hook.original);
        if (status != MH_OK)
            break;
    }
    if (status == MH_OK) {
        for (const auto& hook : hooks) {
            status = MH_QueueEnableHook(hook.target);
            if (status != MH_OK)
                break;
        }
    }
    if (status == MH_OK)
        status = MH_ApplyQueued();

    if (status != MH_OK) {
        if (error_out) {
            const HookSpec* failed = created < hooks.size() ? &hooks[created] : nullptr;
            *error_out = minhook_error("failed installing native hook", failed, status);
        }
        MH_QueueDisableHook(MH_ALL_HOOKS);
        MH_ApplyQueued();
        for (std::size_t i = 0; i < created; ++i)
            MH_RemoveHook(hooks[i].target);
        return false;
    }

    s_hooks_installed.store(true, std::memory_order_release);
    return true;
}

void ProbeRuntime::uninstall_native_hooks()
{
    std::scoped_lock lock(s_hook_mutex);
    if (!s_hooks_installed.exchange(false, std::memory_order_acq_rel))
        return;
    const auto hooks = unique_hook_specs();
    MH_QueueDisableHook(MH_ALL_HOOKS);
    MH_ApplyQueued();
    for (const auto& hook : hooks)
        MH_RemoveHook(hook.target);
    MH_Uninitialize();
}

bool ProbeRuntime::native_hooks_installed() const
{
    return s_hooks_installed.load(std::memory_order_acquire);
}

} // namespace savor::probe
