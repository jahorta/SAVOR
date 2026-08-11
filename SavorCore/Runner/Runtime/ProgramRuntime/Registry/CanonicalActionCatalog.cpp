#include "CanonicalActionCatalog.h"

#include "Utils/Hash.h"

#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>
#include <string>

namespace savor::runtime::program {
namespace {

constexpr std::array<CanonicalActionDefinition, 23> kActions{{
    {CanonicalAction::SavestateSaveImmutableArtifact, "runtime.savestate.save_immutable_artifact", "(SavestateArtifactSaveRequest)->PendingSavestateArtifactPublicationReceipt"},
    {CanonicalAction::ExecutionContinueUntil, "runtime.execution.continue_until", "(ContinueUntilRequest)->ContinueUntilResult"},
    {CanonicalAction::ExecutionStepFrames, "runtime.execution.step_frames", "(StepFramesRequest)->ExecutionResult"},
    {CanonicalAction::InputAcquireLease, "runtime.input.acquire_lease", "(InputLeaseRequest)->InputLeaseHandle"},
    {CanonicalAction::InputApplyState, "runtime.input.apply_state", "(InputStateRequest)->InputExecutionBinding"},
    {CanonicalAction::InputBeginDelivery, "runtime.input.begin_delivery", "(InputDeliveryRequest)->InputExecutionBinding"},
    {CanonicalAction::InputCompleteDelivery, "runtime.input.complete_delivery", "(CompleteInputDeliveryRequest)->InputDeliveryReceipt"},
    {CanonicalAction::MoviePrepareReadOnlyPlayback, "runtime.movie.prepare_read_only_playback", "(MoviePlaybackRequest)->PreparedMoviePlaybackHandle"},
    {CanonicalAction::MovieStartPlayback, "runtime.movie.start_playback", "(PreparedMoviePlaybackHandle)->MovieSessionHandle"},
    {CanonicalAction::MovieStopPlayback, "runtime.movie.stop_playback", "(MovieSessionHandle)->MovieTerminalReceipt"},
    {CanonicalAction::MovieStartRecording, "runtime.movie.start_recording", "(MovieRecordingRequest)->MovieSessionHandle"},
    {CanonicalAction::MovieStopRecording, "runtime.movie.stop_recording", "(MovieSessionHandle)->MovieArtifactRef"},
    {CanonicalAction::GuestReadU8, "runtime.guest.read_u8", "(GuestScalarReadRequest)->u8"},
    {CanonicalAction::GuestReadU16, "runtime.guest.read_u16", "(GuestScalarReadRequest)->u16"},
    {CanonicalAction::GuestReadU32, "runtime.guest.read_u32", "(GuestScalarReadRequest)->u32"},
    {CanonicalAction::GuestReadU64, "runtime.guest.read_u64", "(GuestScalarReadRequest)->u64"},
    {CanonicalAction::GuestRunCoherentQuery, "runtime.guest.run_coherent_query", "(CoherentQueryRequest)->TypedObservation"},
    {CanonicalAction::GuestWriteData, "runtime.guest.write_data", "(GuestDataWriteRequest)->GuestMutationHandle"},
    {CanonicalAction::GuestPatchExecutable, "runtime.guest.patch_executable", "(ExecutablePatchRequest)->GuestMutationHandle"},
    {CanonicalAction::CaptureMark, "runtime.capture.mark", "(CaptureMarkerRequest)->CaptureMarkerReceipt"},
    {CanonicalAction::ScreenshotCapture, "runtime.screenshot.capture", "(ScreenshotRequest)->ScreenshotArtifactRef"},
    {CanonicalAction::TelemetryEmit, "runtime.telemetry.emit", "(TypedTelemetryRecord)->TelemetryReceipt"},
    {CanonicalAction::ExecutionRequirePausedPc, "runtime.execution.require_paused_pc", "(RequirePausedPcRequest)->PausedPcReceipt"},
}};

consteval bool CanonicalActionDefinitionsAreComplete()
{
    constexpr auto expected = static_cast<std::size_t>(
        CanonicalAction::ExecutionRequirePausedPc) + 1u;
    if (kActions.size() != expected)
        return false;
    std::array<bool, expected> seen{};
    for (std::size_t index = 0; index < kActions.size(); ++index)
    {
        const auto& entry = kActions[index];
        const auto key = static_cast<std::size_t>(entry.action);
        if (key >= expected || seen[key] || entry.name.empty() ||
            entry.signature.empty())
        {
            return false;
        }
        seen[key] = true;
        for (std::size_t other = 0; other < index; ++other)
            if (kActions[other].name == entry.name) return false;
    }
    return std::ranges::all_of(seen, [](bool value) { return value; });
}

static_assert(CanonicalActionDefinitionsAreComplete());

const CanonicalActionDefinition* FindActionDefinition(
    CanonicalAction action) noexcept
{
    const auto found = std::ranges::find(
        kActions, action, &CanonicalActionDefinition::action);
    return found == kActions.end() ? nullptr : &*found;
}

enum class ActionOutputShape : std::uint8_t
{
    SapReceipt,
    ResourceHandle,
    ArtifactReference,
    U8,
    U16,
    U32,
    U64,
};

ContentHash256 ContractHash(std::string_view contract)
{
    const std::string digest =
        hash::sha256(contract.data(), contract.size());
    const std::optional<ContentHash256> parsed =
        ContentHash256::FromHex(digest);
    if (!parsed)
        throw std::logic_error("Canonical action signature hash failed");
    return *parsed;
}

SchemaIdentity RuntimeSchemaIdentity(
    std::string canonical_id,
    std::string_view canonical_contract)
{
    return {
        .canonical_id = std::move(canonical_id),
        .version = 1,
        .schema_hash = ContractHash(canonical_contract),
    };
}

std::string TypeContract(const TypeRef& type)
{
    if (!type.is_named())
    {
        return "builtin:" +
            std::to_string(
                static_cast<std::uint8_t>(type.builtin));
    }
    return type.named->canonical_id + "/" +
        std::to_string(type.named->version) + "#" +
        type.named->schema_hash.ToHex();
}

ExactDependencyIdentity Identity(
    CanonicalAction action)
{
    const CanonicalActionDefinition* entry = FindActionDefinition(action);
    if (entry == nullptr)
        throw std::out_of_range("Unknown canonical action");
    return {
        .canonical_id = std::string(entry->name),
        .version = 1,
    };
}

bool UsesTypedRequestRecord(CanonicalAction action) noexcept
{
    switch (action)
    {
    case CanonicalAction::ExecutionContinueUntil:
    case CanonicalAction::ExecutionStepFrames:
    case CanonicalAction::InputAcquireLease:
    case CanonicalAction::InputApplyState:
    case CanonicalAction::InputBeginDelivery:
    case CanonicalAction::InputCompleteDelivery:
    case CanonicalAction::GuestReadU8:
    case CanonicalAction::GuestReadU16:
    case CanonicalAction::GuestReadU32:
    case CanonicalAction::GuestReadU64:
    case CanonicalAction::GuestRunCoherentQuery:
    case CanonicalAction::ExecutionRequirePausedPc:
        return true;
    default:
        return false;
    }
}

ActionOutputShape OutputShape(CanonicalAction action) noexcept
{
    switch (action)
    {
    case CanonicalAction::InputAcquireLease:
    case CanonicalAction::MoviePrepareReadOnlyPlayback:
    case CanonicalAction::MovieStartPlayback:
    case CanonicalAction::MovieStartRecording:
    case CanonicalAction::GuestWriteData:
    case CanonicalAction::GuestPatchExecutable:
        return ActionOutputShape::ResourceHandle;
    case CanonicalAction::MovieStopRecording:
    case CanonicalAction::ScreenshotCapture:
        return ActionOutputShape::ArtifactReference;
    case CanonicalAction::SavestateSaveImmutableArtifact:
        return ActionOutputShape::SapReceipt;
    case CanonicalAction::GuestReadU8:
        return ActionOutputShape::U8;
    case CanonicalAction::GuestReadU16:
        return ActionOutputShape::U16;
    case CanonicalAction::GuestReadU32:
        return ActionOutputShape::U32;
    case CanonicalAction::GuestReadU64:
        return ActionOutputShape::U64;
    default:
        return ActionOutputShape::SapReceipt;
    }
}

std::optional<SchemaIdentity> SharedOutputSchemaIdentity(
    CanonicalAction action)
{
    switch (action)
    {
    case CanonicalAction::ExecutionContinueUntil:
    {
        const TypeRef reason = CanonicalRuntimeType(
            CanonicalRuntimeSchema::ContinueUntilCompletionReason);
        const TypeRef stop = CanonicalRuntimeType(
            CanonicalRuntimeSchema::OptionalRoutedStopReceipt);
        return RuntimeSchemaIdentity(
            "runtime.execution.ContinueUntilResult",
            "record ContinueUntilResult/1(reason:" +
                TypeContract(reason) + ",routed_stop:" +
                TypeContract(stop) +
                ",pc:u32,movie_input_count:u64,workset_epoch:u64)");
    }
    case CanonicalAction::ExecutionStepFrames:
        return RuntimeSchemaIdentity(
            "runtime.execution.ExecutionResult",
            "bytes(max=65536;ExecutionResult/1)");
    case CanonicalAction::ExecutionRequirePausedPc:
        return RuntimeSchemaIdentity(
            "runtime.execution.PausedPcReceipt",
            "record PausedPcReceipt/1(pc:u32,vi_count:u64,workset_epoch:u64)");
    case CanonicalAction::InputApplyState:
    case CanonicalAction::InputBeginDelivery:
        return RuntimeSchemaIdentity(
            "runtime.input.InputExecutionBinding",
            "bytes(max=65536;InputExecutionBinding/1)");
    case CanonicalAction::InputCompleteDelivery:
        return RuntimeSchemaIdentity(
            "runtime.input.InputDeliveryReceipt",
            "bytes(max=65536;InputDeliveryReceipt/1)");
    default:
        return std::nullopt;
    }
}

std::optional<CanonicalAction> DirectHandleInput(
    CanonicalAction action) noexcept
{
    switch (action)
    {
    case CanonicalAction::MovieStartPlayback:
        return CanonicalAction::MoviePrepareReadOnlyPlayback;
    case CanonicalAction::MovieStopPlayback:
        return CanonicalAction::MovieStartPlayback;
    case CanonicalAction::MovieStopRecording:
        return CanonicalAction::MovieStartRecording;
    default:
        return std::nullopt;
    }
}

SchemaIdentity ActionSchemaIdentity(
    CanonicalAction action,
    std::string_view suffix,
    std::string_view representation)
{
    const CanonicalActionDefinition* entry = FindActionDefinition(action);
    if (entry == nullptr)
        throw std::out_of_range("Unknown canonical action");
    const std::string id =
        std::string(entry->name) + "." +
        std::string(suffix);
    const std::string contract =
        id + "/1:" + std::string(representation) +
        ";semantic=" + std::string(entry->signature);
    return {
        .canonical_id = id,
        .version = 1,
        .schema_hash = ContractHash(contract),
    };
}

SchemaIdentity ResourceContractIdentity(CanonicalAction action)
{
    return ActionSchemaIdentity(
        action,
        "ResourceContract",
        "bytes(max=65536;resource-contract)");
}

SchemaIdentity ArtifactPayloadIdentity(CanonicalAction action)
{
    return ActionSchemaIdentity(
        action,
        "ArtifactPayload",
        "bytes(max=65536;artifact-payload-contract)");
}

std::vector<RecordFieldDefinition> TypedRequestFields(
    CanonicalAction action)
{
    const TypeRef u64 = TypeRef::Builtin(BuiltinType::U64);
    const TypeRef input_lease = CanonicalActionOutputType(
        CanonicalAction::InputAcquireLease);
    switch (action)
    {
    case CanonicalAction::ExecutionContinueUntil:
        return {
            {"semantic_points",
             CanonicalRuntimeType(
                 CanonicalRuntimeSchema::SemanticPointSet)},
            {"input_binding",
             CanonicalRuntimeType(
                 CanonicalRuntimeSchema::
                     OptionalInputExecutionBinding)},
            {"playback_session",
             CanonicalRuntimeType(
                 CanonicalRuntimeSchema::
                     OptionalMoviePlaybackSession)},
            {"expected_movie_input_count",
             CanonicalRuntimeType(
                 CanonicalRuntimeSchema::
                     OptionalMovieInputCount)},
            {"static_config",
             CanonicalRuntimeType(
                 CanonicalRuntimeSchema::
                     ContinueUntilStaticConfig)},
        };
    case CanonicalAction::ExecutionStepFrames:
        return {
            {"count", u64},
            {"input_binding",
             CanonicalRuntimeType(
                 CanonicalRuntimeSchema::
                     OptionalInputExecutionBinding)},
            {"static_config",
             CanonicalRuntimeType(
                 CanonicalRuntimeSchema::
                     ExecutionAdvanceStaticConfig)},
        };
    case CanonicalAction::InputAcquireLease:
        return {{
            "static_config",
            CanonicalRuntimeType(
                CanonicalRuntimeSchema::
                    InputLeaseStaticConfig),
        }};
    case CanonicalAction::InputApplyState:
    case CanonicalAction::InputBeginDelivery:
        return {
            {"lease", input_lease},
            {"input",
             CanonicalRuntimeType(
                 CanonicalRuntimeSchema::InputFramePayload)},
        };
    case CanonicalAction::InputCompleteDelivery:
        return {
            {"lease", input_lease},
            {"binding", CanonicalActionOutputType(
                 CanonicalAction::InputBeginDelivery)},
        };
    case CanonicalAction::GuestReadU8:
    case CanonicalAction::GuestReadU16:
    case CanonicalAction::GuestReadU32:
    case CanonicalAction::GuestReadU64:
        return {
            {"stop_receipt",
             CanonicalRuntimeType(
                 CanonicalRuntimeSchema::
                     OptionalContinueUntilResult)},
            {"address", u64},
            {"static_config",
             CanonicalRuntimeType(
                 CanonicalRuntimeSchema::
                     ObservationStaticConfig)},
        };
    case CanonicalAction::GuestRunCoherentQuery:
        return {
            {"stop_receipt",
             CanonicalRuntimeType(
                 CanonicalRuntimeSchema::
                     OptionalContinueUntilResult)},
            {"static_config",
             CanonicalRuntimeType(
                 CanonicalRuntimeSchema::
                     ObservationStaticConfig)},
        };
    case CanonicalAction::ExecutionRequirePausedPc:
        return {{"expected_pc", u64}};
    default:
        return {};
    }
}

std::string TypedRequestRepresentation(CanonicalAction action)
{
    std::string representation = "record{";
    const auto fields = TypedRequestFields(action);
    for (std::size_t index = 0; index < fields.size(); ++index)
    {
        if (index != 0)
            representation += ",";
        representation += fields[index].name + ":" +
            TypeContract(fields[index].type);
    }
    representation += "}";
    return representation;
}

ActionDescriptor Descriptor(
    CanonicalAction action,
    SessionServiceCapabilityMask services,
    ActionEffectMask effects,
    std::uint64_t bounded_host_timeout_milliseconds,
    ActionEpochPolicy epoch = ActionEpochPolicy::RequiresCurrentEpoch,
    ActionReplayClass replay = ActionReplayClass::RecordedEvidence,
    ActionCancellationMode cancellation =
        ActionCancellationMode::Cooperative,
    ActionResourceBehavior resource =
        ActionResourceBehavior::None,
    ActionCleanupGuarantee cleanup =
        ActionCleanupGuarantee::None,
    bool taints_on_cleanup = false,
    ActionIdempotency idempotency =
        ActionIdempotency::NotRetryable)
{
    const bool cancellation_driven =
        action == CanonicalAction::ExecutionContinueUntil ||
        action == CanonicalAction::ExecutionStepFrames ||
        false;
    const TypeRef input = CanonicalActionInputType(action);
    const TypeRef output = CanonicalActionOutputType(action);
    std::vector<std::string> diagnostic_categories{
        "invalid_request",
        "stale_epoch",
        "cancelled",
        "service_failure",
        "cleanup_failure",
    };
    if (!cancellation_driven)
        diagnostic_categories.emplace_back("timeout");

    return {
        .identity = Identity(action),
        .providing_pack = {
            .canonical_id = "runtime.session",
            .version = 1,
        },
        .input_type = input,
        .output_type = output,
        .domain_observation_type = output,
        .receipt_type = output,
        .diagnostic_type = output,
        .required_services = services,
        .effects = effects,
        .epoch_policy = epoch,
        .replay_class = replay,
        .cancellation = cancellation,
        .timing = cancellation_driven
            ? ActionTimingClass::CancellationDriven
            : ActionTimingClass::BoundedHostOperation,
        .maximum_non_cancellable_milliseconds =
            !cancellation_driven &&
                cancellation ==
                    ActionCancellationMode::BeforeMutationOnly
                ? bounded_host_timeout_milliseconds
                : std::uint64_t{0},
        .default_host_timeout_milliseconds =
            cancellation_driven
                ? 0
                : bounded_host_timeout_milliseconds,
        .resource_behavior = resource,
        .cleanup = cleanup,
        .taints_on_unproven_cleanup = taints_on_cleanup,
        .idempotency = idempotency,
        .diagnostic_categories = std::move(diagnostic_categories),
    };
}

} // namespace

std::string_view CanonicalActionName(CanonicalAction action) noexcept
{
    const CanonicalActionDefinition* entry = FindActionDefinition(action);
    return entry == nullptr ? std::string_view{} : entry->name;
}

std::span<const CanonicalActionDefinition>
CanonicalActionDefinitions() noexcept
{
    return kActions;
}

ExactDependencyIdentity CanonicalActionIdentity(CanonicalAction action)
{
    const auto descriptors = BuildCanonicalRuntimeActionDescriptors();
    const auto found = std::ranges::find(
        descriptors,
        CanonicalActionName(action),
        [](const ActionDescriptor& descriptor)
        {
            return std::string_view(descriptor.identity.canonical_id);
        });
    if (found == descriptors.end())
        throw std::out_of_range("Unknown canonical action");
    return found->identity;
}

std::optional<CanonicalAction> FindCanonicalAction(
    const ExactDependencyIdentity& identity)
{
    const auto found = std::ranges::find(
        kActions,
        identity.canonical_id,
        &CanonicalActionDefinition::name);
    if (found == kActions.end() || identity.version != 1)
        return std::nullopt;
    return CanonicalActionIdentity(found->action) == identity
        ? std::optional<CanonicalAction>(found->action)
        : std::nullopt;
}

std::optional<SchemaIdentity>
CanonicalActionInputSchemaIdentity(CanonicalAction action)
{
    if (const auto source = DirectHandleInput(action))
        return CanonicalActionOutputSchemaIdentity(*source);
    if (UsesTypedRequestRecord(action))
    {
        return ActionSchemaIdentity(
            action,
            "Request",
            TypedRequestRepresentation(action));
    }
    return ActionSchemaIdentity(
        action,
        "Request",
        "bytes(max=65536;sap1-request)");
}

std::optional<SchemaIdentity>
CanonicalActionOutputSchemaIdentity(CanonicalAction action)
{
    if (const auto shared = SharedOutputSchemaIdentity(action))
        return shared;
    switch (OutputShape(action))
    {
    case ActionOutputShape::U8:
    case ActionOutputShape::U16:
    case ActionOutputShape::U32:
    case ActionOutputShape::U64:
        return std::nullopt;
    case ActionOutputShape::SapReceipt:
        return ActionSchemaIdentity(
            action,
            "Result",
            "bytes(max=65536;sap1-result)");
    case ActionOutputShape::ResourceHandle:
    {
        const SchemaIdentity contract =
            ResourceContractIdentity(action);
        const std::string representation =
            "resource-handle<" +
            contract.canonical_id + "/" +
            std::to_string(contract.version) + "#" +
            contract.schema_hash.ToHex() + ">";
        return ActionSchemaIdentity(
            action,
            "Result",
            representation);
    }
    case ActionOutputShape::ArtifactReference:
    {
        const SchemaIdentity payload =
            ArtifactPayloadIdentity(action);
        const std::string representation =
            "artifact-reference<" +
            payload.canonical_id + "/" +
            std::to_string(payload.version) + "#" +
            payload.schema_hash.ToHex() + ">";
        return ActionSchemaIdentity(
            action,
            "Result",
            representation);
    }
    }
    return std::nullopt;
}

std::optional<SchemaIdentity>
CanonicalActionResourceContractSchemaIdentity(
    CanonicalAction action)
{
    return OutputShape(action) ==
            ActionOutputShape::ResourceHandle
        ? std::optional<SchemaIdentity>(
              ResourceContractIdentity(action))
        : std::nullopt;
}

std::optional<SchemaIdentity>
CanonicalActionArtifactPayloadSchemaIdentity(
    CanonicalAction action)
{
    if (action == CanonicalAction::SavestateSaveImmutableArtifact)
    {
        return ArtifactPayloadIdentity(action);
    }
    const ActionOutputShape shape = OutputShape(action);
    return shape == ActionOutputShape::ArtifactReference
        ? std::optional<SchemaIdentity>(
              ArtifactPayloadIdentity(action))
        : std::nullopt;
}

std::optional<SchemaIdentity>
CanonicalActionArtifactReferenceSchemaIdentity(
    CanonicalAction action)
{
    switch (OutputShape(action))
    {
    case ActionOutputShape::ArtifactReference:
        return CanonicalActionOutputSchemaIdentity(action);
    default:
        return std::nullopt;
    }
}

TypeRef CanonicalActionInputType(CanonicalAction action)
{
    const auto schema =
        CanonicalActionInputSchemaIdentity(action);
    if (!schema)
        throw std::logic_error("Canonical action input has no schema");
    return TypeRef::Named(*schema);
}

TypeRef CanonicalActionOutputType(CanonicalAction action)
{
    switch (OutputShape(action))
    {
    case ActionOutputShape::U8:
        return TypeRef::Builtin(BuiltinType::U8);
    case ActionOutputShape::U16:
        return TypeRef::Builtin(BuiltinType::U16);
    case ActionOutputShape::U32:
        return TypeRef::Builtin(BuiltinType::U32);
    case ActionOutputShape::U64:
        return TypeRef::Builtin(BuiltinType::U64);
    default:
        break;
    }
    const auto schema =
        CanonicalActionOutputSchemaIdentity(action);
    if (!schema)
        throw std::logic_error("Canonical action output has no schema");
    return TypeRef::Named(*schema);
}

SchemaIdentity CanonicalRuntimeSchemaIdentity(
    CanonicalRuntimeSchema schema)
{
    switch (schema)
    {
    case CanonicalRuntimeSchema::InputFramePayload:
        return RuntimeSchemaIdentity(
            "runtime.input.InputFramePayload",
            "bytes(max=64;canonical controller-frame payload)");
    case CanonicalRuntimeSchema::SemanticPointSet:
        return RuntimeSchemaIdentity(
            "runtime.stop_points.SemanticPointSetV1",
            "bytes(max=65536;SPS1 ordered semantic alternatives and bounded hit-time samples)");
    case CanonicalRuntimeSchema::ContinueUntilStaticConfig:
        return RuntimeSchemaIdentity(
            "runtime.execution.ContinueUntilStaticConfig",
            "bytes(max=4096;CUC1 current-point, immediate-reentry, movie, interruption policies)");
    case CanonicalRuntimeSchema::ExecutionAdvanceStaticConfig:
        return RuntimeSchemaIdentity(
            "runtime.execution.AdvanceStaticConfig",
            "bytes(max=4096;EAC1 movie, throttle, interruption policies)");
    case CanonicalRuntimeSchema::InputLeaseStaticConfig:
        return RuntimeSchemaIdentity(
            "runtime.input.InputLeaseStaticConfig",
            "bytes(max=4096;ILC2 port, priority, suspension, interruption borrowing, movie exclusion)");
    case CanonicalRuntimeSchema::ObservationStaticConfig:
        return RuntimeSchemaIdentity(
            "runtime.guest.ObservationStaticConfig",
            "bytes(max=16384;OSC1 observation identity, requiredness, coherent-query and checked-dereference policy)");
    case CanonicalRuntimeSchema::StopEvidencePayload:
        return RuntimeSchemaIdentity(
            "runtime.stop_points.StopEvidencePayload",
            "bytes(max=65536;bounded router-side sample and physical-evidence payload)");
    case CanonicalRuntimeSchema::OptionalInputExecutionBinding:
    {
        const TypeRef element = CanonicalActionOutputType(
            CanonicalAction::InputApplyState);
        return RuntimeSchemaIdentity(
            "runtime.input.OptionalInputExecutionBinding",
            "optional<" + TypeContract(element) + ">");
    }
    case CanonicalRuntimeSchema::OptionalMoviePlaybackSession:
    {
        const TypeRef element = CanonicalActionOutputType(
            CanonicalAction::MovieStartPlayback);
        return RuntimeSchemaIdentity(
            "runtime.movie.OptionalPlaybackSession",
            "optional<" + TypeContract(element) + ">");
    }
    case CanonicalRuntimeSchema::OptionalMovieInputCount:
        return RuntimeSchemaIdentity(
            "runtime.movie.OptionalInputCount",
            "optional<" + TypeContract(
                TypeRef::Builtin(BuiltinType::U64)) + ">");
    case CanonicalRuntimeSchema::ContinueUntilCompletionReason:
        return RuntimeSchemaIdentity(
            "runtime.execution.ContinueUntilCompletionReason",
            "enum ContinueUntilCompletionReason/1{Breakpoint=0,CursorOverrun=1,MovieEnded=2}");
    case CanonicalRuntimeSchema::RoutedStopReceipt:
    {
        const TypeRef evidence = CanonicalRuntimeType(
            CanonicalRuntimeSchema::StopEvidencePayload);
        return RuntimeSchemaIdentity(
            "runtime.stop_points.RoutedStopReceipt",
            "record RoutedStopReceipt/1(stop_sequence:u64,workset_epoch:u64,pc:u32,sample_snapshot_id:u64,evidence:" +
                TypeContract(evidence) + ")");
    }
    case CanonicalRuntimeSchema::OptionalRoutedStopReceipt:
    {
        const TypeRef element = CanonicalRuntimeType(
            CanonicalRuntimeSchema::RoutedStopReceipt);
        return RuntimeSchemaIdentity(
            "runtime.stop_points.OptionalRoutedStopReceipt",
            "optional<" + TypeContract(element) + ">");
    }
    case CanonicalRuntimeSchema::OptionalContinueUntilResult:
    {
        const TypeRef element = CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil);
        return RuntimeSchemaIdentity(
            "runtime.execution.OptionalContinueUntilResult",
            "optional<" + TypeContract(element) + ">");
    }
    }
    throw std::out_of_range("Unknown canonical runtime schema");
}

TypeRef CanonicalRuntimeType(CanonicalRuntimeSchema schema)
{
    return TypeRef::Named(CanonicalRuntimeSchemaIdentity(schema));
}

std::vector<TypeSchemaDefinition>
BuildCanonicalRuntimeActionSchemas()
{
    constexpr std::uint64_t kMaximumSapBytes = 64u * 1024u;
    std::vector<TypeSchemaDefinition> schemas;
    schemas.reserve(96);
    const auto append = [&schemas](TypeSchemaDefinition definition)
    {
        const auto existing = std::ranges::find(
            schemas,
            definition.identity,
            &TypeSchemaDefinition::identity);
        if (existing == schemas.end())
        {
            schemas.push_back(std::move(definition));
            return;
        }
        if (*existing != definition)
            throw std::logic_error(
                "Canonical schema identity has conflicting definitions");
    };

    append({
        .identity = CanonicalRuntimeSchemaIdentity(
            CanonicalRuntimeSchema::InputFramePayload),
        .kind = TypeSchemaKind::BoundedBytes,
        .maximum_size = 64,
    });
    for (const auto [schema, maximum] :
         std::array{
             std::pair{
                  CanonicalRuntimeSchema::SemanticPointSet,
                 std::uint64_t{65536}},
             std::pair{
                 CanonicalRuntimeSchema::ContinueUntilStaticConfig,
                 std::uint64_t{4096}},
             std::pair{
                 CanonicalRuntimeSchema::ExecutionAdvanceStaticConfig,
                 std::uint64_t{4096}},
             std::pair{
                 CanonicalRuntimeSchema::InputLeaseStaticConfig,
                 std::uint64_t{4096}},
             std::pair{
                 CanonicalRuntimeSchema::ObservationStaticConfig,
                 std::uint64_t{16384}},
             std::pair{
                 CanonicalRuntimeSchema::StopEvidencePayload,
                 std::uint64_t{65536}},
         })
    {
        append({
            .identity = CanonicalRuntimeSchemaIdentity(schema),
            .kind = TypeSchemaKind::BoundedBytes,
            .maximum_size = maximum,
        });
    }
    append({
        .identity = CanonicalRuntimeSchemaIdentity(
            CanonicalRuntimeSchema::
                OptionalInputExecutionBinding),
        .kind = TypeSchemaKind::Optional,
        .element_type = CanonicalActionOutputType(
            CanonicalAction::InputApplyState),
    });
    append({
        .identity = CanonicalRuntimeSchemaIdentity(
            CanonicalRuntimeSchema::OptionalMoviePlaybackSession),
        .kind = TypeSchemaKind::Optional,
        .element_type = CanonicalActionOutputType(
            CanonicalAction::MovieStartPlayback),
    });
    append({
        .identity = CanonicalRuntimeSchemaIdentity(
            CanonicalRuntimeSchema::OptionalMovieInputCount),
        .kind = TypeSchemaKind::Optional,
        .element_type = TypeRef::Builtin(BuiltinType::U64),
    });
    append({
        .identity = CanonicalRuntimeSchemaIdentity(
            CanonicalRuntimeSchema::ContinueUntilCompletionReason),
        .kind = TypeSchemaKind::ClosedEnum,
        .enum_members = {
            {"Breakpoint", static_cast<std::int64_t>(
                 ContinueUntilCompletionReasonV1::Breakpoint)},
            {"CursorOverrun", static_cast<std::int64_t>(
                 ContinueUntilCompletionReasonV1::CursorOverrun)},
            {"MovieEnded", static_cast<std::int64_t>(
                 ContinueUntilCompletionReasonV1::MovieEnded)},
        },
    });
    append({
        .identity = CanonicalRuntimeSchemaIdentity(
            CanonicalRuntimeSchema::RoutedStopReceipt),
        .kind = TypeSchemaKind::Record,
        .record_fields = {
            {"stop_sequence", TypeRef::Builtin(BuiltinType::U64)},
            {"workset_epoch", TypeRef::Builtin(BuiltinType::U64)},
            {"pc", TypeRef::Builtin(BuiltinType::U32)},
            {"sample_snapshot_id", TypeRef::Builtin(BuiltinType::U64)},
            {"evidence", CanonicalRuntimeType(
                 CanonicalRuntimeSchema::StopEvidencePayload)},
        },
    });
    append({
        .identity = CanonicalRuntimeSchemaIdentity(
            CanonicalRuntimeSchema::OptionalRoutedStopReceipt),
        .kind = TypeSchemaKind::Optional,
        .element_type = CanonicalRuntimeType(
            CanonicalRuntimeSchema::RoutedStopReceipt),
    });
    append({
        .identity = CanonicalRuntimeSchemaIdentity(
            CanonicalRuntimeSchema::
                OptionalContinueUntilResult),
        .kind = TypeSchemaKind::Optional,
        .element_type = CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil),
    });

    for (const CanonicalActionDefinition& definition : kActions)
    {
        const CanonicalAction action = definition.action;
        if (!DirectHandleInput(action))
        {
            if (UsesTypedRequestRecord(action))
            {
                append({
                    .identity =
                        *CanonicalActionInputSchemaIdentity(action),
                    .kind = TypeSchemaKind::Record,
                    .record_fields = TypedRequestFields(action),
                });
            }
            else
            {
                append({
                    .identity =
                        *CanonicalActionInputSchemaIdentity(action),
                    .kind = TypeSchemaKind::BoundedBytes,
                    .maximum_size = kMaximumSapBytes,
                });
            }
        }

        if (action == CanonicalAction::ExecutionContinueUntil)
        {
            append({
                .identity =
                    *CanonicalActionOutputSchemaIdentity(action),
                .kind = TypeSchemaKind::Record,
                .record_fields = {
                    {"reason",
                     CanonicalRuntimeType(
                         CanonicalRuntimeSchema::
                             ContinueUntilCompletionReason)},
                    {"routed_stop",
                     CanonicalRuntimeType(
                         CanonicalRuntimeSchema::
                             OptionalRoutedStopReceipt)},
                    {"pc",
                     TypeRef::Builtin(BuiltinType::U32)},
                    {"movie_input_count",
                     TypeRef::Builtin(BuiltinType::U64)},
                    {"workset_epoch",
                     TypeRef::Builtin(BuiltinType::U64)},
                },
            });
            continue;
        }
        if (action == CanonicalAction::ExecutionRequirePausedPc)
        {
            append({
                .identity =
                    *CanonicalActionOutputSchemaIdentity(action),
                .kind = TypeSchemaKind::Record,
                .record_fields = {
                    {"pc", TypeRef::Builtin(BuiltinType::U32)},
                    {"vi_count", TypeRef::Builtin(BuiltinType::U64)},
                    {"workset_epoch", TypeRef::Builtin(BuiltinType::U64)},
                },
            });
            continue;
        }

        switch (OutputShape(action))
        {
        case ActionOutputShape::SapReceipt:
            append({
                .identity =
                    *CanonicalActionOutputSchemaIdentity(action),
                .kind = TypeSchemaKind::BoundedBytes,
                .maximum_size = kMaximumSapBytes,
            });
            break;
        case ActionOutputShape::ResourceHandle:
        {
            const SchemaIdentity contract =
                *CanonicalActionResourceContractSchemaIdentity(
                    action);
            append({
                .identity = contract,
                .kind = TypeSchemaKind::BoundedBytes,
                .maximum_size = kMaximumSapBytes,
            });
            append({
                .identity =
                    *CanonicalActionOutputSchemaIdentity(action),
                .kind = TypeSchemaKind::ResourceHandle,
                .element_type = TypeRef::Named(contract),
            });
            break;
        }
        case ActionOutputShape::ArtifactReference:
        {
            const SchemaIdentity payload =
                *CanonicalActionArtifactPayloadSchemaIdentity(
                    action);
            append({
                .identity = payload,
                .kind = TypeSchemaKind::BoundedBytes,
                .maximum_size = kMaximumSapBytes,
            });
            append({
                .identity =
                    *CanonicalActionOutputSchemaIdentity(action),
                .kind = TypeSchemaKind::ArtifactReference,
                .element_type = TypeRef::Named(payload),
            });
            break;
        }
        case ActionOutputShape::U8:
        case ActionOutputShape::U16:
        case ActionOutputShape::U32:
        case ActionOutputShape::U64:
            break;
        }
        if (action == CanonicalAction::SavestateSaveImmutableArtifact)
        {
            append({
                .identity = ArtifactPayloadIdentity(action),
                .kind = TypeSchemaKind::BoundedBytes,
                .maximum_size = kMaximumSapBytes,
            });
        }
    }
    std::ranges::sort(
        schemas,
        {},
        [](const TypeSchemaDefinition& definition)
        {
            return std::pair{
                definition.identity.canonical_id,
                definition.identity.version};
        });
    return schemas;
}

std::vector<SchemaIdentity>
CanonicalActionTypeSchemaClosure(CanonicalAction action)
{
    const auto schemas = BuildCanonicalRuntimeActionSchemas();
    std::vector<SchemaIdentity> roots;
    if (const auto input = CanonicalActionInputSchemaIdentity(action))
        roots.push_back(*input);
    if (const auto output = CanonicalActionOutputSchemaIdentity(action))
        roots.push_back(*output);

    std::vector<SchemaIdentity> closure;
    std::set<SchemaIdentity> visited;
    const auto visit = [&](const auto& self,
                           const SchemaIdentity& identity) -> void
    {
        if (!visited.insert(identity).second)
            return;
        const auto found = std::ranges::find(
            schemas,
            identity,
            &TypeSchemaDefinition::identity);
        if (found == schemas.end())
            throw std::logic_error(
                "Canonical action schema closure is incomplete");
        if (found->element_type &&
            found->element_type->is_named())
        {
            self(self, *found->element_type->named);
        }
        for (const auto& field : found->record_fields)
        {
            if (field.type.is_named())
                self(self, *field.type.named);
        }
        closure.push_back(identity);
    };
    for (const auto& root : roots)
        visit(visit, root);
    std::ranges::sort(closure);
    return closure;
}

CapabilityPackIdentity CanonicalRuntimePackIdentity()
{
    const auto descriptors =
        BuildCanonicalRuntimeActionDescriptors();
    if (descriptors.empty())
        throw std::logic_error("Canonical runtime action catalog is empty");
    return descriptors.front().providing_pack;
}

RuntimeCompatibility CanonicalRuntimeCompatibility()
{
    return {
        .game_id = "GEAE8P",
        .executable_identity = "soal-usa.GEAE8E",
        .address_map_revision =
            "savor.builtin-soal-usa-addresses/1",
    };
}

namespace {

CapabilityPackIdentity RuntimePackIdentity(
    const std::vector<ActionDescriptor>& descriptors)
{
    CapabilityPackManifest manifest{
        .identity = {
            .canonical_id = "runtime.session",
            .version = 1,
        },
        .compatibility = CanonicalRuntimeCompatibility(),
    };
    for (const TypeSchemaDefinition& schema :
         BuildCanonicalRuntimeActionSchemas())
    {
        manifest.schemas.push_back(schema.identity);
    }
    for (const ActionDescriptor& descriptor : descriptors)
        manifest.actions.push_back(descriptor.identity);
    return {
        .canonical_id = "runtime.session",
        .version = 1,
        .manifest_hash =
            ComputeCapabilityPackManifestContractHash(manifest),
    };
}

} // namespace

std::vector<ActionDescriptor>
BuildCanonicalRuntimeActionDescriptors()
{
    const auto service = [](SessionServiceCapability capability) {
        return ServiceMask(capability);
    };
    const auto effect = [](ActionEffect value) {
        return EffectMask(value);
    };
    const auto services = [&](SessionServiceCapability first,
                              SessionServiceCapability second) {
        return service(first) | service(second);
    };
    const auto effects = [&](ActionEffect first, ActionEffect second) {
        return effect(first) | effect(second);
    };

    std::vector<ActionDescriptor> result;
    result.reserve(kActions.size());
    result.push_back(Descriptor(
        CanonicalAction::SavestateSaveImmutableArtifact,
        services(
            SessionServiceCapability::Savestate,
            SessionServiceCapability::Artifact),
        effect(ActionEffect::ArtifactIo),
        60000,
        ActionEpochPolicy::RequiresCurrentEpoch,
        ActionReplayClass::ExternalCommit,
        ActionCancellationMode::CleanupRequired,
        ActionResourceBehavior::None,
        ActionCleanupGuarantee::VerifiedCompensation,
        true,
        ActionIdempotency::ReceiptProven));

    result.push_back(Descriptor(
        CanonicalAction::ExecutionContinueUntil,
        service(SessionServiceCapability::Execution),
        effect(ActionEffect::AdvanceEmulation),
        0));
    result.push_back(Descriptor(
        CanonicalAction::ExecutionStepFrames,
        service(SessionServiceCapability::Execution),
        effect(ActionEffect::AdvanceEmulation),
        0));
    result.push_back(Descriptor(
        CanonicalAction::InputAcquireLease,
        service(SessionServiceCapability::Input),
        0,
        5000,
        ActionEpochPolicy::RequiresCurrentEpoch,
        ActionReplayClass::RecordedEvidence,
        ActionCancellationMode::CleanupRequired,
        ActionResourceBehavior::Promotable,
        ActionCleanupGuarantee::VerifiedCompensation,
        true));
    for (CanonicalAction action : {
             CanonicalAction::InputApplyState,
             CanonicalAction::InputBeginDelivery,
         })
    {
        result.push_back(Descriptor(
            action,
            service(SessionServiceCapability::Input),
            effect(ActionEffect::PublishInput),
            30000,
            ActionEpochPolicy::RequiresCurrentEpoch,
            ActionReplayClass::RecordedEvidence,
            ActionCancellationMode::CleanupRequired,
            ActionResourceBehavior::None,
            ActionCleanupGuarantee::VerifiedCompensation,
            true));
    }
    result.push_back(Descriptor(
        CanonicalAction::InputCompleteDelivery,
        service(SessionServiceCapability::Input),
        0,
        5000,
        ActionEpochPolicy::RequiresCurrentEpoch,
        ActionReplayClass::RecordedEvidence,
        ActionCancellationMode::BeforeMutationOnly,
        ActionResourceBehavior::None,
        ActionCleanupGuarantee::None,
        false,
        ActionIdempotency::ReceiptProven));

    result.push_back(Descriptor(
        CanonicalAction::MoviePrepareReadOnlyPlayback,
        service(SessionServiceCapability::Movie),
        effect(ActionEffect::MoviePlayback),
        60000,
        ActionEpochPolicy::RequiresCurrentEpoch,
        ActionReplayClass::RecordedEvidence,
        ActionCancellationMode::CleanupRequired,
        ActionResourceBehavior::Promotable,
        ActionCleanupGuarantee::VerifiedCompensation,
        true));
    result.push_back(Descriptor(
        CanonicalAction::MovieStartPlayback,
        service(SessionServiceCapability::Movie),
        effect(ActionEffect::MoviePlayback),
        60000,
        ActionEpochPolicy::RequiresCurrentEpoch,
        ActionReplayClass::RecordedEvidence,
        ActionCancellationMode::CleanupRequired,
        ActionResourceBehavior::Promotable,
        ActionCleanupGuarantee::VerifiedCompensation,
        true));
    result.push_back(Descriptor(
        CanonicalAction::MovieStopPlayback,
        service(SessionServiceCapability::Movie),
        effect(ActionEffect::MoviePlayback),
        30000,
        ActionEpochPolicy::RequiresCurrentEpoch,
        ActionReplayClass::RecordedEvidence,
        ActionCancellationMode::CleanupRequired,
        ActionResourceBehavior::None,
        ActionCleanupGuarantee::VerifiedCompensation,
        true,
        ActionIdempotency::NaturallyIdempotent));
    result.push_back(Descriptor(
        CanonicalAction::MovieStartRecording,
        service(SessionServiceCapability::Movie),
        effect(ActionEffect::MovieRecording),
        60000,
        ActionEpochPolicy::RequiresCurrentEpoch,
        ActionReplayClass::RecordedEvidence,
        ActionCancellationMode::CleanupRequired,
        ActionResourceBehavior::Promotable,
        ActionCleanupGuarantee::VerifiedCompensation,
        true));
    result.push_back(Descriptor(
        CanonicalAction::MovieStopRecording,
        services(
            SessionServiceCapability::Movie,
            SessionServiceCapability::Artifact),
        effects(
            ActionEffect::MovieRecording,
            ActionEffect::ArtifactIo),
        60000,
        ActionEpochPolicy::RequiresCurrentEpoch,
        ActionReplayClass::ExternalCommit,
        ActionCancellationMode::CleanupRequired,
        ActionResourceBehavior::None,
        ActionCleanupGuarantee::VerifiedCompensation,
        true,
        ActionIdempotency::ReceiptProven));

    for (CanonicalAction action : {
             CanonicalAction::GuestReadU8,
             CanonicalAction::GuestReadU16,
             CanonicalAction::GuestReadU32,
             CanonicalAction::GuestReadU64,
             CanonicalAction::GuestRunCoherentQuery,
         })
    {
        result.push_back(Descriptor(
            action,
            service(SessionServiceCapability::GuestMemory),
            effect(ActionEffect::ReadGuest),
            5000,
            ActionEpochPolicy::RequiresCurrentEpoch,
            ActionReplayClass::RecordedEvidence,
            ActionCancellationMode::BeforeMutationOnly,
            ActionResourceBehavior::None,
            ActionCleanupGuarantee::None,
            false,
            ActionIdempotency::NaturallyIdempotent));
    }
    for (CanonicalAction action : {
             CanonicalAction::GuestWriteData,
             CanonicalAction::GuestPatchExecutable,
         })
    {
        result.push_back(Descriptor(
            action,
            service(SessionServiceCapability::GuestMutation),
            effect(ActionEffect::MutateGuest),
            30000,
            ActionEpochPolicy::RequiresCurrentEpoch,
            ActionReplayClass::RecordedEvidence,
            ActionCancellationMode::CleanupRequired,
            ActionResourceBehavior::Promotable,
            ActionCleanupGuarantee::VerifiedCompensation,
            true));
    }

    result.push_back(Descriptor(
        CanonicalAction::CaptureMark,
        service(SessionServiceCapability::Capture),
        effect(ActionEffect::Capture),
        5000));
    result.push_back(Descriptor(
        CanonicalAction::ScreenshotCapture,
        services(
            SessionServiceCapability::Screenshot,
            SessionServiceCapability::Artifact),
        effect(ActionEffect::ArtifactIo),
        30000,
        ActionEpochPolicy::RequiresCurrentEpoch,
        ActionReplayClass::ExternalCommit,
        ActionCancellationMode::BeforeMutationOnly,
        ActionResourceBehavior::None,
        ActionCleanupGuarantee::VerifiedCompensation,
        true,
        ActionIdempotency::ReceiptProven));
    result.push_back(Descriptor(
        CanonicalAction::TelemetryEmit,
        service(SessionServiceCapability::Telemetry),
        effect(ActionEffect::Telemetry),
        5000,
        ActionEpochPolicy::EpochAgnostic,
        ActionReplayClass::ExternalCommit,
        ActionCancellationMode::BeforeMutationOnly,
        ActionResourceBehavior::None,
        ActionCleanupGuarantee::None,
        false,
        ActionIdempotency::ReceiptProven));
    result.push_back(Descriptor(
        CanonicalAction::ExecutionRequirePausedPc,
        service(SessionServiceCapability::Execution),
        0,
        5000,
        ActionEpochPolicy::RequiresCurrentEpoch,
        ActionReplayClass::RecordedEvidence,
        ActionCancellationMode::BeforeMutationOnly,
        ActionResourceBehavior::None,
        ActionCleanupGuarantee::None,
        false,
        ActionIdempotency::NaturallyIdempotent));

    if (result.size() != kActions.size())
        throw std::logic_error("Canonical action descriptor catalog is incomplete");

    for (const CanonicalActionDefinition& definition : kActions)
    {
        const auto found = std::ranges::find(
            result,
            definition.name,
            [](const ActionDescriptor& descriptor)
            {
                return std::string_view(descriptor.identity.canonical_id);
            });
        if (found == result.end())
            throw std::logic_error(
                "Canonical action descriptor catalog is incomplete");
    }

    std::ranges::sort(
        result,
        {},
        [](const ActionDescriptor& descriptor)
        {
            return std::pair{
                descriptor.identity.canonical_id,
                descriptor.identity.version};
        });

    for (ActionDescriptor& descriptor : result)
    {
        descriptor.identity.signature_hash =
            ComputeActionDescriptorContractHash(descriptor);
    }
    const CapabilityPackIdentity pack =
        RuntimePackIdentity(result);
    for (ActionDescriptor& descriptor : result)
        descriptor.providing_pack = pack;
    return result;
}

} // namespace savor::runtime::program
