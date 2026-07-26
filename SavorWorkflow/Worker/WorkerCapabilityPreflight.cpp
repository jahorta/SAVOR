#include "WorkerCapabilityPreflight.h"

#include "ProcessWorker.h"

#include <array>
#include <sstream>
#include <utility>

namespace savor {
namespace {

struct CapabilityName {
    runtime::WorkerCapability capability;
    const char* name;
};

constexpr std::array<CapabilityName, 7> kCapabilityNames{{
    {runtime::WorkerCapability::SessionLifecycle, "SessionLifecycle"},
    {runtime::WorkerCapability::Screenshot, "Screenshot"},
    {runtime::WorkerCapability::HostEvents, "HostEvents"},
    {runtime::WorkerCapability::CancellationProtocol, "CancellationProtocol"},
    {runtime::WorkerCapability::Shutdown, "Shutdown"},
    {runtime::WorkerCapability::ProgramInvocation, "ProgramInvocation"},
    {runtime::WorkerCapability::InteractiveVisualDebug, "InteractiveVisualDebug"},
}};

} // namespace

std::string DescribeWorkerCapabilityMask(
    runtime::WorkerCapabilityMask capabilities)
{
    std::ostringstream description;
    bool first = true;
    for (const auto& item : kCapabilityNames)
    {
        if (!runtime::HasCapability(capabilities, item.capability))
            continue;
        if (!first)
            description << ",";
        description << item.name;
        first = false;
    }
    if (first)
        return "None";
    return description.str();
}

WorkerCapabilityPreflightResult RunWorkerCapabilityPreflight(
    const WorkerCapabilityPreflightRequest& request)
{
    if (request.worker_exe_path.empty())
    {
        return {
            .status = WorkerCapabilityPreflightStatus::LaunchFailed,
            .missing_capabilities = request.required_capabilities,
            .message = "worker capability preflight requires an executable path",
        };
    }

    ProcessWorker worker;
    std::string launch_error;
    if (!worker.launch_and_negotiate(
            ProcessLaunchOptions{
                .worker_id = request.worker_id,
                .exe_path = request.worker_exe_path,
                .log_directory = request.log_directory,
                .hello_timeout_ms = request.timeout_ms,
            },
            &launch_error))
    {
        return {
            .status = WorkerCapabilityPreflightStatus::LaunchFailed,
            .missing_capabilities = request.required_capabilities,
            .message = launch_error.empty()
                ? "failed launching or negotiating the WRMS worker"
                : std::move(launch_error),
        };
    }

    const auto advertised = worker.process_capabilities();
    const auto missing = request.required_capabilities & ~advertised;
    worker.stop();

    if (missing != 0)
    {
        std::ostringstream message;
        message
            << "RuntimeUnavailable: worker negotiated WRMS v1 but is missing "
            << DescribeWorkerCapabilityMask(missing)
            << " (advertised "
            << DescribeWorkerCapabilityMask(advertised)
            << "); canonical ProgramRuntime is not implemented in Dependency Slice 1";
        return {
            .status = WorkerCapabilityPreflightStatus::RuntimeUnavailable,
            .advertised_capabilities = advertised,
            .missing_capabilities = missing,
            .non_retryable = true,
            .message = message.str(),
        };
    }

    return {
        .status = WorkerCapabilityPreflightStatus::Available,
        .advertised_capabilities = advertised,
        .message = "worker capability preflight passed",
    };
}

} // namespace savor
