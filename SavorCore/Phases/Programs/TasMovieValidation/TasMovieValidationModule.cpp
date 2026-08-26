#include "TasMovieValidationModule.h"

#include "../../../Runner/Runtime/ProgramKind.h"
#include "../../../Runner/Runtime/ProgramRuntime/Actions/CanonicalActionPayload.h"
#include "../../../Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "../../../Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "../../../Runner/Runtime/ProgramRuntime/Composition/CompositionSupport.h"
#include "../../../Runner/Runtime/ProgramRuntime/Composition/SemanticObservationComposition.h"
#include "../../../Runner/Runtime/ProgramRuntime/ProgramRuntime.h"
#include "../../../Runner/Runtime/ProgramRuntime/Registry/ActionRegistry.h"
#include "../../../Runner/Runtime/ProgramRuntime/Registry/CapabilityPackRegistry.h"
#include "../../../Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "../../../Runner/Runtime/ProgramRuntime/Registry/TypeSchemaRegistry.h"
#include "../../../Runner/Runtime/ProgramRuntime/Store/ProgramDefinitionStore.h"
#include "../../../Runner/Runtime/ProgramRuntime/Verify/ProgramVerifier.h"
#include "../../../Tas/DtmFile.h"
#include "../../../Utils/Hash.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace savor::runtime::tasmovie {
namespace {

using namespace program;
using composition::detail::ModuleFragmentBuilder;
using Field = CanonicalActionPayloadField;

constexpr std::array<TasMovieBoundaryCatalogEntryV1, 3> kBoundaryCatalog{{
    {BeforeRandSeedSetPointId, BeforeRandSeedSetPc},
    {FieldFastPreseedPointId, FieldFastPreseedPc},
    {FieldDeferredPreseedPointId, FieldDeferredPreseedPc},
}};

constexpr std::string_view kInputCountContract =
    "record DtmInputCount/1{value:u64}";
constexpr std::string_view kOperationContract =
    "enum TasMovieValidationOperation/1{EstablishRootCursor=0,Validate=1}";
constexpr std::string_view kCheckpointContract =
    "record TasMovieCheckpoint/1{pc:u32,input_count:soa.tas_movie_validation.DtmInputCount/1}";
constexpr std::string_view kItineraryContract =
    "list<TasMovieCheckpoint/1>(max=4096)";
constexpr std::string_view kOutcomeContract =
    "enum TasMovieValidationOutcome/1{RootCursorEstablished=0,Valid=1,Invalid=2}";
constexpr std::string_view kFailureReasonContract =
    "enum TasMovieValidationFailureReason/1{MovieDesynchronized=0,ExpectedTerminalNotReached=1,Unknown=2}";
constexpr std::string_view kDiagnosticsContract =
    "record TasMovieValidationFailureDiagnostics/1{expected_pc:u32,expected_input_count:optional<DtmInputCount/1>,actual_pc:u32,actual_input_count:DtmInputCount/1,last_verified_itinerary_index:optional<u64>}";
constexpr std::string_view kFailureContract =
    "record TasMovieValidationFailure/1{reason:TasMovieValidationFailureReason/1,diagnostics:TasMovieValidationFailureDiagnostics/1}";
constexpr std::string_view kResultContract =
    "record TasMovieValidationResult/1{outcome:TasMovieValidationOutcome/1,candidate_checkpoint:optional<TasMovieCheckpoint/1>,failure:optional<TasMovieValidationFailure/1>}";

void SetDiagnostic(std::string* diagnostic, std::string message)
{
    if (diagnostic)
        *diagnostic = std::move(message);
}

std::string TypeContract(const TypeRef& type)
{
    if (!type.is_named())
    {
        return "builtin:" + std::to_string(
            static_cast<std::uint8_t>(type.builtin));
    }
    return type.named->canonical_id + "/" +
        std::to_string(type.named->version) + "#" +
        type.named->schema_hash.ToHex();
}

TypeRef InputCountType()
{
    return TypeRef::Named(DtmInputCountSchemaIdentityV1());
}

TypeRef OperationType()
{
    return TypeRef::Named(
        TasMovieValidationOperationSchemaIdentityV1());
}

TypeRef CheckpointType()
{
    return TypeRef::Named(TasMovieCheckpointSchemaIdentityV1());
}

TypeRef ItineraryType()
{
    return TypeRef::Named(TasMovieItinerarySchemaIdentityV1());
}

TypeRef OutcomeType()
{
    return TypeRef::Named(
        TasMovieValidationOutcomeSchemaIdentityV1());
}

TypeRef FailureReasonType()
{
    return TypeRef::Named(
        TasMovieValidationFailureReasonSchemaIdentityV1());
}

TypeRef DiagnosticsType()
{
    return TypeRef::Named(
        TasMovieValidationFailureDiagnosticsSchemaIdentityV1());
}

TypeRef FailureType()
{
    return TypeRef::Named(
        TasMovieValidationFailureSchemaIdentityV1());
}

TypeRef ResultType()
{
    return TypeRef::Named(
        TasMovieValidationResultSchemaIdentityV1());
}

SchemaIdentity OptionalInputCountSchemaIdentity()
{
    return composition::ExactSchema(
        "soa.tas_movie_validation.OptionalDtmInputCount",
        1,
        "optional<" + TypeContract(InputCountType()) + ">");
}

SchemaIdentity OptionalItineraryIndexSchemaIdentity()
{
    return composition::ExactSchema(
        "soa.tas_movie_validation.OptionalItineraryIndex",
        1,
        "optional<" + TypeContract(
            TypeRef::Builtin(BuiltinType::U64)) + ">");
}

SchemaIdentity OptionalCheckpointSchemaIdentity()
{
    return composition::ExactSchema(
        "soa.tas_movie_validation.OptionalCheckpoint",
        1,
        "optional<" + TypeContract(CheckpointType()) + ">");
}

SchemaIdentity OptionalFailureSchemaIdentity()
{
    return composition::ExactSchema(
        "soa.tas_movie_validation.OptionalFailure",
        1,
        "optional<" + TypeContract(FailureType()) + ">");
}

SchemaIdentity OptionalCaptureRequestSchemaIdentity()
{
    return composition::ExactSchema(
        "soa.tas_movie_validation.OptionalCaptureRequest",
        1,
        "optional<" + TypeContract(CanonicalActionInputType(
            CanonicalAction::SavestateSaveImmutableArtifact)) + ">");
}

SchemaIdentity ModuleRequestSchemaIdentity()
{
    const std::string contract =
        "record TasMovieValidationModuleRequest/1{operation:" +
        TypeContract(OperationType()) + ",prepare_request:" +
        TypeContract(CanonicalActionInputType(
            CanonicalAction::MoviePrepareReadOnlyPlayback)) + ",itinerary:" +
        TypeContract(ItineraryType()) + ",capture_request:" +
        TypeContract(TypeRef::Named(
            OptionalCaptureRequestSchemaIdentity())) + "}";
    return composition::ExactSchema(
        "soa.tas_movie_validation.ModuleRequest",
        1,
        contract);
}

TypeRef ModuleRequestType()
{
    return TypeRef::Named(ModuleRequestSchemaIdentity());
}

bool IsValidOperation(TasMovieValidationOperationV1 value) noexcept
{
    return value ==
            TasMovieValidationOperationV1::EstablishRootCursor ||
        value == TasMovieValidationOperationV1::Validate;
}

bool IsValidOutcome(TasMovieValidationOutcomeV1 value) noexcept
{
    return value ==
            TasMovieValidationOutcomeV1::RootCursorEstablished ||
        value == TasMovieValidationOutcomeV1::Valid ||
        value == TasMovieValidationOutcomeV1::Invalid;
}

bool IsValidFailureReason(
    TasMovieValidationFailureReasonV1 value) noexcept
{
    return value ==
            TasMovieValidationFailureReasonV1::MovieDesynchronized ||
        value == TasMovieValidationFailureReasonV1::
            ExpectedTerminalNotReached ||
        value == TasMovieValidationFailureReasonV1::Unknown;
}

bool ValidateRequestShape(
    const TasMovieValidationRequestV1& request,
    std::optional<std::uint64_t> dtm_input_count,
    std::string* diagnostic)
{
    SetDiagnostic(diagnostic, {});
    if (!IsValidOperation(request.operation))
    {
        SetDiagnostic(diagnostic, "TMV1 operation is unknown");
        return false;
    }
    if (request.dtm_path.empty() ||
        request.dtm_path.size() > MaximumPathBytes)
    {
        SetDiagnostic(
            diagnostic,
            "TMV1 DTM path must contain 1..4096 bytes");
        return false;
    }
    if (request.startup_savestate_path &&
        (request.startup_savestate_path->empty() ||
         request.startup_savestate_path->size() > MaximumPathBytes))
    {
        SetDiagnostic(
            diagnostic,
            "TMV1 startup savestate path must contain 1..4096 bytes");
        return false;
    }
    if (request.final_checkpoint_path &&
        (request.final_checkpoint_path->empty() ||
         request.final_checkpoint_path->size() > MaximumPathBytes))
    {
        SetDiagnostic(
            diagnostic,
            "TMV1 checkpoint path must contain 1..4096 bytes");
        return false;
    }
    if (request.operation ==
        TasMovieValidationOperationV1::EstablishRootCursor)
    {
        if (!request.itinerary.checkpoints.empty() ||
            request.final_checkpoint_path)
        {
            SetDiagnostic(
                diagnostic,
                "Root-cursor establishment accepts neither an itinerary nor a capture path");
            return false;
        }
        return true;
    }

    const auto& checkpoints = request.itinerary.checkpoints;
    if (checkpoints.empty() ||
        checkpoints.size() > MaximumItineraryEntries)
    {
        SetDiagnostic(
            diagnostic,
            "Validation itinerary must contain 1..4096 checkpoints");
        return false;
    }
    std::optional<std::uint64_t> prior;
    for (const TasMovieCheckpointV1& checkpoint : checkpoints)
    {
        if (!TasMovieBoundaryCatalogContainsPcV1(checkpoint.pc))
        {
            SetDiagnostic(
                diagnostic,
                "Validation itinerary contains a PC outside the immutable boundary catalog");
            return false;
        }
        if (prior && checkpoint.input_count.value <= *prior)
        {
            SetDiagnostic(
                diagnostic,
                "Validation itinerary input counts must increase strictly");
            return false;
        }
        if (dtm_input_count &&
            checkpoint.input_count.value >= *dtm_input_count)
        {
            SetDiagnostic(
                diagnostic,
                "Validation itinerary input count is not before the DTM end boundary");
            return false;
        }
        prior = checkpoint.input_count.value;
    }
    return true;
}

class ScalarWriter final
{
public:
    ScalarWriter()
    {
        bytes_.insert(bytes_.end(), {'T', 'M', 'V', '1'});
    }

    void U8(std::uint8_t value) { bytes_.push_back(value); }
    void U32(std::uint32_t value)
    {
        for (unsigned shift = 0; shift != 32; shift += 8)
            bytes_.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void U64(std::uint64_t value)
    {
        for (unsigned shift = 0; shift != 64; shift += 8)
            bytes_.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void String(std::string_view value)
    {
        U32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    [[nodiscard]] std::vector<std::uint8_t> Finish() &&
    {
        return std::move(bytes_);
    }

private:
    std::vector<std::uint8_t> bytes_;
};

class ScalarReader final
{
public:
    explicit ScalarReader(std::span<const std::uint8_t> bytes)
        : bytes_(bytes)
    {
        valid_ = bytes_.size() >= 4 &&
            bytes_[0] == 'T' && bytes_[1] == 'M' &&
            bytes_[2] == 'V' && bytes_[3] == '1';
        if (valid_)
            offset_ = 4;
    }

    bool U8(std::uint8_t& value)
    {
        if (!Take(1))
            return false;
        value = bytes_[offset_++];
        return true;
    }
    bool U32(std::uint32_t& value)
    {
        if (!Take(4))
            return false;
        value = 0;
        for (unsigned shift = 0; shift != 32; shift += 8)
            value |= static_cast<std::uint32_t>(bytes_[offset_++]) << shift;
        return true;
    }
    bool U64(std::uint64_t& value)
    {
        if (!Take(8))
            return false;
        value = 0;
        for (unsigned shift = 0; shift != 64; shift += 8)
            value |= static_cast<std::uint64_t>(bytes_[offset_++]) << shift;
        return true;
    }
    bool String(std::string& value)
    {
        std::uint32_t size = 0;
        if (!U32(size) || size > MaximumPathBytes || !Take(size))
            return false;
        value.assign(
            reinterpret_cast<const char*>(bytes_.data() + offset_),
            size);
        offset_ += size;
        return true;
    }
    [[nodiscard]] bool done() const noexcept
    {
        return valid_ && offset_ == bytes_.size();
    }

private:
    bool Take(std::size_t count)
    {
        if (!valid_ || count > bytes_.size() - offset_)
        {
            valid_ = false;
            return false;
        }
        return true;
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
    bool valid_ = false;
};

class GraphReader final
{
public:
    explicit GraphReader(const ProgramValueGraph& graph)
    {
        valid_ = static_cast<bool>(graph.root);
        for (const ProgramValue& value : graph.values)
        {
            if (!value.id || !values_.emplace(value.id, &value).second)
            {
                valid_ = false;
                return;
            }
        }
        root_ = Find(graph.root);
        valid_ = valid_ && root_ != nullptr;
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const ProgramValue* root() const noexcept { return root_; }
    [[nodiscard]] const ProgramValue* Find(ProgramValueId id) const
    {
        const auto found = values_.find(id);
        return found == values_.end() ? nullptr : found->second;
    }

private:
    bool valid_ = false;
    const ProgramValue* root_ = nullptr;
    std::map<ProgramValueId, const ProgramValue*> values_;
};

template <typename T>
bool ReadScalar(
    const GraphReader& graph,
    ProgramValueId id,
    BuiltinType type,
    T& value)
{
    const ProgramValue* candidate = graph.Find(id);
    const auto* scalar = candidate
        ? std::get_if<T>(&candidate->payload)
        : nullptr;
    if (!candidate || candidate->type != TypeRef::Builtin(type) ||
        !scalar)
    {
        return false;
    }
    value = *scalar;
    return true;
}

bool ReadEnum(
    const GraphReader& graph,
    ProgramValueId id,
    const SchemaIdentity& schema,
    std::int64_t& value)
{
    const ProgramValue* candidate = graph.Find(id);
    const auto* enumeration = candidate
        ? std::get_if<EnumValue>(&candidate->payload)
        : nullptr;
    if (!candidate || candidate->type != TypeRef::Named(schema) ||
        !enumeration || enumeration->schema != schema)
    {
        return false;
    }
    value = enumeration->value;
    return true;
}

const ProgramValue* OptionalElement(
    const GraphReader& graph,
    ProgramValueId id,
    const SchemaIdentity& schema,
    bool& valid)
{
    valid = false;
    const ProgramValue* candidate = graph.Find(id);
    const auto* optional = candidate
        ? std::get_if<OptionalValue>(&candidate->payload)
        : nullptr;
    if (!candidate || candidate->type != TypeRef::Named(schema) ||
        !optional)
    {
        return nullptr;
    }
    if (!optional->value)
    {
        valid = true;
        return nullptr;
    }
    const ProgramValue* element = graph.Find(*optional->value);
    valid = element != nullptr;
    return element;
}

bool DecodeInputCount(
    const GraphReader& graph,
    ProgramValueId id,
    DtmInputCount& count)
{
    const ProgramValue* value = graph.Find(id);
    const auto* record = value
        ? std::get_if<RecordValue>(&value->payload)
        : nullptr;
    return value && value->type == InputCountType() && record &&
        record->fields.size() == 1 &&
        ReadScalar(
            graph,
            record->fields[0],
            BuiltinType::U64,
            count.value);
}

bool DecodeCheckpoint(
    const GraphReader& graph,
    ProgramValueId id,
    TasMovieCheckpointV1& checkpoint)
{
    const ProgramValue* value = graph.Find(id);
    const auto* record = value
        ? std::get_if<RecordValue>(&value->payload)
        : nullptr;
    return value && value->type == CheckpointType() && record &&
        record->fields.size() == 2 &&
        ReadScalar(
            graph,
            record->fields[0],
            BuiltinType::U32,
            checkpoint.pc) &&
        DecodeInputCount(graph, record->fields[1], checkpoint.input_count);
}

std::optional<std::vector<Byte>> ActionRequestBytes(
    CanonicalAction action,
    const std::string& path,
    std::string label,
    std::string* diagnostic)
{
    CanonicalActionPayload payload;
    if (!payload.AddUtf8(Field::Path, path) ||
        !payload.AddUtf8(Field::Label, std::move(label)))
    {
        SetDiagnostic(
            diagnostic,
            "Canonical action request path could not be represented");
        return std::nullopt;
    }
    const auto schema = CanonicalActionInputSchemaIdentity(action);
    if (!schema)
    {
        SetDiagnostic(
            diagnostic,
            "Canonical action request has no exact nominal schema");
        return std::nullopt;
    }
    CanonicalActionPayloadResult encoded =
        EncodeCanonicalActionPayload(payload, *schema);
    if (!encoded.ok || encoded.graph.values.size() != 1)
    {
        SetDiagnostic(
            diagnostic,
            encoded.diagnostic.empty()
                ? "Canonical action request encoding failed"
                : std::move(encoded.diagnostic));
        return std::nullopt;
    }
    const auto* bytes = std::get_if<std::vector<Byte>>(
        &encoded.graph.values.front().payload);
    if (!bytes)
    {
        SetDiagnostic(
            diagnostic,
            "Canonical action request did not encode to nominal bytes");
        return std::nullopt;
    }
    return *bytes;
}

ProgramValueGraph EncodeModuleRequest(
    const TasMovieValidationRequestV1& request,
    std::string* diagnostic)
{
    SetDiagnostic(diagnostic, {});
    const auto playback = ActionRequestBytes(
        CanonicalAction::MoviePrepareReadOnlyPlayback,
        request.dtm_path,
        "TAS movie complete validation playback",
        diagnostic);
    if (!playback)
        return {};
    std::optional<std::vector<Byte>> capture;
    if (request.final_checkpoint_path)
    {
        capture = ActionRequestBytes(
            CanonicalAction::SavestateSaveImmutableArtifact,
            *request.final_checkpoint_path,
            "TAS movie root validation checkpoint",
            diagnostic);
        if (!capture)
            return {};
    }

    std::vector<ProgramValue> values;
    std::uint64_t next_id = 1;
    const auto add = [&](TypeRef type, ProgramValuePayload payload) {
        ProgramValue value{
            ProgramValueId(next_id++),
            std::move(type),
            std::move(payload)};
        const ProgramValueId id = value.id;
        values.push_back(std::move(value));
        return id;
    };

    const ProgramValueId operation = add(
        OperationType(),
        EnumValue{
            TasMovieValidationOperationSchemaIdentityV1(),
            static_cast<std::int64_t>(request.operation)});
    const ProgramValueId playback_request = add(
        CanonicalActionInputType(
            CanonicalAction::MoviePrepareReadOnlyPlayback),
        *playback);

    std::vector<ProgramValueId> checkpoint_ids;
    checkpoint_ids.reserve(request.itinerary.checkpoints.size());
    for (const TasMovieCheckpointV1& checkpoint :
         request.itinerary.checkpoints)
    {
        const ProgramValueId pc = add(
            TypeRef::Builtin(BuiltinType::U32),
            checkpoint.pc);
        const ProgramValueId count_value = add(
            TypeRef::Builtin(BuiltinType::U64),
            checkpoint.input_count.value);
        const ProgramValueId count = add(
            InputCountType(),
            RecordValue{{count_value}});
        checkpoint_ids.push_back(add(
            CheckpointType(),
            RecordValue{{pc, count}}));
    }
    const ProgramValueId itinerary = add(
        ItineraryType(),
        ListValue{std::move(checkpoint_ids)});

    std::optional<ProgramValueId> capture_id;
    if (capture)
    {
        capture_id = add(
            CanonicalActionInputType(
                CanonicalAction::SavestateSaveImmutableArtifact),
            *capture);
    }
    const ProgramValueId optional_capture = add(
        TypeRef::Named(OptionalCaptureRequestSchemaIdentity()),
        OptionalValue{capture_id});
    const ProgramValueId root = add(
        ModuleRequestType(),
        RecordValue{{
            operation,
            playback_request,
            itinerary,
            optional_capture,
        }});
    return {root, std::move(values)};
}

bool PreflightRequest(
    const TasMovieValidationRequestV1& request,
    std::string* diagnostic)
{
    savor::tas::DtmFile dtm;
    if (!dtm.load(request.dtm_path))
    {
        SetDiagnostic(
            diagnostic,
            "TAS movie validation DTM could not be loaded");
        return false;
    }
    std::string reason;
    if (!dtm.supports_gc_poll_editing(&reason))
    {
        SetDiagnostic(
            diagnostic,
            "TAS movie validation requires a GC-only aligned DTM: " +
                reason);
        return false;
    }
    const std::uint64_t payload_count =
        static_cast<std::uint64_t>(dtm.gc_poll_count());
    if (dtm.info().input_count != payload_count)
    {
        SetDiagnostic(
            diagnostic,
            "TAS movie validation rejects a DTM whose header input count differs from its payload");
        return false;
    }
    const bool has_startup = request.startup_savestate_path.has_value();
    if (dtm.info().starts_from_savestate != has_startup)
    {
        SetDiagnostic(
            diagnostic,
            "TAS movie validation startup savestate disagrees with the DTM header");
        return false;
    }
    if (has_startup)
    {
        const std::filesystem::path expected(request.dtm_path + ".sav");
        if (std::filesystem::path(*request.startup_savestate_path) != expected ||
            !std::filesystem::is_regular_file(expected))
        {
            SetDiagnostic(
                diagnostic,
                "TAS movie validation requires the exact readable <dtm>.sav startup artifact");
            return false;
        }
    }
    return ValidateRequestShape(request, payload_count, diagnostic);
}

} // namespace

std::optional<std::uint64_t>
DtmInputCount::PayloadByteOffset() const noexcept
{
    if (value >
        (std::numeric_limits<std::uint64_t>::max)() /
            GameCubeDtmInputRecordBytes)
    {
        return std::nullopt;
    }
    return value * GameCubeDtmInputRecordBytes;
}

std::span<const TasMovieBoundaryCatalogEntryV1>
TasMovieBoundaryCatalogV1() noexcept
{
    return kBoundaryCatalog;
}

bool TasMovieBoundaryCatalogContainsPcV1(std::uint32_t pc) noexcept
{
    return std::ranges::any_of(
        kBoundaryCatalog,
        [pc](const TasMovieBoundaryCatalogEntryV1& entry) {
            return entry.pc == pc;
        });
}

SchemaIdentity DtmInputCountSchemaIdentityV1()
{
    return composition::ExactSchema(
        "soa.tas_movie_validation.DtmInputCount",
        1,
        kInputCountContract);
}

SchemaIdentity TasMovieValidationOperationSchemaIdentityV1()
{
    return composition::ExactSchema(
        "soa.tas_movie_validation.Operation",
        1,
        kOperationContract);
}

SchemaIdentity TasMovieCheckpointSchemaIdentityV1()
{
    return composition::ExactSchema(
        "soa.tas_movie_validation.Checkpoint",
        1,
        kCheckpointContract);
}

SchemaIdentity TasMovieItinerarySchemaIdentityV1()
{
    return composition::ExactSchema(
        "soa.tas_movie_validation.Itinerary",
        1,
        kItineraryContract);
}

SchemaIdentity TasMovieValidationOutcomeSchemaIdentityV1()
{
    return composition::ExactSchema(
        "soa.tas_movie_validation.Outcome",
        1,
        kOutcomeContract);
}

SchemaIdentity TasMovieValidationFailureReasonSchemaIdentityV1()
{
    return composition::ExactSchema(
        "soa.tas_movie_validation.FailureReason",
        1,
        kFailureReasonContract);
}

SchemaIdentity TasMovieValidationFailureDiagnosticsSchemaIdentityV1()
{
    return composition::ExactSchema(
        "soa.tas_movie_validation.FailureDiagnostics",
        1,
        kDiagnosticsContract);
}

SchemaIdentity TasMovieValidationFailureSchemaIdentityV1()
{
    return composition::ExactSchema(
        "soa.tas_movie_validation.Failure",
        1,
        kFailureContract);
}

SchemaIdentity TasMovieValidationResultSchemaIdentityV1()
{
    return composition::ExactSchema(
        "soa.tas_movie_validation.Result",
        1,
        kResultContract);
}

bool ValidateTasMovieValidationResultV1(
    const TasMovieValidationResultV1& result,
    std::string* diagnostic)
{
    SetDiagnostic(diagnostic, {});
    if (!IsValidOutcome(result.outcome))
    {
        SetDiagnostic(diagnostic, "TAS movie validation outcome is unknown");
        return false;
    }
    switch (result.outcome)
    {
    case TasMovieValidationOutcomeV1::RootCursorEstablished:
        if (!result.candidate_checkpoint || result.failure ||
            !TasMovieBoundaryCatalogContainsPcV1(
                result.candidate_checkpoint->pc))
        {
            SetDiagnostic(
                diagnostic,
                "RootCursorEstablished requires one catalog checkpoint and no failure");
            return false;
        }
        return true;
    case TasMovieValidationOutcomeV1::Valid:
        if (result.candidate_checkpoint || result.failure)
        {
            SetDiagnostic(
                diagnostic,
                "Valid permits neither a candidate checkpoint nor a failure");
            return false;
        }
        return true;
    case TasMovieValidationOutcomeV1::Invalid:
        if (result.candidate_checkpoint || !result.failure ||
            !IsValidFailureReason(result.failure->reason) ||
            result.failure->diagnostics.expected_pc == 0)
        {
            SetDiagnostic(
                diagnostic,
                "Invalid requires exactly one typed failure and no candidate checkpoint");
            return false;
        }
        return true;
    }
    return false;
}

ProgramValueGraph EncodeTasMovieValidationResultV1(
    const TasMovieValidationResultV1& result)
{
    if (!ValidateTasMovieValidationResultV1(result))
        return {};

    std::vector<ProgramValue> values;
    std::uint64_t next_id = 1;
    const auto add = [&](TypeRef type, ProgramValuePayload payload) {
        ProgramValue value{
            ProgramValueId(next_id++),
            std::move(type),
            std::move(payload)};
        const ProgramValueId id = value.id;
        values.push_back(std::move(value));
        return id;
    };
    const auto add_count = [&](DtmInputCount count) {
        const ProgramValueId scalar = add(
            TypeRef::Builtin(BuiltinType::U64),
            count.value);
        return add(InputCountType(), RecordValue{{scalar}});
    };
    const auto add_checkpoint = [&](const TasMovieCheckpointV1& checkpoint) {
        const ProgramValueId pc = add(
            TypeRef::Builtin(BuiltinType::U32),
            checkpoint.pc);
        return add(
            CheckpointType(),
            RecordValue{{pc, add_count(checkpoint.input_count)}});
    };

    const ProgramValueId outcome = add(
        OutcomeType(),
        EnumValue{
            TasMovieValidationOutcomeSchemaIdentityV1(),
            static_cast<std::int64_t>(result.outcome)});
    std::optional<ProgramValueId> candidate;
    if (result.candidate_checkpoint)
        candidate = add_checkpoint(*result.candidate_checkpoint);
    const ProgramValueId optional_candidate = add(
        TypeRef::Named(OptionalCheckpointSchemaIdentity()),
        OptionalValue{candidate});

    std::optional<ProgramValueId> failure;
    if (result.failure)
    {
        const auto& source = *result.failure;
        const auto& source_diagnostics = source.diagnostics;
        const ProgramValueId reason = add(
            FailureReasonType(),
            EnumValue{
                TasMovieValidationFailureReasonSchemaIdentityV1(),
                static_cast<std::int64_t>(source.reason)});
        const ProgramValueId expected_pc = add(
            TypeRef::Builtin(BuiltinType::U32),
            source_diagnostics.expected_pc);
        std::optional<ProgramValueId> expected_count;
        if (source_diagnostics.expected_input_count)
        {
            expected_count = add_count(
                *source_diagnostics.expected_input_count);
        }
        const ProgramValueId optional_expected = add(
            TypeRef::Named(OptionalInputCountSchemaIdentity()),
            OptionalValue{expected_count});
        const ProgramValueId actual_pc = add(
            TypeRef::Builtin(BuiltinType::U32),
            source_diagnostics.actual_pc);
        const ProgramValueId actual_count = add_count(
            source_diagnostics.actual_input_count);
        std::optional<ProgramValueId> last_verified;
        if (source_diagnostics.last_verified_itinerary_index)
        {
            last_verified = add(
                TypeRef::Builtin(BuiltinType::U64),
                *source_diagnostics.last_verified_itinerary_index);
        }
        const ProgramValueId optional_last = add(
            TypeRef::Named(OptionalItineraryIndexSchemaIdentity()),
            OptionalValue{last_verified});
        const ProgramValueId diagnostics = add(
            DiagnosticsType(),
            RecordValue{{
                expected_pc,
                optional_expected,
                actual_pc,
                actual_count,
                optional_last,
            }});
        failure = add(
            FailureType(),
            RecordValue{{reason, diagnostics}});
    }
    const ProgramValueId optional_failure = add(
        TypeRef::Named(OptionalFailureSchemaIdentity()),
        OptionalValue{failure});
    const ProgramValueId root = add(
        ResultType(),
        RecordValue{{outcome, optional_candidate, optional_failure}});
    return {root, std::move(values)};
}

bool DecodeTasMovieValidationResultV1(
    const ProgramValueGraph& graph,
    TasMovieValidationResultV1& result,
    std::string* diagnostic)
{
    SetDiagnostic(diagnostic, {});
    GraphReader reader(graph);
    const ProgramValue* root = reader.root();
    const auto* record = root
        ? std::get_if<RecordValue>(&root->payload)
        : nullptr;
    if (!reader.valid() || !root || root->type != ResultType() ||
        !record || record->fields.size() != 3)
    {
        SetDiagnostic(
            diagnostic,
            "TAS movie output root is not the exact Result/1 record");
        return false;
    }

    TasMovieValidationResultV1 decoded;
    std::int64_t outcome = 0;
    if (!ReadEnum(
            reader,
            record->fields[0],
            TasMovieValidationOutcomeSchemaIdentityV1(),
            outcome))
    {
        SetDiagnostic(diagnostic, "TAS movie output outcome is malformed");
        return false;
    }
    decoded.outcome = static_cast<TasMovieValidationOutcomeV1>(outcome);

    bool optional_valid = false;
    const ProgramValue* candidate = OptionalElement(
        reader,
        record->fields[1],
        OptionalCheckpointSchemaIdentity(),
        optional_valid);
    if (!optional_valid)
    {
        SetDiagnostic(
            diagnostic,
            "TAS movie output candidate optional is malformed");
        return false;
    }
    if (candidate)
    {
        TasMovieCheckpointV1 checkpoint;
        if (!DecodeCheckpoint(reader, candidate->id, checkpoint))
        {
            SetDiagnostic(
                diagnostic,
                "TAS movie output candidate checkpoint is malformed");
            return false;
        }
        decoded.candidate_checkpoint = checkpoint;
    }

    const ProgramValue* failure_value = OptionalElement(
        reader,
        record->fields[2],
        OptionalFailureSchemaIdentity(),
        optional_valid);
    if (!optional_valid)
    {
        SetDiagnostic(
            diagnostic,
            "TAS movie output failure optional is malformed");
        return false;
    }
    if (failure_value)
    {
        const auto* failure_record =
            std::get_if<RecordValue>(&failure_value->payload);
        if (failure_value->type != FailureType() || !failure_record ||
            failure_record->fields.size() != 2)
        {
            SetDiagnostic(
                diagnostic,
                "TAS movie output failure is malformed");
            return false;
        }
        TasMovieValidationFailureV1 failure;
        std::int64_t reason = 0;
        const ProgramValue* diagnostics_value =
            reader.Find(failure_record->fields[1]);
        const auto* diagnostics_record = diagnostics_value
            ? std::get_if<RecordValue>(&diagnostics_value->payload)
            : nullptr;
        if (!ReadEnum(
                reader,
                failure_record->fields[0],
                TasMovieValidationFailureReasonSchemaIdentityV1(),
                reason) ||
            !diagnostics_value ||
            diagnostics_value->type != DiagnosticsType() ||
            !diagnostics_record || diagnostics_record->fields.size() != 5)
        {
            SetDiagnostic(
                diagnostic,
                "TAS movie output failure diagnostics are malformed");
            return false;
        }
        failure.reason =
            static_cast<TasMovieValidationFailureReasonV1>(reason);
        auto& destination = failure.diagnostics;
        if (!ReadScalar(
                reader,
                diagnostics_record->fields[0],
                BuiltinType::U32,
                destination.expected_pc) ||
            !ReadScalar(
                reader,
                diagnostics_record->fields[2],
                BuiltinType::U32,
                destination.actual_pc) ||
            !DecodeInputCount(
                reader,
                diagnostics_record->fields[3],
                destination.actual_input_count))
        {
            SetDiagnostic(
                diagnostic,
                "TAS movie output scalar diagnostics are malformed");
            return false;
        }
        const ProgramValue* expected = OptionalElement(
            reader,
            diagnostics_record->fields[1],
            OptionalInputCountSchemaIdentity(),
            optional_valid);
        if (!optional_valid ||
            (expected && !DecodeInputCount(
                reader,
                expected->id,
                destination.expected_input_count.emplace())))
        {
            SetDiagnostic(
                diagnostic,
                "TAS movie output expected-count diagnostic is malformed");
            return false;
        }
        const ProgramValue* last = OptionalElement(
            reader,
            diagnostics_record->fields[4],
            OptionalItineraryIndexSchemaIdentity(),
            optional_valid);
        std::uint64_t last_value = 0;
        if (!optional_valid ||
            (last && !ReadScalar(
                reader,
                last->id,
                BuiltinType::U64,
                last_value)))
        {
            SetDiagnostic(
                diagnostic,
                "TAS movie output last-verified diagnostic is malformed");
            return false;
        }
        if (last)
            destination.last_verified_itinerary_index = last_value;
        decoded.failure = std::move(failure);
    }

    if (!ValidateTasMovieValidationResultV1(decoded, diagnostic))
        return false;
    result = std::move(decoded);
    return true;
}

std::vector<std::uint8_t> EncodeTasMovieValidationExecutionInputV1(
    const TasMovieValidationRequestV1& request,
    std::string* diagnostic)
{
    if (!ValidateRequestShape(request, std::nullopt, diagnostic))
        return {};
    ScalarWriter writer;
    writer.U8(static_cast<std::uint8_t>(request.operation));
    writer.String(request.dtm_path);
    writer.U8(request.startup_savestate_path ? 1u : 0u);
    if (request.startup_savestate_path)
        writer.String(*request.startup_savestate_path);
    writer.U32(static_cast<std::uint32_t>(
        request.itinerary.checkpoints.size()));
    for (const TasMovieCheckpointV1& checkpoint :
         request.itinerary.checkpoints)
    {
        writer.U32(checkpoint.pc);
        writer.U64(checkpoint.input_count.value);
    }
    writer.U8(request.final_checkpoint_path ? 1u : 0u);
    if (request.final_checkpoint_path)
        writer.String(*request.final_checkpoint_path);
    return std::move(writer).Finish();
}

bool DecodeTasMovieValidationExecutionInputV1(
    std::span<const std::uint8_t> payload,
    TasMovieValidationRequestV1& request,
    std::string* diagnostic)
{
    SetDiagnostic(diagnostic, {});
    ScalarReader reader(payload);
    std::uint8_t operation = 0;
    std::string dtm_path;
    std::uint8_t has_startup = 0;
    std::string startup_path;
    std::uint32_t count = 0;
    if (!reader.U8(operation) || !reader.String(dtm_path) ||
        !reader.U8(has_startup) || has_startup > 1 ||
        (has_startup && !reader.String(startup_path)) ||
        !reader.U32(count) || count > MaximumItineraryEntries)
    {
        SetDiagnostic(
            diagnostic,
            "TMV1 scalar binding header or itinerary count is malformed");
        return false;
    }
    TasMovieValidationRequestV1 decoded;
    decoded.operation =
        static_cast<TasMovieValidationOperationV1>(operation);
    decoded.dtm_path = std::move(dtm_path);
    if (has_startup)
        decoded.startup_savestate_path = std::move(startup_path);
    decoded.itinerary.checkpoints.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        TasMovieCheckpointV1 checkpoint;
        if (!reader.U32(checkpoint.pc) ||
            !reader.U64(checkpoint.input_count.value))
        {
            SetDiagnostic(
                diagnostic,
                "TMV1 scalar binding contains a truncated checkpoint");
            return false;
        }
        decoded.itinerary.checkpoints.push_back(checkpoint);
    }
    std::uint8_t has_capture = 0;
    if (!reader.U8(has_capture) || has_capture > 1)
    {
        SetDiagnostic(
            diagnostic,
            "TMV1 scalar binding capture flag is malformed");
        return false;
    }
    if (has_capture)
    {
        std::string path;
        if (!reader.String(path))
        {
            SetDiagnostic(
                diagnostic,
                "TMV1 scalar binding capture path is malformed");
            return false;
        }
        decoded.final_checkpoint_path = std::move(path);
    }
    if (!reader.done())
    {
        SetDiagnostic(
            diagnostic,
            "TMV1 scalar binding is truncated or contains trailing bytes");
        return false;
    }
    if (!ValidateRequestShape(decoded, std::nullopt, diagnostic))
        return false;
    request = std::move(decoded);
    return true;
}

std::vector<std::uint8_t> EncodeTasMovieItineraryArtifactV1(
    const TasMovieItineraryV1& itinerary,
    std::string* diagnostic)
{
    if (itinerary.checkpoints.empty()
        || itinerary.checkpoints.size() > MaximumItineraryEntries)
    {
        SetDiagnostic(diagnostic, "TMI1 itinerary entry count is outside [1,4096]");
        return {};
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(8 + itinerary.checkpoints.size() * 12);
    bytes.insert(bytes.end(), {'T', 'M', 'I', '1'});
    const auto append_u32 = [&bytes](std::uint32_t value) {
        for (int i = 0; i < 4; ++i)
            bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    };
    const auto append_u64 = [&bytes](std::uint64_t value) {
        for (int i = 0; i < 8; ++i)
            bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    };
    append_u32(static_cast<std::uint32_t>(itinerary.checkpoints.size()));
    for (const auto& checkpoint : itinerary.checkpoints)
    {
        append_u32(checkpoint.pc);
        append_u64(checkpoint.input_count.value);
    }
    SetDiagnostic(diagnostic, {});
    return bytes;
}

bool DecodeTasMovieItineraryArtifactV1(
    std::span<const std::uint8_t> payload,
    TasMovieItineraryV1& itinerary,
    std::string* diagnostic)
{
    itinerary = {};
    if (payload.size() < 8
        || payload[0] != 'T' || payload[1] != 'M'
        || payload[2] != 'I' || payload[3] != '1')
    {
        SetDiagnostic(diagnostic, "TMI1 magic or header is invalid");
        return false;
    }
    const auto read_u32 = [&payload](std::size_t offset) {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i)
            value |= static_cast<std::uint32_t>(payload[offset + i]) << (i * 8);
        return value;
    };
    const auto read_u64 = [&payload](std::size_t offset) {
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i)
            value |= static_cast<std::uint64_t>(payload[offset + i]) << (i * 8);
        return value;
    };
    const auto count = read_u32(4);
    if (count == 0 || count > MaximumItineraryEntries
        || payload.size() != 8ull + static_cast<std::size_t>(count) * 12ull)
    {
        SetDiagnostic(diagnostic, "TMI1 entry count or byte length is invalid");
        return false;
    }
    itinerary.checkpoints.reserve(count);
    std::size_t offset = 8;
    for (std::uint32_t i = 0; i < count; ++i, offset += 12)
        itinerary.checkpoints.push_back({read_u32(offset), DtmInputCount{read_u64(offset + 4)}});
    SetDiagnostic(diagnostic, {});
    return true;
}

bool ValidateTasMovieItineraryArtifactV1(
    const TasMovieItineraryV1& itinerary,
    std::uint64_t total_dtm_input_count,
    std::uint32_t required_final_pc,
    std::string* diagnostic)
{
    if (itinerary.checkpoints.empty()
        || itinerary.checkpoints.size() > MaximumItineraryEntries)
    {
        SetDiagnostic(diagnostic, "TMI1 itinerary entry count is outside [1,4096]");
        return false;
    }
    std::optional<std::uint64_t> previous;
    for (const auto& checkpoint : itinerary.checkpoints)
    {
        if (!TasMovieBoundaryCatalogContainsPcV1(checkpoint.pc))
        {
            SetDiagnostic(diagnostic, "TMI1 contains a PC outside the TAS Movie boundary catalog");
            return false;
        }
        if (checkpoint.input_count.value >= total_dtm_input_count)
        {
            SetDiagnostic(diagnostic, "TMI1 input count is not before the DTM input-stream end");
            return false;
        }
        if (previous.has_value() && checkpoint.input_count.value <= *previous)
        {
            SetDiagnostic(diagnostic, "TMI1 input counts are not strictly increasing");
            return false;
        }
        previous = checkpoint.input_count.value;
    }
    if (itinerary.checkpoints.back().pc != required_final_pc)
    {
        SetDiagnostic(diagnostic, "TMI1 final PC does not match the required terminal PC");
        return false;
    }
    SetDiagnostic(diagnostic, {});
    return true;
}

namespace {

class StaticConfigWriter final
{
public:
    explicit StaticConfigWriter(std::array<char, 4> magic)
    {
        for (char value : magic)
            bytes_.push_back(static_cast<Byte>(value));
    }

    void U8(std::uint8_t value) { bytes_.push_back(value); }
    void Bool(bool value) { U8(value ? 1u : 0u); }
    void U32(std::uint32_t value)
    {
        for (unsigned shift = 0; shift != 32; shift += 8)
            bytes_.push_back(static_cast<Byte>(value >> shift));
    }
    void String(std::string_view value)
    {
        U32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void Hash(const ContentHash256& hash)
    {
        bytes_.insert(bytes_.end(), hash.bytes.begin(), hash.bytes.end());
    }
    [[nodiscard]] std::vector<Byte> Finish() &&
    {
        return std::move(bytes_);
    }

private:
    std::vector<Byte> bytes_;
};

std::vector<Byte> ContinueConfig()
{
    StaticConfigWriter writer({'C', 'U', 'C', '2'});
    writer.U8(1); // Ignore the current retained point.
    writer.U8(0); // Static movie policy Ignore; ownership supplies success.
    writer.U8(static_cast<std::uint8_t>(
        ExecutionThrottlePolicy::RequireDisabled));
    writer.U8(static_cast<std::uint8_t>(
        ExecutionInterruptionPolicy::Reject));
    return std::move(writer).Finish();
}

RuntimeProfile InvocationRuntimeProfile(
    const ProgramDependencyLock& dependencies)
{
    return {
        .profile_id = "soa-usa-jit64-v1",
        .game_id = std::string(capabilities::kSupportedGameId),
        .disc_identity = std::string(capabilities::kSupportedGameId),
        .executable_identity = std::string(
            capabilities::kSupportedExecutableIdentity),
        .backend = "jit64",
        .capability_packs = dependencies.capability_packs,
    };
}

std::string RuntimeProfileHash(const RuntimeProfile& profile)
{
    std::string canonical = profile.profile_id;
    const auto append = [&canonical](std::string_view value) {
        canonical.push_back('\0');
        canonical.append(value);
    };
    append(profile.game_id);
    append(profile.disc_identity);
    append(profile.executable_identity);
    append(profile.backend);
    return hash::sha256(canonical.data(), canonical.size());
}

InstructionTarget ActionTarget(
    const ExactDependencyIdentity& dependency)
{
    return {
        .kind = InstructionTargetKind::Action,
        .dependency = dependency,
    };
}

ProgramValueId Required(
    std::optional<ProgramValueId> value,
    std::string_view operation)
{
    if (!value)
    {
        throw std::logic_error(
            "TAS movie validation lowering failed at " +
            std::string(operation));
    }
    return *value;
}

ProgramValueId Constant(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    LiteralValue literal,
    std::string selector,
    ProgramScopeId scope)
{
    const TypeRef type = literal.type;
    return Required(
        builder.AddInstruction(
            function,
            block,
            InstructionOpcode::Constant,
            type,
            {},
            {},
            std::move(selector),
            std::move(literal),
            scope),
        "constant");
}

ProgramValueId ConstantU32(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    std::uint32_t value,
    std::string selector,
    ProgramScopeId scope)
{
    return Constant(
        builder,
        function,
        block,
        LiteralValue{
            TypeRef::Builtin(BuiltinType::U32),
            value},
        std::move(selector),
        scope);
}

ProgramValueId ConstantU64(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    std::uint64_t value,
    std::string selector,
    ProgramScopeId scope)
{
    return Constant(
        builder,
        function,
        block,
        LiteralValue{
            TypeRef::Builtin(BuiltinType::U64),
            value},
        std::move(selector),
        scope);
}

ProgramValueId ConstantEnum(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    const SchemaIdentity& schema,
    std::int64_t value,
    std::string selector,
    ProgramScopeId scope)
{
    return Constant(
        builder,
        function,
        block,
        LiteralValue{
            TypeRef::Named(schema),
            EnumValue{schema, value}},
        std::move(selector),
        scope);
}

ProgramValueId ConstantBytes(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    CanonicalRuntimeSchema schema,
    std::vector<Byte> bytes,
    std::string selector,
    ProgramScopeId scope)
{
    return Constant(
        builder,
        function,
        block,
        LiteralValue{
            CanonicalRuntimeType(schema),
            std::move(bytes)},
        std::move(selector),
        scope);
}

ProgramValueId Project(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    TypeRef type,
    ProgramValueId record,
    std::string field,
    ProgramScopeId scope)
{
    return Required(
        builder.AddInstruction(
            function,
            block,
            InstructionOpcode::RecordProject,
            std::move(type),
            std::array{record},
            {},
            std::move(field),
            std::nullopt,
            scope),
        "record projection");
}

ProgramValueId Construct(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    TypeRef type,
    std::span<const ProgramValueId> fields,
    std::string selector,
    ProgramScopeId scope)
{
    return Required(
        builder.AddInstruction(
            function,
            block,
            InstructionOpcode::RecordConstruct,
            std::move(type),
            fields,
            {},
            std::move(selector),
            std::nullopt,
            scope),
        "record construction");
}

ProgramValueId Optional(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    TypeRef type,
    std::optional<ProgramValueId> value,
    std::string selector,
    ProgramScopeId scope)
{
    std::array<ProgramValueId, 1> storage{};
    std::span<const ProgramValueId> operands;
    if (value)
    {
        storage[0] = *value;
        operands = storage;
    }
    return Required(
        builder.AddInstruction(
            function,
            block,
            InstructionOpcode::OptionalConstruct,
            std::move(type),
            operands,
            {},
            std::move(selector),
            std::nullopt,
            scope),
        "optional construction");
}

ProgramValueId Binary(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    InstructionOpcode opcode,
    TypeRef type,
    ProgramValueId lhs,
    ProgramValueId rhs,
    std::string selector,
    ProgramScopeId scope)
{
    return Required(
        builder.AddInstruction(
            function,
            block,
            opcode,
            std::move(type),
            std::array{lhs, rhs},
            {},
            std::move(selector),
            std::nullopt,
            scope),
        "binary instruction");
}

void AddCanonicalAction(
    ModuleFragmentBuilder& builder,
    CanonicalAction action)
{
    builder.AddActionImport(CanonicalActionIdentity(action));
    for (const SchemaIdentity& schema :
         CanonicalActionTypeSchemaClosure(action))
    {
        builder.AddTypeImport(schema);
    }
}

ProgramValueId Continue(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    ProgramValueId subscription,
    ProgramValueId no_publication,
    ProgramValueId playback,
    ProgramValueId expected_count,
    ProgramValueId config,
    ProgramScopeId scope,
    std::string selector)
{
    const ProgramValueId request = Construct(
        builder,
        function,
        block,
        CanonicalActionInputType(
            CanonicalAction::ExecutionContinueUntil),
        std::array{
            subscription,
            no_publication,
            playback,
            expected_count,
            config,
        },
        selector + "/request",
        scope);
    return Required(
        builder.AddInstruction(
            function,
            block,
            InstructionOpcode::AwaitAction,
            CanonicalActionOutputType(
                CanonicalAction::ExecutionContinueUntil),
            std::array{request},
            ActionTarget(CanonicalActionIdentity(
                CanonicalAction::ExecutionContinueUntil)),
            selector,
            std::nullopt,
            {}),
        "ContinueUntil observation");
}

void BranchToComplete(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    ProgramBlockId complete,
    ProgramValueId result,
    std::string selector)
{
    builder.SetTerminator(
        function,
        block,
        Terminator{
            .kind = TerminatorKind::Branch,
            .edges = {{complete, {result}}},
        },
        std::move(selector));
}

void LowerInvalid(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    ProgramBlockId complete,
    TasMovieValidationFailureReasonV1 reason,
    ProgramValueId expected_pc,
    ProgramValueId optional_expected_count,
    ProgramValueId actual_pc,
    ProgramValueId actual_count_u64,
    ProgramValueId optional_last_verified,
    ProgramScopeId scope,
    std::string prefix)
{
    const ProgramValueId actual_count = Construct(
        builder,
        function,
        block,
        InputCountType(),
        std::array{actual_count_u64},
        prefix + "/actual-count",
        scope);
    const ProgramValueId diagnostics = Construct(
        builder,
        function,
        block,
        DiagnosticsType(),
        std::array{
            expected_pc,
            optional_expected_count,
            actual_pc,
            actual_count,
            optional_last_verified,
        },
        prefix + "/diagnostics",
        scope);
    const ProgramValueId reason_value = ConstantEnum(
        builder,
        function,
        block,
        TasMovieValidationFailureReasonSchemaIdentityV1(),
        static_cast<std::int64_t>(reason),
        prefix + "/reason",
        scope);
    const ProgramValueId failure = Construct(
        builder,
        function,
        block,
        FailureType(),
        std::array{reason_value, diagnostics},
        prefix + "/failure",
        scope);
    const ProgramValueId no_candidate = Optional(
        builder,
        function,
        block,
        TypeRef::Named(OptionalCheckpointSchemaIdentity()),
        std::nullopt,
        prefix + "/no-candidate",
        scope);
    const ProgramValueId optional_failure = Optional(
        builder,
        function,
        block,
        TypeRef::Named(OptionalFailureSchemaIdentity()),
        failure,
        prefix + "/optional-failure",
        scope);
    const ProgramValueId outcome = ConstantEnum(
        builder,
        function,
        block,
        TasMovieValidationOutcomeSchemaIdentityV1(),
        static_cast<std::int64_t>(
            TasMovieValidationOutcomeV1::Invalid),
        prefix + "/outcome",
        scope);
    const ProgramValueId result = Construct(
        builder,
        function,
        block,
        ResultType(),
        std::array{outcome, no_candidate, optional_failure},
        prefix + "/result",
        scope);
    BranchToComplete(
        builder,
        function,
        block,
        complete,
        result,
        prefix + "/complete");
}

void AddLocalTypes(
    ModuleFragmentBuilder& builder)
{
    builder.AddLocalType({
        .identity = DtmInputCountSchemaIdentityV1(),
        .kind = TypeSchemaKind::Record,
        .record_fields = {{
            "value", TypeRef::Builtin(BuiltinType::U64)}},
    });
    builder.AddLocalType({
        .identity = TasMovieValidationOperationSchemaIdentityV1(),
        .kind = TypeSchemaKind::ClosedEnum,
        .enum_members = {
            {"EstablishRootCursor", static_cast<std::int64_t>(
                 TasMovieValidationOperationV1::EstablishRootCursor)},
            {"Validate", static_cast<std::int64_t>(
                 TasMovieValidationOperationV1::Validate)},
        },
    });
    builder.AddLocalType({
        .identity = TasMovieCheckpointSchemaIdentityV1(),
        .kind = TypeSchemaKind::Record,
        .record_fields = {
            {"pc", TypeRef::Builtin(BuiltinType::U32)},
            {"input_count", InputCountType()},
        },
    });
    builder.AddLocalType({
        .identity = TasMovieItinerarySchemaIdentityV1(),
        .kind = TypeSchemaKind::BoundedList,
        .maximum_size = MaximumItineraryEntries,
        .element_type = CheckpointType(),
    });
    builder.AddLocalType({
        .identity = TasMovieValidationOutcomeSchemaIdentityV1(),
        .kind = TypeSchemaKind::ClosedEnum,
        .enum_members = {
            {"RootCursorEstablished", static_cast<std::int64_t>(
                 TasMovieValidationOutcomeV1::RootCursorEstablished)},
            {"Valid", static_cast<std::int64_t>(
                 TasMovieValidationOutcomeV1::Valid)},
            {"Invalid", static_cast<std::int64_t>(
                 TasMovieValidationOutcomeV1::Invalid)},
        },
    });
    builder.AddLocalType({
        .identity = TasMovieValidationFailureReasonSchemaIdentityV1(),
        .kind = TypeSchemaKind::ClosedEnum,
        .enum_members = {
            {"MovieDesynchronized", static_cast<std::int64_t>(
                 TasMovieValidationFailureReasonV1::MovieDesynchronized)},
            {"ExpectedTerminalNotReached", static_cast<std::int64_t>(
                 TasMovieValidationFailureReasonV1::ExpectedTerminalNotReached)},
            {"Unknown", static_cast<std::int64_t>(
                 TasMovieValidationFailureReasonV1::Unknown)},
        },
    });
    builder.AddLocalType({
        .identity = OptionalInputCountSchemaIdentity(),
        .kind = TypeSchemaKind::Optional,
        .element_type = InputCountType(),
    });
    builder.AddLocalType({
        .identity = OptionalItineraryIndexSchemaIdentity(),
        .kind = TypeSchemaKind::Optional,
        .element_type = TypeRef::Builtin(BuiltinType::U64),
    });
    builder.AddLocalType({
        .identity = TasMovieValidationFailureDiagnosticsSchemaIdentityV1(),
        .kind = TypeSchemaKind::Record,
        .record_fields = {
            {"expected_pc", TypeRef::Builtin(BuiltinType::U32)},
            {"expected_input_count", TypeRef::Named(
                 OptionalInputCountSchemaIdentity())},
            {"actual_pc", TypeRef::Builtin(BuiltinType::U32)},
            {"actual_input_count", InputCountType()},
            {"last_verified_itinerary_index", TypeRef::Named(
                 OptionalItineraryIndexSchemaIdentity())},
        },
    });
    builder.AddLocalType({
        .identity = TasMovieValidationFailureSchemaIdentityV1(),
        .kind = TypeSchemaKind::Record,
        .record_fields = {
            {"reason", FailureReasonType()},
            {"diagnostics", DiagnosticsType()},
        },
    });
    builder.AddLocalType({
        .identity = OptionalCheckpointSchemaIdentity(),
        .kind = TypeSchemaKind::Optional,
        .element_type = CheckpointType(),
    });
    builder.AddLocalType({
        .identity = OptionalFailureSchemaIdentity(),
        .kind = TypeSchemaKind::Optional,
        .element_type = FailureType(),
    });
    builder.AddLocalType({
        .identity = TasMovieValidationResultSchemaIdentityV1(),
        .kind = TypeSchemaKind::Record,
        .record_fields = {
            {"outcome", OutcomeType()},
            {"candidate_checkpoint", TypeRef::Named(
                 OptionalCheckpointSchemaIdentity())},
            {"failure", TypeRef::Named(
                 OptionalFailureSchemaIdentity())},
        },
    });
    builder.AddLocalType({
        .identity = OptionalCaptureRequestSchemaIdentity(),
        .kind = TypeSchemaKind::Optional,
        .element_type = CanonicalActionInputType(
            CanonicalAction::SavestateSaveImmutableArtifact),
    });
    builder.AddLocalType({
        .identity = ModuleRequestSchemaIdentity(),
        .kind = TypeSchemaKind::Record,
        .record_fields = {
            {"operation", OperationType()},
            {"prepare_request", CanonicalActionInputType(
                 CanonicalAction::MoviePrepareReadOnlyPlayback)},
            {"itinerary", ItineraryType()},
            {"capture_request", TypeRef::Named(
                 OptionalCaptureRequestSchemaIdentity())},
        },
    });
}

ProgramModule ConstructTasMovieValidationModuleV1()
{
    ProgramModule module{
        .identity = {
            .canonical_id = std::string(ModuleCanonicalId),
            .revision = ModuleRevision,
        },
    };
    ModuleFragmentBuilder builder(
        module,
        "SavorCore/TasMovieValidationModule",
        "soa.tas_movie_validation/validate");
    AddLocalTypes(builder);
    for (CanonicalAction action : {
             CanonicalAction::MoviePrepareReadOnlyPlayback,
             CanonicalAction::MovieStartPlayback,
             CanonicalAction::ExecutionContinueUntil,
             CanonicalAction::SavestateSaveImmutableArtifact,
         })
    {
        AddCanonicalAction(builder, action);
    }
    builder.AddCapabilityImport(CanonicalRuntimePackIdentity());
    builder.AddCapabilityImport(capabilities::FieldPackIdentity());

    const ValueDefinition request = builder.NewArgument(
        ModuleRequestType());
    ProgramFunction& function = builder.AddFunction(
        std::string(Entrypoint),
        std::array{request},
        ResultType(),
        TypeRef::Builtin(BuiltinType::Bool),
        true);
    function.blocks.reserve(32);

    BasicBlock& entry = builder.AddBlock(function);
    BasicBlock& establish_config = builder.AddBlock(function);
    BasicBlock& validate_config = builder.AddBlock(function);
    const ValueDefinition setup_config_argument = builder.NewArgument(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::SemanticPointSet));
    BasicBlock& setup = builder.AddBlock(
        function,
        std::array{setup_config_argument});
    BasicBlock& establish = builder.AddBlock(function);
    BasicBlock& establish_breakpoint = builder.AddBlock(
        function,
        std::array{builder.NewArgument(CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil))});
    BasicBlock& establish_movie_end = builder.AddBlock(
        function,
        std::array{builder.NewArgument(CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil))});
    BasicBlock& establish_unknown = builder.AddBlock(
        function,
        std::array{builder.NewArgument(CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil))});
    BasicBlock& validate_init = builder.AddBlock(function);

    const ValueDefinition loop_index = builder.NewArgument(
        TypeRef::Builtin(BuiltinType::U64));
    const ValueDefinition loop_last = builder.NewArgument(
        TypeRef::Named(OptionalItineraryIndexSchemaIdentity()));
    BasicBlock& validate_loop = builder.AddBlock(
        function,
        std::array{loop_index, loop_last});

    const auto validation_arguments = [&]() {
        return std::array{
            builder.NewArgument(CanonicalActionOutputType(
                CanonicalAction::ExecutionContinueUntil)),
            builder.NewArgument(TypeRef::Builtin(BuiltinType::U64)),
            builder.NewArgument(TypeRef::Named(
                OptionalItineraryIndexSchemaIdentity())),
            builder.NewArgument(TypeRef::Builtin(BuiltinType::U64)),
            builder.NewArgument(TypeRef::Builtin(BuiltinType::U32)),
            builder.NewArgument(InputCountType()),
            builder.NewArgument(TypeRef::Builtin(BuiltinType::U64)),
        };
    };
    const auto breakpoint_arguments = validation_arguments();
    BasicBlock& validate_breakpoint = builder.AddBlock(
        function,
        breakpoint_arguments);
    const auto cursor_arguments = validation_arguments();
    BasicBlock& validate_cursor = builder.AddBlock(
        function,
        cursor_arguments);
    const auto movie_arguments = validation_arguments();
    BasicBlock& validate_movie_end = builder.AddBlock(
        function,
        movie_arguments);

    const auto not_below_arguments = [&]() {
        return std::array{
            builder.NewArgument(TypeRef::Builtin(BuiltinType::U64)),
            builder.NewArgument(TypeRef::Named(
                OptionalItineraryIndexSchemaIdentity())),
            builder.NewArgument(TypeRef::Builtin(BuiltinType::U64)),
            builder.NewArgument(TypeRef::Builtin(BuiltinType::U32)),
            builder.NewArgument(InputCountType()),
            builder.NewArgument(TypeRef::Builtin(BuiltinType::U64)),
            builder.NewArgument(TypeRef::Builtin(BuiltinType::U32)),
            builder.NewArgument(TypeRef::Builtin(BuiltinType::U64)),
        };
    }();
    BasicBlock& validate_not_below = builder.AddBlock(
        function,
        not_below_arguments);
    const auto equality_arguments = not_below_arguments;
    // Block arguments must own distinct SSA identities.
    std::array<ValueDefinition, 8> distinct_equality_arguments;
    for (std::size_t index = 0;
         index < distinct_equality_arguments.size();
         ++index)
    {
        distinct_equality_arguments[index] = builder.NewArgument(
            equality_arguments[index].type);
    }
    BasicBlock& validate_equality = builder.AddBlock(
        function,
        distinct_equality_arguments);
    std::array<ValueDefinition, 6> accepted_arguments{
        builder.NewArgument(TypeRef::Builtin(BuiltinType::U64)),
        builder.NewArgument(TypeRef::Named(
            OptionalItineraryIndexSchemaIdentity())),
        builder.NewArgument(TypeRef::Builtin(BuiltinType::U64)),
        builder.NewArgument(TypeRef::Builtin(BuiltinType::U32)),
        builder.NewArgument(InputCountType()),
        builder.NewArgument(TypeRef::Builtin(BuiltinType::U64)),
    };
    BasicBlock& validate_accepted = builder.AddBlock(
        function,
        accepted_arguments);

    std::array<ValueDefinition, 3> final_arguments{
        builder.NewArgument(TypeRef::Builtin(BuiltinType::U32)),
        builder.NewArgument(InputCountType()),
        builder.NewArgument(TypeRef::Named(
            OptionalItineraryIndexSchemaIdentity())),
    };
    BasicBlock& capture_check = builder.AddBlock(
        function,
        final_arguments);
    std::array<ValueDefinition, 3> capture_arguments;
    std::array<ValueDefinition, 3> no_capture_arguments;
    std::array<ValueDefinition, 3> tail_arguments;
    for (std::size_t index = 0; index < final_arguments.size(); ++index)
    {
        capture_arguments[index] = builder.NewArgument(
            final_arguments[index].type);
        no_capture_arguments[index] = builder.NewArgument(
            final_arguments[index].type);
        tail_arguments[index] = builder.NewArgument(
            final_arguments[index].type);
    }
    BasicBlock& capture = builder.AddBlock(function, capture_arguments);
    BasicBlock& no_capture = builder.AddBlock(
        function,
        no_capture_arguments);
    BasicBlock& tail = builder.AddBlock(function, tail_arguments);

    std::array<ValueDefinition, 4> tail_unknown_arguments{
        builder.NewArgument(CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil)),
        builder.NewArgument(TypeRef::Builtin(BuiltinType::U32)),
        builder.NewArgument(InputCountType()),
        builder.NewArgument(TypeRef::Named(
            OptionalItineraryIndexSchemaIdentity())),
    };
    BasicBlock& tail_unknown = builder.AddBlock(
        function,
        tail_unknown_arguments);
    BasicBlock& valid = builder.AddBlock(function);
    const ValueDefinition completed_result = builder.NewArgument(ResultType());
    BasicBlock& complete = builder.AddBlock(
        function,
        std::array{completed_result});

    const ProgramScopeId scope = builder.NewScope();

    std::array<ValueDefinition, 8> desync_arguments;
    for (std::size_t index = 0; index < desync_arguments.size(); ++index)
    {
        desync_arguments[index] = builder.NewArgument(
            not_below_arguments[index].type);
    }
    BasicBlock& validate_desync = builder.AddBlock(
        function,
        desync_arguments);

    // Select the immutable breakpoint catalog before entering the shared
    // setup path. The two catalogs are intentionally encoded independently
    // even while they contain the same initial boundary.
    const ProgramValueId operation = Project(
        builder,
        function,
        entry,
        OperationType(),
        request.id,
        "operation",
        scope);
    builder.SetTerminator(
        function,
        entry,
        Terminator{
            .kind = TerminatorKind::EnumSwitch,
            .condition_or_selector = operation,
            .enum_cases = {
                {
                    .enum_value = static_cast<std::int64_t>(
                        TasMovieValidationOperationV1::EstablishRootCursor),
                    .edge = {.target = establish_config.id},
                },
                {
                    .enum_value = static_cast<std::int64_t>(
                        TasMovieValidationOperationV1::Validate),
                    .edge = {.target = validate_config.id},
                },
            },
        },
        "operation/select-stop-catalog");

    const std::array<composition::SemanticPointReference, 1> root_points{{
        {
            .capability_pack = capabilities::FieldPackIdentity(),
            .canonical_id = std::string(BeforeRandSeedSetPointId),
            .kind = program::SemanticPointKind::ProgramCounter,
            .physical_pc = BeforeRandSeedSetPc,
        },
    }};
    const ProgramValueId root_stop_config = ConstantBytes(
        builder,
        function,
        establish_config,
        CanonicalRuntimeSchema::SemanticPointSet,
        composition::EncodeSemanticPointSetV1(root_points),
        "root/stop-catalog",
        scope);
    builder.SetTerminator(
        function,
        establish_config,
        Terminator{
            .kind = TerminatorKind::Branch,
            .edges = {{setup.id, {root_stop_config}}},
        },
        "root/shared-setup");

    const std::array<composition::SemanticPointReference, 3> global_points{{
        {
            .capability_pack = capabilities::FieldPackIdentity(),
            .canonical_id = std::string(BeforeRandSeedSetPointId),
            .kind = program::SemanticPointKind::ProgramCounter,
            .physical_pc = BeforeRandSeedSetPc,
        },
        {
            .capability_pack = capabilities::FieldPackIdentity(),
            .canonical_id = std::string(FieldFastPreseedPointId),
            .kind = program::SemanticPointKind::ProgramCounter,
            .physical_pc = FieldFastPreseedPc,
        },
        {
            .capability_pack = capabilities::FieldPackIdentity(),
            .canonical_id = std::string(FieldDeferredPreseedPointId),
            .kind = program::SemanticPointKind::ProgramCounter,
            .physical_pc = FieldDeferredPreseedPc,
        },
    }};
    const ProgramValueId global_stop_config = ConstantBytes(
        builder,
        function,
        validate_config,
        CanonicalRuntimeSchema::SemanticPointSet,
        composition::EncodeSemanticPointSetV1(global_points),
        "validation/stop-catalog",
        scope);
    builder.SetTerminator(
        function,
        validate_config,
        Terminator{
            .kind = TerminatorKind::Branch,
            .edges = {{setup.id, {global_stop_config}}},
        },
        "validation/shared-setup");

    (void)builder.AddInstruction(
        function,
        setup,
        InstructionOpcode::EnterScope,
        std::nullopt,
        {},
        {},
        "scope/playback-and-stop-group",
        std::nullopt,
        scope);
    const ProgramValueId prepare_request = Project(
        builder,
        function,
        setup,
        CanonicalActionInputType(
            CanonicalAction::MoviePrepareReadOnlyPlayback),
        request.id,
        "prepare_request",
        scope);
    const ProgramValueId prepared_playback_handle = Required(
        builder.AddInstruction(
            function,
            setup,
            InstructionOpcode::AwaitAction,
            CanonicalActionOutputType(
                CanonicalAction::MoviePrepareReadOnlyPlayback),
            std::array{prepare_request},
            ActionTarget(CanonicalActionIdentity(
                CanonicalAction::MoviePrepareReadOnlyPlayback)),
            "movie/prepare-and-stop-core",
            std::nullopt,
            scope),
        "prepared movie playback handle");
    const ProgramValueId semantic_points = setup_config_argument.id;
    const ProgramValueId playback_handle = Required(
        builder.AddInstruction(
            function,
            setup,
            InstructionOpcode::AwaitAction,
            CanonicalActionOutputType(
                CanonicalAction::MovieStartPlayback),
            std::array{prepared_playback_handle},
            ActionTarget(CanonicalActionIdentity(
                CanonicalAction::MovieStartPlayback)),
            "movie/start-exact-read-only-playback",
            std::nullopt,
            scope),
        "movie playback handle");
    const ProgramValueId no_publication = Optional(
        builder,
        function,
        setup,
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::OptionalInputExecutionBinding),
        std::nullopt,
        "continue/no-input-publication",
        scope);
    const ProgramValueId playback = Optional(
        builder,
        function,
        setup,
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::OptionalMoviePlaybackSession),
        playback_handle,
        "continue/playback-session",
        scope);
    const ProgramValueId no_expected_count = Optional(
        builder,
        function,
        setup,
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::OptionalMovieInputCount),
        std::nullopt,
        "continue/no-expected-count",
        scope);
    const ProgramValueId continue_config = ConstantBytes(
        builder,
        function,
        setup,
        CanonicalRuntimeSchema::ContinueUntilStaticConfig,
        ContinueConfig(),
        "continue/static-config-unthrottled",
        scope);
    builder.SetTerminator(
        function,
        setup,
        Terminator{
            .kind = TerminatorKind::EnumSwitch,
            .condition_or_selector = operation,
            .enum_cases = {
                {
                    .enum_value = static_cast<std::int64_t>(
                        TasMovieValidationOperationV1::EstablishRootCursor),
                    .edge = {.target = establish.id},
                },
                {
                    .enum_value = static_cast<std::int64_t>(
                        TasMovieValidationOperationV1::Validate),
                    .edge = {.target = validate_init.id},
                },
            },
        },
        "operation/enter-runtime");

    const ProgramValueId root_observation = Continue(
        builder,
        function,
        establish,
        semantic_points,
        no_publication,
        playback,
        no_expected_count,
        continue_config,
        scope,
        "root/continue-to-terminal");
    const ProgramValueId root_reason = Project(
        builder,
        function,
        establish,
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::ContinueUntilCompletionReason),
        root_observation,
        "reason",
        scope);
    builder.SetTerminator(
        function,
        establish,
        Terminator{
            .kind = TerminatorKind::EnumSwitch,
            .condition_or_selector = root_reason,
            .enum_cases = {
                {
                    .enum_value = static_cast<std::int64_t>(
                        ContinueUntilCompletionReasonV1::Breakpoint),
                    .edge = {
                        .target = establish_breakpoint.id,
                        .arguments = {root_observation},
                    },
                },
                {
                    .enum_value = static_cast<std::int64_t>(
                        ContinueUntilCompletionReasonV1::MovieEnded),
                    .edge = {
                        .target = establish_movie_end.id,
                        .arguments = {root_observation},
                    },
                },
                {
                    .enum_value = static_cast<std::int64_t>(
                        ContinueUntilCompletionReasonV1::CursorOverrun),
                    .edge = {
                        .target = establish_unknown.id,
                        .arguments = {root_observation},
                    },
                },
            },
        },
        "root/classify-terminal");

    const ProgramValueId established_pc = Project(
        builder,
        function,
        establish_breakpoint,
        TypeRef::Builtin(BuiltinType::U32),
        establish_breakpoint.arguments[0].id,
        "pc",
        scope);
    const ProgramValueId established_count_u64 = Project(
        builder,
        function,
        establish_breakpoint,
        TypeRef::Builtin(BuiltinType::U64),
        establish_breakpoint.arguments[0].id,
        "movie_input_count",
        scope);
    const ProgramValueId established_count = Construct(
        builder,
        function,
        establish_breakpoint,
        InputCountType(),
        std::array{established_count_u64},
        "root/candidate/input-count",
        scope);
    const ProgramValueId candidate = Construct(
        builder,
        function,
        establish_breakpoint,
        CheckpointType(),
        std::array{established_pc, established_count},
        "root/candidate",
        scope);
    const ProgramValueId optional_candidate = Optional(
        builder,
        function,
        establish_breakpoint,
        TypeRef::Named(OptionalCheckpointSchemaIdentity()),
        candidate,
        "root/optional-candidate",
        scope);
    const ProgramValueId no_failure = Optional(
        builder,
        function,
        establish_breakpoint,
        TypeRef::Named(OptionalFailureSchemaIdentity()),
        std::nullopt,
        "root/no-failure",
        scope);
    const ProgramValueId established_outcome = ConstantEnum(
        builder,
        function,
        establish_breakpoint,
        TasMovieValidationOutcomeSchemaIdentityV1(),
        static_cast<std::int64_t>(
            TasMovieValidationOutcomeV1::RootCursorEstablished),
        "root/outcome",
        scope);
    const ProgramValueId established_result = Construct(
        builder,
        function,
        establish_breakpoint,
        ResultType(),
        std::array{
            established_outcome,
            optional_candidate,
            no_failure,
        },
        "root/result",
        scope);
    BranchToComplete(
        builder,
        function,
        establish_breakpoint,
        complete.id,
        established_result,
        "root/complete");

    const auto lower_establishment_failure = [&]
        (BasicBlock& block,
         ProgramValueId observation,
         TasMovieValidationFailureReasonV1 reason,
         std::string prefix)
    {
        const ProgramValueId expected_pc = ConstantU32(
            builder,
            function,
            block,
            BeforeRandSeedSetPc,
            prefix + "/expected-pc",
            scope);
        const ProgramValueId no_expected = Optional(
            builder,
            function,
            block,
            TypeRef::Named(OptionalInputCountSchemaIdentity()),
            std::nullopt,
            prefix + "/no-expected-count",
            scope);
        const ProgramValueId actual_pc = Project(
            builder,
            function,
            block,
            TypeRef::Builtin(BuiltinType::U32),
            observation,
            "pc",
            scope);
        const ProgramValueId actual_count = Project(
            builder,
            function,
            block,
            TypeRef::Builtin(BuiltinType::U64),
            observation,
            "movie_input_count",
            scope);
        const ProgramValueId no_last = Optional(
            builder,
            function,
            block,
            TypeRef::Named(OptionalItineraryIndexSchemaIdentity()),
            std::nullopt,
            prefix + "/no-last-verified",
            scope);
        LowerInvalid(
            builder,
            function,
            block,
            complete.id,
            reason,
            expected_pc,
            no_expected,
            actual_pc,
            actual_count,
            no_last,
            scope,
            std::move(prefix));
    };
    lower_establishment_failure(
        establish_movie_end,
        establish_movie_end.arguments[0].id,
        TasMovieValidationFailureReasonV1::ExpectedTerminalNotReached,
        "root/movie-ended");
    lower_establishment_failure(
        establish_unknown,
        establish_unknown.arguments[0].id,
        TasMovieValidationFailureReasonV1::Unknown,
        "root/unexpected-cursor-overrun");

    const ProgramValueId itinerary = Project(
        builder,
        function,
        validate_init,
        ItineraryType(),
        request.id,
        "itinerary",
        scope);
    const ProgramValueId itinerary_size = Required(
        builder.AddInstruction(
            function,
            validate_init,
            InstructionOpcode::ListSize,
            TypeRef::Builtin(BuiltinType::U64),
            std::array{itinerary},
            {},
            "validation/itinerary-size",
            std::nullopt,
            scope),
        "itinerary size");
    const ProgramValueId first_index = ConstantU64(
        builder,
        function,
        validate_init,
        0,
        "validation/first-index",
        scope);
    const ProgramValueId no_last_verified = Optional(
        builder,
        function,
        validate_init,
        TypeRef::Named(OptionalItineraryIndexSchemaIdentity()),
        std::nullopt,
        "validation/no-last-verified",
        scope);
    builder.SetTerminator(
        function,
        validate_init,
        Terminator{
            .kind = TerminatorKind::Branch,
            .edges = {{
                .target = validate_loop.id,
                .arguments = {first_index, no_last_verified},
            }},
        },
        "validation/first-checkpoint");

    const ProgramValueId current_checkpoint = Required(
        builder.AddInstruction(
            function,
            validate_loop,
            InstructionOpcode::ListIndex,
            CheckpointType(),
            std::array{itinerary, loop_index.id},
            {},
            "validation/current-checkpoint",
            std::nullopt,
            scope),
        "itinerary checkpoint");
    const ProgramValueId expected_pc = Project(
        builder,
        function,
        validate_loop,
        TypeRef::Builtin(BuiltinType::U32),
        current_checkpoint,
        "pc",
        scope);
    const ProgramValueId expected_count_record = Project(
        builder,
        function,
        validate_loop,
        InputCountType(),
        current_checkpoint,
        "input_count",
        scope);
    const ProgramValueId expected_count_u64 = Project(
        builder,
        function,
        validate_loop,
        TypeRef::Builtin(BuiltinType::U64),
        expected_count_record,
        "value",
        scope);
    const ProgramValueId expected_movie_count = Optional(
        builder,
        function,
        validate_loop,
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::OptionalMovieInputCount),
        expected_count_u64,
        "validation/expected-movie-count",
        scope);
    const ProgramValueId validation_observation = Continue(
        builder,
        function,
        validate_loop,
        semantic_points,
        no_publication,
        playback,
        expected_movie_count,
        continue_config,
        scope,
        "validation/continue");
    const ProgramValueId validation_reason = Project(
        builder,
        function,
        validate_loop,
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::ContinueUntilCompletionReason),
        validation_observation,
        "reason",
        scope);
    const std::vector<ProgramValueId> validation_edge_arguments{
        validation_observation,
        loop_index.id,
        loop_last.id,
        itinerary_size,
        expected_pc,
        expected_count_record,
        expected_count_u64,
    };
    builder.SetTerminator(
        function,
        validate_loop,
        Terminator{
            .kind = TerminatorKind::EnumSwitch,
            .condition_or_selector = validation_reason,
            .enum_cases = {
                {
                    .enum_value = static_cast<std::int64_t>(
                        ContinueUntilCompletionReasonV1::Breakpoint),
                    .edge = {
                        .target = validate_breakpoint.id,
                        .arguments = validation_edge_arguments,
                    },
                },
                {
                    .enum_value = static_cast<std::int64_t>(
                        ContinueUntilCompletionReasonV1::CursorOverrun),
                    .edge = {
                        .target = validate_cursor.id,
                        .arguments = validation_edge_arguments,
                    },
                },
                {
                    .enum_value = static_cast<std::int64_t>(
                        ContinueUntilCompletionReasonV1::MovieEnded),
                    .edge = {
                        .target = validate_movie_end.id,
                        .arguments = validation_edge_arguments,
                    },
                },
            },
        },
        "validation/classify-observation");

    const ProgramValueId observed_breakpoint_pc = Project(
        builder,
        function,
        validate_breakpoint,
        TypeRef::Builtin(BuiltinType::U32),
        breakpoint_arguments[0].id,
        "pc",
        scope);
    const ProgramValueId observed_breakpoint_count = Project(
        builder,
        function,
        validate_breakpoint,
        TypeRef::Builtin(BuiltinType::U64),
        breakpoint_arguments[0].id,
        "movie_input_count",
        scope);
    const ProgramValueId below_expected = Binary(
        builder,
        function,
        validate_breakpoint,
        InstructionOpcode::Less,
        TypeRef::Builtin(BuiltinType::Bool),
        observed_breakpoint_count,
        breakpoint_arguments[6].id,
        "validation/breakpoint-below-expected",
        scope);
    builder.SetTerminator(
        function,
        validate_breakpoint,
        Terminator{
            .kind = TerminatorKind::ConditionalBranch,
            .condition_or_selector = below_expected,
            .edges = {
                {
                    .target = validate_loop.id,
                    .arguments = {
                        breakpoint_arguments[1].id,
                        breakpoint_arguments[2].id,
                    },
                },
                {
                    .target = validate_not_below.id,
                    .arguments = {
                        breakpoint_arguments[1].id,
                        breakpoint_arguments[2].id,
                        breakpoint_arguments[3].id,
                        breakpoint_arguments[4].id,
                        breakpoint_arguments[5].id,
                        breakpoint_arguments[6].id,
                        observed_breakpoint_pc,
                        observed_breakpoint_count,
                    },
                },
            },
        },
        "validation/below-expected-retry");

    const ProgramValueId above_expected = Binary(
        builder,
        function,
        validate_not_below,
        InstructionOpcode::Greater,
        TypeRef::Builtin(BuiltinType::Bool),
        not_below_arguments[7].id,
        not_below_arguments[5].id,
        "validation/breakpoint-above-expected",
        scope);
    builder.SetTerminator(
        function,
        validate_not_below,
        Terminator{
            .kind = TerminatorKind::ConditionalBranch,
            .condition_or_selector = above_expected,
            .edges = {
                {
                    .target = validate_desync.id,
                    .arguments = {
                        not_below_arguments[0].id,
                        not_below_arguments[1].id,
                        not_below_arguments[2].id,
                        not_below_arguments[3].id,
                        not_below_arguments[4].id,
                        not_below_arguments[5].id,
                        not_below_arguments[6].id,
                        not_below_arguments[7].id,
                    },
                },
                {
                    .target = validate_equality.id,
                    .arguments = {
                        not_below_arguments[0].id,
                        not_below_arguments[1].id,
                        not_below_arguments[2].id,
                        not_below_arguments[3].id,
                        not_below_arguments[4].id,
                        not_below_arguments[5].id,
                        not_below_arguments[6].id,
                        not_below_arguments[7].id,
                    },
                },
            },
        },
        "validation/above-expected-invalid");

    LowerInvalid(
        builder,
        function,
        validate_desync,
        complete.id,
        TasMovieValidationFailureReasonV1::MovieDesynchronized,
        desync_arguments[3].id,
        Optional(
            builder,
            function,
            validate_desync,
            TypeRef::Named(OptionalInputCountSchemaIdentity()),
            desync_arguments[4].id,
            "validation/desync/expected-count",
            scope),
        desync_arguments[6].id,
        desync_arguments[7].id,
        desync_arguments[1].id,
        scope,
        "validation/desync");

    const ProgramValueId pc_matches = Binary(
        builder,
        function,
        validate_equality,
        InstructionOpcode::Equal,
        TypeRef::Builtin(BuiltinType::Bool),
        distinct_equality_arguments[6].id,
        distinct_equality_arguments[3].id,
        "validation/equal-count-pc-matches",
        scope);
    builder.SetTerminator(
        function,
        validate_equality,
        Terminator{
            .kind = TerminatorKind::ConditionalBranch,
            .condition_or_selector = pc_matches,
            .edges = {
                {
                    .target = validate_accepted.id,
                    .arguments = {
                        distinct_equality_arguments[0].id,
                        distinct_equality_arguments[1].id,
                        distinct_equality_arguments[2].id,
                        distinct_equality_arguments[3].id,
                        distinct_equality_arguments[4].id,
                        distinct_equality_arguments[5].id,
                    },
                },
                {
                    .target = validate_loop.id,
                    .arguments = {
                        distinct_equality_arguments[0].id,
                        distinct_equality_arguments[1].id,
                    },
                },
            },
        },
        "validation/equal-count-pc-dispatch");

    const ProgramValueId accepted_last = Optional(
        builder,
        function,
        validate_accepted,
        TypeRef::Named(OptionalItineraryIndexSchemaIdentity()),
        accepted_arguments[0].id,
        "validation/accepted-last-index",
        scope);
    const ProgramValueId one = ConstantU64(
        builder,
        function,
        validate_accepted,
        1,
        "validation/one",
        scope);
    const ProgramValueId next_index = Binary(
        builder,
        function,
        validate_accepted,
        InstructionOpcode::AddChecked,
        TypeRef::Builtin(BuiltinType::U64),
        accepted_arguments[0].id,
        one,
        "validation/next-index",
        scope);
    const ProgramValueId has_next = Binary(
        builder,
        function,
        validate_accepted,
        InstructionOpcode::Less,
        TypeRef::Builtin(BuiltinType::Bool),
        next_index,
        accepted_arguments[2].id,
        "validation/has-next-checkpoint",
        scope);
    builder.SetTerminator(
        function,
        validate_accepted,
        Terminator{
            .kind = TerminatorKind::ConditionalBranch,
            .condition_or_selector = has_next,
            .edges = {
                {
                    .target = validate_loop.id,
                    .arguments = {next_index, accepted_last},
                },
                {
                    .target = capture_check.id,
                    .arguments = {
                        accepted_arguments[3].id,
                        accepted_arguments[4].id,
                        accepted_last,
                    },
                },
            },
        },
        "validation/advance-or-finalize");

    const auto lower_validation_terminal = [&]
        (BasicBlock& block,
         const std::array<ValueDefinition, 7>& arguments,
         TasMovieValidationFailureReasonV1 reason,
         std::string prefix)
    {
        const ProgramValueId actual_pc = Project(
            builder,
            function,
            block,
            TypeRef::Builtin(BuiltinType::U32),
            arguments[0].id,
            "pc",
            scope);
        const ProgramValueId actual_count = Project(
            builder,
            function,
            block,
            TypeRef::Builtin(BuiltinType::U64),
            arguments[0].id,
            "movie_input_count",
            scope);
        const ProgramValueId optional_expected = Optional(
            builder,
            function,
            block,
            TypeRef::Named(OptionalInputCountSchemaIdentity()),
            arguments[5].id,
            prefix + "/expected-count",
            scope);
        LowerInvalid(
            builder,
            function,
            block,
            complete.id,
            reason,
            arguments[4].id,
            optional_expected,
            actual_pc,
            actual_count,
            arguments[2].id,
            scope,
            std::move(prefix));
    };
    lower_validation_terminal(
        validate_cursor,
        cursor_arguments,
        TasMovieValidationFailureReasonV1::MovieDesynchronized,
        "validation/cursor-overrun");
    lower_validation_terminal(
        validate_movie_end,
        movie_arguments,
        TasMovieValidationFailureReasonV1::ExpectedTerminalNotReached,
        "validation/premature-movie-end");

    const ProgramValueId capture_request_optional = Project(
        builder,
        function,
        capture_check,
        TypeRef::Named(OptionalCaptureRequestSchemaIdentity()),
        request.id,
        "capture_request",
        scope);
    const ProgramValueId capture_present = Required(
        builder.AddInstruction(
            function,
            capture_check,
            InstructionOpcode::OptionalIsPresent,
            TypeRef::Builtin(BuiltinType::Bool),
            std::array{capture_request_optional},
            {},
            "capture/request-present",
            std::nullopt,
            scope),
        "capture presence");
    builder.SetTerminator(
        function,
        capture_check,
        Terminator{
            .kind = TerminatorKind::ConditionalBranch,
            .condition_or_selector = capture_present,
            .edges = {
                {
                    .target = capture.id,
                    .arguments = {
                        final_arguments[0].id,
                        final_arguments[1].id,
                        final_arguments[2].id,
                    },
                },
                {
                    .target = no_capture.id,
                    .arguments = {
                        final_arguments[0].id,
                        final_arguments[1].id,
                        final_arguments[2].id,
                    },
                },
            },
        },
        "capture/optional-dispatch");

    const ProgramValueId final_capture_optional = Project(
        builder,
        function,
        capture,
        TypeRef::Named(OptionalCaptureRequestSchemaIdentity()),
        request.id,
        "capture_request",
        scope);
    const ProgramValueId final_capture_request = Required(
        builder.AddInstruction(
            function,
            capture,
            InstructionOpcode::OptionalExtract,
            CanonicalActionInputType(
                CanonicalAction::SavestateSaveImmutableArtifact),
            std::array{final_capture_optional},
            {},
            "capture/extract-after-guard",
            std::nullopt,
            scope),
        "capture request extraction");
    (void)builder.AddInstruction(
        function,
        capture,
        InstructionOpcode::AwaitAction,
        CanonicalActionOutputType(
            CanonicalAction::SavestateSaveImmutableArtifact),
        std::array{final_capture_request},
        ActionTarget(CanonicalActionIdentity(
            CanonicalAction::SavestateSaveImmutableArtifact)),
        "capture/save-final-checkpoint-while-paused",
        std::nullopt,
        {});
    builder.SetTerminator(
        function,
        capture,
        Terminator{
            .kind = TerminatorKind::Branch,
            .edges = {{
                .target = tail.id,
                .arguments = {
                    capture_arguments[0].id,
                    capture_arguments[1].id,
                    capture_arguments[2].id,
                },
            }},
        },
        "capture/continue-tail");
    builder.SetTerminator(
        function,
        no_capture,
        Terminator{
            .kind = TerminatorKind::Branch,
            .edges = {{
                .target = tail.id,
                .arguments = {
                    no_capture_arguments[0].id,
                    no_capture_arguments[1].id,
                    no_capture_arguments[2].id,
                },
            }},
        },
        "capture/skipped-continue-tail");

    const ProgramValueId tail_no_expected = Optional(
        builder,
        function,
        tail,
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::OptionalMovieInputCount),
        std::nullopt,
        "tail/no-expected-count",
        scope);
    const ProgramValueId tail_observation = Continue(
        builder,
        function,
        tail,
        semantic_points,
        no_publication,
        playback,
        tail_no_expected,
        continue_config,
        scope,
        "tail/continue-to-movie-end");
    const ProgramValueId tail_reason = Project(
        builder,
        function,
        tail,
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::ContinueUntilCompletionReason),
        tail_observation,
        "reason",
        scope);
    builder.SetTerminator(
        function,
        tail,
        Terminator{
            .kind = TerminatorKind::EnumSwitch,
            .condition_or_selector = tail_reason,
            .enum_cases = {
                {
                    .enum_value = static_cast<std::int64_t>(
                        ContinueUntilCompletionReasonV1::Breakpoint),
                    .edge = {
                        .target = tail.id,
                        .arguments = {
                            tail_arguments[0].id,
                            tail_arguments[1].id,
                            tail_arguments[2].id,
                        },
                    },
                },
                {
                    .enum_value = static_cast<std::int64_t>(
                        ContinueUntilCompletionReasonV1::MovieEnded),
                    .edge = {.target = valid.id},
                },
                {
                    .enum_value = static_cast<std::int64_t>(
                        ContinueUntilCompletionReasonV1::CursorOverrun),
                    .edge = {
                        .target = tail_unknown.id,
                        .arguments = {
                            tail_observation,
                            tail_arguments[0].id,
                            tail_arguments[1].id,
                            tail_arguments[2].id,
                        },
                    },
                },
            },
        },
        "tail/classify-observation");

    const ProgramValueId tail_actual_pc = Project(
        builder,
        function,
        tail_unknown,
        TypeRef::Builtin(BuiltinType::U32),
        tail_unknown_arguments[0].id,
        "pc",
        scope);
    const ProgramValueId tail_actual_count = Project(
        builder,
        function,
        tail_unknown,
        TypeRef::Builtin(BuiltinType::U64),
        tail_unknown_arguments[0].id,
        "movie_input_count",
        scope);
    const ProgramValueId tail_expected = Optional(
        builder,
        function,
        tail_unknown,
        TypeRef::Named(OptionalInputCountSchemaIdentity()),
        tail_unknown_arguments[2].id,
        "tail/expected-final-count",
        scope);
    LowerInvalid(
        builder,
        function,
        tail_unknown,
        complete.id,
        TasMovieValidationFailureReasonV1::Unknown,
        tail_unknown_arguments[1].id,
        tail_expected,
        tail_actual_pc,
        tail_actual_count,
        tail_unknown_arguments[3].id,
        scope,
        "tail/unexpected-cursor-overrun");

    const ProgramValueId valid_outcome = ConstantEnum(
        builder,
        function,
        valid,
        TasMovieValidationOutcomeSchemaIdentityV1(),
        static_cast<std::int64_t>(TasMovieValidationOutcomeV1::Valid),
        "validation/valid-outcome",
        scope);
    const ProgramValueId valid_no_candidate = Optional(
        builder,
        function,
        valid,
        TypeRef::Named(OptionalCheckpointSchemaIdentity()),
        std::nullopt,
        "validation/valid-no-candidate",
        scope);
    const ProgramValueId valid_no_failure = Optional(
        builder,
        function,
        valid,
        TypeRef::Named(OptionalFailureSchemaIdentity()),
        std::nullopt,
        "validation/valid-no-failure",
        scope);
    const ProgramValueId valid_result = Construct(
        builder,
        function,
        valid,
        ResultType(),
        std::array{
            valid_outcome,
            valid_no_candidate,
            valid_no_failure,
        },
        "validation/valid-result",
        scope);
    BranchToComplete(
        builder,
        function,
        valid,
        complete.id,
        valid_result,
        "validation/complete-valid");

    const ProgramValueId domain_outcome = Constant(
        builder,
        function,
        complete,
        LiteralValue{
            TypeRef::Builtin(BuiltinType::Bool),
            true,
        },
        "domain/outcome-true",
        scope);
    (void)builder.AddInstruction(
        function,
        complete,
        InstructionOpcode::ExitScope,
        std::nullopt,
        {},
        {},
        "scope/release-playback-and-stop-group",
        std::nullopt,
        scope);
    builder.SetTerminator(
        function,
        complete,
        Terminator{
            .kind = TerminatorKind::Return,
            .return_value = completed_result.id,
            .domain_outcome = domain_outcome,
        },
        "return/typed-outcome");

    module.accepted_policies = {
        .state_policies = {
            InvocationStatePolicy::EstablishBaseline,
        },
        .execution_intents = {
            ExecutionIntent::Live,
        },
        .permits_movie_playback = true,
    };
    module.budgets = {
        .maximum_instructions = 2'000'000,
        .maximum_calls = 32,
        .maximum_call_depth = 8,
        .maximum_action_requests = 65'536,
        .maximum_emissions = 1,
        .maximum_artifacts = 1,
        .maximum_values = 1'000'000,
        .maximum_value_bytes = 256u * 1024u * 1024u,
        .maximum_trace_events = 262'144,
    };
    module.entrypoints = {{
        .name = std::string(Entrypoint),
        .function = function.id,
        .input_type = ModuleRequestType(),
        .output_type = ResultType(),
        .domain_outcome_type = TypeRef::Builtin(BuiltinType::Bool),
        .required_capability_packs = module.required_capability_packs,
        .accepted_policies = module.accepted_policies,
    }};
    module.identity.module_hash = ComputeProgramModuleHashV1(module);
    return module;
}

std::optional<ProgramDependencyLock> VerifyTasMovieValidationModuleV1(
    const ProgramModule& module,
    std::string* diagnostic)
{
    ProgramDefinitionStore modules;
    TypeSchemaRegistry schemas;
    ActionRegistry actions(&schemas);
    CapabilityPackRegistry packs(&schemas, &actions);
    const RegistryResult source_registered =
        capabilities::RegisterSourceCapabilityPacks(
            schemas,
            actions,
            packs);
    if (!source_registered.success)
    {
        SetDiagnostic(
            diagnostic,
            "TAS Movie Validation source capability registration failed: " +
                source_registered.error.message);
        return std::nullopt;
    }

    const auto stored = modules.RegisterCompiled(module);
    if (!stored.success)
    {
        SetDiagnostic(
            diagnostic,
            "TAS Movie Validation module registration failed: " +
                stored.error.message);
        return std::nullopt;
    }
    ProgramVerifier verifier(modules, schemas, actions, packs);
    const ProgramVerificationResult verified = verifier.Verify(
        stored.module->identity,
        capabilities::SupportedSoaUsaCompatibility());
    if (!verified.success || !verified.verified)
    {
        SetDiagnostic(
            diagnostic,
            verified.diagnostics.empty()
                ? "TAS Movie Validation module verification failed"
                : "TAS Movie Validation module verification failed: " +
                    verified.diagnostics.front().message);
        return std::nullopt;
    }
    return verified.verified->dependency_lock;
}

std::optional<EncodedModuleEnvelope>
EncodeTasMovieValidationModuleEnvelopeV1(
    const ProgramModule& module,
    std::string* diagnostic)
{
    SetDiagnostic(diagnostic, {});
    const EncodeResult encoded = EncodeProgramModuleV1(module);
    if (!encoded)
    {
        SetDiagnostic(
            diagnostic,
            "TAS Movie Validation module encoding failed: " +
                encoded.status.message);
        return std::nullopt;
    }
    return EncodedModuleEnvelope{
        .identity = {
            .canonical_id = module.identity.canonical_id,
            .revision = module.identity.revision,
            .canonical_hash = module.identity.module_hash.ToHex(),
        },
        .format_version = kProgramCodecVersionV1,
        .development_only = false,
        .payload = encoded.bytes,
    };
}

std::optional<ProgramInvocation> ResolveTasMovieValidationExecutionV1(
    const TasMovieValidationRequestV1& request,
    InvocationId invocation_id,
    AttemptId attempt_id,
    const ModuleIdentity& module_identity,
    const ProgramDependencyLock& dependencies,
    const RuntimeProfile& runtime_profile,
    const InvocationExecutionPolicy& execution,
    const ProgramBudgets& limits,
    std::string* diagnostic)
{
    SetDiagnostic(diagnostic, {});
    if (!invocation_id || !attempt_id)
    {
        SetDiagnostic(
            diagnostic,
            "TAS Movie Validation execution and attempt identities must be nonzero");
        return std::nullopt;
    }
    if (!PreflightRequest(request, diagnostic))
        return std::nullopt;
    ProgramValueGraph input = EncodeModuleRequest(request, diagnostic);
    if (!input.root || input.values.empty())
    {
        if (!diagnostic || diagnostic->empty())
        {
            SetDiagnostic(
                diagnostic,
                "TAS Movie Validation module request could not be encoded");
        }
        return std::nullopt;
    }
    return ProgramInvocation{
        .invocation_id = invocation_id,
        .attempt_id = attempt_id,
        .module = module_identity,
        .entrypoint = std::string(Entrypoint),
        .dependencies = dependencies,
        .runtime_profile = runtime_profile,
        .state = {
            .policy = InvocationStatePolicy::EstablishBaseline,
            .session_lineage = std::string(ArtifactLineage),
        },
        .execution = execution,
        .input = std::move(input),
        .limits = limits,
        .provenance = {
            .requesting_component = "SavorDb.PK_TasMovie",
            .attributes = {{
                "contract",
                "soa.tas_movie_validation/validate@1",
            }},
        },
    };
}

std::optional<std::string>
ComputeTasMovieValidationInvocationCompatibilityV1(
    const ModuleIdentity& module_identity,
    const ProgramDependencyLock& dependencies,
    const RuntimeProfile& runtime_profile,
    const InvocationExecutionPolicy& execution,
    const ProgramBudgets& limits,
    std::string* diagnostic)
{
    TasMovieValidationRequestV1 request{
        .operation = TasMovieValidationOperationV1::Validate,
        .dtm_path = "compatibility.dtm",
        .itinerary = {{
            {BeforeRandSeedSetPc, DtmInputCount{0}},
        }},
    };
    ProgramValueGraph input = EncodeModuleRequest(request, diagnostic);
    if (!input.root || input.values.empty())
        return std::nullopt;
    const ProgramInvocation invocation{
        .invocation_id = InvocationId(1),
        .attempt_id = AttemptId(1),
        .module = module_identity,
        .entrypoint = std::string(Entrypoint),
        .dependencies = dependencies,
        .runtime_profile = runtime_profile,
        .state = {
            .policy = InvocationStatePolicy::EstablishBaseline,
            .session_lineage = std::string(ArtifactLineage),
        },
        .execution = execution,
        .input = std::move(input),
        .limits = limits,
    };
    const std::string hash =
        ComputeProgramInvocationCompatibilityHashV1(invocation);
    if (hash.size() != 64)
    {
        SetDiagnostic(
            diagnostic,
            "TAS Movie Validation invocation compatibility hash is incomplete");
        return std::nullopt;
    }
    return hash;
}

bool DecodeTasMovieValidationProgramResultAgainstDefinition(
    std::span<const Byte> encoded_result,
    const ModuleIdentity& expected_module,
    const ProgramDependencyLock& expected_dependencies,
    TasMovieValidationResultV1& validation_result,
    std::string* diagnostic)
{
    SetDiagnostic(diagnostic, {});
    const auto decoded = DecodeProgramResultV1(encoded_result);
    if (!decoded)
    {
        SetDiagnostic(
            diagnostic,
            "TAS Movie Validation ProgramResultV1 is not canonical: " +
                decoded.status.message);
        return false;
    }
    const ProgramResult& result = *decoded.value;
    if (result.module != expected_module ||
        result.entrypoint != Entrypoint)
    {
        SetDiagnostic(
            diagnostic,
            "Program result does not belong to the exact TAS Movie Validation module and entrypoint");
        return false;
    }
    if (result.resolved_dependencies != expected_dependencies)
    {
        SetDiagnostic(
            diagnostic,
            "Program result does not carry the verified TAS Movie Validation dependency lock");
        return false;
    }
    if (result.infrastructure != ProgramInfrastructureStatus::Completed ||
        result.cleanup != ProgramCleanupStatus::Clean ||
        result.session_disposition != SessionDisposition::Clean ||
        !result.output || !result.domain_outcome)
    {
        SetDiagnostic(
            diagnostic,
            "TAS Movie Validation ProgramResult did not complete with clean output");
        return false;
    }

    GraphReader domain(*result.domain_outcome);
    const ProgramValue* domain_root = domain.root();
    const auto* succeeded = domain_root
        ? std::get_if<bool>(&domain_root->payload)
        : nullptr;
    if (!domain.valid() || !domain_root ||
        domain_root->type != TypeRef::Builtin(BuiltinType::Bool) ||
        !succeeded || !*succeeded)
    {
        SetDiagnostic(
            diagnostic,
            "TAS Movie Validation ProgramResult domain outcome is not true");
        return false;
    }
    return DecodeTasMovieValidationResultV1(
        *result.output,
        validation_result,
        diagnostic);
}

class TasMovieValidationFullPhaseDefinition final
    : public ITasMovieValidationFullPhaseDefinitionV1
{
public:
    TasMovieValidationFullPhaseDefinition()
    {
        std::string diagnostic;
        ProgramModule module = ConstructTasMovieValidationModuleV1();
        const auto dependencies = VerifyTasMovieValidationModuleV1(
            module,
            &diagnostic);
        if (!dependencies)
        {
            throw std::logic_error(
                "TAS Movie Validation Full Phase definition is invalid: " +
                diagnostic);
        }
        auto module_envelope =
            EncodeTasMovieValidationModuleEnvelopeV1(
                module,
                &diagnostic);
        if (!module_envelope)
        {
            throw std::logic_error(
                "TAS Movie Validation Full Phase definition is invalid: " +
                diagnostic);
        }
        const RuntimeProfile runtime_profile =
            InvocationRuntimeProfile(*dependencies);
        const ContentHash256 dependency_lock_hash =
            ComputeProgramDependencyLockHashV1(*dependencies);
        if (dependency_lock_hash.empty())
        {
            throw std::logic_error(
                "TAS Movie Validation dependency-lock hash could not be computed");
        }
        const InvocationExecutionPolicy execution{
            .intent = ExecutionIntent::Live,
            .allow_movie_playback = true,
            .allow_movie_recording = false,
            .allow_input = false,
            .allow_capture = false,
            .record_trace = false,
        };
        const auto compatibility =
            ComputeTasMovieValidationInvocationCompatibilityV1(
                module.identity,
                *dependencies,
                runtime_profile,
                execution,
                module.budgets,
                &diagnostic);
        if (!compatibility)
        {
            throw std::logic_error(
                "TAS Movie Validation Full Phase definition is invalid: " +
                diagnostic);
        }

        module_identity_ = module.identity;
        dependency_lock_ = *dependencies;
        runtime_profile_ = runtime_profile;
        module_envelope_ = std::move(*module_envelope);
        runtime_ = {
            .module = module_envelope_.identity,
            .entrypoint = std::string(Entrypoint),
            .dependency_lock_sha256 = dependency_lock_hash.ToHex(),
            .verified_dependency_sha256 = *compatibility,
            .runtime_profile_sha256 = RuntimeProfileHash(runtime_profile_),
            .state_policy = InvocationStatePolicy::EstablishBaseline,
            .execution = execution,
            .limits = module.budgets,
            .baseline_lineage = std::string(ArtifactLineage),
            .movie_policy_sha256 = []
            {
                constexpr std::string_view value =
                    "soa.tas_movie_validation/read-only-owned-playback/v1";
                return hash::sha256(value.data(), value.size());
            }(),
            .service_policy_sha256 = []
            {
                constexpr std::string_view value =
                    "soa.tas_movie_validation/passive-stop-state-artifact/v1";
                return hash::sha256(value.data(), value.size());
            }(),
        };

        std::string canonical = "savor.full_phase/definition/v1";
        const auto append = [&canonical](std::string_view value)
        {
            canonical.push_back('\0');
            canonical.append(value);
        };
        append(ModuleCanonicalId);
        append(std::to_string(savor::PK_TasMovie));
        append(std::to_string(ProgramVersion));
        append(runtime_.module.canonical_hash);
        append(runtime_.entrypoint);
        append(runtime_.dependency_lock_sha256);
        append(runtime_.verified_dependency_sha256);
        append(runtime_.runtime_profile_sha256);
        append(runtime_.baseline_lineage);
        append(runtime_.movie_policy_sha256);
        append(runtime_.service_policy_sha256);
        append("soa.tas_movie_validation/complete-validation/v1");
        identity_ = {
            .program_kind = static_cast<std::int32_t>(
                savor::PK_TasMovie),
            .program_version = ProgramVersion,
            .canonical_id = std::string(FullPhaseCanonicalId),
            .contract_revision = 1,
            .canonical_sha256 = hash::sha256(
                canonical.data(),
                canonical.size()),
        };
    }

    const fullphase::FullPhaseProgramIdentity& identity()
        const noexcept override
    {
        return identity_;
    }

    const fullphase::FullPhaseRuntimeContract& runtime_contract()
        const noexcept override
    {
        return runtime_;
    }

    const EncodedModuleEnvelope& module_envelope()
        const noexcept override
    {
        return module_envelope_;
    }

    std::optional<ProgramInvocation> BuildResolvedExecution(
        std::span<const std::uint8_t> input_payload,
        ProgramExecutionId execution_id,
        AttemptId attempt_id,
        std::string* diagnostic) const override
    {
        TasMovieValidationRequestV1 request;
        if (!DecodeTasMovieValidationExecutionInputV1(
                input_payload,
                request,
                diagnostic))
        {
            return std::nullopt;
        }
        return ResolveTasMovieValidationExecutionV1(
            request,
            execution_id,
            attempt_id,
            module_identity_,
            dependency_lock_,
            runtime_profile_,
            runtime_.execution,
            runtime_.limits,
            diagnostic);
    }

    bool DecodeProgramResult(
        std::span<const Byte> encoded_result,
        TasMovieValidationResultV1& result,
        std::string* diagnostic) const override
    {
        return DecodeTasMovieValidationProgramResultAgainstDefinition(
            encoded_result,
            module_identity_,
            dependency_lock_,
            result,
            diagnostic);
    }

private:
    fullphase::FullPhaseProgramIdentity identity_;
    fullphase::FullPhaseRuntimeContract runtime_;
    EncodedModuleEnvelope module_envelope_;
    ModuleIdentity module_identity_;
    ProgramDependencyLock dependency_lock_;
    RuntimeProfile runtime_profile_;
};

} // namespace

std::shared_ptr<const ITasMovieValidationFullPhaseDefinitionV1>
TasMovieValidationFullPhaseDefinitionV1()
{
    static const auto definition =
        std::make_shared<const TasMovieValidationFullPhaseDefinition>();
    return definition;
}

namespace {

SchemaIdentity SterilizationOutcomeSchemaIdentity()
{
    return composition::ExactSchema(
        "soa.tas_movie_checkpoint_sterilize.Outcome",
        1,
        "enum TasMovieCheckpointSterilizationOutcome/1{Sterilized=0}");
}

SchemaIdentity SterilizationResultSchemaIdentity()
{
    return composition::ExactSchema(
        "soa.tas_movie_checkpoint_sterilize.Result",
        1,
        "record TasMovieCheckpointSterilizationResult/1{outcome:" +
            TypeContract(TypeRef::Named(
                SterilizationOutcomeSchemaIdentity())) + "}");
}

TypeRef SterilizationOutcomeType()
{
    return TypeRef::Named(SterilizationOutcomeSchemaIdentity());
}

TypeRef SterilizationResultType()
{
    return TypeRef::Named(SterilizationResultSchemaIdentity());
}

bool ValidateSterilizationRequest(
    const TasMovieCheckpointSterilizationRequestV1& request,
    std::string* diagnostic)
{
    const auto valid_path = [](const std::string& value) {
        return !value.empty() && value.size() <= MaximumPathBytes;
    };
    if (!valid_path(request.source_savestate_path) ||
        !valid_path(request.source_dtm_path) ||
        !valid_path(request.output_savestate_path))
    {
        SetDiagnostic(
            diagnostic,
            "TCS1 requires three paths containing 1..4096 bytes");
        return false;
    }
    if (std::filesystem::path(request.source_dtm_path) !=
        std::filesystem::path(request.source_savestate_path + ".dtm"))
    {
        SetDiagnostic(
            diagnostic,
            "TCS1 source DTM must be the exact <savestate>.dtm sidecar");
        return false;
    }
    if (std::filesystem::path(request.output_savestate_path).extension() !=
            ".sav" ||
        std::filesystem::path(request.output_savestate_path) ==
            std::filesystem::path(request.source_savestate_path))
    {
        SetDiagnostic(
            diagnostic,
            "TCS1 output must be a distinct .sav path");
        return false;
    }
    return true;
}

class TcsWriter final
{
public:
    TcsWriter() { bytes_.insert(bytes_.end(), {'T', 'C', 'S', '1'}); }
    void String(std::string_view value)
    {
        const auto size = static_cast<std::uint32_t>(value.size());
        for (unsigned shift = 0; shift != 32; shift += 8)
            bytes_.push_back(static_cast<std::uint8_t>(size >> shift));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    std::vector<std::uint8_t> Finish() && { return std::move(bytes_); }
private:
    std::vector<std::uint8_t> bytes_;
};

class TcsReader final
{
public:
    explicit TcsReader(std::span<const std::uint8_t> bytes) : bytes_(bytes)
    {
        valid_ = bytes.size() >= 4 && bytes[0] == 'T' &&
            bytes[1] == 'C' && bytes[2] == 'S' && bytes[3] == '1';
        if (valid_) offset_ = 4;
    }
    bool String(std::string& value)
    {
        if (!Take(4)) return false;
        std::uint32_t size = 0;
        for (unsigned shift = 0; shift != 32; shift += 8)
            size |= static_cast<std::uint32_t>(bytes_[offset_++]) << shift;
        if (size == 0 || size > MaximumPathBytes || !Take(size)) return false;
        value.assign(
            reinterpret_cast<const char*>(bytes_.data() + offset_), size);
        offset_ += size;
        return true;
    }
    bool done() const noexcept { return valid_ && offset_ == bytes_.size(); }
private:
    bool Take(std::size_t count)
    {
        if (!valid_ || count > bytes_.size() - offset_)
        {
            valid_ = false;
            return false;
        }
        return true;
    }
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
    bool valid_ = false;
};

ProgramValueGraph EncodeSterilizationResultGraph()
{
    ProgramValue outcome{
        ProgramValueId(1),
        SterilizationOutcomeType(),
        EnumValue{
            SterilizationOutcomeSchemaIdentity(),
            static_cast<std::int64_t>(
                TasMovieCheckpointSterilizationOutcomeV1::Sterilized)}};
    ProgramValue result{
        ProgramValueId(2),
        SterilizationResultType(),
        RecordValue{{outcome.id}}};
    return {result.id, {std::move(outcome), std::move(result)}};
}

bool DecodeSterilizationResultGraph(
    const ProgramValueGraph& graph,
    TasMovieCheckpointSterilizationResultV1& result,
    std::string* diagnostic)
{
    GraphReader reader(graph);
    const ProgramValue* root = reader.root();
    const auto* record = root
        ? std::get_if<RecordValue>(&root->payload)
        : nullptr;
    std::int64_t outcome = -1;
    if (!reader.valid() || !root || root->type != SterilizationResultType() ||
        !record || record->fields.size() != 1 ||
        !ReadEnum(
            reader, record->fields.front(),
            SterilizationOutcomeSchemaIdentity(), outcome) ||
        outcome != static_cast<std::int64_t>(
            TasMovieCheckpointSterilizationOutcomeV1::Sterilized))
    {
        SetDiagnostic(
            diagnostic,
            "checkpoint sterilization result is not the exact Result/1 shape");
        return false;
    }
    result.outcome =
        TasMovieCheckpointSterilizationOutcomeV1::Sterilized;
    return true;
}

ProgramModule ConstructSterilizationModuleV1()
{
    ProgramModule module{
        .identity = {
            .canonical_id = std::string(SterilizationModuleCanonicalId),
            .revision = SterilizationModuleRevision,
        },
    };
    ModuleFragmentBuilder builder(
        module,
        "SavorCore/TasMovieCheckpointSterilizationModule",
        "soa.tas_movie_checkpoint_sterilize/sterilize");
    builder.AddLocalType({
        .identity = SterilizationOutcomeSchemaIdentity(),
        .kind = TypeSchemaKind::ClosedEnum,
        .enum_members = {{
            "Sterilized",
            static_cast<std::int64_t>(
                TasMovieCheckpointSterilizationOutcomeV1::Sterilized)}},
    });
    builder.AddLocalType({
        .identity = SterilizationResultSchemaIdentity(),
        .kind = TypeSchemaKind::Record,
        .record_fields = {{"outcome", SterilizationOutcomeType()}},
    });
    AddCanonicalAction(
        builder,
        CanonicalAction::SavestateSaveImmutableArtifact);
    builder.AddCapabilityImport(CanonicalRuntimePackIdentity());

    const ValueDefinition request = builder.NewArgument(
        CanonicalActionInputType(
            CanonicalAction::SavestateSaveImmutableArtifact));
    ProgramFunction& function = builder.AddFunction(
        std::string(SterilizationEntrypoint),
        std::array{request},
        SterilizationResultType(),
        TypeRef::Builtin(BuiltinType::Bool),
        true);
    BasicBlock& entry = builder.AddBlock(function);
    (void)builder.AddInstruction(
        function,
        entry,
        InstructionOpcode::AwaitAction,
        CanonicalActionOutputType(
            CanonicalAction::SavestateSaveImmutableArtifact),
        std::array{request.id},
        ActionTarget(CanonicalActionIdentity(
            CanonicalAction::SavestateSaveImmutableArtifact)),
        "sterilize/save-native-movie-inactive-checkpoint");
    const ProgramValueId outcome = Constant(
        builder,
        function,
        entry,
        LiteralValue{
            SterilizationOutcomeType(),
            EnumValue{
                SterilizationOutcomeSchemaIdentity(),
                static_cast<std::int64_t>(
                    TasMovieCheckpointSterilizationOutcomeV1::Sterilized)}},
        "sterilize/outcome",
        {});
    const ProgramValueId result = Construct(
        builder,
        function,
        entry,
        SterilizationResultType(),
        std::array{outcome},
        "sterilize/result",
        {});
    const ProgramValueId domain = Constant(
        builder,
        function,
        entry,
        LiteralValue{TypeRef::Builtin(BuiltinType::Bool), true},
        "sterilize/domain-outcome",
        {});
    builder.SetTerminator(
        function,
        entry,
        Terminator{
            .kind = TerminatorKind::Return,
            .return_value = result,
            .domain_outcome = domain,
        },
        "sterilize/return");

    module.accepted_policies = {
        .state_policies = {InvocationStatePolicy::RestoreBaseline},
        .execution_intents = {ExecutionIntent::Live},
    };
    module.budgets = {
        .maximum_instructions = 256,
        .maximum_calls = 4,
        .maximum_call_depth = 2,
        .maximum_action_requests = 1,
        .maximum_emissions = 1,
        .maximum_artifacts = 1,
        .maximum_values = 64,
        .maximum_value_bytes = 64u * 1024u,
        .maximum_trace_events = 128,
    };
    module.entrypoints = {{
        .name = std::string(SterilizationEntrypoint),
        .function = function.id,
        .input_type = CanonicalActionInputType(
            CanonicalAction::SavestateSaveImmutableArtifact),
        .output_type = SterilizationResultType(),
        .domain_outcome_type = TypeRef::Builtin(BuiltinType::Bool),
        .required_capability_packs = module.required_capability_packs,
        .accepted_policies = module.accepted_policies,
    }};
    module.identity.module_hash = ComputeProgramModuleHashV1(module);
    return module;
}

std::optional<ProgramDependencyLock> VerifySterilizationModuleV1(
    const ProgramModule& module,
    std::string* diagnostic)
{
    ProgramDefinitionStore modules;
    TypeSchemaRegistry schemas;
    ActionRegistry actions(&schemas);
    CapabilityPackRegistry packs(&schemas, &actions);
    const RegistryResult registered =
        capabilities::RegisterSourceCapabilityPacks(schemas, actions, packs);
    if (!registered.success)
    {
        SetDiagnostic(diagnostic, registered.error.message);
        return std::nullopt;
    }
    const auto stored = modules.RegisterCompiled(module);
    if (!stored.success)
    {
        SetDiagnostic(diagnostic, stored.error.message);
        return std::nullopt;
    }
    ProgramVerifier verifier(modules, schemas, actions, packs);
    const ProgramVerificationResult verified = verifier.Verify(
        stored.module->identity,
        capabilities::SupportedSoaUsaCompatibility());
    if (!verified.success || !verified.verified)
    {
        SetDiagnostic(
            diagnostic,
            verified.diagnostics.empty()
                ? "checkpoint sterilization module verification failed"
                : verified.diagnostics.front().message);
        return std::nullopt;
    }
    return verified.verified->dependency_lock;
}

std::optional<EncodedModuleEnvelope> EncodeSterilizationModuleEnvelopeV1(
    const ProgramModule& module,
    std::string* diagnostic)
{
    const EncodeResult encoded = EncodeProgramModuleV1(module);
    if (!encoded)
    {
        SetDiagnostic(diagnostic, encoded.status.message);
        return std::nullopt;
    }
    return EncodedModuleEnvelope{
        .identity = {
            .canonical_id = module.identity.canonical_id,
            .revision = module.identity.revision,
            .canonical_hash = module.identity.module_hash.ToHex(),
        },
        .format_version = kProgramCodecVersionV1,
        .development_only = false,
        .payload = encoded.bytes,
    };
}

ProgramValueGraph EncodeSterilizationModuleRequest(
    const TasMovieCheckpointSterilizationRequestV1& request,
    std::string* diagnostic)
{
    const auto bytes = ActionRequestBytes(
        CanonicalAction::SavestateSaveImmutableArtifact,
        request.output_savestate_path,
        "TAS Movie sterilized checkpoint",
        diagnostic);
    if (!bytes) return {};
    ProgramValue value{
        ProgramValueId(1),
        CanonicalActionInputType(
            CanonicalAction::SavestateSaveImmutableArtifact),
        *bytes};
    return {value.id, {std::move(value)}};
}

std::optional<std::string> ComputeSterilizationCompatibilityV1(
    const ModuleIdentity& module_identity,
    const ProgramDependencyLock& dependencies,
    const RuntimeProfile& runtime_profile,
    const InvocationExecutionPolicy& execution,
    const ProgramBudgets& limits,
    std::string* diagnostic)
{
    TasMovieCheckpointSterilizationRequestV1 request{
        .source_savestate_path = "source.sav",
        .source_dtm_path = "source.sav.dtm",
        .output_savestate_path = "sterilized.sav",
    };
    ProgramValueGraph input = EncodeSterilizationModuleRequest(
        request, diagnostic);
    if (!input.root) return std::nullopt;
    ProgramInvocation invocation{
        .invocation_id = InvocationId(1),
        .attempt_id = AttemptId(1),
        .module = module_identity,
        .entrypoint = std::string(SterilizationEntrypoint),
        .dependencies = dependencies,
        .runtime_profile = runtime_profile,
        .state = {
            .policy = InvocationStatePolicy::RestoreBaseline,
            .session_lineage = std::string(SterilizationArtifactLineage),
        },
        .execution = execution,
        .input = std::move(input),
        .limits = limits,
    };
    const std::string hash =
        ComputeProgramInvocationCompatibilityHashV1(invocation);
    return hash.size() == 64 ? std::optional<std::string>(hash) : std::nullopt;
}

bool DecodeSterilizationProgramResult(
    std::span<const Byte> encoded_result,
    const ModuleIdentity& expected_module,
    const ProgramDependencyLock& expected_dependencies,
    TasMovieCheckpointSterilizationResultV1& output,
    std::string* diagnostic)
{
    const auto decoded = DecodeProgramResultV1(encoded_result);
    if (!decoded || !decoded.value ||
        decoded.value->module != expected_module ||
        decoded.value->entrypoint != SterilizationEntrypoint ||
        decoded.value->resolved_dependencies != expected_dependencies ||
        decoded.value->infrastructure != ProgramInfrastructureStatus::Completed ||
        decoded.value->cleanup != ProgramCleanupStatus::Clean ||
        decoded.value->session_disposition != SessionDisposition::Clean ||
        !decoded.value->output || !decoded.value->domain_outcome)
    {
        SetDiagnostic(
            diagnostic,
            "checkpoint sterilization ProgramResult is not an exact clean completion");
        return false;
    }
    GraphReader domain(*decoded.value->domain_outcome);
    const ProgramValue* root = domain.root();
    const auto* succeeded = root ? std::get_if<bool>(&root->payload) : nullptr;
    if (!domain.valid() || !root || !succeeded || !*succeeded)
    {
        SetDiagnostic(
            diagnostic,
            "checkpoint sterilization domain outcome is not true");
        return false;
    }
    return DecodeSterilizationResultGraph(
        *decoded.value->output, output, diagnostic);
}

class TasMovieCheckpointSterilizationFullPhaseDefinition final
    : public ITasMovieCheckpointSterilizationFullPhaseDefinitionV1
{
public:
    TasMovieCheckpointSterilizationFullPhaseDefinition()
    {
        std::string diagnostic;
        ProgramModule module = ConstructSterilizationModuleV1();
        auto dependencies = VerifySterilizationModuleV1(module, &diagnostic);
        auto envelope = dependencies
            ? EncodeSterilizationModuleEnvelopeV1(module, &diagnostic)
            : std::nullopt;
        if (!dependencies || !envelope)
            throw std::logic_error(
                "TAS Movie checkpoint sterilization Full Phase is invalid: " +
                diagnostic);
        const RuntimeProfile profile = InvocationRuntimeProfile(*dependencies);
        const InvocationExecutionPolicy execution{
            .intent = ExecutionIntent::Live,
            .allow_movie_playback = false,
            .allow_movie_recording = false,
            .allow_input = false,
            .allow_capture = false,
            .record_trace = false,
        };
        const auto compatibility = ComputeSterilizationCompatibilityV1(
            module.identity, *dependencies, profile, execution,
            module.budgets, &diagnostic);
        if (!compatibility)
            throw std::logic_error(
                "TAS Movie checkpoint sterilization compatibility failed: " +
                diagnostic);
        module_identity_ = module.identity;
        dependency_lock_ = *dependencies;
        runtime_profile_ = profile;
        module_envelope_ = std::move(*envelope);
        const ContentHash256 dependency_hash =
            ComputeProgramDependencyLockHashV1(dependency_lock_);
        runtime_ = {
            .module = module_envelope_.identity,
            .entrypoint = std::string(SterilizationEntrypoint),
            .dependency_lock_sha256 = dependency_hash.ToHex(),
            .verified_dependency_sha256 = *compatibility,
            .runtime_profile_sha256 = RuntimeProfileHash(runtime_profile_),
            .state_policy = InvocationStatePolicy::RestoreBaseline,
            .execution = execution,
            .limits = module.budgets,
            .baseline_lineage = std::string(SterilizationArtifactLineage),
            .movie_policy_sha256 = [] {
                constexpr std::string_view value =
                    "tasmovie.checkpoint_sterilize/detach/v1";
                return hash::sha256(value.data(), value.size());
            }(),
            .service_policy_sha256 = [] {
                constexpr std::string_view value =
                    "tasmovie.checkpoint_sterilize/native-save/v1";
                return hash::sha256(value.data(), value.size());
            }(),
        };
        std::string canonical = "savor.full_phase/definition/v1";
        const auto append = [&canonical](std::string_view value) {
            canonical.push_back('\0');
            canonical.append(value);
        };
        append(SterilizationModuleCanonicalId);
        append(std::to_string(savor::PK_TasMovieCheckpointSterilize));
        append(runtime_.module.canonical_hash);
        append(runtime_.verified_dependency_sha256);
        append(runtime_.baseline_lineage);
        append("tasmovie.checkpoint_sterilize/v1");
        identity_ = {
            .program_kind = static_cast<std::int32_t>(
                savor::PK_TasMovieCheckpointSterilize),
            .program_version = 1,
            .canonical_id = std::string(
                SterilizationFullPhaseCanonicalId),
            .contract_revision = 1,
            .canonical_sha256 = hash::sha256(
                canonical.data(), canonical.size()),
        };
    }

    const fullphase::FullPhaseProgramIdentity& identity()
        const noexcept override { return identity_; }
    const fullphase::FullPhaseRuntimeContract& runtime_contract()
        const noexcept override { return runtime_; }
    const EncodedModuleEnvelope& module_envelope()
        const noexcept override { return module_envelope_; }

    std::optional<ProgramInvocation> BuildResolvedExecution(
        std::span<const std::uint8_t> payload,
        ProgramExecutionId execution_id,
        AttemptId attempt_id,
        std::string* diagnostic) const override
    {
        TasMovieCheckpointSterilizationRequestV1 request;
        if (!DecodeTasMovieCheckpointSterilizationExecutionInputV1(
                payload, request, diagnostic) ||
            !execution_id || !attempt_id)
            return std::nullopt;
        if (!std::filesystem::is_regular_file(
                request.source_savestate_path) ||
            !std::filesystem::is_regular_file(request.source_dtm_path))
        {
            SetDiagnostic(
                diagnostic,
                "checkpoint sterilization source pair is unavailable");
            return std::nullopt;
        }
        ProgramValueGraph input = EncodeSterilizationModuleRequest(
            request, diagnostic);
        if (!input.root) return std::nullopt;
        return ProgramInvocation{
            .invocation_id = execution_id,
            .attempt_id = attempt_id,
            .module = module_identity_,
            .entrypoint = std::string(SterilizationEntrypoint),
            .dependencies = dependency_lock_,
            .runtime_profile = runtime_profile_,
            .state = {
                .policy = InvocationStatePolicy::RestoreBaseline,
                .session_lineage = std::string(SterilizationArtifactLineage),
            },
            .execution = runtime_.execution,
            .input = std::move(input),
            .limits = runtime_.limits,
            .provenance = {
                .requesting_component =
                    "SavorDb.PK_TasMovieCheckpointSterilize",
                .attributes = {{
                    "contract",
                    "tasmovie.checkpoint_sterilize@1",
                }},
            },
        };
    }

    bool DecodeProgramResult(
        std::span<const Byte> encoded_result,
        TasMovieCheckpointSterilizationResultV1& result,
        std::string* diagnostic) const override
    {
        return DecodeSterilizationProgramResult(
            encoded_result, module_identity_, dependency_lock_, result,
            diagnostic);
    }

private:
    fullphase::FullPhaseProgramIdentity identity_;
    fullphase::FullPhaseRuntimeContract runtime_;
    EncodedModuleEnvelope module_envelope_;
    ModuleIdentity module_identity_;
    ProgramDependencyLock dependency_lock_;
    RuntimeProfile runtime_profile_;
};

} // namespace

std::vector<std::uint8_t>
EncodeTasMovieCheckpointSterilizationExecutionInputV1(
    const TasMovieCheckpointSterilizationRequestV1& request,
    std::string* diagnostic)
{
    if (!ValidateSterilizationRequest(request, diagnostic)) return {};
    TcsWriter writer;
    writer.String(request.source_savestate_path);
    writer.String(request.source_dtm_path);
    writer.String(request.output_savestate_path);
    return std::move(writer).Finish();
}

bool DecodeTasMovieCheckpointSterilizationExecutionInputV1(
    std::span<const std::uint8_t> payload,
    TasMovieCheckpointSterilizationRequestV1& request,
    std::string* diagnostic)
{
    TcsReader reader(payload);
    TasMovieCheckpointSterilizationRequestV1 decoded;
    if (!reader.String(decoded.source_savestate_path) ||
        !reader.String(decoded.source_dtm_path) ||
        !reader.String(decoded.output_savestate_path) || !reader.done() ||
        !ValidateSterilizationRequest(decoded, diagnostic))
    {
        if (!diagnostic || diagnostic->empty())
            SetDiagnostic(diagnostic, "TCS1 payload is malformed");
        return false;
    }
    request = std::move(decoded);
    return true;
}

std::shared_ptr<const
    ITasMovieCheckpointSterilizationFullPhaseDefinitionV1>
TasMovieCheckpointSterilizationFullPhaseDefinitionV1()
{
    static const auto definition = std::make_shared<const
        TasMovieCheckpointSterilizationFullPhaseDefinition>();
    return definition;
}

} // namespace savor::runtime::tasmovie
