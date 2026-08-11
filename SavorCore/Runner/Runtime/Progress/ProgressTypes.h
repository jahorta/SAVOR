#pragma once

#include "../RuntimeTypes.h"
#include "../StopPoints/StopPointTypes.h"
#include "../../../../SavorCaptureFormat/CaptureFormat.h"
#include "../../../../SavorProbe/ProbeProfile.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace savor::runtime::progress {

enum class ProgressProviderKind : std::uint8_t
{
    BreakpointCapture = 1,
    RuntimeSample = 2,
    PhaseLibrary = 3,
};

struct ProgressSchemaIdentity
{
    std::string canonical_id;
    std::uint32_t revision = 0;
    std::string sha256;

    [[nodiscard]] explicit operator bool() const noexcept;
    auto operator<=>(const ProgressSchemaIdentity&) const = default;
};

struct ProgressFormatterIdentity
{
    std::string canonical_id;
    std::uint32_t revision = 0;
    std::string sha256;

    [[nodiscard]] explicit operator bool() const noexcept;
    auto operator<=>(const ProgressFormatterIdentity&) const = default;
};

struct ProgressPointDescriptor
{
    std::string canonical_id;
    ProgressProviderKind provider = ProgressProviderKind::RuntimeSample;
    std::optional<std::uint32_t> breakpoint_pc;
    ProgressSchemaIdentity schema;
    ProgressFormatterIdentity formatter;
    std::vector<std::string> required_capture_fields;
    std::string display_name;

    auto operator<=>(const ProgressPointDescriptor&) const = default;
};

struct ProgressLibraryDescriptor
{
    std::string canonical_id;
    std::uint32_t revision = 0;
    std::string provider_identity;
    std::vector<ProgressPointDescriptor> points;
    std::string canonical_sha256;

    [[nodiscard]] explicit operator bool() const noexcept;
    auto operator<=>(const ProgressLibraryDescriptor&) const = default;
};

struct ProgressPointBindingV1
{
    std::string library_id;
    std::uint32_t library_revision = 0;
    std::string library_sha256;
    std::string point_id;
    ProgressProviderKind provider = ProgressProviderKind::RuntimeSample;
    std::optional<std::uint32_t> breakpoint_pc;
    ProgressSchemaIdentity schema;
    ProgressFormatterIdentity formatter;
    // Exact registered semantic PCs at which a RuntimeSample provider is
    // requested. Empty for breakpoint-backed and phase-library providers.
    std::vector<std::uint32_t> runtime_sample_trigger_pcs;
    std::vector<std::uint8_t> configuration;

    auto operator<=>(const ProgressPointBindingV1&) const = default;
};

struct ProgressPlanV1
{
    ProgressPlanV1();

    std::uint32_t version = 1;
    std::vector<ProgressPointBindingV1> points;
    std::string content_sha256;

    [[nodiscard]] explicit operator bool() const noexcept;
    auto operator<=>(const ProgressPlanV1&) const = default;
};

struct ProgressPointSelectionV1
{
    std::string library_id;
    std::string point_id;

    auto operator<=>(const ProgressPointSelectionV1&) const = default;
};

// Planner-facing delta over one program kind's declared defaults. The
// resolved ProgressPlanV1, rather than this authoring request, is what is
// durably bound to and dispatched with a workset.
struct ProgressPlanSelectionV1
{
    std::vector<std::string> disabled_default_library_ids;
    std::vector<ProgressPointSelectionV1> disabled_points;
    std::vector<std::string> added_library_ids;

    auto operator<=>(const ProgressPlanSelectionV1&) const = default;
};

struct ProgressPlanResolutionV1
{
    std::optional<ProgressPlanV1> plan;
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return plan.has_value();
    }
};

struct CanonicalProgressEventV1
{
    WorkerWorksetId workset_id;
    WorkerWorksetItemId item_id;
    std::string durable_job_id;
    InvocationId invocation_id;
    AttemptId attempt_id;
    std::uint64_t ordinal = 0;
    std::string library_id;
    std::uint32_t library_revision = 0;
    std::string progress_point_id;
    std::optional<RoutedStopSequence> routed_sequence;
    std::optional<StopSampleSnapshotId> sample_snapshot_id;
    std::optional<WorksetEpoch> trigger_epoch;
    ProgressSchemaIdentity schema;
    std::vector<std::uint8_t> typed_payload;
    std::string display_text;

    auto operator<=>(const CanonicalProgressEventV1&) const = default;
};

struct ProgressValidationResult
{
    bool ok = false;
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return ok;
    }
};

class ProgressLibraryRegistry final
{
public:
    ProgressLibraryRegistry();

    [[nodiscard]] const std::vector<ProgressLibraryDescriptor>&
    libraries() const noexcept
    {
        return libraries_;
    }

    [[nodiscard]] const ProgressLibraryDescriptor* FindLibrary(
        std::string_view canonical_id,
        std::uint32_t revision) const noexcept;
    [[nodiscard]] const ProgressPointDescriptor* FindPoint(
        std::string_view library_id,
        std::uint32_t library_revision,
        std::string_view point_id) const noexcept;
    [[nodiscard]] const ProgressPointDescriptor* FindFormatter(
        const ProgressFormatterIdentity& formatter) const noexcept;
    [[nodiscard]] std::string canonical_sha256() const;

private:
    std::vector<ProgressLibraryDescriptor> libraries_;
};

[[nodiscard]] const ProgressLibraryRegistry& ProductionProgressRegistry();

[[nodiscard]] std::string ComputeProgressPlanHashV1(
    const ProgressPlanV1& plan);
[[nodiscard]] ProgressValidationResult ValidateProgressPlanV1(
    const ProgressPlanV1& plan,
    const ProgressLibraryRegistry& registry = ProductionProgressRegistry());
[[nodiscard]] ProgressPlanV1 ResolveProgressPlanV1(
    std::span<const std::string_view> library_ids,
    std::span<const std::uint32_t> runtime_sample_trigger_pcs = {},
    const ProgressLibraryRegistry& registry = ProductionProgressRegistry());
[[nodiscard]] ProgressPlanResolutionV1 ResolveProgressPlanSelectionV1(
    std::span<const std::string> default_library_ids,
    std::span<const std::uint32_t> runtime_sample_trigger_pcs,
    const ProgressPlanSelectionV1& selection = {},
    const ProgressLibraryRegistry& registry = ProductionProgressRegistry());
[[nodiscard]] ProgressValidationResult ValidateCaptureProgressFormatters(
    const savor::probe::Profile& profile,
    const ProgressPlanV1* plan = nullptr,
    const ProgressLibraryRegistry& registry = ProductionProgressRegistry());
[[nodiscard]] std::optional<savor::probe::ProbeDefinition>
BuildBreakpointProgressProbeV1(
    const ProgressPointBindingV1& binding,
    const ProgressLibraryRegistry& registry = ProductionProgressRegistry());
[[nodiscard]] std::string ComputeResolvedObservationHashV1(
    const savor::probe::Profile& profile,
    const ProgressPlanV1& plan);

[[nodiscard]] std::vector<std::uint8_t> EncodeCaptureEventPayloadV1(
    const savor::capture_format::Event& event);
[[nodiscard]] std::string FormatCaptureProgressText(
    const ProgressPointDescriptor& point,
    const savor::capture_format::Event& event);

[[nodiscard]] std::vector<std::uint8_t> EncodeViProgressPayloadV1(
    std::uint64_t vi_count);
[[nodiscard]] std::vector<std::uint8_t>
EncodeScriptLocationProgressPayloadV1(
    std::string_view file,
    std::string_view section,
    std::uint32_t pc);

} // namespace savor::runtime::progress
