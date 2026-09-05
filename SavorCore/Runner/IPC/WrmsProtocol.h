#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace savor::wrms {

inline constexpr std::array<std::uint8_t, 4> Magic{ 'W', 'R', 'M', 'S' };
inline constexpr std::uint16_t ProtocolVersion = 6;
inline constexpr std::size_t HeaderSize = 20;
inline constexpr std::size_t MaximumPayloadSize = 64u * 1024u * 1024u;

enum class MessageKind : std::uint16_t {
    ProcessHello = 0x0001,
    OpenSession = 0x0010,
    CancelInvocation = 0x0013,
    Shutdown = 0x0015,
    ControlExecution = 0x0016,
    SubmitWorkset = 0x0017,
    CancelWorksetItem = 0x0018,
    CancelWorkset = 0x0019,
    AcknowledgeTerminal = 0x001a,
    // Host-only control-plane probe. The worker transport answers directly;
    // it never enters guest execution or the WorkerRuntime actor.
    LivenessProbe = 0x001b,

    CommandResult = 0x0100,
    OpenSessionResult = 0x0101,
    ShutdownResult = 0x0103,
    SessionEvent = 0x0104,
    InvocationProgress = 0x0105,
    HostEvent = 0x0107,
    RuntimeDiagnostic = 0x0108,
    ExecutionResult = 0x0109,
    ExecutionState = 0x010a,
    WorksetState = 0x010b,
    WorksetItemStarted = 0x010c,
    WorksetItemTerminal = 0x010d,
    WorksetCredits = 0x010e,
    WorksetSummary = 0x010f,
};

[[nodiscard]] bool IsKnownMessageKind(MessageKind kind) noexcept;

enum class MessageDirection : std::uint8_t {
    ParentToWorker,
    WorkerToParent,
};

[[nodiscard]] MessageDirection DirectionOf(MessageKind kind) noexcept;

enum class MessageChannel : std::uint8_t {
    Command,
    Response,
    Event,
};

// WRMS v6 physically isolates correlated responses from asynchronous events.
[[nodiscard]] MessageChannel ChannelOf(MessageKind kind) noexcept;

struct FrameHeader {
    std::uint16_t version = ProtocolVersion;
    MessageKind kind = MessageKind::ProcessHello;
    std::uint32_t payload_size = 0;
    std::uint64_t request_id = 0;

    friend bool operator==(const FrameHeader&, const FrameHeader&) = default;
};

struct FrameView {
    FrameHeader header;
    std::span<const std::uint8_t> payload;
};

enum class FrameError : std::uint8_t {
    None,
    TruncatedHeader,
    WrongMagic,
    UnsupportedVersion,
    UnknownMessageKind,
    PayloadTooLarge,
    TruncatedPayload,
};

enum class FrameDecodeStatus : std::uint8_t {
    Complete,
    NeedMoreData,
    Error,
};

struct FrameDecodeResult {
    FrameDecodeStatus status = FrameDecodeStatus::NeedMoreData;
    FrameError error = FrameError::None;
    std::size_t required_size = HeaderSize;
    std::size_t consumed_size = 0;
    FrameView frame{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return status == FrameDecodeStatus::Complete;
    }
};

// A partial header or payload is NeedMoreData while more input is expected.
// The same input is a typed truncation error when end_of_stream is true.
// A complete result views the caller-owned input and consumes exactly one frame.
[[nodiscard]] FrameDecodeResult DecodeFrame(
    std::span<const std::uint8_t> input,
    bool end_of_stream = false) noexcept;

struct FrameEncodeResult {
    FrameError error = FrameError::None;
    std::vector<std::uint8_t> bytes;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == FrameError::None;
    }
};

// Validates the kind and payload bound before allocating the encoded frame.
[[nodiscard]] FrameEncodeResult EncodeFrame(
    MessageKind kind,
    std::uint64_t request_id,
    std::span<const std::uint8_t> payload);

enum class CommandStatus : std::uint8_t {
    Succeeded = 0,
    Rejected = 1,
    Failed = 2,
    Unsupported = 3,
};

enum class ShutdownStatus : std::uint8_t {
    Graceful = 0,
    CleanupFailed = 1,
};

enum class SessionEventType : std::uint8_t {
    Snapshot = 0,
    StateChanged = 1,
    Tainted = 2,
};

enum class InvocationTerminalStatus : std::uint8_t {
    Succeeded = 0,
    Failed = 1,
    Cancelled = 2,
    InfrastructureFailure = 3,
    CleanupFailure = 4,
    TimedOut = 5,
};

enum class WorkerStateCode : std::uint8_t {
    Starting = 0,
    AwaitingSession = 1,
    Ready = 2,
    InitializingWorkset = 3,
    Running = 4,
    Cancelling = 5,
    Tainted = 6,
    Stopping = 7,
    Stopped = 8,
};

enum class ExecutionControlKind : std::uint8_t {
    Pause = 0,
    Resume = 1,
    // Reserved for the removed guest-instruction-step control. Encoding or
    // decoding this value is invalid; StepFrame intentionally remains 3.
    ReservedGuestInstruction = 2,
    StepFrame = 3,
};

enum class ExecutionActivityCode : std::uint8_t {
    IdlePaused = 0,
    InteractiveRunning = 1,
    HandlingInterruption = 2,
    Failed = 3,
};

enum class ExecutionTerminalStatusCode : std::uint8_t {
    RequestedCompletion = 0,
    StepsCompleted = 1,
    Paused = 2,
    Cancelled = 3,
    TimedOut = 4,
    CoreStalled = 5,
    MovieEnded = 6,
    UnexpectedStop = 7,
    InterruptionUnavailable = 8,
    InterruptionAborted = 9,
    InterruptionDepthExceeded = 10,
    InterruptionFailed = 11,
    WorksetEpochMismatch = 12,
    Unsupported = 13,
    BackendFailure = 14,
    CleanupFailure = 15,
};

enum class SessionDispositionCode : std::uint8_t {
    Closed = 0,
    Clean = 1,
    CleanWithDiagnostics = 2,
    Tainted = 3,
};

enum class WorkerModeCode : std::uint8_t {
    Headless = 0,
    Visual = 1,
    VisualDebug = 2,
};

enum class RejectionCode : std::uint16_t {
    None = 0,
    Unsupported = 1,
    InvalidState = 2,
    InvalidArgument = 3,
    SessionUnavailable = 4,
    SessionMismatch = 5,
    SessionTainted = 6,
    ProgramRuntimeUnavailable = 7,
    InvocationAlreadyActive = 8,
    InvocationNotActive = 9,
    InvocationMismatch = 10,
    DuplicateCancellation = 11,
    WorksetEpochMismatch = 12,
    BackendFailure = 13,
    RuntimeStopping = 14,
    InternalFailure = 15,
    WorksetAlreadyActive = 16,
    WorksetNotFound = 17,
    WorksetItemNotFound = 18,
    ProgramPackageRejected = 19,
    CapacityExceeded = 20,
    TerminalNotFound = 21,
    TerminalMismatch = 22,
    WorksetItemAlreadyTerminal = 23,
};

struct ProcessHelloPayload {
    std::uint64_t worker_id = 0;
    std::uint32_t process_id = 0;
    std::vector<std::uint8_t> encoded_runtime_contract;

    friend bool operator==(const ProcessHelloPayload&, const ProcessHelloPayload&) = default;
};

struct OpenSessionPayload {
    std::string runtime_root;
    std::string user_directory;
    std::string iso_path;
    WorkerModeCode worker_mode = WorkerModeCode::Headless;
    std::uint64_t render_window_handle = 0;
    std::string runtime_artifact_root;
    std::string session_filesystem_preparation_id;
    std::uint64_t process_generation = 0;

    friend bool operator==(const OpenSessionPayload&, const OpenSessionPayload&) = default;
};

struct SubmitWorksetPayload {
    std::vector<std::uint8_t> encoded_workset;
    std::string workset_sha256;
    std::uint32_t cancellation_sidecar_version = 1;
    std::vector<std::uint64_t> initially_cancelled_item_ids;
    std::string cancellation_sidecar_sha256;

    friend bool operator==(
        const SubmitWorksetPayload&,
        const SubmitWorksetPayload&) = default;
};

struct SubmitWorksetResultPayload {
    std::uint32_t format_version = 1;
    std::uint64_t workset_id = 0;
    std::uint32_t sidecar_version = 1;
    std::uint32_t applied_item_count = 0;
    std::string applied_sidecar_sha256;
    bool already_admitted = false;

    friend bool operator==(
        const SubmitWorksetResultPayload&,
        const SubmitWorksetResultPayload&) = default;
};

struct CancelWorksetItemPayload {
    std::uint64_t workset_id = 0;
    std::uint64_t item_id = 0;
    std::string reason;

    friend bool operator==(
        const CancelWorksetItemPayload&,
        const CancelWorksetItemPayload&) = default;
};

struct CancelWorksetPayload {
    std::uint64_t workset_id = 0;
    std::string reason;

    friend bool operator==(
        const CancelWorksetPayload&,
        const CancelWorksetPayload&) = default;
};

struct AcknowledgeTerminalPayload {
    std::uint64_t workset_id = 0;
    std::uint64_t item_id = 0;
    std::uint32_t item_ordinal = 0;
    std::uint64_t invocation_id = 0;
    std::uint64_t attempt_id = 0;
    std::uint64_t terminal_id = 0;
    std::uint64_t terminal_order = 0;

    friend bool operator==(
        const AcknowledgeTerminalPayload&,
        const AcknowledgeTerminalPayload&) = default;
};

struct CancelInvocationPayload {
    std::uint64_t invocation_id = 0;
    std::string reason;

    friend bool operator==(const CancelInvocationPayload&, const CancelInvocationPayload&) = default;
};

struct ShutdownPayload {
    std::uint32_t grace_period_ms = 0;

    friend bool operator==(const ShutdownPayload&, const ShutdownPayload&) = default;
};

struct ControlExecutionPayload {
    ExecutionControlKind control = ExecutionControlKind::Pause;
    std::uint64_t workset_id = 0;
    std::uint64_t item_id = 0;
    std::uint32_t count = 0;
    std::uint32_t timeout_ms = 0;

    friend bool operator==(
        const ControlExecutionPayload&,
        const ControlExecutionPayload&) = default;
};

struct CommandResultPayload {
    std::uint64_t command_sequence = 0;
    MessageKind command_kind = MessageKind::OpenSession;
    CommandStatus status = CommandStatus::Succeeded;
    RejectionCode rejection_code = RejectionCode::None;
    std::string error_code;
    std::string message;
    std::vector<std::uint8_t> result;

    friend bool operator==(const CommandResultPayload&, const CommandResultPayload&) = default;
};

struct OpenSessionResultPayload {
    bool success = false;
    std::uint64_t session_id = 0;
    std::uint64_t workset_epoch = 0;
    WorkerModeCode worker_mode = WorkerModeCode::Headless;
    WorkerStateCode worker_state = WorkerStateCode::Starting;
    SessionDispositionCode session_disposition = SessionDispositionCode::Closed;
    RejectionCode rejection_code = RejectionCode::None;
    std::string error_code;
    std::string message;

    friend bool operator==(const OpenSessionResultPayload&, const OpenSessionResultPayload&) = default;
};

struct ShutdownResultPayload {
    ShutdownStatus status = ShutdownStatus::Graceful;
    SessionDispositionCode final_disposition = SessionDispositionCode::Closed;
    RejectionCode rejection_code = RejectionCode::None;
    std::string error_code;
    std::string message;

    friend bool operator==(const ShutdownResultPayload&, const ShutdownResultPayload&) = default;
};

struct SessionEventPayload {
    SessionEventType event_type = SessionEventType::Snapshot;
    std::uint64_t session_id = 0;
    std::uint64_t workset_epoch = 0;
    WorkerModeCode worker_mode = WorkerModeCode::Headless;
    WorkerStateCode worker_state = WorkerStateCode::Starting;
    SessionDispositionCode session_disposition = SessionDispositionCode::Closed;
    RejectionCode rejection_code = RejectionCode::None;
    std::string code;
    std::string message;

    friend bool operator==(const SessionEventPayload&, const SessionEventPayload&) = default;
};

struct InvocationProgressPayload {
    std::uint64_t invocation_id = 0;
    std::uint64_t attempt_id = 0;
    std::uint64_t workset_id = 0;
    std::uint64_t item_id = 0;
    std::uint32_t item_ordinal = 0;
    std::uint64_t ordinal = 0;
    std::string durable_job_id;
    std::string library_id;
    std::uint32_t library_revision = 0;
    std::string progress_point_id;
    bool has_routed_provenance = false;
    std::uint64_t routed_sequence = 0;
    std::uint64_t sample_snapshot_id = 0;
    std::uint64_t trigger_epoch = 0;
    std::string schema_id;
    std::uint32_t schema_revision = 0;
    std::string schema_sha256;
    std::vector<std::uint8_t> typed_payload;
    std::string display_text;

    friend bool operator==(const InvocationProgressPayload&, const InvocationProgressPayload&) = default;
};

enum class WorksetStateCode : std::uint8_t {
    Validating = 0,
    Admitted = 1,
    Initializing = 2,
    Ready = 3,
    Running = 4,
    ResettingItem = 5,
    Draining = 6,
    Completed = 7,
    Cancelled = 8,
    Failed = 9,
};

struct WorksetStatePayload {
    std::uint64_t outbound_sequence = 0;
    std::uint64_t workset_id = 0;
    WorksetStateCode state = WorksetStateCode::Validating;
    std::uint32_t next_item_ordinal = 0;
    RejectionCode rejection_code = RejectionCode::None;
    std::string message;

    friend bool operator==(
        const WorksetStatePayload&,
        const WorksetStatePayload&) = default;
};

struct WorksetResidenceSnapshotV1 {
    std::uint32_t format_version = 1;
    bool has_resident_workset = false;
    std::uint64_t workset_id = 0;
    WorksetStateCode state = WorksetStateCode::Validating;
    std::string cancellation_sidecar_sha256;

    friend bool operator==(
        const WorksetResidenceSnapshotV1&,
        const WorksetResidenceSnapshotV1&) = default;
};

struct WorksetItemStartedPayload {
    std::uint64_t outbound_sequence = 0;
    std::uint64_t workset_id = 0;
    std::uint64_t item_id = 0;
    std::uint32_t item_ordinal = 0;
    std::uint64_t invocation_id = 0;
    std::uint64_t attempt_id = 0;
    std::uint64_t session_id = 0;
    std::uint64_t workset_epoch = 0;
    std::string baseline_sha256;
    std::string baseline_lineage;
    bool baseline_state_established = false;

    friend bool operator==(
        const WorksetItemStartedPayload&,
        const WorksetItemStartedPayload&) = default;
};

struct WorksetArtifactPayload {
    std::string artifact_id;
    std::string schema_id;
    std::uint32_t schema_version = 0;
    std::string schema_sha256;
    std::string content_sha256;
    std::string storage_reference;
    bool complete = false;

    friend bool operator==(
        const WorksetArtifactPayload&,
        const WorksetArtifactPayload&) = default;
};

struct WorksetItemTerminalPayload {
    std::uint64_t outbound_sequence = 0;
    std::uint64_t workset_id = 0;
    std::uint64_t item_id = 0;
    std::uint32_t item_ordinal = 0;
    std::uint64_t invocation_id = 0;
    std::uint64_t attempt_id = 0;
    std::uint64_t terminal_id = 0;
    std::uint64_t terminal_order = 0;
    InvocationTerminalStatus status = InvocationTerminalStatus::Failed;
    SessionDispositionCode session_disposition =
        SessionDispositionCode::Closed;
    std::uint64_t workset_epoch = 0;
    bool unstarted = false;
    std::uint8_t cancellation_reason = 0;
    RejectionCode rejection_code = RejectionCode::None;
    std::string error_code;
    std::string message;
    std::vector<std::uint8_t> result;
    std::vector<WorksetArtifactPayload> workset_artifacts;
    std::vector<std::string> diagnostics;

    friend bool operator==(
        const WorksetItemTerminalPayload&,
        const WorksetItemTerminalPayload&) = default;
};

struct WorksetCreditsPayload {
    std::uint64_t outbound_sequence = 0;
    std::uint32_t available_item_credits = 0;
    std::uint32_t active_and_staged_items = 0;
    std::uint32_t retained_terminals = 0;

    friend bool operator==(
        const WorksetCreditsPayload&,
        const WorksetCreditsPayload&) = default;
};

struct WorksetSummaryPayload {
    std::uint64_t outbound_sequence = 0;
    std::uint64_t workset_id = 0;
    std::uint32_t item_count = 0;
    std::uint32_t completed_count = 0;
    std::uint32_t unstarted_count = 0;
    std::uint32_t initially_suppressed_count = 0;

    friend bool operator==(
        const WorksetSummaryPayload&,
        const WorksetSummaryPayload&) = default;
};

struct HostEventPayload {
    std::uint64_t session_id = 0;
    std::uint64_t workset_epoch = 0;
    std::uint64_t sequence = 0;
    std::string name;
    std::vector<std::uint8_t> event_data;

    friend bool operator==(const HostEventPayload&, const HostEventPayload&) = default;
};

struct RuntimeDiagnosticPayload {
    RejectionCode rejection_code = RejectionCode::InternalFailure;
    std::uint64_t command_sequence = 0;
    std::uint64_t invocation_id = 0;
    std::string message;

    friend bool operator==(
        const RuntimeDiagnosticPayload&,
        const RuntimeDiagnosticPayload&) = default;
};

struct ExecutionResultPayload {
    std::uint64_t command_sequence = 0;
    ExecutionControlKind control = ExecutionControlKind::Pause;
    CommandStatus status = CommandStatus::Succeeded;
    std::uint64_t session_id = 0;
    std::uint64_t workset_epoch = 0;
    std::uint64_t operation_id = 0;
    bool has_execution_state = false;
    ExecutionActivityCode activity = ExecutionActivityCode::IdlePaused;
    bool has_terminal_status = false;
    ExecutionTerminalStatusCode terminal_status =
        ExecutionTerminalStatusCode::RequestedCompletion;
    std::uint64_t completed_count = 0;
    std::uint32_t program_counter = 0;
    RejectionCode rejection_code = RejectionCode::None;
    std::string error_code;
    std::string message;

    friend bool operator==(
        const ExecutionResultPayload&,
        const ExecutionResultPayload&) = default;
};

struct ExecutionStatePayload {
    std::uint64_t session_id = 0;
    std::uint64_t workset_epoch = 0;
    std::uint64_t operation_id = 0;
    ExecutionActivityCode activity = ExecutionActivityCode::IdlePaused;
    bool has_active_control = false;
    ExecutionControlKind active_control = ExecutionControlKind::Pause;
    std::uint64_t completed_count = 0;
    std::uint32_t program_counter = 0;
    RejectionCode rejection_code = RejectionCode::None;
    std::string code;
    std::string message;

    friend bool operator==(
        const ExecutionStatePayload&,
        const ExecutionStatePayload&) = default;
};

enum class PayloadError : std::uint8_t {
    None,
    Truncated,
    FieldTooLarge,
    InvalidBoolean,
    InvalidMessageKind,
    InvalidEnumValue,
    TrailingBytes,
    PayloadTooLarge,
    InvalidValue,
};

struct PayloadCodecResult {
    PayloadError error = PayloadError::None;
    std::size_t offset = 0;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == PayloadError::None;
    }
};

#define SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(Type)                                      \
    [[nodiscard]] PayloadCodecResult EncodePayload(                                 \
        const Type& value, std::vector<std::uint8_t>& output);                      \
    [[nodiscard]] PayloadCodecResult DecodePayload(                                 \
        std::span<const std::uint8_t> input, Type& output)

SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(ProcessHelloPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(OpenSessionPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(SubmitWorksetPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(SubmitWorksetResultPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(CancelWorksetItemPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(CancelWorksetPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(AcknowledgeTerminalPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(CancelInvocationPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(ShutdownPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(ControlExecutionPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(CommandResultPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(OpenSessionResultPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(ShutdownResultPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(SessionEventPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(InvocationProgressPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(WorksetStatePayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(WorksetResidenceSnapshotV1);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(WorksetItemStartedPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(WorksetItemTerminalPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(WorksetCreditsPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(WorksetSummaryPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(HostEventPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(RuntimeDiagnosticPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(ExecutionResultPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(ExecutionStatePayload);

#undef SAVOR_WRMS_DECLARE_PAYLOAD_CODEC

} // namespace savor::wrms
