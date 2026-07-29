#pragma once

#include "Runner/Runtime/RuntimeTypes.h"
#include "Runner/Runtime/Worksets/WorksetTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
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
        runtime::CapabilityMask(runtime::WorkerCapability::WorksetDispatch) };
    bool require_complete_exact_catalog{ true };
    std::string expected_catalog_sha256;
    std::string expected_runtime_profile_sha256;
    std::string expected_dependency_manifest_sha256;
};

struct WorkerCapabilityPreflightResult {
    WorkerCapabilityPreflightStatus status{
        WorkerCapabilityPreflightStatus::RuntimeUnavailable };
    runtime::WorkerCapabilityMask advertised_capabilities{ 0 };
    runtime::WorkerCapabilityMask missing_capabilities{ 0 };
    std::optional<runtime::WorkerRuntimeManifest> runtime_manifest;
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
