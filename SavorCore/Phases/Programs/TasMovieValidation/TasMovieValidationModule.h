#pragma once

#include "../../../Runner/Runtime/FullPhase/FullPhaseProgram.h"
#include "../../../Runner/Runtime/ProgramRuntime/Model/ProgramModel.h"

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace savor::runtime::tasmovie {

inline constexpr std::string_view ModuleCanonicalId =
    "soa.tas_movie_validation";
inline constexpr std::uint32_t ModuleRevision = 1;
inline constexpr std::int32_t ProgramVersion = 1;
inline constexpr std::string_view Entrypoint = "validate";
inline constexpr std::string_view ArtifactLineage =
    "soa.tas_movie_validation/read-only-movie/v1";
inline constexpr std::string_view FullPhaseCanonicalId =
    "savor.full_phase.tas_movie_validation";

inline constexpr std::string_view BeforeRandSeedSetPointId =
    "soa.field.point.prebattle.BeforeRandSeedSet";
inline constexpr std::uint32_t BeforeRandSeedSetPc = 0x80101E48u;
inline constexpr std::size_t MaximumItineraryEntries = 4096;
inline constexpr std::size_t MaximumPathBytes = 4096;
inline constexpr std::uint64_t GameCubeDtmInputRecordBytes = 8;

struct DtmInputCount
{
    std::uint64_t value = 0;

    // Returns the byte boundary within the DTM input payload. N identifies
    // the boundary after N complete fixed-size GameCube records and before
    // record index N.
    [[nodiscard]] std::optional<std::uint64_t>
    PayloadByteOffset() const noexcept;

    auto operator<=>(const DtmInputCount&) const = default;
};

enum class TasMovieValidationOperationV1 : std::int64_t
{
    EstablishRootCursor = 0,
    Validate = 1,
};

struct TasMovieCheckpointV1
{
    std::uint32_t pc = 0;
    DtmInputCount input_count;

    auto operator<=>(const TasMovieCheckpointV1&) const = default;
};

struct TasMovieItineraryV1
{
    std::vector<TasMovieCheckpointV1> checkpoints;

    auto operator<=>(const TasMovieItineraryV1&) const = default;
};

enum class TasMovieValidationOutcomeV1 : std::int64_t
{
    RootCursorEstablished = 0,
    Valid = 1,
    Invalid = 2,
};

enum class TasMovieValidationFailureReasonV1 : std::int64_t
{
    MovieDesynchronized = 0,
    ExpectedTerminalNotReached = 1,
    Unknown = 2,
};

struct TasMovieValidationFailureDiagnosticsV1
{
    std::uint32_t expected_pc = 0;
    std::optional<DtmInputCount> expected_input_count;
    std::uint32_t actual_pc = 0;
    DtmInputCount actual_input_count;
    std::optional<std::uint64_t> last_verified_itinerary_index;

    auto operator<=>(
        const TasMovieValidationFailureDiagnosticsV1&) const = default;
};

struct TasMovieValidationFailureV1
{
    TasMovieValidationFailureReasonV1 reason =
        TasMovieValidationFailureReasonV1::Unknown;
    TasMovieValidationFailureDiagnosticsV1 diagnostics;

    auto operator<=>(const TasMovieValidationFailureV1&) const = default;
};

struct TasMovieValidationResultV1
{
    TasMovieValidationOutcomeV1 outcome =
        TasMovieValidationOutcomeV1::Invalid;
    std::optional<TasMovieCheckpointV1> candidate_checkpoint;
    std::optional<TasMovieValidationFailureV1> failure;

    auto operator<=>(const TasMovieValidationResultV1&) const = default;
};

// Native workset-facing request. BuildResolvedExecution materializes the DTM,
// performs strict preflight, then lowers the two paths to the existing exact
// nominal canonical action request values consumed by the immutable module.
struct TasMovieValidationRequestV1
{
    TasMovieValidationOperationV1 operation =
        TasMovieValidationOperationV1::EstablishRootCursor;
    std::string dtm_path;
    std::optional<std::string> startup_savestate_path;
    TasMovieItineraryV1 itinerary;
    std::optional<std::string> final_checkpoint_path;

    auto operator<=>(const TasMovieValidationRequestV1&) const = default;
};

struct TasMovieBoundaryCatalogEntryV1
{
    std::string_view stable_point_id;
    std::uint32_t pc = 0;

    auto operator<=>(const TasMovieBoundaryCatalogEntryV1&) const = default;
};

class ITasMovieValidationFullPhaseDefinitionV1
    : public fullphase::IFullPhaseProgramDefinition
{
public:
    [[nodiscard]] fullphase::FullPhaseWorksetPolicy workset_policy()
        const noexcept final
    {
        return {.minimum_item_count = 1, .maximum_item_count = 1};
    }

    [[nodiscard]] virtual bool DecodeProgramResult(
        std::span<const program::Byte> encoded_result,
        TasMovieValidationResultV1& result,
        std::string* diagnostic = nullptr) const = 0;
};

[[nodiscard]] std::span<const TasMovieBoundaryCatalogEntryV1>
TasMovieBoundaryCatalogV1() noexcept;
[[nodiscard]] bool TasMovieBoundaryCatalogContainsPcV1(
    std::uint32_t pc) noexcept;

[[nodiscard]] program::SchemaIdentity
DtmInputCountSchemaIdentityV1();
[[nodiscard]] program::SchemaIdentity
TasMovieValidationOperationSchemaIdentityV1();
[[nodiscard]] program::SchemaIdentity
TasMovieCheckpointSchemaIdentityV1();
[[nodiscard]] program::SchemaIdentity
TasMovieItinerarySchemaIdentityV1();
[[nodiscard]] program::SchemaIdentity
TasMovieValidationOutcomeSchemaIdentityV1();
[[nodiscard]] program::SchemaIdentity
TasMovieValidationFailureReasonSchemaIdentityV1();
[[nodiscard]] program::SchemaIdentity
TasMovieValidationFailureDiagnosticsSchemaIdentityV1();
[[nodiscard]] program::SchemaIdentity
TasMovieValidationFailureSchemaIdentityV1();
[[nodiscard]] program::SchemaIdentity
TasMovieValidationResultSchemaIdentityV1();

[[nodiscard]] bool ValidateTasMovieValidationResultV1(
    const TasMovieValidationResultV1& result,
    std::string* diagnostic = nullptr);
[[nodiscard]] program::ProgramValueGraph
EncodeTasMovieValidationResultV1(
    const TasMovieValidationResultV1& result);
[[nodiscard]] bool DecodeTasMovieValidationResultV1(
    const program::ProgramValueGraph& graph,
    TasMovieValidationResultV1& result,
    std::string* diagnostic = nullptr);

// Strict little-endian TMV1 scalar workset binding.
[[nodiscard]] std::vector<std::uint8_t>
EncodeTasMovieValidationExecutionInputV1(
    const TasMovieValidationRequestV1& request,
    std::string* diagnostic = nullptr);
[[nodiscard]] bool DecodeTasMovieValidationExecutionInputV1(
    std::span<const std::uint8_t> payload,
    TasMovieValidationRequestV1& request,
    std::string* diagnostic = nullptr);

// Canonical durable itinerary sidecar. TMI1 is intentionally independent of
// the scalar TMV1 invocation binding: it stores only the immutable ordered
// checkpoint facts shared by every validation of the same complete DTM.
[[nodiscard]] std::vector<std::uint8_t> EncodeTasMovieItineraryArtifactV1(
    const TasMovieItineraryV1& itinerary,
    std::string* diagnostic = nullptr);
[[nodiscard]] bool DecodeTasMovieItineraryArtifactV1(
    std::span<const std::uint8_t> payload,
    TasMovieItineraryV1& itinerary,
    std::string* diagnostic = nullptr);
[[nodiscard]] bool ValidateTasMovieItineraryArtifactV1(
    const TasMovieItineraryV1& itinerary,
    std::uint64_t total_dtm_input_count,
    std::uint32_t required_final_pc,
    std::string* diagnostic = nullptr);

[[nodiscard]] std::shared_ptr<
    const ITasMovieValidationFullPhaseDefinitionV1>
TasMovieValidationFullPhaseDefinitionV1();

inline constexpr std::string_view SterilizationModuleCanonicalId =
    "soa.tas_movie_checkpoint_sterilize";
inline constexpr std::uint32_t SterilizationModuleRevision = 1;
inline constexpr std::string_view SterilizationEntrypoint = "sterilize";
inline constexpr std::string_view SterilizationArtifactLineage =
    "soa.tas_movie_checkpoint_sterilize/movie-paired-savestate/v1";
inline constexpr std::string_view SterilizationDerivationMethod =
    "tasmovie.checkpoint_sterilize.v1";
inline constexpr std::string_view SterilizationFullPhaseCanonicalId =
    "savor.full_phase.tas_movie_checkpoint_sterilize";

enum class TasMovieCheckpointSterilizationOutcomeV1 : std::int64_t
{
    Sterilized = 0,
};

struct TasMovieCheckpointSterilizationRequestV1
{
    std::string source_savestate_path;
    std::string source_dtm_path;
    std::string output_savestate_path;

    auto operator<=>(
        const TasMovieCheckpointSterilizationRequestV1&) const = default;
};

struct TasMovieCheckpointSterilizationResultV1
{
    TasMovieCheckpointSterilizationOutcomeV1 outcome =
        TasMovieCheckpointSterilizationOutcomeV1::Sterilized;

    auto operator<=>(
        const TasMovieCheckpointSterilizationResultV1&) const = default;
};

class ITasMovieCheckpointSterilizationFullPhaseDefinitionV1
    : public fullphase::IFullPhaseProgramDefinition
{
public:
    [[nodiscard]] fullphase::FullPhaseWorksetPolicy workset_policy()
        const noexcept final
    {
        return {.minimum_item_count = 1, .maximum_item_count = 1};
    }

    [[nodiscard]] virtual bool DecodeProgramResult(
        std::span<const program::Byte> encoded_result,
        TasMovieCheckpointSterilizationResultV1& result,
        std::string* diagnostic = nullptr) const = 0;
};

[[nodiscard]] std::vector<std::uint8_t>
EncodeTasMovieCheckpointSterilizationExecutionInputV1(
    const TasMovieCheckpointSterilizationRequestV1& request,
    std::string* diagnostic = nullptr);
[[nodiscard]] bool
DecodeTasMovieCheckpointSterilizationExecutionInputV1(
    std::span<const std::uint8_t> payload,
    TasMovieCheckpointSterilizationRequestV1& request,
    std::string* diagnostic = nullptr);

[[nodiscard]] std::shared_ptr<const
    ITasMovieCheckpointSterilizationFullPhaseDefinitionV1>
TasMovieCheckpointSterilizationFullPhaseDefinitionV1();

} // namespace savor::runtime::tasmovie
