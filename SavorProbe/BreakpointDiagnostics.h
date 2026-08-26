#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace savor::probe {

enum class BreakpointDiagnosticCause : std::uint8_t
{
    Unknown = 0,
    StopRouter = 1,
};

class BreakpointDiagnosticCauseScope
{
public:
    explicit BreakpointDiagnosticCauseScope(
        BreakpointDiagnosticCause cause,
        std::uint64_t correlation = 0) noexcept;
    ~BreakpointDiagnosticCauseScope();

    BreakpointDiagnosticCauseScope(
        const BreakpointDiagnosticCauseScope&) = delete;
    BreakpointDiagnosticCauseScope& operator=(
        const BreakpointDiagnosticCauseScope&) = delete;

private:
    BreakpointDiagnosticCause previous_cause_ =
        BreakpointDiagnosticCause::Unknown;
    std::uint64_t previous_correlation_ = 0;
};

void ConfigureBreakpointDiagnostics(bool enabled) noexcept;
[[nodiscard]] bool BreakpointDiagnosticsEnabled() noexcept;

// NativeStopHooks owns the process-wide MinHook runtime. These functions are
// called only while that runtime is active.
[[nodiscard]] bool InstallBreakpointDiagnostics(
    std::string* error_out = nullptr);
void UninstallBreakpointDiagnostics() noexcept;
[[nodiscard]] bool BreakpointDiagnosticsInstalled() noexcept;

// Formats a stable snapshot off the Dolphin CPU thread. Raw addresses remain
// in the output when process symbols are unavailable.
[[nodiscard]] std::string DescribeRecentBreakpointDiagnostics(
    std::size_t maximum_events = 32);

} // namespace savor::probe
