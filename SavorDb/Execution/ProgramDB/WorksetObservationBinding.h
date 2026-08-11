#pragma once

#include "ProgramKindDescriptor.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace savor::db::execution::programdb {

inline constexpr std::string_view
    kProgressDisableDefaultLibrariesArgument =
        "observation.progress.disable_default_libraries";
inline constexpr std::string_view kProgressDisablePointsArgument =
    "observation.progress.disable_points";
inline constexpr std::string_view kProgressAddLibrariesArgument =
    "observation.progress.add_libraries";
inline constexpr std::string_view kCaptureProfileJsonArgument =
    "observation.capture.profile_json";
inline constexpr std::string_view kCaptureProfileSidecarArgument =
    "observation.capture.profile_sidecar_path";
inline constexpr std::string_view kCaptureProfileSha256Argument =
    "observation.capture.profile_sha256";

struct WorksetObservationDefaultsV1
{
    std::vector<std::string> progress_library_ids;
    std::vector<std::uint32_t> runtime_sample_trigger_pcs;

    auto operator<=>(const WorksetObservationDefaultsV1&) const = default;
};

struct ResolvedWorksetObservationBindingV1
{
    std::optional<savor::runtime::WorksetCaptureBindingV1> capture;
    savor::runtime::progress::ProgressPlanV1 progress_plan;
    std::vector<std::uint8_t> encoded_capture_binding;
    std::vector<std::uint8_t> encoded_progress_plan;
    std::string capture_binding_sha256;
    std::string progress_plan_sha256;
};

[[nodiscard]] bool ResolveWorksetObservationBindingV1(
    const ProgramJobMaterializationContext& context,
    const WorksetObservationDefaultsV1& defaults,
    const std::filesystem::path& capture_output_directory,
    ResolvedWorksetObservationBindingV1* output,
    std::string* error_out = nullptr);

} // namespace savor::db::execution::programdb
