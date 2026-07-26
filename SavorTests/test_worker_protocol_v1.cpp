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

TEST(WorkerProtocolV1, RoundTripsEveryTypedPayload)
{
    ExpectPayloadRoundTrip(ProcessHelloPayload{
        17,
        4242,
        0x1020304050607080ull,
        "savor-worker-test",
    });

    ExpectPayloadRoundTrip(OpenSessionPayload{
        "C:/runtime",
        "C:/users/worker-17",
        "D:/games/soa.gcm",
        true,
        0xfedcba9876543210ull,
        "C:/shots",
        5000,
        true,
    });

    ExpectPayloadRoundTrip(PrepareModulePayload{
        .canonical_id = "battle.single-turn",
        .revision = 7,
        .canonical_hash = "sha256:0123456789abcdef",
        .format_version = 3,
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
