#pragma once

#include "CapabilityPackRegistry.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace savor::runtime::program {

enum class CanonicalAction : std::uint8_t
{
    SavestateSaveImmutableArtifact,
    ExecutionContinueUntil,
    ExecutionStepFrames,
    InputAcquireLease,
    InputApplyState,
    InputBeginDelivery,
    InputCompleteDelivery,
    MoviePrepareReadOnlyPlayback,
    MovieStartPlayback,
    MovieAdoptRestoredReadOnlyPlayback,
    MovieObserveState,
    MovieStopPlayback,
    MovieStartRecording,
    MovieStopRecording,
    GuestReadU8,
    GuestReadU16,
    GuestReadU32,
    GuestReadU64,
    GuestRunCoherentQuery,
    GuestWriteData,
    GuestPatchExecutable,
    CaptureMark,
    ScreenshotCapture,
    TelemetryEmit,
    ExecutionRequirePausedPc,
    ExecutionContinueUntilInputObserved,
    ExecutionObservePausedPc,
};

struct CanonicalActionDefinition
{
    CanonicalAction action;
    std::string_view name;
    std::string_view signature;
};

enum class CanonicalReducer : std::uint8_t
{
    BattlePrepareCommandInteraction,
    BattleCommandInteractionInitialize,
    BattleCommandInteractionAdvance,
    BattleCommandInteractionCompleteSegment,
    BattleCommandInteractionFinalize,
};

// Exact auxiliary schemas used by canonical action request records. These are
// ordinary value-model types: policy/configuration bytes are bounded immutable
// leaves, while every live handle, receipt, count, address, and deadline stays
// as its own typed record field.
enum class CanonicalRuntimeSchema : std::uint8_t
{
    InputFramePayload,
    SemanticPointSet,
    ContinueUntilStaticConfig,
    ExecutionAdvanceStaticConfig,
    InputLeaseStaticConfig,
    ObservationStaticConfig,
    MovieRecordingStaticConfig,
    MovieState,
    MovieStateObservation,
    StopEvidencePayload,
    OptionalInputExecutionBinding,
    OptionalMoviePlaybackSession,
    OptionalMovieInputCount,
    ContinueUntilCompletionReason,
    RoutedStopReceipt,
    OptionalRoutedStopReceipt,
    OptionalContinueUntilResult,
};

enum class ContinueUntilCompletionReasonV1 : std::int64_t
{
    Breakpoint = 0,
    CursorOverrun = 1,
    MovieEnded = 2,
};

[[nodiscard]] std::string_view CanonicalActionName(
    CanonicalAction action) noexcept;
[[nodiscard]] std::span<const CanonicalActionDefinition>
CanonicalActionDefinitions() noexcept;
[[nodiscard]] ExactDependencyIdentity CanonicalActionIdentity(
    CanonicalAction action);
[[nodiscard]] std::optional<CanonicalAction> FindCanonicalAction(
    const ExactDependencyIdentity& identity);

[[nodiscard]] std::string_view CanonicalReducerName(
    CanonicalReducer reducer) noexcept;
[[nodiscard]] ExactDependencyIdentity CanonicalReducerIdentity(
    CanonicalReducer reducer);

// Canonical actions use exact nominal request/result types even where the
// current payload representation is bounded SAP1 bytes. Resource, artifact,
// and scalar results retain their corresponding value-model kinds so verified
// actions cannot exchange structurally similar but semantically unrelated
// values.
[[nodiscard]] std::optional<SchemaIdentity>
CanonicalActionInputSchemaIdentity(CanonicalAction action);
[[nodiscard]] std::optional<SchemaIdentity>
CanonicalActionOutputSchemaIdentity(CanonicalAction action);
[[nodiscard]] std::optional<SchemaIdentity>
CanonicalActionResourceContractSchemaIdentity(
    CanonicalAction action);
[[nodiscard]] std::optional<SchemaIdentity>
CanonicalActionArtifactPayloadSchemaIdentity(
    CanonicalAction action);
[[nodiscard]] std::optional<SchemaIdentity>
CanonicalActionArtifactReferenceSchemaIdentity(
    CanonicalAction action);
[[nodiscard]] TypeRef CanonicalActionInputType(CanonicalAction action);
[[nodiscard]] TypeRef CanonicalActionOutputType(CanonicalAction action);
[[nodiscard]] SchemaIdentity CanonicalRuntimeSchemaIdentity(
    CanonicalRuntimeSchema schema);
[[nodiscard]] TypeRef CanonicalRuntimeType(CanonicalRuntimeSchema schema);
[[nodiscard]] std::vector<SchemaIdentity>
CanonicalActionTypeSchemaClosure(CanonicalAction action);
[[nodiscard]] std::vector<TypeSchemaDefinition>
BuildCanonicalRuntimeActionSchemas();

[[nodiscard]] RuntimeCompatibility CanonicalRuntimeCompatibility();
[[nodiscard]] CapabilityPackIdentity CanonicalRuntimePackIdentity();
[[nodiscard]] std::vector<ActionDescriptor>
    BuildCanonicalRuntimeActionDescriptors();

} // namespace savor::runtime::program
