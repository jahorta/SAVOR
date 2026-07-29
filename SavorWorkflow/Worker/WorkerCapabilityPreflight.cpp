#include "WorkerCapabilityPreflight.h"

#include "ProcessWorker.h"

#include <algorithm>
#include <array>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace savor {
namespace {

struct CapabilityName {
    runtime::WorkerCapability capability;
    const char* name;
};

constexpr std::array<CapabilityName, 8> kCapabilityNames{{
    {runtime::WorkerCapability::SessionLifecycle, "SessionLifecycle"},
    {runtime::WorkerCapability::Screenshot, "Screenshot"},
    {runtime::WorkerCapability::HostEvents, "HostEvents"},
    {runtime::WorkerCapability::CancellationProtocol, "CancellationProtocol"},
    {runtime::WorkerCapability::Shutdown, "Shutdown"},
    {runtime::WorkerCapability::ProgramInvocation, "ProgramInvocation"},
    {runtime::WorkerCapability::InteractiveVisualDebug, "InteractiveVisualDebug"},
    {runtime::WorkerCapability::WorksetDispatch, "WorksetDispatch"},
}};

bool CompleteSha256(std::string_view value)
{
    return value.size() == 64 &&
        std::all_of(
            value.begin(),
            value.end(),
            [](char character)
            {
                return (character >= '0' && character <= '9') ||
                    (character >= 'a' && character <= 'f');
            });
}

bool HasCompleteProductionCatalogShape(
    const runtime::WorkerRuntimeManifest& manifest,
    std::string* error_out)
{
    static constexpr std::array<
        std::pair<std::string_view, std::string_view>,
        9>
        kProductionModules{{
            {"soa.seed_probe", "probe"},
            {"soa.navigation.context", "capture"},
            {"soa.tas_movie", "play_and_checkpoint"},
            {"soa.tas_frame_detector", "detect"},
            {"soa.battle.context", "capture"},
            {"soa.battle.macro_probe", "probe"},
            {"soa.battle.single_turn", "execute"},
            {"soa.battle.completion", "complete"},
            {"soa.battle.results_screen", "advance"},
        }};

    if (manifest.catalog_status !=
            runtime::RuntimeCatalogStatus::CompleteExact ||
        manifest.modules.size() != kProductionModules.size() ||
        !CompleteSha256(manifest.catalog_sha256) ||
        !CompleteSha256(manifest.runtime_profile_sha256) ||
        !CompleteSha256(manifest.dependency_manifest_sha256))
    {
        if (error_out)
        {
            *error_out =
                "the worker catalog is not a complete exact nine-module "
                "production catalog";
        }
        return false;
    }

    std::unordered_set<std::string> seen;
    seen.reserve(kProductionModules.size());
    for (const runtime::RuntimeModuleManifestEntry& module :
         manifest.modules)
    {
        const auto expected = std::find_if(
            kProductionModules.begin(),
            kProductionModules.end(),
            [&](const auto& candidate)
            {
                return candidate.first == module.module.canonical_id;
            });
        if (module.development_only ||
            module.module.revision == 0 ||
            !CompleteSha256(module.module.canonical_hash) ||
            !CompleteSha256(module.dependency_manifest_sha256) ||
            module.dependency_manifest_sha256 !=
                manifest.dependency_manifest_sha256 ||
            expected == kProductionModules.end() ||
            !seen.emplace(module.module.canonical_id).second ||
            module.entrypoints.size() != 1 ||
            module.entrypoints.front() != expected->second)
        {
            if (error_out)
            {
                *error_out =
                    "the worker catalog contains an unexpected, incomplete, "
                    "development, or duplicate module";
            }
            return false;
        }
    }
    return true;
}

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
    const auto manifest = worker.runtime_manifest();
    worker.stop();

    if (missing != 0)
    {
        std::ostringstream message;
        message
            << "RuntimeUnavailable: worker negotiated WRMS v1 but is missing "
            << DescribeWorkerCapabilityMask(missing)
            << " (advertised "
            << DescribeWorkerCapabilityMask(advertised)
            << "); the required hard-cutover worker surface is unavailable";
        return {
            .status = WorkerCapabilityPreflightStatus::RuntimeUnavailable,
            .advertised_capabilities = advertised,
            .missing_capabilities = missing,
            .runtime_manifest = manifest,
            .non_retryable = true,
            .message = message.str(),
        };
    }

    if (request.require_complete_exact_catalog)
    {
        std::string catalog_error;
        const bool shape_matches =
            manifest.has_value() &&
            HasCompleteProductionCatalogShape(
                *manifest,
                &catalog_error);
        const bool catalog_hash_matches =
            request.expected_catalog_sha256.empty() ||
            (manifest &&
             manifest->catalog_sha256 ==
                 request.expected_catalog_sha256);
        const bool runtime_profile_matches =
            request.expected_runtime_profile_sha256.empty() ||
            (manifest &&
             manifest->runtime_profile_sha256 ==
                 request.expected_runtime_profile_sha256);
        const bool dependency_manifest_matches =
            request.expected_dependency_manifest_sha256.empty() ||
            (manifest &&
             manifest->dependency_manifest_sha256 ==
                 request.expected_dependency_manifest_sha256);
        if (!shape_matches || !catalog_hash_matches ||
            !runtime_profile_matches ||
            !dependency_manifest_matches)
        {
            std::ostringstream message;
            message
                << "RuntimeUnavailable: worker negotiated WorksetDispatch "
                   "but database execution requires the CompleteExact "
                   "nine-module production catalog";
            if (!catalog_error.empty())
                message << " (" << catalog_error << ")";
            if (!catalog_hash_matches)
                message << " (catalog hash mismatch)";
            if (!runtime_profile_matches)
                message << " (runtime profile mismatch)";
            if (!dependency_manifest_matches)
                message << " (dependency manifest mismatch)";
            return {
                .status =
                    WorkerCapabilityPreflightStatus::RuntimeUnavailable,
                .advertised_capabilities = advertised,
                .missing_capabilities = 0,
                .runtime_manifest = manifest,
                .non_retryable = true,
                .message = message.str(),
            };
        }
    }

    return {
        .status = WorkerCapabilityPreflightStatus::Available,
        .advertised_capabilities = advertised,
        .runtime_manifest = manifest,
        .message = "worker capability preflight passed",
    };
}

} // namespace savor
