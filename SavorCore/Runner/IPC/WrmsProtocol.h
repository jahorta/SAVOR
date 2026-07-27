#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace savor::wrms {

inline constexpr std::array<std::uint8_t, 4> Magic{ 'W', 'R', 'M', 'S' };
inline constexpr std::uint16_t ProtocolVersion = 1;
inline constexpr std::size_t HeaderSize = 20;
inline constexpr std::size_t MaximumPayloadSize = 64u * 1024u * 1024u;

enum class MessageKind : std::uint16_t {
    ProcessHello = 0x0001,

    OpenSession = 0x0010,
    PrepareModule = 0x0011,
    SubmitInvocation = 0x0012,
    CancelInvocation = 0x0013,
    CaptureScreenshot = 0x0014,
    Shutdown = 0x0015,
    ControlExecution = 0x0016,

    CommandResult = 0x0100,
    OpenSessionResult = 0x0101,
    ScreenshotResult = 0x0102,
    ShutdownResult = 0x0103,
    SessionEvent = 0x0104,
    InvocationProgress = 0x0105,
    InvocationTerminal = 0x0106,
    HostEvent = 0x0107,
    RuntimeDiagnostic = 0x0108,
    ExecutionResult = 0x0109,
    ExecutionState = 0x010a,
};

[[nodiscard]] bool IsKnownMessageKind(MessageKind kind) noexcept;

enum class MessageDirection : std::uint8_t {
    ParentToWorker,
    WorkerToParent,
};

[[nodiscard]] MessageDirection DirectionOf(MessageKind kind) noexcept;

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

enum class ScreenshotStatus : std::uint8_t {
    Captured = 0,
    Failed = 1,
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
    Running = 3,
    Cancelling = 4,
    Tainted = 5,
    Stopping = 6,
    Stopped = 7,
};

enum class ExecutionControlKind : std::uint8_t {
    Pause = 0,
    Resume = 1,
    StepInstruction = 2,
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
    ViStalled = 5,
    MovieEnded = 6,
    ConsumedStop = 7,
    UnexpectedStop = 8,
    GuardFailed = 9,
    InterruptionUnavailable = 10,
    InterruptionAborted = 11,
    InterruptionDepthExceeded = 12,
    InterruptionFailed = 13,
    StateEpochMismatch = 14,
    Unsupported = 15,
    BackendFailure = 16,
    CleanupFailure = 17,
};

enum class SessionDispositionCode : std::uint8_t {
    Closed = 0,
    Clean = 1,
    CleanWithDiagnostics = 2,
    Tainted = 3,
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
    StateEpochMismatch = 12,
    BackendFailure = 13,
    RuntimeStopping = 14,
    InternalFailure = 15,
};

struct ProcessHelloPayload {
    std::uint64_t worker_id = 0;
    std::uint32_t process_id = 0;
    std::uint64_t capability_mask = 0;
    std::string build_identity;

    friend bool operator==(const ProcessHelloPayload&, const ProcessHelloPayload&) = default;
};

struct OpenSessionPayload {
    std::string runtime_root;
    std::string user_directory;
    std::string iso_path;
    bool visual_requested = false;
    std::uint64_t render_window_handle = 0;
    std::string screenshot_directory;
    std::uint32_t screenshot_timeout_ms = 0;
    bool screenshot_on_terminal = false;

    friend bool operator==(const OpenSessionPayload&, const OpenSessionPayload&) = default;
};

struct PrepareModulePayload {
    std::string canonical_id;
    std::uint32_t revision = 0;
    std::string canonical_hash;
    std::uint32_t format_version = 0;
    std::vector<std::uint8_t> encoded_module;

    friend bool operator==(const PrepareModulePayload&, const PrepareModulePayload&) = default;
};

struct SubmitInvocationPayload {
    std::uint64_t invocation_id = 0;
    std::uint64_t attempt_id = 0;
    std::string module_canonical_id;
    std::uint32_t module_revision = 0;
    std::string module_canonical_hash;
    std::string entrypoint;
    std::uint64_t expected_state_epoch = 0;
    std::vector<std::uint8_t> encoded_invocation;

    friend bool operator==(const SubmitInvocationPayload&, const SubmitInvocationPayload&) = default;
};

struct CancelInvocationPayload {
    std::uint64_t invocation_id = 0;
    std::string reason;

    friend bool operator==(const CancelInvocationPayload&, const CancelInvocationPayload&) = default;
};

struct CaptureScreenshotPayload {
    std::uint64_t session_id = 0;
    std::string output_path;
    std::uint32_t timeout_ms = 0;

    friend bool operator==(const CaptureScreenshotPayload&, const CaptureScreenshotPayload&) = default;
};

struct ShutdownPayload {
    std::uint32_t grace_period_ms = 0;

    friend bool operator==(const ShutdownPayload&, const ShutdownPayload&) = default;
};

struct ControlExecutionPayload {
    ExecutionControlKind control = ExecutionControlKind::Pause;
    std::uint64_t session_id = 0;
    std::uint64_t expected_state_epoch = 0;
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
    std::uint64_t state_epoch = 0;
    std::uint64_t capability_mask = 0;
    WorkerStateCode worker_state = WorkerStateCode::Starting;
    SessionDispositionCode session_disposition = SessionDispositionCode::Closed;
    RejectionCode rejection_code = RejectionCode::None;
    std::string error_code;
    std::string message;

    friend bool operator==(const OpenSessionResultPayload&, const OpenSessionResultPayload&) = default;
};

struct ScreenshotResultPayload {
    ScreenshotStatus status = ScreenshotStatus::Captured;
    std::uint64_t session_id = 0;
    std::uint64_t state_epoch = 0;
    std::string output_path;
    RejectionCode rejection_code = RejectionCode::None;
    std::string error_code;
    std::string message;

    friend bool operator==(const ScreenshotResultPayload&, const ScreenshotResultPayload&) = default;
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
    std::uint64_t state_epoch = 0;
    std::uint64_t capability_mask = 0;
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
    std::uint64_t ordinal = 0;
    std::vector<std::uint8_t> progress;

    friend bool operator==(const InvocationProgressPayload&, const InvocationProgressPayload&) = default;
};

struct InvocationTerminalPayload {
    std::uint64_t invocation_id = 0;
    std::uint64_t attempt_id = 0;
    InvocationTerminalStatus status = InvocationTerminalStatus::Succeeded;
    SessionDispositionCode session_disposition = SessionDispositionCode::Closed;
    std::uint64_t state_epoch = 0;
    RejectionCode rejection_code = RejectionCode::None;
    std::string error_code;
    std::string message;
    std::vector<std::uint8_t> result;

    friend bool operator==(const InvocationTerminalPayload&, const InvocationTerminalPayload&) = default;
};

struct HostEventPayload {
    std::uint64_t session_id = 0;
    std::uint64_t state_epoch = 0;
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
    std::uint64_t state_epoch = 0;
    std::uint64_t operation_id = 0;
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
    std::uint64_t state_epoch = 0;
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
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(PrepareModulePayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(SubmitInvocationPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(CancelInvocationPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(CaptureScreenshotPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(ShutdownPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(ControlExecutionPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(CommandResultPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(OpenSessionResultPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(ScreenshotResultPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(ShutdownResultPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(SessionEventPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(InvocationProgressPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(InvocationTerminalPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(HostEventPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(RuntimeDiagnosticPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(ExecutionResultPayload);
SAVOR_WRMS_DECLARE_PAYLOAD_CODEC(ExecutionStatePayload);

#undef SAVOR_WRMS_DECLARE_PAYLOAD_CODEC

} // namespace savor::wrms
