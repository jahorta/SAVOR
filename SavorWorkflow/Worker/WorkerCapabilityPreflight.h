#pragma once

#include "Runner/Runtime/RuntimeTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace savor {

enum class WorkerCapabilityPreflightStatus : std::uint8_t {
    Available,
    RuntimeUnavailable,
    LaunchFailed,
    ProtocolFailure,
};

struct WorkerCapabilityPreflightRequest {
    std::string worker_exe_path;
    std::string log_directory;
    std::size_t worker_id{ 0 };
    std::uint32_t timeout_ms{ 10000 };
    runtime::WorkerCapabilityMask required_capabilities{
        runtime::CapabilityMask(runtime::WorkerCapability::ProgramInvocation) };
};

struct WorkerCapabilityPreflightResult {
    WorkerCapabilityPreflightStatus status{
        WorkerCapabilityPreflightStatus::RuntimeUnavailable };
    runtime::WorkerCapabilityMask advertised_capabilities{ 0 };
    runtime::WorkerCapabilityMask missing_capabilities{ 0 };
    bool non_retryable{ false };
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return status == WorkerCapabilityPreflightStatus::Available;
    }
};

[[nodiscard]] WorkerCapabilityPreflightResult RunWorkerCapabilityPreflight(
    const WorkerCapabilityPreflightRequest& request);

[[nodiscard]] std::string DescribeWorkerCapabilityMask(
    runtime::WorkerCapabilityMask capabilities);

} // namespace savor
