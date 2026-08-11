#include "WorksetObservationBinding.h"

#include "../../../SavorCore/Runner/Runtime/Worksets/WorksetWireCodec.h"
#include "../../../SavorCore/Utils/Hash.h"
#include "../../../SavorProbe/ProbeProfile.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <ranges>
#include <set>
#include <span>
#include <string_view>

namespace savor::db::execution::programdb {
namespace {

constexpr std::size_t kMaximumCaptureProfileBytes = 4u * 1024u * 1024u;

bool Fail(std::string message, std::string* error_out)
{
    if (error_out != nullptr)
        *error_out = std::move(message);
    return false;
}

std::optional<std::string> ArgumentText(
    const ProgramJobMaterializationContext& context,
    std::string_view key)
{
    if (!context.graph)
        return std::nullopt;
    const auto found = std::ranges::find(
        context.graph->arguments,
        key,
        &WorkflowGraphArgument::argument_key);
    if (found == context.graph->arguments.end() ||
        !found->text_value)
    {
        return std::nullopt;
    }
    return *found->text_value;
}

std::string Trim(std::string_view value)
{
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front())))
    {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back())))
    {
        value.remove_suffix(1);
    }
    return std::string(value);
}

bool SplitUnique(
    std::string_view encoded,
    std::vector<std::string>& output,
    std::string* error_out)
{
    output.clear();
    std::set<std::string> unique;
    std::size_t offset = 0;
    while (offset <= encoded.size())
    {
        const std::size_t end = encoded.find(';', offset);
        const std::string value = Trim(encoded.substr(
            offset,
            end == std::string_view::npos
                ? encoded.size() - offset
                : end - offset));
        if (!value.empty() && !unique.emplace(value).second)
        {
            return Fail(
                "Workset observation argument contains a duplicate value",
                error_out);
        }
        if (!value.empty())
            output.push_back(value);
        if (end == std::string_view::npos)
            break;
        offset = end + 1;
    }
    return true;
}

bool ReadBoundedProfile(
    const std::filesystem::path& path,
    std::string& output,
    std::string* error_out)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > kMaximumCaptureProfileBytes)
    {
        return Fail(
            "Capture profile sidecar is unavailable or exceeds 4 MiB",
            error_out);
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return Fail("Capture profile sidecar could not be opened", error_out);
    output.assign(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
    if (!stream.eof() || output.size() != size)
        return Fail("Capture profile sidecar could not be read exactly", error_out);
    return true;
}

} // namespace

bool ResolveWorksetObservationBindingV1(
    const ProgramJobMaterializationContext& context,
    const WorksetObservationDefaultsV1& defaults,
    const std::filesystem::path& capture_output_directory,
    ResolvedWorksetObservationBindingV1* output,
    std::string* error_out)
{
    if (output == nullptr || capture_output_directory.empty())
        return Fail("Workset observation output is incomplete", error_out);

    savor::runtime::progress::ProgressPlanSelectionV1 selection;
    if (const auto value = ArgumentText(
            context,
            kProgressDisableDefaultLibrariesArgument))
    {
        if (!SplitUnique(
                *value,
                selection.disabled_default_library_ids,
                error_out))
        {
            return false;
        }
    }
    if (const auto value = ArgumentText(
            context,
            kProgressAddLibrariesArgument))
    {
        if (!SplitUnique(
                *value,
                selection.added_library_ids,
                error_out))
        {
            return false;
        }
    }
    if (const auto value = ArgumentText(
            context,
            kProgressDisablePointsArgument))
    {
        std::vector<std::string> points;
        if (!SplitUnique(*value, points, error_out))
            return false;
        for (const std::string& encoded : points)
        {
            const std::size_t delimiter = encoded.find('#');
            if (delimiter == std::string::npos || delimiter == 0 ||
                delimiter + 1 >= encoded.size() ||
                encoded.find('#', delimiter + 1) != std::string::npos)
            {
                return Fail(
                    "Disabled progress points must use library_id#point_id",
                    error_out);
            }
            selection.disabled_points.push_back({
                .library_id = encoded.substr(0, delimiter),
                .point_id = encoded.substr(delimiter + 1),
            });
        }
    }

    const auto resolved =
        savor::runtime::progress::ResolveProgressPlanSelectionV1(
            defaults.progress_library_ids,
            defaults.runtime_sample_trigger_pcs,
            selection);
    if (!resolved)
        return Fail(resolved.message, error_out);

    ResolvedWorksetObservationBindingV1 candidate;
    candidate.progress_plan = *resolved.plan;
    const auto inline_profile = ArgumentText(
        context,
        kCaptureProfileJsonArgument);
    const auto sidecar = ArgumentText(
        context,
        kCaptureProfileSidecarArgument);
    if (inline_profile && sidecar)
    {
        return Fail(
            "Workset capture profile must be inline or sidecar, not both",
            error_out);
    }
    if (inline_profile || sidecar)
    {
        std::string profile_json;
        savor::runtime::CaptureProfileStorageV1 storage =
            savor::runtime::CaptureProfileStorageV1::Inline;
        std::filesystem::path sidecar_path;
        if (inline_profile)
        {
            profile_json = *inline_profile;
            if (profile_json.size() > kMaximumCaptureProfileBytes)
                return Fail("Inline capture profile exceeds 4 MiB", error_out);
        }
        else
        {
            storage = savor::runtime::CaptureProfileStorageV1::
                ContentAddressedSidecar;
            sidecar_path = *sidecar;
            if (!ReadBoundedProfile(sidecar_path, profile_json, error_out))
                return false;
        }
        const std::string profile_sha256 = hash::sha256(
            profile_json.data(),
            profile_json.size());
        if (const auto expected = ArgumentText(
                context,
                kCaptureProfileSha256Argument);
            expected && *expected != profile_sha256)
        {
            return Fail(
                "Capture profile hash argument does not match its content",
                error_out);
        }
        savor::probe::ProfileParseResult parsed =
            savor::probe::parse_profile_json(profile_json);
        if (!parsed.profile)
        {
            return Fail(
                savor::probe::format_profile_errors(parsed),
                error_out);
        }
        savor::runtime::WorksetCaptureBindingV1 binding{
            .version = 1,
            .storage = storage,
            .profile_json = inline_profile ? profile_json : std::string{},
            .profile_sidecar_path = std::move(sidecar_path),
            .profile_sha256 = profile_sha256,
            .expected_module_sha256 =
                parsed.profile->expected_module_sha256,
            .resolved_observation_sha256 =
                savor::runtime::progress::
                    ComputeResolvedObservationHashV1(
                        *parsed.profile,
                        candidate.progress_plan),
            .output_directory = capture_output_directory,
        };
        binding.content_sha256 =
            savor::runtime::ComputeWorksetCaptureBindingHashV1(binding);
        candidate.capture = std::move(binding);
    }

    auto encoded = savor::runtime::EncodeWorksetCaptureBindingV1(
        candidate.capture,
        candidate.encoded_capture_binding);
    if (!encoded)
        return Fail(encoded.message, error_out);
    encoded = savor::runtime::EncodeProgressPlanV1(
        candidate.progress_plan,
        candidate.encoded_progress_plan);
    if (!encoded)
        return Fail(encoded.message, error_out);
    candidate.capture_binding_sha256 = candidate.capture
        ? candidate.capture->content_sha256
        : savor::runtime::EmptyWorksetCaptureBindingHashV1();
    candidate.progress_plan_sha256 =
        candidate.progress_plan.content_sha256;
    *output = std::move(candidate);
    if (error_out != nullptr)
        error_out->clear();
    return true;
}

} // namespace savor::db::execution::programdb
