#include <gtest/gtest.h>

#include "Runner/IPC/WrmsProtocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace savor::wrms;

template <typename Payload>
void ExpectPayloadRoundTrip(const Payload& expected)
{
    std::vector<std::uint8_t> encoded{ 0xff };
    const auto encode_result = EncodePayload(expected, encoded);
    ASSERT_EQ(encode_result.error, PayloadError::None);

    Payload decoded;
    const auto decode_result = DecodePayload(encoded, decoded);
    ASSERT_EQ(decode_result.error, PayloadError::None);
    EXPECT_EQ(decoded, expected);
}

std::vector<std::uint8_t> MakeFrame(
    MessageKind kind,
    std::uint64_t request_id,
    std::span<const std::uint8_t> payload)
{
    auto encoded = EncodeFrame(kind, request_id, payload);
    EXPECT_EQ(encoded.error, FrameError::None);
    return std::move(encoded.bytes);
}

TEST(WorkerProtocolV1, EncodesExactWrmsLittleEndianGoldenFrame)
{
    constexpr std::array<std::uint8_t, 2> payload{ 0xaa, 0xbb };
    constexpr std::uint64_t request_id = 0x1122334455667788ull;

    const auto encoded = EncodeFrame(
        MessageKind::SubmitInvocation,
        request_id,
        payload);

    ASSERT_EQ(encoded.error, FrameError::None);
    const std::vector<std::uint8_t> expected{
        0x57, 0x52, 0x4d, 0x53, // WRMS
        0x01, 0x00,             // protocol version 1
        0x12, 0x00,             // SubmitInvocation
        0x02, 0x00, 0x00, 0x00, // payload length
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, // request ID
        0xaa, 0xbb,
    };
    EXPECT_EQ(encoded.bytes, expected);

    const auto decoded = DecodeFrame(encoded.bytes);
    ASSERT_EQ(decoded.status, FrameDecodeStatus::Complete);
    EXPECT_EQ(decoded.error, FrameError::None);
    EXPECT_EQ(decoded.consumed_size, expected.size());
    EXPECT_EQ(decoded.required_size, expected.size());
    EXPECT_EQ(decoded.frame.header.version, ProtocolVersion);
    EXPECT_EQ(decoded.frame.header.kind, MessageKind::SubmitInvocation);
    EXPECT_EQ(decoded.frame.header.payload_size, payload.size());
    EXPECT_EQ(decoded.frame.header.request_id, request_id);
    EXPECT_EQ(
        std::vector<std::uint8_t>(
            decoded.frame.payload.begin(),
            decoded.frame.payload.end()),
        std::vector<std::uint8_t>(payload.begin(), payload.end()));
}

TEST(WorkerProtocolV1, CoreStalledRetainsTheFormerViStallWireValue)
{
    EXPECT_EQ(
        static_cast<std::uint8_t>(
            ExecutionTerminalStatusCode::CoreStalled),
        5u);
}

TEST(WorkerProtocolV1, EncodesExactAdditiveExecutionGoldenFrames)
{
    {
        std::vector<std::uint8_t> payload;
        ASSERT_TRUE(EncodePayload(
            ControlExecutionPayload{
                .control = ExecutionControlKind::StepFrame,
                .session_id = 0x0102030405060708ull,
                .expected_state_epoch = 0x1112131415161718ull,
                .count = 2,
                .timeout_ms = 5000,
            },
            payload));
        const auto encoded = EncodeFrame(
            MessageKind::ControlExecution,
            0x2122232425262728ull,
            payload);
        ASSERT_EQ(encoded.error, FrameError::None);
        const std::vector<std::uint8_t> expected{
            0x57, 0x52, 0x4d, 0x53, // WRMS
            0x01, 0x00,             // protocol version 1
            0x16, 0x00,             // ControlExecution
            0x19, 0x00, 0x00, 0x00, // payload length
            0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,
            0x03, // StepFrame
            0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
            0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
            0x02, 0x00, 0x00, 0x00,
            0x88, 0x13, 0x00, 0x00,
        };
        EXPECT_EQ(encoded.bytes, expected);
    }

    {
        std::vector<std::uint8_t> payload;
        ASSERT_TRUE(EncodePayload(
            ExecutionResultPayload{
                .command_sequence = 0x0102030405060708ull,
                .control = ExecutionControlKind::StepFrame,
                .status = CommandStatus::Succeeded,
                .session_id = 0x1112131415161718ull,
                .state_epoch = 0x2122232425262728ull,
                .operation_id = 0x3132333435363738ull,
                .activity = ExecutionActivityCode::IdlePaused,
                .has_terminal_status = true,
                .terminal_status =
                    ExecutionTerminalStatusCode::StepsCompleted,
                .completed_count = 2,
                .program_counter = 0x801dc288u,
                .rejection_code = RejectionCode::None,
            },
            payload));
        const auto encoded = EncodeFrame(
            MessageKind::ExecutionResult,
            0x4142434445464748ull,
            payload);
        ASSERT_EQ(encoded.error, FrameError::None);
        const std::vector<std::uint8_t> expected{
            0x57, 0x52, 0x4d, 0x53, // WRMS
            0x01, 0x00,             // protocol version 1
            0x09, 0x01,             // ExecutionResult
            0x3b, 0x00, 0x00, 0x00, // payload length
            0x48, 0x47, 0x46, 0x45, 0x44, 0x43, 0x42, 0x41,
            0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
            0x03, // StepFrame
            0x00, // Succeeded
            0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
            0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,
            0x38, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31,
            0x00, // IdlePaused
            0x01, // has terminal status
            0x01, // StepsCompleted
            0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x88, 0xc2, 0x1d, 0x80,
            0x00, 0x00, // no rejection
            0x00, 0x00, 0x00, 0x00, // empty error code
            0x00, 0x00, 0x00, 0x00, // empty message
        };
        EXPECT_EQ(encoded.bytes, expected);
    }

    {
        std::vector<std::uint8_t> payload;
        ASSERT_TRUE(EncodePayload(
            ExecutionStatePayload{
                .session_id = 0x1112131415161718ull,
                .state_epoch = 0x2122232425262728ull,
                .operation_id = 0x3132333435363738ull,
                .activity = ExecutionActivityCode::InteractiveRunning,
                .has_active_control = true,
                .active_control = ExecutionControlKind::Resume,
                .completed_count = 0,
                .program_counter = 0x801dc288u,
                .rejection_code = RejectionCode::None,
            },
            payload));
        const auto encoded = EncodeFrame(
            MessageKind::ExecutionState,
            0x5152535455565758ull,
            payload);
        ASSERT_EQ(encoded.error, FrameError::None);
        const std::vector<std::uint8_t> expected{
            0x57, 0x52, 0x4d, 0x53, // WRMS
            0x01, 0x00,             // protocol version 1
            0x0a, 0x01,             // ExecutionState
            0x31, 0x00, 0x00, 0x00, // payload length
            0x58, 0x57, 0x56, 0x55, 0x54, 0x53, 0x52, 0x51,
            0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
            0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,
            0x38, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31,
            0x01, // InteractiveRunning
            0x01, // has active control
            0x01, // Resume
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x88, 0xc2, 0x1d, 0x80,
            0x00, 0x00, // no rejection
            0x00, 0x00, 0x00, 0x00, // empty code
            0x00, 0x00, 0x00, 0x00, // empty message
        };
        EXPECT_EQ(encoded.bytes, expected);
    }
}

TEST(WorkerProtocolV1, RoundTripsEveryTypedPayload)
{
    ExpectPayloadRoundTrip(ProcessHelloPayload{
        17,
        4242,
        0x1020304050607080ull,
        "savor-worker-test",
    });
    ExpectPayloadRoundTrip(RuntimeManifestPayload{
        .encoded_manifest = {0x01, 0x02, 0x03}});

    ExpectPayloadRoundTrip(OpenSessionPayload{
        .runtime_root = "C:/runtime",
        .user_directory = "C:/users/worker-17",
        .iso_path = "D:/games/soa.gcm",
        .visual_requested = true,
        .render_window_handle = 0xfedcba9876543210ull,
        .runtime_artifact_root = "C:/artifacts",
    });

    ExpectPayloadRoundTrip(PrepareModulePayload{
        .canonical_id = "battle.single-turn",
        .revision = 7,
        .canonical_hash = "sha256:0123456789abcdef",
        .format_version = 3,
        .development_only = true,
        .encoded_module = { 0x00, 0x10, 0x20, 0xff },
    });

    ExpectPayloadRoundTrip(SubmitInvocationPayload{
        .invocation_id = 101,
        .attempt_id = 7,
        .module_canonical_id = "battle.single-turn",
        .module_revision = 7,
        .module_canonical_hash = "sha256:0123456789abcdef",
        .entrypoint = "execute",
        .expected_state_epoch = 55,
        .encoded_invocation = { 0xde, 0xad, 0xbe, 0xef },
    });
    ExpectPayloadRoundTrip(SubmitWorksetPayload{
        .encoded_workset = {0x57, 0x53, 0x01}});
    ExpectPayloadRoundTrip(CancelWorksetItemPayload{
        .workset_id = 500,
        .item_id = 3,
        .reason = "cancel item"});
    ExpectPayloadRoundTrip(CancelWorksetPayload{
        .workset_id = 500,
        .reason = "cancel workset"});
    ExpectPayloadRoundTrip(AcknowledgeTerminalPayload{
        .workset_id = 500,
        .item_id = 3,
        .item_ordinal = 2,
        .invocation_id = 101,
        .attempt_id = 7,
        .terminal_id = 900,
        .terminal_order = 44});

    ExpectPayloadRoundTrip(CancelInvocationPayload{
        101,
        "caller requested cancellation",
    });

    ExpectPayloadRoundTrip(CaptureScreenshotPayload{
        55,
        "C:/shots/final.png",
        2500,
    });

    ExpectPayloadRoundTrip(ShutdownPayload{ 5000 });

    ExpectPayloadRoundTrip(ControlExecutionPayload{
        .control = ExecutionControlKind::StepFrame,
        .session_id = 55,
        .expected_state_epoch = 3,
        .count = 2,
        .timeout_ms = 5000,
    });

    ExpectPayloadRoundTrip(CommandResultPayload{
        .command_sequence = 44,
        .command_kind = MessageKind::SubmitInvocation,
        .status = CommandStatus::Rejected,
        .rejection_code = RejectionCode::ProgramRuntimeUnavailable,
        .error_code = "runtime_unavailable",
        .message = "ProgramInvocation is not advertised",
        .result = { 0x01, 0x02 },
    });

    ExpectPayloadRoundTrip(OpenSessionResultPayload{
        .success = true,
        .session_id = 55,
        .state_epoch = 3,
        .capability_mask = 0x55aa55aa55aa55aaull,
        .worker_state = WorkerStateCode::Ready,
        .session_disposition = SessionDispositionCode::Clean,
        .rejection_code = RejectionCode::None,
        .error_code = {},
        .message = "session ready",
    });

    ExpectPayloadRoundTrip(ScreenshotResultPayload{
        .status = ScreenshotStatus::Captured,
        .session_id = 55,
        .state_epoch = 3,
        .output_path = "C:/shots/final.png",
        .rejection_code = RejectionCode::None,
        .error_code = {},
        .message = "captured",
    });

    ExpectPayloadRoundTrip(ShutdownResultPayload{
        .status = ShutdownStatus::CleanupFailed,
        .final_disposition = SessionDispositionCode::Tainted,
        .rejection_code = RejectionCode::SessionTainted,
        .error_code = "cleanup_failed",
        .message = "input release could not be proven",
    });

    ExpectPayloadRoundTrip(SessionEventPayload{
        .event_type = SessionEventType::Tainted,
        .session_id = 55,
        .state_epoch = 4,
        .capability_mask = 0x000000000000000full,
        .worker_state = WorkerStateCode::Stopping,
        .session_disposition = SessionDispositionCode::Tainted,
        .rejection_code = RejectionCode::SessionTainted,
        .code = "session_tainted",
        .message = "state replacement integrity is uncertain",
    });

    ExpectPayloadRoundTrip(InvocationProgressPayload{
        .invocation_id = 101,
        .attempt_id = 7,
        .workset_id = 500,
        .item_id = 3,
        .item_ordinal = 2,
        .ordinal = 9,
        .progress = { 0x01, 0x00, 0x01, 0x00 },
    });

    ExpectPayloadRoundTrip(InvocationTerminalPayload{
        .invocation_id = 101,
        .attempt_id = 7,
        .status = InvocationTerminalStatus::InfrastructureFailure,
        .session_disposition = SessionDispositionCode::Tainted,
        .state_epoch = 4,
        .rejection_code = RejectionCode::BackendFailure,
        .error_code = "backend_failed",
        .message = "backend stopped responding",
        .result = { 0xca, 0xfe },
    });
    ExpectPayloadRoundTrip(WorksetStatePayload{
        .outbound_sequence = 31,
        .workset_id = 500,
        .state = WorksetStateCode::Running,
        .next_item_ordinal = 2});
    ExpectPayloadRoundTrip(WorksetResidenceSnapshotV1{
        .format_version = 1,
        .has_resident_workset = true,
        .workset_id = 500,
        .state = WorksetStateCode::Running});
    ExpectPayloadRoundTrip(WorksetItemStartedPayload{
        .outbound_sequence = 32,
        .workset_id = 500,
        .item_id = 3,
        .item_ordinal = 2,
        .invocation_id = 101,
        .attempt_id = 7,
        .session_id = 55,
        .state_epoch = 4,
        .baseline_sha256 = std::string(64, 'a'),
        .baseline_lineage = "seed-probe/neutral",
        .baseline_restored = true});
    ExpectPayloadRoundTrip(WorksetItemTerminalPayload{
        .outbound_sequence = 33,
        .workset_id = 500,
        .item_id = 3,
        .item_ordinal = 2,
        .invocation_id = 101,
        .attempt_id = 7,
        .terminal_id = 900,
        .terminal_order = 44,
        .status = InvocationTerminalStatus::Succeeded,
        .session_disposition = SessionDispositionCode::Clean,
        .state_epoch = 4,
        .unstarted = false,
        .result = {0xaa, 0xbb}});
    ExpectPayloadRoundTrip(WorksetCreditsPayload{
        .outbound_sequence = 34,
        .available_item_credits = 61,
        .active_and_staged_items = 3,
        .retained_terminals = 1});
    ExpectPayloadRoundTrip(WorksetSummaryPayload{
        .outbound_sequence = 35,
        .workset_id = 500,
        .item_count = 3,
        .completed_count = 2,
        .unstarted_count = 1});

    ExpectPayloadRoundTrip(HostEventPayload{
        55,
        4,
        909,
        "render_window_changed",
        { '{', '}', '\n' },
    });

    ExpectPayloadRoundTrip(RuntimeDiagnosticPayload{
        .rejection_code = RejectionCode::InvocationMismatch,
        .command_sequence = 44,
        .invocation_id = 101,
        .message = "ignored stale callback",
    });

    ExpectPayloadRoundTrip(ExecutionResultPayload{
        .command_sequence = 45,
        .control = ExecutionControlKind::StepFrame,
        .status = CommandStatus::Succeeded,
        .session_id = 55,
        .state_epoch = 3,
        .operation_id = 901,
        .activity = ExecutionActivityCode::IdlePaused,
        .has_terminal_status = true,
        .terminal_status = ExecutionTerminalStatusCode::StepsCompleted,
        .completed_count = 2,
        .program_counter = 0x801dc288,
        .rejection_code = RejectionCode::None,
        .error_code = {},
        .message = "frame step completed",
    });

    ExpectPayloadRoundTrip(ExecutionStatePayload{
        .session_id = 55,
        .state_epoch = 3,
        .operation_id = 902,
        .activity = ExecutionActivityCode::InteractiveRunning,
        .has_active_control = true,
        .active_control = ExecutionControlKind::Resume,
        .completed_count = 0,
        .program_counter = 0x801dc288,
        .rejection_code = RejectionCode::None,
        .code = {},
        .message = "interactive execution is running",
    });
}

TEST(WorkerProtocolV1, KeepsExecutionControlAdditiveAndDirectional)
{
    EXPECT_EQ(ProtocolVersion, 1u);
    EXPECT_TRUE(IsKnownMessageKind(MessageKind::ControlExecution));
    EXPECT_TRUE(IsKnownMessageKind(MessageKind::ExecutionResult));
    EXPECT_TRUE(IsKnownMessageKind(MessageKind::ExecutionState));
    EXPECT_EQ(
        DirectionOf(MessageKind::ControlExecution),
        MessageDirection::ParentToWorker);
    EXPECT_EQ(
        DirectionOf(MessageKind::ExecutionResult),
        MessageDirection::WorkerToParent);
    EXPECT_EQ(
        DirectionOf(MessageKind::ExecutionState),
        MessageDirection::WorkerToParent);
}

TEST(WorkerProtocolV1, KeepsWorksetTransportAdditiveAndDirectional)
{
    EXPECT_EQ(ProtocolVersion, 1u);
    EXPECT_TRUE(IsKnownMessageKind(MessageKind::SubmitInvocation));
    EXPECT_TRUE(IsKnownMessageKind(MessageKind::RuntimeManifest));
    EXPECT_TRUE(IsKnownMessageKind(MessageKind::SubmitWorkset));
    EXPECT_TRUE(IsKnownMessageKind(MessageKind::CancelWorksetItem));
    EXPECT_TRUE(IsKnownMessageKind(MessageKind::CancelWorkset));
    EXPECT_TRUE(IsKnownMessageKind(MessageKind::AcknowledgeTerminal));
    EXPECT_TRUE(IsKnownMessageKind(MessageKind::LivenessProbe));
    EXPECT_TRUE(IsKnownMessageKind(MessageKind::WorksetItemTerminal));
    EXPECT_EQ(
        DirectionOf(MessageKind::SubmitWorkset),
        MessageDirection::ParentToWorker);
    EXPECT_EQ(
        DirectionOf(MessageKind::LivenessProbe),
        MessageDirection::ParentToWorker);
    EXPECT_EQ(
        DirectionOf(MessageKind::RuntimeManifest),
        MessageDirection::WorkerToParent);
    EXPECT_EQ(
        DirectionOf(MessageKind::WorksetItemTerminal),
        MessageDirection::WorkerToParent);
}

TEST(WorkerProtocolV1, RejectsInvalidExecutionControlWithoutPublishingOutput)
{
    const ControlExecutionPayload valid{
        .control = ExecutionControlKind::Pause,
        .session_id = 7,
        .expected_state_epoch = 9,
        .timeout_ms = 1000,
    };
    std::vector<std::uint8_t> encoded;
    ASSERT_TRUE(EncodePayload(valid, encoded));
    ASSERT_FALSE(encoded.empty());

    encoded[0] = 0xff;
    ControlExecutionPayload unchanged;
    unchanged.session_id = 777;
    const auto decoded = DecodePayload(encoded, unchanged);
    EXPECT_EQ(decoded.error, PayloadError::InvalidEnumValue);
    EXPECT_EQ(unchanged.session_id, 777u);

    auto invalid = valid;
    invalid.control = static_cast<ExecutionControlKind>(0xff);
    std::vector<std::uint8_t> unchanged_output{0x5a};
    const auto encoded_invalid = EncodePayload(invalid, unchanged_output);
    EXPECT_EQ(encoded_invalid.error, PayloadError::InvalidEnumValue);
    EXPECT_EQ(unchanged_output, (std::vector<std::uint8_t>{0x5a}));

    auto reserved = valid;
    reserved.control = ExecutionControlKind::ReservedGuestInstruction;
    unchanged_output = {0x6b};
    const auto encoded_reserved =
        EncodePayload(reserved, unchanged_output);
    EXPECT_EQ(encoded_reserved.error, PayloadError::InvalidEnumValue);
    EXPECT_EQ(unchanged_output, (std::vector<std::uint8_t>{0x6b}));

    encoded.clear();
    ASSERT_TRUE(EncodePayload(valid, encoded));
    encoded[0] = static_cast<std::uint8_t>(
        ExecutionControlKind::ReservedGuestInstruction);
    const auto decoded_reserved = DecodePayload(encoded, unchanged);
    EXPECT_EQ(decoded_reserved.error, PayloadError::InvalidEnumValue);
}

TEST(WorkerProtocolV1, PreservesCompleteEncodedEnvelopeMetadata)
{
    const PrepareModulePayload module{
        .canonical_id = "navigation.context",
        .revision = 19,
        .canonical_hash = "sha256:module-hash",
        .format_version = 4,
        .encoded_module = { 0x00, 0x7f, 0x80, 0xff },
    };
    std::vector<std::uint8_t> encoded;
    ASSERT_TRUE(EncodePayload(module, encoded));
    PrepareModulePayload decoded_module;
    ASSERT_TRUE(DecodePayload(encoded, decoded_module));
    EXPECT_EQ(decoded_module, module);

    const SubmitInvocationPayload invocation{
        .invocation_id = 7001,
        .attempt_id = 23,
        .module_canonical_id = module.canonical_id,
        .module_revision = module.revision,
        .module_canonical_hash = module.canonical_hash,
        .entrypoint = "capture-and-route",
        .expected_state_epoch = 991,
        .encoded_invocation = { 0x10, 0x00, 0x20, 0x00 },
    };
    encoded.clear();
    ASSERT_TRUE(EncodePayload(invocation, encoded));
    SubmitInvocationPayload decoded_invocation;
    ASSERT_TRUE(DecodePayload(encoded, decoded_invocation));
    EXPECT_EQ(decoded_invocation, invocation);
    EXPECT_EQ(decoded_invocation.expected_state_epoch, 991u);
}

TEST(WorkerProtocolV1, PreservesAttemptIdentityAcrossProgressAndTerminal)
{
    constexpr std::uint64_t invocation_id = 91;
    constexpr std::uint64_t first_attempt = 4;
    constexpr std::uint64_t retry_attempt = 5;

    const InvocationProgressPayload progress{
        .invocation_id = invocation_id,
        .attempt_id = retry_attempt,
        .workset_id = 500,
        .item_id = 3,
        .item_ordinal = 2,
        .ordinal = 12,
        .progress = { 0x44 },
    };
    std::vector<std::uint8_t> encoded;
    ASSERT_TRUE(EncodePayload(progress, encoded));
    InvocationProgressPayload decoded_progress;
    ASSERT_TRUE(DecodePayload(encoded, decoded_progress));
    EXPECT_EQ(decoded_progress.invocation_id, invocation_id);
    EXPECT_EQ(decoded_progress.attempt_id, retry_attempt);
    EXPECT_NE(decoded_progress.attempt_id, first_attempt);

    const InvocationTerminalPayload terminal{
        .invocation_id = invocation_id,
        .attempt_id = retry_attempt,
        .status = InvocationTerminalStatus::TimedOut,
        .session_disposition = SessionDispositionCode::Clean,
        .state_epoch = 88,
    };
    encoded.clear();
    ASSERT_TRUE(EncodePayload(terminal, encoded));
    InvocationTerminalPayload decoded_terminal;
    ASSERT_TRUE(DecodePayload(encoded, decoded_terminal));
    EXPECT_EQ(decoded_terminal.invocation_id, invocation_id);
    EXPECT_EQ(decoded_terminal.attempt_id, retry_attempt);
    EXPECT_NE(decoded_terminal.attempt_id, first_attempt);
    EXPECT_EQ(
        decoded_terminal.status,
        InvocationTerminalStatus::TimedOut);
}

TEST(WorkerProtocolV1, RuntimeDiagnosticsAreNotSessionEvents)
{
    const RuntimeDiagnosticPayload diagnostic{
        .rejection_code = RejectionCode::InvocationMismatch,
        .invocation_id = 99,
        .message = "stale callback ignored",
    };
    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(EncodePayload(diagnostic, payload));
    const auto frame = EncodeFrame(
        MessageKind::RuntimeDiagnostic,
        0,
        payload);
    ASSERT_TRUE(frame);

    const auto decoded = DecodeFrame(frame.bytes);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.frame.header.kind, MessageKind::RuntimeDiagnostic);
    EXPECT_NE(decoded.frame.header.kind, MessageKind::SessionEvent);
    EXPECT_EQ(
        DirectionOf(decoded.frame.header.kind),
        MessageDirection::WorkerToParent);
}

TEST(WorkerProtocolV1, OversizedTerminalCanBeReplacedByBoundedFailure)
{
    InvocationTerminalPayload oversized{
        .invocation_id = 501,
        .attempt_id = 8,
        .status = InvocationTerminalStatus::Succeeded,
        .session_disposition = SessionDispositionCode::Clean,
        .state_epoch = 41,
    };
    oversized.result.resize(MaximumPayloadSize);
    std::vector<std::uint8_t> output{ 0x5a };
    const auto oversized_result = EncodePayload(oversized, output);
    EXPECT_EQ(oversized_result.error, PayloadError::PayloadTooLarge);
    EXPECT_EQ(output, (std::vector<std::uint8_t>{ 0x5a }));

    const InvocationTerminalPayload fallback{
        .invocation_id = oversized.invocation_id,
        .attempt_id = oversized.attempt_id,
        .status = InvocationTerminalStatus::InfrastructureFailure,
        .session_disposition = oversized.session_disposition,
        .state_epoch = oversized.state_epoch,
        .rejection_code = RejectionCode::InternalFailure,
        .error_code = "TerminalEncodingFailed",
        .message = "terminal output exceeded the WRMS payload bound",
    };
    output.clear();
    EXPECT_TRUE(EncodePayload(fallback, output));
    EXPECT_LT(output.size(), MaximumPayloadSize);
}

TEST(WorkerProtocolV1, RejectsUnknownRuntimeEnumsWithoutPublishingOutput)
{
    const OpenSessionResultPayload valid{
        .success = true,
        .session_id = 1,
        .state_epoch = 1,
        .worker_state = WorkerStateCode::Ready,
        .session_disposition = SessionDispositionCode::Clean,
    };
    std::vector<std::uint8_t> encoded;
    ASSERT_TRUE(EncodePayload(valid, encoded));
    ASSERT_GT(encoded.size(), 28u);

    const auto expect_invalid = [&](std::size_t offset, std::uint8_t value) {
        auto corrupt = encoded;
        corrupt[offset] = value;
        OpenSessionResultPayload unchanged;
        unchanged.session_id = 777;
        const auto result = DecodePayload(corrupt, unchanged);
        EXPECT_EQ(result.error, PayloadError::InvalidEnumValue);
        EXPECT_EQ(unchanged.session_id, 777u);
    };

    // bool + session + epoch + capabilities
    constexpr std::size_t worker_state_offset = 1 + 8 + 8 + 8;
    constexpr std::size_t disposition_offset = worker_state_offset + 1;
    constexpr std::size_t rejection_offset = disposition_offset + 1;
    expect_invalid(worker_state_offset, 0xff);
    expect_invalid(disposition_offset, 0xff);
    expect_invalid(rejection_offset, 0xff);

    auto invalid_encode = valid;
    invalid_encode.worker_state = static_cast<WorkerStateCode>(0xff);
    std::vector<std::uint8_t> unchanged_output{ 0x5a };
    const auto encode_result =
        EncodePayload(invalid_encode, unchanged_output);
    EXPECT_EQ(encode_result.error, PayloadError::InvalidEnumValue);
    EXPECT_EQ(unchanged_output, (std::vector<std::uint8_t>{ 0x5a }));

    const InvocationTerminalPayload valid_terminal{
        .invocation_id = 9,
        .attempt_id = 3,
        .status = InvocationTerminalStatus::TimedOut,
        .session_disposition = SessionDispositionCode::Clean,
        .state_epoch = 5,
    };
    encoded.clear();
    ASSERT_TRUE(EncodePayload(valid_terminal, encoded));
    ASSERT_GT(encoded.size(), 16u);
    encoded[16] = 0xff;
    InvocationTerminalPayload unchanged_terminal;
    unchanged_terminal.attempt_id = 777;
    const auto terminal_decode =
        DecodePayload(encoded, unchanged_terminal);
    EXPECT_EQ(terminal_decode.error, PayloadError::InvalidEnumValue);
    EXPECT_EQ(unchanged_terminal.attempt_id, 777u);

    auto invalid_terminal = valid_terminal;
    invalid_terminal.status =
        static_cast<InvocationTerminalStatus>(0xff);
    unchanged_output = { 0xa5 };
    const auto terminal_encode =
        EncodePayload(invalid_terminal, unchanged_output);
    EXPECT_EQ(terminal_encode.error, PayloadError::InvalidEnumValue);
    EXPECT_EQ(unchanged_output, (std::vector<std::uint8_t>{ 0xa5 }));
}

TEST(WorkerProtocolV1, ReportsNeedMoreDataForEveryFragment)
{
    constexpr std::array<std::uint8_t, 5> payload{ 1, 2, 3, 4, 5 };
    const auto frame = MakeFrame(MessageKind::HostEvent, 99, payload);
    ASSERT_EQ(frame.size(), HeaderSize + payload.size());

    for (std::size_t prefix_size = 0; prefix_size < frame.size(); ++prefix_size) {
        const auto result = DecodeFrame(
            std::span<const std::uint8_t>(frame).first(prefix_size));
        EXPECT_EQ(result.status, FrameDecodeStatus::NeedMoreData)
            << "prefix_size=" << prefix_size;
        EXPECT_EQ(result.error, FrameError::None)
            << "prefix_size=" << prefix_size;
        EXPECT_EQ(result.consumed_size, 0u)
            << "prefix_size=" << prefix_size;
        EXPECT_GT(result.required_size, prefix_size)
            << "prefix_size=" << prefix_size;
    }

    const auto complete = DecodeFrame(frame);
    EXPECT_EQ(complete.status, FrameDecodeStatus::Complete);
}

TEST(WorkerProtocolV1, EndOfStreamTurnsFragmentsIntoTypedTruncationErrors)
{
    constexpr std::array<std::uint8_t, 4> payload{ 9, 8, 7, 6 };
    const auto frame = MakeFrame(MessageKind::CommandResult, 5, payload);

    const auto empty = DecodeFrame({}, true);
    EXPECT_EQ(empty.status, FrameDecodeStatus::Error);
    EXPECT_EQ(empty.error, FrameError::TruncatedHeader);

    const auto partial_header = DecodeFrame(
        std::span<const std::uint8_t>(frame).first(HeaderSize - 1),
        true);
    EXPECT_EQ(partial_header.status, FrameDecodeStatus::Error);
    EXPECT_EQ(partial_header.error, FrameError::TruncatedHeader);

    const auto partial_payload = DecodeFrame(
        std::span<const std::uint8_t>(frame).first(frame.size() - 1),
        true);
    EXPECT_EQ(partial_payload.status, FrameDecodeStatus::Error);
    EXPECT_EQ(partial_payload.error, FrameError::TruncatedPayload);
}

TEST(WorkerProtocolV1, RejectsInvalidHeaderFields)
{
    constexpr std::array<std::uint8_t, 1> payload{ 0x42 };
    const auto valid = MakeFrame(MessageKind::OpenSession, 123, payload);

    auto wrong_magic = valid;
    wrong_magic[0] = 'X';
    auto result = DecodeFrame(wrong_magic);
    EXPECT_EQ(result.status, FrameDecodeStatus::Error);
    EXPECT_EQ(result.error, FrameError::WrongMagic);

    auto wrong_version = valid;
    wrong_version[4] = 2;
    wrong_version[5] = 0;
    result = DecodeFrame(wrong_version);
    EXPECT_EQ(result.status, FrameDecodeStatus::Error);
    EXPECT_EQ(result.error, FrameError::UnsupportedVersion);

    auto unknown_kind = valid;
    unknown_kind[6] = 0xff;
    unknown_kind[7] = 0x7f;
    result = DecodeFrame(unknown_kind);
    EXPECT_EQ(result.status, FrameDecodeStatus::Error);
    EXPECT_EQ(result.error, FrameError::UnknownMessageKind);
}

TEST(WorkerProtocolV1, RejectsOversizedLengthBeforeExposingOrAllocatingPayload)
{
    auto header = MakeFrame(MessageKind::PrepareModule, 77, {});
    ASSERT_EQ(header.size(), HeaderSize);

    constexpr auto oversized = static_cast<std::uint32_t>(MaximumPayloadSize + 1);
    header[8] = static_cast<std::uint8_t>(oversized);
    header[9] = static_cast<std::uint8_t>(oversized >> 8);
    header[10] = static_cast<std::uint8_t>(oversized >> 16);
    header[11] = static_cast<std::uint8_t>(oversized >> 24);

    const auto result = DecodeFrame(header);
    EXPECT_EQ(result.status, FrameDecodeStatus::Error);
    EXPECT_EQ(result.error, FrameError::PayloadTooLarge);
    EXPECT_EQ(result.required_size, 0u);
    EXPECT_EQ(result.consumed_size, 0u);
    EXPECT_TRUE(result.frame.payload.empty());
}

TEST(WorkerProtocolV1, RejectsTrailingAndInvalidTypedPayloadWithoutPublishingOutput)
{
    const ShutdownPayload expected_shutdown{ 5000 };
    std::vector<std::uint8_t> encoded_shutdown;
    ASSERT_EQ(
        EncodePayload(expected_shutdown, encoded_shutdown).error,
        PayloadError::None);
    encoded_shutdown.push_back(0xff);

    ShutdownPayload unchanged_shutdown{ 1234 };
    auto result = DecodePayload(encoded_shutdown, unchanged_shutdown);
    EXPECT_EQ(result.error, PayloadError::TrailingBytes);
    EXPECT_EQ(unchanged_shutdown, (ShutdownPayload{ 1234 }));

    OpenSessionPayload valid_session;
    std::vector<std::uint8_t> encoded_session;
    ASSERT_EQ(
        EncodePayload(valid_session, encoded_session).error,
        PayloadError::None);
    ASSERT_GT(encoded_session.size(), 12u);
    encoded_session[12] = 2; // visual_requested must be exactly zero or one.

    OpenSessionPayload unchanged_session;
    unchanged_session.runtime_root = "unchanged";
    result = DecodePayload(encoded_session, unchanged_session);
    EXPECT_EQ(result.error, PayloadError::InvalidBoolean);
    EXPECT_EQ(unchanged_session.runtime_root, "unchanged");

    ScreenshotResultPayload valid_screenshot;
    std::vector<std::uint8_t> encoded_screenshot;
    ASSERT_EQ(
        EncodePayload(valid_screenshot, encoded_screenshot).error,
        PayloadError::None);
    ASSERT_FALSE(encoded_screenshot.empty());
    encoded_screenshot[0] = 0xff;

    ScreenshotResultPayload unchanged_screenshot;
    unchanged_screenshot.output_path = "unchanged.png";
    result = DecodePayload(encoded_screenshot, unchanged_screenshot);
    EXPECT_EQ(result.error, PayloadError::InvalidEnumValue);
    EXPECT_EQ(unchanged_screenshot.output_path, "unchanged.png");
}

TEST(WorkerProtocolV1, PreservesRequestIdsAcrossBackToBackFrames)
{
    const auto first = MakeFrame(MessageKind::OpenSession, 0x1111222233334444ull, {});
    const auto second = MakeFrame(MessageKind::Shutdown, 0xaaaabbbbccccddddull, {});

    std::vector<std::uint8_t> stream = first;
    stream.insert(stream.end(), second.begin(), second.end());

    const auto first_result = DecodeFrame(stream);
    ASSERT_EQ(first_result.status, FrameDecodeStatus::Complete);
    EXPECT_EQ(first_result.frame.header.request_id, 0x1111222233334444ull);
    EXPECT_EQ(first_result.consumed_size, first.size());

    const auto second_result = DecodeFrame(
        std::span<const std::uint8_t>(stream).subspan(first_result.consumed_size));
    ASSERT_EQ(second_result.status, FrameDecodeStatus::Complete);
    EXPECT_EQ(second_result.frame.header.request_id, 0xaaaabbbbccccddddull);
    EXPECT_EQ(second_result.consumed_size, second.size());
}

TEST(WorkerProtocolV1, RejectsLegacyTagPrefixWithoutDispatch)
{
    int dispatch_count = 0;
    const auto decode_and_dispatch = [&dispatch_count](
        std::span<const std::uint8_t> bytes) {
        const auto result = DecodeFrame(bytes);
        if (result.status == FrameDecodeStatus::Complete)
            ++dispatch_count;
        return result;
    };

    // MSG_READY in the legacy protocol began with the native u32 tag 0x01.
    constexpr std::array<std::uint8_t, 4> legacy_ready_prefix{
        0x01, 0x00, 0x00, 0x00,
    };
    const auto result = decode_and_dispatch(legacy_ready_prefix);

    EXPECT_EQ(result.status, FrameDecodeStatus::Error);
    EXPECT_EQ(result.error, FrameError::WrongMagic);
    EXPECT_EQ(dispatch_count, 0);
}

} // namespace
