#include "BreakpointDiagnostics.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include <Windows.h>
#include <DbgHelp.h>
#include <MinHook.h>
#include <intrin.h>

#include "Core/HW/CPU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#pragma comment(lib, "Dbghelp.lib")

namespace savor::probe {
namespace {

constexpr std::size_t kEventCapacity = 256;
constexpr std::size_t kStackCapacity = 16;

enum class EventKind : std::uint8_t
{
    Break,
    SetStepping,
};

struct DiagnosticEvent
{
    std::uint64_t sequence = 0;
    std::uint64_t timestamp_ticks = 0;
    std::uint32_t thread_id = 0;
    EventKind kind = EventKind::Break;
    bool requested_stepping = false;
    BreakpointDiagnosticCause cause = BreakpointDiagnosticCause::Unknown;
    std::uint64_t correlation = 0;
    std::uintptr_t direct_caller = 0;
    std::array<std::uintptr_t, kStackCapacity> stack{};
    std::uint16_t stack_depth = 0;
    std::uint32_t guest_pc = 0;
    std::uint32_t cpu_state = 0;
};

using BreakFn = void(__fastcall*)(CPU::CPUManager*);
using SetSteppingFn = void(__fastcall*)(CPU::CPUManager*, bool);

std::atomic<bool> s_enabled{false};
std::atomic<bool> s_installed{false};
std::atomic<std::uint64_t> s_next_sequence{1};
std::atomic<std::uint64_t> s_dropped{0};
std::atomic<std::uint32_t> s_active_callbacks{0};
std::atomic_flag s_ring_lock = ATOMIC_FLAG_INIT;
std::mutex s_install_mutex;
std::array<DiagnosticEvent, kEventCapacity> s_events{};
BreakFn s_break = nullptr;
SetSteppingFn s_set_stepping = nullptr;
void* s_break_target = nullptr;
void* s_set_stepping_target = nullptr;

struct ThreadCause
{
    BreakpointDiagnosticCause cause = BreakpointDiagnosticCause::Unknown;
    std::uint64_t correlation = 0;
};
thread_local ThreadCause s_thread_cause;

template <typename Function, typename Member>
Function MemberFunctionAddress(Member member)
{
    static_assert(std::is_member_function_pointer_v<Member>);
    static_assert(std::is_pointer_v<Function>);
    static_assert(sizeof(Function) == sizeof(Member));
    Function function = nullptr;
    std::memcpy(&function, &member, sizeof(function));
    return function;
}

class ActiveCallback
{
public:
    ActiveCallback() noexcept
    {
        s_active_callbacks.fetch_add(1, std::memory_order_acq_rel);
    }

    ~ActiveCallback()
    {
        s_active_callbacks.fetch_sub(1, std::memory_order_acq_rel);
        s_active_callbacks.notify_all();
    }
};

void Capture(
    EventKind kind,
    bool requested_stepping,
    CPU::CPUManager* cpu,
    void* direct_caller) noexcept
{
    if (!s_installed.load(std::memory_order_acquire))
        return;
    if (s_ring_lock.test_and_set(std::memory_order_acquire))
    {
        s_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    DiagnosticEvent event;
    event.sequence = s_next_sequence.fetch_add(1, std::memory_order_relaxed);
    event.thread_id = GetCurrentThreadId();
    event.kind = kind;
    event.requested_stepping = requested_stepping;
    event.cause = s_thread_cause.cause;
    event.correlation = s_thread_cause.correlation;
    event.direct_caller = reinterpret_cast<std::uintptr_t>(direct_caller);
    LARGE_INTEGER timestamp{};
    QueryPerformanceCounter(&timestamp);
    event.timestamp_ticks = static_cast<std::uint64_t>(timestamp.QuadPart);
    event.stack_depth = static_cast<std::uint16_t>(RtlCaptureStackBackTrace(
        2,
        static_cast<ULONG>(event.stack.size()),
        reinterpret_cast<PVOID*>(event.stack.data()),
        nullptr));
    if (cpu != nullptr)
        event.cpu_state = static_cast<std::uint32_t>(cpu->GetState());
    try
    {
        event.guest_pc = Core::System::GetInstance()
                             .GetPowerPC()
                             .GetPPCState()
                             .pc;
    }
    catch (...)
    {
        event.guest_pc = 0;
    }
    s_events[(event.sequence - 1) % s_events.size()] = event;
    s_ring_lock.clear(std::memory_order_release);
}

void __fastcall BreakHook(CPU::CPUManager* cpu)
{
    ActiveCallback callback;
    Capture(EventKind::Break, true, cpu, _ReturnAddress());
    s_break(cpu);
}

void __fastcall SetSteppingHook(CPU::CPUManager* cpu, bool stepping)
{
    ActiveCallback callback;
    Capture(EventKind::SetStepping, stepping, cpu, _ReturnAddress());
    s_set_stepping(cpu, stepping);
}

std::string MinHookFailure(std::string_view action, MH_STATUS status)
{
    std::string message(action);
    message += ": ";
    message += MH_StatusToString(status);
    return message;
}

void RemoveHooksNoThrow() noexcept
{
    if (s_break_target != nullptr)
        (void)MH_QueueDisableHook(s_break_target);
    if (s_set_stepping_target != nullptr)
        (void)MH_QueueDisableHook(s_set_stepping_target);
    (void)MH_ApplyQueued();
    while (const auto active =
               s_active_callbacks.load(std::memory_order_acquire))
    {
        s_active_callbacks.wait(active, std::memory_order_relaxed);
    }
    if (s_break_target != nullptr)
        (void)MH_RemoveHook(s_break_target);
    if (s_set_stepping_target != nullptr)
        (void)MH_RemoveHook(s_set_stepping_target);
    s_break_target = nullptr;
    s_set_stepping_target = nullptr;
    s_break = nullptr;
    s_set_stepping = nullptr;
}

const char* CauseName(BreakpointDiagnosticCause cause) noexcept
{
    switch (cause)
    {
    case BreakpointDiagnosticCause::StopRouter:
        return "stop_router";
    case BreakpointDiagnosticCause::Unknown:
        break;
    }
    return "unknown";
}

const char* EventName(EventKind kind) noexcept
{
    return kind == EventKind::Break ? "Break" : "SetStepping";
}

std::string Symbolize(std::uintptr_t address)
{
    if (address == 0)
        return "0x0000000000000000";

    static std::mutex symbol_mutex;
    static bool initialized = false;
    static bool available = false;
    std::lock_guard lock(symbol_mutex);
    const HANDLE process = GetCurrentProcess();
    if (!initialized)
    {
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
        available = SymInitialize(process, nullptr, TRUE) == TRUE;
        initialized = true;
    }

    std::ostringstream text;
    text << "0x" << std::hex << std::setw(16) << std::setfill('0')
         << address << std::dec;
    if (!available)
        return text.str();

    std::array<unsigned char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> storage{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage.data());
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    if (SymFromAddr(
            process,
            static_cast<DWORD64>(address),
            &displacement,
            symbol))
    {
        text << ' ' << symbol->Name;
        if (displacement != 0)
            text << "+0x" << std::hex << displacement << std::dec;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD line_displacement = 0;
        if (SymGetLineFromAddr64(
                process,
                static_cast<DWORD64>(address),
                &line_displacement,
                &line))
        {
            text << " (" << line.FileName << ':' << line.LineNumber << ')';
        }
    }
    return text.str();
}

} // namespace

BreakpointDiagnosticCauseScope::BreakpointDiagnosticCauseScope(
    BreakpointDiagnosticCause cause,
    std::uint64_t correlation) noexcept
    : previous_cause_(s_thread_cause.cause),
      previous_correlation_(s_thread_cause.correlation)
{
    s_thread_cause = {cause, correlation};
}

BreakpointDiagnosticCauseScope::~BreakpointDiagnosticCauseScope()
{
    s_thread_cause = {previous_cause_, previous_correlation_};
}

void ConfigureBreakpointDiagnostics(bool enabled) noexcept
{
    s_enabled.store(enabled, std::memory_order_release);
}

bool BreakpointDiagnosticsEnabled() noexcept
{
    return s_enabled.load(std::memory_order_acquire);
}

bool InstallBreakpointDiagnostics(std::string* error_out)
{
    std::scoped_lock lock(s_install_mutex);
    if (!BreakpointDiagnosticsEnabled() ||
        s_installed.load(std::memory_order_acquire))
    {
        return true;
    }

    s_break_target = reinterpret_cast<void*>(
        MemberFunctionAddress<BreakFn>(&CPU::CPUManager::Break));
    s_set_stepping_target = reinterpret_cast<void*>(
        MemberFunctionAddress<SetSteppingFn>(&CPU::CPUManager::SetStepping));

    MH_STATUS status = MH_CreateHook(
        s_break_target,
        reinterpret_cast<void*>(&BreakHook),
        reinterpret_cast<void**>(&s_break));
    if (status == MH_OK)
    {
        status = MH_CreateHook(
            s_set_stepping_target,
            reinterpret_cast<void*>(&SetSteppingHook),
            reinterpret_cast<void**>(&s_set_stepping));
    }
    if (status == MH_OK)
        status = MH_QueueEnableHook(s_break_target);
    if (status == MH_OK)
        status = MH_QueueEnableHook(s_set_stepping_target);
    if (status == MH_OK)
        status = MH_ApplyQueued();
    if (status != MH_OK)
    {
        if (error_out != nullptr)
            *error_out = MinHookFailure(
                "failed installing breakpoint diagnostics", status);
        RemoveHooksNoThrow();
        return false;
    }

    while (s_ring_lock.test_and_set(std::memory_order_acquire))
        std::this_thread::yield();
    s_events = {};
    s_next_sequence.store(1, std::memory_order_relaxed);
    s_dropped.store(0, std::memory_order_relaxed);
    s_ring_lock.clear(std::memory_order_release);
    s_installed.store(true, std::memory_order_release);
    return true;
}

void UninstallBreakpointDiagnostics() noexcept
{
    std::scoped_lock lock(s_install_mutex);
    if (!s_installed.exchange(false, std::memory_order_acq_rel))
        return;
    RemoveHooksNoThrow();
}

bool BreakpointDiagnosticsInstalled() noexcept
{
    return s_installed.load(std::memory_order_acquire);
}

std::string DescribeRecentBreakpointDiagnostics(std::size_t maximum_events)
{
    std::vector<DiagnosticEvent> events;
    if (BreakpointDiagnosticsInstalled())
    {
        while (s_ring_lock.test_and_set(std::memory_order_acquire))
            std::this_thread::yield();
        events.reserve(s_events.size());
        for (const auto& event : s_events)
        {
            if (event.sequence != 0)
                events.push_back(event);
        }
        s_ring_lock.clear(std::memory_order_release);
        std::ranges::sort(events, {}, &DiagnosticEvent::sequence);
        if (events.size() > maximum_events)
        {
            events.erase(
                events.begin(),
                events.end() - static_cast<std::ptrdiff_t>(maximum_events));
        }
    }

    std::ostringstream text;
    text << "breakpoint_diagnostics enabled="
         << (BreakpointDiagnosticsEnabled() ? 1 : 0)
         << " installed=" << (BreakpointDiagnosticsInstalled() ? 1 : 0)
         << " events=" << events.size()
         << " dropped=" << s_dropped.load(std::memory_order_relaxed);
    for (const auto& event : events)
    {
        text << '\n'
             << "  sequence=" << event.sequence
             << " ticks=" << event.timestamp_ticks
             << " thread=" << event.thread_id
             << " kind=" << EventName(event.kind)
             << " requested_stepping="
             << (event.requested_stepping ? 1 : 0)
             << " cause=" << CauseName(event.cause)
             << " correlation=" << event.correlation
             << " guest_pc=0x" << std::hex << std::setw(8)
             << std::setfill('0') << event.guest_pc << std::dec
             << " cpu_state=" << event.cpu_state
             << " caller=" << Symbolize(event.direct_caller);
        const std::size_t depth = std::min<std::size_t>(
            event.stack_depth, event.stack.size());
        for (std::size_t index = 0; index < depth; ++index)
        {
            text << '\n' << "    #" << index << ' '
                 << Symbolize(event.stack[index]);
        }
    }
    return text.str();
}

} // namespace savor::probe
