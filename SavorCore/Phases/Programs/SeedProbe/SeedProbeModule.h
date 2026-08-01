#pragma once

#include "../../../Core/Input/InputPlan.h"
#include "../../RNGSeedDeltaMap.h"
#include "../../../Runner/Runtime/FullPhase/FullPhaseProgram.h"
#include "../../../Runner/Runtime/IProgramRuntimePort.h"
#include "../../../Runner/Runtime/ProgramRuntime/Model/ProgramModel.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace savor::runtime::seedprobe {

inline constexpr std::string_view ModuleCanonicalId = "soa.seed_probe";
inline constexpr std::uint32_t ModuleRevision = 2;
inline constexpr std::int32_t ProgramVersion = 2;
inline constexpr std::string_view Entrypoint = "probe";
inline constexpr std::string_view BaselineLineage =
    "soa.seed_probe/restore-baseline/v2";

inline constexpr std::uint32_t PreBattleAfterRandSeedSetPc =
    0x8000A1DCu;
inline constexpr std::uint32_t FieldReturnRandSeedCommittedPc =
    0x801012B4u;
inline constexpr std::uint32_t RngSeedAddress = 0x803469A8u;

enum class SeedProbeEndpointV2 : std::int64_t
{
    AfterRandSeedSet = 0,
    RandSeedCommitted = 1,
};

struct SeedProbeRequestV2
{
    savor::GCInputFrame frame{};

    bool operator==(const SeedProbeRequestV2&) const = default;
};

struct SemanticStopReceiptV2
{
    std::uint64_t stop_sequence = 0;
    StateEpoch state_epoch;
    std::uint32_t pc = 0;
    std::uint64_t sample_snapshot_id = 0;
    std::vector<program::Byte> evidence;

    bool operator==(const SemanticStopReceiptV2&) const = default;
};

struct InputPublicationReceiptV2
{
    std::uint64_t lease_id = 0;
    std::uint64_t publication_id = 0;
    StateEpoch state_epoch;
    savor::GCInputFrame frame{};

    bool operator==(const InputPublicationReceiptV2&) const = default;
};

struct InputPollReceiptV2
{
    bool acknowledged = false;
    std::uint64_t poll_receipt_id = 0;
    std::uint64_t publication_id = 0;
    StateEpoch state_epoch;

    bool operator==(const InputPollReceiptV2&) const = default;
};

struct SeedProbeResultV2
{
    std::uint32_t raw_seed = 0;
    SeedProbeEndpointV2 endpoint =
        SeedProbeEndpointV2::AfterRandSeedSet;
    SemanticStopReceiptV2 semantic_stop;
    InputPublicationReceiptV2 publication;
    InputPollReceiptV2 guest_poll;

    bool operator==(const SeedProbeResultV2&) const = default;
};

struct SeedProbeSurveyPlanSettingsV2
{
    std::int32_t samples_per_axis = 0;
    std::int32_t min_value = 0;
    std::int32_t max_value = 255;
    bool ignore_trigger_minmax = false;
    bool cap_trigger_top = false;

    bool operator==(const SeedProbeSurveyPlanSettingsV2&) const = default;
};

// Typed SeedProbe surface of the generic Full Phase definition. These methods
// are pure: callers provide authored settings or factual Survey observations,
// and receive scalar frames to persist as jobs. Database coordination and
// result interpretation remain outside the compiled phase definition.
class ISeedProbeFullPhaseDefinitionV2
    : public fullphase::IFullPhaseProgramDefinition
{
public:
    [[nodiscard]] virtual bool DecodeProgramResult(
        std::span<const program::Byte> encoded_result,
        SeedProbeResultV2& result,
        std::string* diagnostic = nullptr) const = 0;
    [[nodiscard]] virtual std::vector<GCInputFrame> PlanSurvey(
        const SeedProbeSurveyPlanSettingsV2& settings) const = 0;
    [[nodiscard]] virtual JCTComboSamples PlanSearch(
        const RandSeedProbeResult& survey,
        std::uint32_t attempts_per_target,
        std::uint32_t sampler_tries) const = 0;
};

[[nodiscard]] program::SchemaIdentity
SeedProbeEndpointSchemaIdentityV2();
[[nodiscard]] program::SchemaIdentity
SeedProbeRequestSchemaIdentityV2();
[[nodiscard]] program::SchemaIdentity
SeedProbeResultSchemaIdentityV2();

[[nodiscard]] std::uint32_t SeedProbeEndpointPc(
    SeedProbeEndpointV2 endpoint) noexcept;
[[nodiscard]] std::string_view SeedProbeEndpointPointId(
    SeedProbeEndpointV2 endpoint) noexcept;
[[nodiscard]] std::optional<SeedProbeEndpointV2>
SeedProbeEndpointFromPc(std::uint32_t pc) noexcept;

[[nodiscard]] program::ProgramValueGraph EncodeSeedProbeRequestV2(
    const SeedProbeRequestV2& request);
[[nodiscard]] bool DecodeSeedProbeRequestV2(
    const program::ProgramValueGraph& graph,
    SeedProbeRequestV2& request,
    std::string* diagnostic = nullptr);

// Compact scalar binding used in WorkerWorkset v2. It is exactly one encoded
// GCInputFrame; stage, desired delta, and confirmation intent remain durable
// coordinator facts and never cross the worker boundary.
[[nodiscard]] std::vector<std::uint8_t> EncodeSeedProbeExecutionInputV2(
    const SeedProbeRequestV2& request);
[[nodiscard]] bool DecodeSeedProbeExecutionInputV2(
    std::span<const std::uint8_t> payload,
    SeedProbeRequestV2& request,
    std::string* diagnostic = nullptr);

[[nodiscard]] bool DecodeSeedProbeResultV2(
    const program::ProgramValueGraph& graph,
    SeedProbeResultV2& result,
    std::string* diagnostic = nullptr);
[[nodiscard]] bool ValidateSeedProbeResultV2(
    const SeedProbeRequestV2& request,
    const SeedProbeResultV2& result,
    StateEpoch terminal_origin_epoch,
    std::string* diagnostic = nullptr);

[[nodiscard]] std::shared_ptr<const ISeedProbeFullPhaseDefinitionV2>
SeedProbeFullPhaseDefinitionV2();

} // namespace savor::runtime::seedprobe
