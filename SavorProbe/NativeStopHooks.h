#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace Core {
class System;
}

namespace PowerPC {
class PowerPCManager;
}

namespace savor::probe {

enum class NativeStopOrigin : std::uint8_t
{
    Jit,
    Memcheck,
};

struct NativePcStop
{
    NativeStopOrigin origin = NativeStopOrigin::Jit;
    std::uint32_t pc = 0;
    PowerPC::PowerPCManager* power_pc = nullptr;
};

struct NativeMemoryStop
{
    NativeStopOrigin origin = NativeStopOrigin::Memcheck;
    std::uint32_t pc = 0;
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    std::uint64_t value = 0;
    bool write = false;
    bool post_write = true;
    Core::System* system = nullptr;
};

struct NativeStopDecision
{
    bool request_break = false;
    bool authoritative_overflow = false;
    std::uint64_t routed_sequence = 0;
};

[[nodiscard]] constexpr bool NativeStopRequiresBreak(
    const NativeStopDecision& decision) noexcept
{
    return decision.request_break || decision.authoritative_overflow;
}

class INativeStopSink
{
public:
    virtual ~INativeStopSink() = default;

    INativeStopSink(const INativeStopSink&) = delete;
    INativeStopSink& operator=(const INativeStopSink&) = delete;

    [[nodiscard]] virtual NativeStopDecision OnPcStop(
        const NativePcStop& stop) noexcept = 0;
    [[nodiscard]] virtual NativeStopDecision OnMemoryStop(
        const NativeMemoryStop& stop) noexcept = 0;

protected:
    INativeStopSink() = default;
};

[[nodiscard]] bool InstallNativeStopHooks(std::string* error_out = nullptr);
void UninstallNativeStopHooks() noexcept;
[[nodiscard]] bool NativeStopHooksInstalled() noexcept;

[[nodiscard]] bool BindNativeStopSink(
    INativeStopSink& sink,
    std::string* error_out = nullptr) noexcept;
// Successful unbinding waits for callbacks already in flight. Calling it from
// a sink callback is rejected to avoid self-deadlock.
[[nodiscard]] bool UnbindNativeStopSink(INativeStopSink& sink) noexcept;
[[nodiscard]] INativeStopSink* BoundNativeStopSink() noexcept;

// Native detours use these entry points to publish one typed event to the
// currently bound sink. They remain public so the dependency-neutral adapter
// can be characterized without installing process-wide hooks in a unit test.
[[nodiscard]] NativeStopDecision DispatchBoundNativePcStop(
    const NativePcStop& stop) noexcept;
[[nodiscard]] NativeStopDecision DispatchBoundNativeMemoryStop(
    const NativeMemoryStop& stop) noexcept;

} // namespace savor::probe
