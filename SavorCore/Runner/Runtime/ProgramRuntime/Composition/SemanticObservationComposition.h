#pragma once

#include "CompositionSupport.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace savor::runtime::program::composition {

struct SemanticPointReference
{
    CapabilityPackIdentity capability_pack;
    std::string canonical_id;
    SemanticPointKind kind = SemanticPointKind::ProgramCounter;
    std::uint32_t physical_pc = 0;

    auto operator<=>(const SemanticPointReference&) const = default;
};

enum class CurrentPointPolicy : std::uint8_t
{
    AcceptCurrent,
    FutureOnly,
};

enum class SemanticMoviePolicy : std::uint8_t
{
    Ignore,
    FailIfEnded,
};

struct HitTimeSampleRequirement
{
    std::string canonical_id;
    TypeRef result_type;
    std::uint32_t maximum_bytes = 0;
    bool required = true;
    // Pure, verifier-known projection from the canonical routed stop receipt
    // to this typed sample. Sampling itself already occurred in the router.
    ExactDependencyIdentity projection_reducer;

    auto operator<=>(const HitTimeSampleRequirement&) const = default;
};

struct SemanticAwaitDefinition
{
    std::vector<SemanticPointReference> alternatives;
    std::vector<HitTimeSampleRequirement> hit_time_samples;
    CurrentPointPolicy current_point = CurrentPointPolicy::AcceptCurrent;
    SemanticMoviePolicy movie_policy = SemanticMoviePolicy::FailIfEnded;
    ExactDependencyIdentity continue_until_action;
    TypeRef receipt_type;

    auto operator<=>(const SemanticAwaitDefinition&) const = default;
};

enum class AddressExpressionKind : std::uint8_t
{
    RegisteredSymbol,
    CompatibilityPinnedAddress,
    CheckedOffset,
    CheckedDereference,
    ReceiptField,
};

struct AddressExpression
{
    std::string canonical_id;
    AddressExpressionKind kind = AddressExpressionKind::RegisteredSymbol;
    TypeRef result_type = TypeRef::Builtin(BuiltinType::U64);
    std::string symbol_or_field;
    std::optional<std::string> base_expression;
    std::uint64_t absolute_address = 0;
    std::int64_t checked_offset = 0;
    std::uint8_t maximum_dereference_depth = 0;
    std::optional<ExactDependencyIdentity> dereference_action;

    auto operator<=>(const AddressExpression&) const = default;
};

enum class ObservationAcquisitionMode : std::uint8_t
{
    HitTimeSample,
    PausedAtPoint,
};

struct ObservationDefinition
{
    std::string canonical_id;
    TypeRef result_type;
    std::optional<std::string> address_expression;
    std::vector<ObservationAcquisitionMode> permitted_modes;
    std::optional<std::string> hit_time_sample;
    std::optional<ExactDependencyIdentity> paused_action;
    std::optional<CapabilityPackIdentity> paused_action_pack;
    enum class RequestValueSource : std::uint8_t
    {
        StopReceipt,
        StopReceiptField,
        Address,
    };
    struct RequestFieldBinding
    {
        std::string field_name;
        TypeRef field_type;
        RequestValueSource source =
            RequestValueSource::StopReceipt;
        std::string source_field;

        auto operator<=>(const RequestFieldBinding&) const = default;
    };
    // Non-canonical capability-pack queries carry their exact request record
    // and an ordered projection contract. Canonical guest actions use their
    // catalog request records and leave these members empty.
    std::optional<TypeRef> paused_request_type;
    std::vector<RequestFieldBinding> paused_request_fields;
    bool coherent_query = false;

    auto operator<=>(const ObservationDefinition&) const = default;
};

enum class ObservationRequirement : std::uint8_t
{
    Required,
    Optional,
};

enum class BaselineUpdatePolicy : std::uint8_t
{
    None,
    First,
    Latest,
};

enum class ObservationPublicationPolicy : std::uint8_t
{
    None,
    AuthoritativeEmission,
    Telemetry,
};

enum class ObservationAdvanceKind : std::uint8_t
{
    None = 0,
    StepFrame = 2,
};

// Canonical, policy-free semantic point set used by every Full Phase module.
// Routing ownership is selected by the consuming operation rather than being
// encoded into the point set.
[[nodiscard]] std::vector<Byte> EncodeSemanticPointSetV1(
    std::span<const SemanticPointReference> alternatives,
    std::span<const HitTimeSampleRequirement> hit_time_samples = {});

struct ResolvedSemanticPointSetV1
{
    std::vector<std::uint32_t> program_counters;
    std::vector<std::uint32_t> hit_time_sample_descriptor_ids;

    auto operator<=>(const ResolvedSemanticPointSetV1&) const = default;
};

struct SemanticPointSetDecodeResultV1
{
    std::optional<ResolvedSemanticPointSetV1> value;
    std::string diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return value.has_value();
    }
};

// Canonical SPS1 parser/linker used both by admission verification and by the
// session action host. It resolves only the homogeneous static capability
// catalog; no workset-provided implementation participates.
[[nodiscard]] SemanticPointSetDecodeResultV1 DecodeSemanticPointSetV1(
    std::span<const Byte> bytes);

struct ObservationUse
{
    std::string canonical_id;
    std::string definition_id;
    ObservationAcquisitionMode mode =
        ObservationAcquisitionMode::PausedAtPoint;
    ObservationRequirement requirement = ObservationRequirement::Required;
    std::string baseline_name;
    BaselineUpdatePolicy baseline = BaselineUpdatePolicy::None;
    ObservationPublicationPolicy publication =
        ObservationPublicationPolicy::None;
    ObservationAdvanceKind advance_before_observation =
        ObservationAdvanceKind::None;
    std::optional<ExactDependencyIdentity> advance_action;
    std::uint64_t advance_count = 0;

    auto operator<=>(const ObservationUse&) const = default;
};

struct SemanticObservationComposition
{
    std::string canonical_id;
    std::uint32_t revision = 0;
    std::string source_name;
    SemanticAwaitDefinition await;
    std::vector<AddressExpression> address_expressions;
    std::vector<ObservationDefinition> observations;
    std::vector<ObservationUse> ordered_uses;
    TypeRef output_type;
};

[[nodiscard]] CompositionResult LowerSemanticObservation(
    const SemanticObservationComposition& definition,
    ProgramModule& module);

} // namespace savor::runtime::program::composition
