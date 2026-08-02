#include "WrmsProtocol.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace savor::wrms {
namespace {

constexpr std::uint8_t ToByte(bool value) noexcept
{
    return value ? 1u : 0u;
}

constexpr bool IsKnownCommandStatus(std::uint8_t value) noexcept
{
    return value <= static_cast<std::uint8_t>(CommandStatus::Unsupported);
}

constexpr bool IsKnownScreenshotStatus(std::uint8_t value) noexcept
{
    return value <= static_cast<std::uint8_t>(ScreenshotStatus::Failed);
}

constexpr bool IsKnownShutdownStatus(std::uint8_t value) noexcept
{
    return value <= static_cast<std::uint8_t>(ShutdownStatus::CleanupFailed);
}

constexpr bool IsKnownSessionEventType(std::uint8_t value) noexcept
{
    return value <= static_cast<std::uint8_t>(SessionEventType::Tainted);
}

constexpr bool IsKnownInvocationTerminalStatus(std::uint8_t value) noexcept
{
    switch (static_cast<InvocationTerminalStatus>(value)) {
    case InvocationTerminalStatus::Succeeded:
    case InvocationTerminalStatus::Failed:
    case InvocationTerminalStatus::Cancelled:
    case InvocationTerminalStatus::InfrastructureFailure:
    case InvocationTerminalStatus::CleanupFailure:
    case InvocationTerminalStatus::TimedOut:
        return true;
    }
    return false;
}

constexpr bool IsKnownWorkerState(std::uint8_t value) noexcept
{
    return value <= static_cast<std::uint8_t>(WorkerStateCode::Stopped);
}

constexpr bool IsKnownExecutionControl(std::uint8_t value) noexcept
{
    switch (static_cast<ExecutionControlKind>(value)) {
    case ExecutionControlKind::Pause:
    case ExecutionControlKind::Resume:
    case ExecutionControlKind::StepFrame:
        return true;
    case ExecutionControlKind::ReservedGuestInstruction:
        return false;
    }
    return false;
}

constexpr bool IsKnownWorksetState(std::uint8_t value) noexcept
{
    return value <= static_cast<std::uint8_t>(WorksetStateCode::Failed);
}

constexpr bool IsKnownExecutionActivity(std::uint8_t value) noexcept
{
    return value <= static_cast<std::uint8_t>(
        ExecutionActivityCode::Failed);
}

constexpr bool IsKnownExecutionTerminalStatus(std::uint8_t value) noexcept
{
    return value <= static_cast<std::uint8_t>(
        ExecutionTerminalStatusCode::CleanupFailure);
}

constexpr bool IsKnownSessionDisposition(std::uint8_t value) noexcept
{
    return value <= static_cast<std::uint8_t>(SessionDispositionCode::Tainted);
}

constexpr bool IsKnownRejectionCode(std::uint16_t value) noexcept
{
    return value <= static_cast<std::uint16_t>(RejectionCode::TerminalMismatch);
}

void AppendU16(std::vector<std::uint8_t>& output, std::uint16_t value)
{
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8));
}

void AppendU32(std::vector<std::uint8_t>& output, std::uint32_t value)
{
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value >> 16));
    output.push_back(static_cast<std::uint8_t>(value >> 24));
}

void AppendU64(std::vector<std::uint8_t>& output, std::uint64_t value)
{
    for (unsigned shift = 0; shift != 64; shift += 8)
        output.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint16_t ReadU16Unchecked(std::span<const std::uint8_t> input, std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>(input[offset])
        | static_cast<std::uint16_t>(input[offset + 1]) << 8;
}

std::uint32_t ReadU32Unchecked(std::span<const std::uint8_t> input, std::size_t offset) noexcept
{
    return static_cast<std::uint32_t>(input[offset])
        | static_cast<std::uint32_t>(input[offset + 1]) << 8
        | static_cast<std::uint32_t>(input[offset + 2]) << 16
        | static_cast<std::uint32_t>(input[offset + 3]) << 24;
}

std::uint64_t ReadU64Unchecked(std::span<const std::uint8_t> input, std::size_t offset) noexcept
{
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift != 64; shift += 8)
        value |= static_cast<std::uint64_t>(input[offset + shift / 8]) << shift;
    return value;
}

class PayloadWriter {
public:
    void u8(std::uint8_t value)
    {
        bytes_.push_back(value);
    }

    void u16(std::uint16_t value)
    {
        AppendU16(bytes_, value);
    }

    void u32(std::uint32_t value)
    {
        AppendU32(bytes_, value);
    }

    void u64(std::uint64_t value)
    {
        AppendU64(bytes_, value);
    }

    void boolean(bool value)
    {
        u8(ToByte(value));
    }

    void string(const std::string& value)
    {
        bytes(value.data(), value.size());
    }

    void blob(std::span<const std::uint8_t> value)
    {
        bytes(value.data(), value.size());
    }

    void fail(PayloadError error)
    {
        if (error_ == PayloadError::None)
            error_ = error;
    }

    [[nodiscard]] PayloadCodecResult finish(std::vector<std::uint8_t>& output)
    {
        if (error_ != PayloadError::None)
            return { error_, bytes_.size() };
        if (bytes_.size() > MaximumPayloadSize)
            return { PayloadError::PayloadTooLarge, bytes_.size() };
        output = std::move(bytes_);
        return {};
    }

private:
    void bytes(const void* data, std::size_t size)
    {
        if (error_ != PayloadError::None)
            return;
        if (size > MaximumPayloadSize
            || size > std::numeric_limits<std::uint32_t>::max()) {
            error_ = PayloadError::FieldTooLarge;
            return;
        }
        const auto total_size = bytes_.size() + sizeof(std::uint32_t) + size;
        if (total_size > MaximumPayloadSize) {
            error_ = PayloadError::PayloadTooLarge;
            return;
        }
        u32(static_cast<std::uint32_t>(size));
        const auto* first = static_cast<const std::uint8_t*>(data);
        if (size != 0)
            bytes_.insert(bytes_.end(), first, first + size);
    }

    std::vector<std::uint8_t> bytes_;
    PayloadError error_ = PayloadError::None;
};

class PayloadReader {
public:
    explicit PayloadReader(std::span<const std::uint8_t> input)
        : input_(input)
    {
        if (input.size() > MaximumPayloadSize)
            error_ = PayloadError::PayloadTooLarge;
    }

    bool u8(std::uint8_t& value)
    {
        if (!available(1))
            return false;
        value = input_[offset_++];
        return true;
    }

    bool u16(std::uint16_t& value)
    {
        if (!available(2))
            return false;
        value = ReadU16Unchecked(input_, offset_);
        offset_ += 2;
        return true;
    }

    bool u32(std::uint32_t& value)
    {
        if (!available(4))
            return false;
        value = ReadU32Unchecked(input_, offset_);
        offset_ += 4;
        return true;
    }

    bool u64(std::uint64_t& value)
    {
        if (!available(8))
            return false;
        value = ReadU64Unchecked(input_, offset_);
        offset_ += 8;
        return true;
    }

    bool boolean(bool& value)
    {
        std::uint8_t encoded = 0;
        if (!u8(encoded))
            return false;
        if (encoded > 1) {
            fail(PayloadError::InvalidBoolean);
            return false;
        }
        value = encoded != 0;
        return true;
    }

    bool string(std::string& value)
    {
        std::span<const std::uint8_t> encoded;
        if (!bytes(encoded))
            return false;
        if (encoded.empty()) {
            value.clear();
            return true;
        }
        value.assign(
            reinterpret_cast<const char*>(encoded.data()),
            encoded.size());
        return true;
    }

    bool blob(std::vector<std::uint8_t>& value)
    {
        std::span<const std::uint8_t> encoded;
        if (!bytes(encoded))
            return false;
        value.assign(encoded.begin(), encoded.end());
        return true;
    }

    void fail(PayloadError error)
    {
        if (error_ == PayloadError::None)
            error_ = error;
    }

    [[nodiscard]] PayloadCodecResult finish()
    {
        if (error_ != PayloadError::None)
            return { error_, offset_ };
        if (offset_ != input_.size())
            return { PayloadError::TrailingBytes, offset_ };
        return { PayloadError::None, offset_ };
    }

private:
    bool available(std::size_t size)
    {
        if (error_ != PayloadError::None)
            return false;
        if (size > input_.size() - offset_) {
            error_ = PayloadError::Truncated;
            return false;
        }
        return true;
    }

    bool bytes(std::span<const std::uint8_t>& value)
    {
        std::uint32_t size = 0;
        if (!u32(size))
            return false;
        if (size > MaximumPayloadSize) {
            fail(PayloadError::FieldTooLarge);
            return false;
        }
        if (!available(size))
            return false;
        value = input_.subspan(offset_, size);
        offset_ += size;
        return true;
    }

    std::span<const std::uint8_t> input_;
    std::size_t offset_ = 0;
    PayloadError error_ = PayloadError::None;
};

template <typename Payload, typename Write>
PayloadCodecResult EncodePayloadImpl(
    const Payload& payload,
    std::vector<std::uint8_t>& output,
    Write&& write)
{
    PayloadWriter writer;
    write(writer, payload);
    return writer.finish(output);
}

template <typename Payload, typename Read>
PayloadCodecResult DecodePayloadImpl(
    std::span<const std::uint8_t> input,
    Payload& output,
    Read&& read)
{
    Payload candidate;
    PayloadReader reader(input);
    read(reader, candidate);
    const auto result = reader.finish();
    if (result)
        output = std::move(candidate);
    return result;
}

} // namespace

bool IsKnownMessageKind(MessageKind kind) noexcept
{
    switch (kind) {
    case MessageKind::ProcessHello:
    case MessageKind::RuntimeManifest:
    case MessageKind::OpenSession:
    case MessageKind::PrepareModule:
    case MessageKind::SubmitInvocation:
    case MessageKind::CancelInvocation:
    case MessageKind::CaptureScreenshot:
    case MessageKind::Shutdown:
    case MessageKind::ControlExecution:
    case MessageKind::SubmitWorkset:
    case MessageKind::CancelWorksetItem:
    case MessageKind::CancelWorkset:
    case MessageKind::AcknowledgeTerminal:
    case MessageKind::LivenessProbe:
    case MessageKind::CommandResult:
    case MessageKind::OpenSessionResult:
    case MessageKind::ScreenshotResult:
    case MessageKind::ShutdownResult:
    case MessageKind::SessionEvent:
    case MessageKind::InvocationProgress:
    case MessageKind::InvocationTerminal:
    case MessageKind::HostEvent:
    case MessageKind::RuntimeDiagnostic:
    case MessageKind::ExecutionResult:
    case MessageKind::ExecutionState:
    case MessageKind::WorksetState:
    case MessageKind::WorksetItemStarted:
    case MessageKind::WorksetItemTerminal:
    case MessageKind::WorksetCredits:
    case MessageKind::WorksetSummary:
        return true;
    }
    return false;
}

MessageDirection DirectionOf(MessageKind kind) noexcept
{
    switch (kind) {
    case MessageKind::OpenSession:
    case MessageKind::PrepareModule:
    case MessageKind::SubmitInvocation:
    case MessageKind::CancelInvocation:
    case MessageKind::CaptureScreenshot:
    case MessageKind::Shutdown:
    case MessageKind::ControlExecution:
    case MessageKind::SubmitWorkset:
    case MessageKind::CancelWorksetItem:
    case MessageKind::CancelWorkset:
    case MessageKind::AcknowledgeTerminal:
    case MessageKind::LivenessProbe:
        return MessageDirection::ParentToWorker;
    default:
        return MessageDirection::WorkerToParent;
    }
}

FrameDecodeResult DecodeFrame(
    std::span<const std::uint8_t> input,
    bool end_of_stream) noexcept
{
    FrameDecodeResult result;

    const auto magic_prefix_size = std::min(input.size(), Magic.size());
    if (!std::equal(
        input.begin(),
        input.begin() + magic_prefix_size,
        Magic.begin())) {
        result.status = FrameDecodeStatus::Error;
        result.error = FrameError::WrongMagic;
        result.required_size = 0;
        return result;
    }

    if (input.size() < HeaderSize) {
        result.required_size = HeaderSize;
        if (end_of_stream) {
            result.status = FrameDecodeStatus::Error;
            result.error = FrameError::TruncatedHeader;
        }
        return result;
    }

    const auto version = ReadU16Unchecked(input, 4);
    if (version != ProtocolVersion) {
        result.status = FrameDecodeStatus::Error;
        result.error = FrameError::UnsupportedVersion;
        result.required_size = 0;
        return result;
    }

    const auto kind = static_cast<MessageKind>(ReadU16Unchecked(input, 6));
    if (!IsKnownMessageKind(kind)) {
        result.status = FrameDecodeStatus::Error;
        result.error = FrameError::UnknownMessageKind;
        result.required_size = 0;
        return result;
    }

    const auto payload_size = ReadU32Unchecked(input, 8);
    if (payload_size > MaximumPayloadSize) {
        result.status = FrameDecodeStatus::Error;
        result.error = FrameError::PayloadTooLarge;
        result.required_size = 0;
        return result;
    }

    const auto frame_size = HeaderSize + static_cast<std::size_t>(payload_size);
    result.required_size = frame_size;
    if (input.size() < frame_size) {
        if (end_of_stream) {
            result.status = FrameDecodeStatus::Error;
            result.error = FrameError::TruncatedPayload;
        }
        return result;
    }

    result.status = FrameDecodeStatus::Complete;
    result.consumed_size = frame_size;
    result.frame.header = FrameHeader{
        version,
        kind,
        payload_size,
        ReadU64Unchecked(input, 12),
    };
    result.frame.payload = input.subspan(HeaderSize, payload_size);
    return result;
}

FrameEncodeResult EncodeFrame(
    MessageKind kind,
    std::uint64_t request_id,
    std::span<const std::uint8_t> payload)
{
    if (!IsKnownMessageKind(kind))
        return { FrameError::UnknownMessageKind, {} };
    if (payload.size() > MaximumPayloadSize
        || payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return { FrameError::PayloadTooLarge, {} };
    }

    FrameEncodeResult result;
    result.bytes.reserve(HeaderSize + payload.size());
    result.bytes.insert(result.bytes.end(), Magic.begin(), Magic.end());
    AppendU16(result.bytes, ProtocolVersion);
    AppendU16(result.bytes, static_cast<std::uint16_t>(kind));
    AppendU32(result.bytes, static_cast<std::uint32_t>(payload.size()));
    AppendU64(result.bytes, request_id);
    result.bytes.insert(result.bytes.end(), payload.begin(), payload.end());
    return result;
}

PayloadCodecResult EncodePayload(
    const ProcessHelloPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.u64(payload.worker_id);
        writer.u32(payload.process_id);
        writer.u64(payload.capability_mask);
        writer.string(payload.build_identity);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    ProcessHelloPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        reader.u64(payload.worker_id);
        reader.u32(payload.process_id);
        reader.u64(payload.capability_mask);
        reader.string(payload.build_identity);
    });
}

PayloadCodecResult EncodePayload(
    const RuntimeManifestPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.blob(payload.encoded_manifest);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    RuntimeManifestPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        reader.blob(payload.encoded_manifest);
    });
}

PayloadCodecResult EncodePayload(
    const OpenSessionPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.string(payload.runtime_root);
        writer.string(payload.user_directory);
        writer.string(payload.iso_path);
        writer.boolean(payload.visual_requested);
        writer.u64(payload.render_window_handle);
        writer.string(payload.runtime_artifact_root);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    OpenSessionPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        reader.string(payload.runtime_root);
        reader.string(payload.user_directory);
        reader.string(payload.iso_path);
        reader.boolean(payload.visual_requested);
        reader.u64(payload.render_window_handle);
        reader.string(payload.runtime_artifact_root);
    });
}

PayloadCodecResult EncodePayload(
    const PrepareModulePayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.string(payload.canonical_id);
        writer.u32(payload.revision);
        writer.string(payload.canonical_hash);
        writer.u32(payload.format_version);
        writer.boolean(payload.development_only);
        writer.blob(payload.encoded_module);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    PrepareModulePayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        reader.string(payload.canonical_id);
        reader.u32(payload.revision);
        reader.string(payload.canonical_hash);
        reader.u32(payload.format_version);
        reader.boolean(payload.development_only);
        reader.blob(payload.encoded_module);
    });
}

PayloadCodecResult EncodePayload(
    const SubmitInvocationPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.u64(payload.invocation_id);
        writer.u64(payload.attempt_id);
        writer.string(payload.module_canonical_id);
        writer.u32(payload.module_revision);
        writer.string(payload.module_canonical_hash);
        writer.string(payload.entrypoint);
        writer.u64(payload.expected_state_epoch);
        writer.blob(payload.encoded_invocation);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    SubmitInvocationPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        reader.u64(payload.invocation_id);
        reader.u64(payload.attempt_id);
        reader.string(payload.module_canonical_id);
        reader.u32(payload.module_revision);
        reader.string(payload.module_canonical_hash);
        reader.string(payload.entrypoint);
        reader.u64(payload.expected_state_epoch);
        reader.blob(payload.encoded_invocation);
    });
}

PayloadCodecResult EncodePayload(
    const SubmitWorksetPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.blob(payload.encoded_workset);
        writer.string(payload.workset_sha256);
        writer.u32(payload.cancellation_sidecar_version);
        if (payload.initially_cancelled_item_ids.size()
            > std::numeric_limits<std::uint32_t>::max()) {
            writer.fail(PayloadError::FieldTooLarge);
            return;
        }
        writer.u32(static_cast<std::uint32_t>(
            payload.initially_cancelled_item_ids.size()));
        for (const auto item_id : payload.initially_cancelled_item_ids)
            writer.u64(item_id);
        writer.string(payload.cancellation_sidecar_sha256);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    SubmitWorksetPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        reader.blob(payload.encoded_workset);
        reader.string(payload.workset_sha256);
        reader.u32(payload.cancellation_sidecar_version);
        std::uint32_t count = 0;
        reader.u32(count);
        if (count > 1024) {
            reader.fail(PayloadError::FieldTooLarge);
            return;
        }
        payload.initially_cancelled_item_ids.resize(count);
        for (auto& item_id : payload.initially_cancelled_item_ids)
            reader.u64(item_id);
        reader.string(payload.cancellation_sidecar_sha256);
    });
}

PayloadCodecResult EncodePayload(
    const SubmitWorksetResultPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        if (payload.format_version != 1 || payload.workset_id == 0
            || payload.sidecar_version != 1
            || payload.applied_sidecar_sha256.size() != 64) {
            writer.fail(PayloadError::InvalidValue);
        }
        writer.u32(payload.format_version);
        writer.u64(payload.workset_id);
        writer.u32(payload.sidecar_version);
        writer.u32(payload.applied_item_count);
        writer.string(payload.applied_sidecar_sha256);
        writer.boolean(payload.already_accepted);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    SubmitWorksetResultPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        reader.u32(payload.format_version);
        reader.u64(payload.workset_id);
        reader.u32(payload.sidecar_version);
        reader.u32(payload.applied_item_count);
        reader.string(payload.applied_sidecar_sha256);
        reader.boolean(payload.already_accepted);
        if (payload.format_version != 1 || payload.workset_id == 0
            || payload.sidecar_version != 1
            || payload.applied_sidecar_sha256.size() != 64) {
            reader.fail(PayloadError::InvalidValue);
        }
    });
}

PayloadCodecResult EncodePayload(
    const CancelWorksetItemPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.u64(payload.workset_id);
        writer.u64(payload.item_id);
        writer.string(payload.reason);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    CancelWorksetItemPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        reader.u64(payload.workset_id);
        reader.u64(payload.item_id);
        reader.string(payload.reason);
    });
}

PayloadCodecResult EncodePayload(
    const CancelWorksetPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.u64(payload.workset_id);
        writer.string(payload.reason);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    CancelWorksetPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        reader.u64(payload.workset_id);
        reader.string(payload.reason);
    });
}

PayloadCodecResult EncodePayload(
    const AcknowledgeTerminalPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.u64(payload.workset_id);
        writer.u64(payload.item_id);
        writer.u32(payload.item_ordinal);
        writer.u64(payload.invocation_id);
        writer.u64(payload.attempt_id);
        writer.u64(payload.terminal_id);
        writer.u64(payload.terminal_order);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    AcknowledgeTerminalPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        reader.u64(payload.workset_id);
        reader.u64(payload.item_id);
        reader.u32(payload.item_ordinal);
        reader.u64(payload.invocation_id);
        reader.u64(payload.attempt_id);
        reader.u64(payload.terminal_id);
        reader.u64(payload.terminal_order);
    });
}

PayloadCodecResult EncodePayload(
    const CancelInvocationPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.u64(payload.invocation_id);
        writer.string(payload.reason);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    CancelInvocationPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        reader.u64(payload.invocation_id);
        reader.string(payload.reason);
    });
}

PayloadCodecResult EncodePayload(
    const CaptureScreenshotPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.u64(payload.session_id);
        writer.string(payload.output_path);
        writer.u32(payload.timeout_ms);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    CaptureScreenshotPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        reader.u64(payload.session_id);
        reader.string(payload.output_path);
        reader.u32(payload.timeout_ms);
    });
}

PayloadCodecResult EncodePayload(
    const ShutdownPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.u32(payload.grace_period_ms);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    ShutdownPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        reader.u32(payload.grace_period_ms);
    });
}

PayloadCodecResult EncodePayload(
    const CommandResultPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.u64(payload.command_sequence);
        if (!IsKnownMessageKind(payload.command_kind)
            || DirectionOf(payload.command_kind) != MessageDirection::ParentToWorker) {
            writer.fail(PayloadError::InvalidMessageKind);
        }
        writer.u16(static_cast<std::uint16_t>(payload.command_kind));
        if (!IsKnownCommandStatus(static_cast<std::uint8_t>(payload.status)))
            writer.fail(PayloadError::InvalidEnumValue);
        writer.u8(static_cast<std::uint8_t>(payload.status));
        if (!IsKnownRejectionCode(
                static_cast<std::uint16_t>(payload.rejection_code))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u16(static_cast<std::uint16_t>(payload.rejection_code));
        writer.string(payload.error_code);
        writer.string(payload.message);
        writer.blob(payload.result);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    CommandResultPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        std::uint16_t command_kind = 0;
        std::uint16_t rejection_code = 0;
        std::uint8_t status = 0;
        reader.u64(payload.command_sequence);
        if (reader.u16(command_kind)) {
            payload.command_kind = static_cast<MessageKind>(command_kind);
            if (!IsKnownMessageKind(payload.command_kind)
                || DirectionOf(payload.command_kind) != MessageDirection::ParentToWorker) {
                reader.fail(PayloadError::InvalidMessageKind);
            }
        }
        if (reader.u8(status)) {
            payload.status = static_cast<CommandStatus>(status);
            if (!IsKnownCommandStatus(status))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        if (reader.u16(rejection_code)) {
            payload.rejection_code =
                static_cast<RejectionCode>(rejection_code);
            if (!IsKnownRejectionCode(rejection_code))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.string(payload.error_code);
        reader.string(payload.message);
        reader.blob(payload.result);
    });
}

PayloadCodecResult EncodePayload(
    const OpenSessionResultPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.boolean(payload.success);
        writer.u64(payload.session_id);
        writer.u64(payload.state_epoch);
        writer.u64(payload.capability_mask);
        if (!IsKnownWorkerState(
                static_cast<std::uint8_t>(payload.worker_state))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u8(static_cast<std::uint8_t>(payload.worker_state));
        if (!IsKnownSessionDisposition(
                static_cast<std::uint8_t>(payload.session_disposition))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u8(static_cast<std::uint8_t>(payload.session_disposition));
        if (!IsKnownRejectionCode(
                static_cast<std::uint16_t>(payload.rejection_code))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u16(static_cast<std::uint16_t>(payload.rejection_code));
        writer.string(payload.error_code);
        writer.string(payload.message);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    OpenSessionResultPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        std::uint8_t worker_state = 0;
        std::uint8_t session_disposition = 0;
        std::uint16_t rejection_code = 0;
        reader.boolean(payload.success);
        reader.u64(payload.session_id);
        reader.u64(payload.state_epoch);
        reader.u64(payload.capability_mask);
        if (reader.u8(worker_state)) {
            payload.worker_state = static_cast<WorkerStateCode>(worker_state);
            if (!IsKnownWorkerState(worker_state))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        if (reader.u8(session_disposition)) {
            payload.session_disposition =
                static_cast<SessionDispositionCode>(session_disposition);
            if (!IsKnownSessionDisposition(session_disposition))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        if (reader.u16(rejection_code)) {
            payload.rejection_code =
                static_cast<RejectionCode>(rejection_code);
            if (!IsKnownRejectionCode(rejection_code))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.string(payload.error_code);
        reader.string(payload.message);
    });
}

PayloadCodecResult EncodePayload(
    const ScreenshotResultPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        if (!IsKnownScreenshotStatus(static_cast<std::uint8_t>(payload.status)))
            writer.fail(PayloadError::InvalidEnumValue);
        writer.u8(static_cast<std::uint8_t>(payload.status));
        writer.u64(payload.session_id);
        writer.u64(payload.state_epoch);
        writer.string(payload.output_path);
        if (!IsKnownRejectionCode(
                static_cast<std::uint16_t>(payload.rejection_code))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u16(static_cast<std::uint16_t>(payload.rejection_code));
        writer.string(payload.error_code);
        writer.string(payload.message);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    ScreenshotResultPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        std::uint8_t status = 0;
        std::uint16_t rejection_code = 0;
        if (reader.u8(status)) {
            payload.status = static_cast<ScreenshotStatus>(status);
            if (!IsKnownScreenshotStatus(status))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.u64(payload.session_id);
        reader.u64(payload.state_epoch);
        reader.string(payload.output_path);
        if (reader.u16(rejection_code)) {
            payload.rejection_code =
                static_cast<RejectionCode>(rejection_code);
            if (!IsKnownRejectionCode(rejection_code))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.string(payload.error_code);
        reader.string(payload.message);
    });
}

PayloadCodecResult EncodePayload(
    const ShutdownResultPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        if (!IsKnownShutdownStatus(static_cast<std::uint8_t>(payload.status)))
            writer.fail(PayloadError::InvalidEnumValue);
        writer.u8(static_cast<std::uint8_t>(payload.status));
        if (!IsKnownSessionDisposition(
                static_cast<std::uint8_t>(payload.final_disposition))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u8(static_cast<std::uint8_t>(payload.final_disposition));
        if (!IsKnownRejectionCode(
                static_cast<std::uint16_t>(payload.rejection_code))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u16(static_cast<std::uint16_t>(payload.rejection_code));
        writer.string(payload.error_code);
        writer.string(payload.message);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    ShutdownResultPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        std::uint8_t status = 0;
        std::uint8_t final_disposition = 0;
        std::uint16_t rejection_code = 0;
        if (reader.u8(status)) {
            payload.status = static_cast<ShutdownStatus>(status);
            if (!IsKnownShutdownStatus(status))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        if (reader.u8(final_disposition)) {
            payload.final_disposition =
                static_cast<SessionDispositionCode>(final_disposition);
            if (!IsKnownSessionDisposition(final_disposition))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        if (reader.u16(rejection_code)) {
            payload.rejection_code =
                static_cast<RejectionCode>(rejection_code);
            if (!IsKnownRejectionCode(rejection_code))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.string(payload.error_code);
        reader.string(payload.message);
    });
}

PayloadCodecResult EncodePayload(
    const SessionEventPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        if (!IsKnownSessionEventType(static_cast<std::uint8_t>(payload.event_type)))
            writer.fail(PayloadError::InvalidEnumValue);
        writer.u8(static_cast<std::uint8_t>(payload.event_type));
        writer.u64(payload.session_id);
        writer.u64(payload.state_epoch);
        writer.u64(payload.capability_mask);
        if (!IsKnownWorkerState(
                static_cast<std::uint8_t>(payload.worker_state))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u8(static_cast<std::uint8_t>(payload.worker_state));
        if (!IsKnownSessionDisposition(
                static_cast<std::uint8_t>(payload.session_disposition))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u8(static_cast<std::uint8_t>(payload.session_disposition));
        if (!IsKnownRejectionCode(
                static_cast<std::uint16_t>(payload.rejection_code))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u16(static_cast<std::uint16_t>(payload.rejection_code));
        writer.string(payload.code);
        writer.string(payload.message);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    SessionEventPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        std::uint8_t event_type = 0;
        std::uint8_t worker_state = 0;
        std::uint8_t session_disposition = 0;
        std::uint16_t rejection_code = 0;
        if (reader.u8(event_type)) {
            payload.event_type = static_cast<SessionEventType>(event_type);
            if (!IsKnownSessionEventType(event_type))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.u64(payload.session_id);
        reader.u64(payload.state_epoch);
        reader.u64(payload.capability_mask);
        if (reader.u8(worker_state)) {
            payload.worker_state = static_cast<WorkerStateCode>(worker_state);
            if (!IsKnownWorkerState(worker_state))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        if (reader.u8(session_disposition)) {
            payload.session_disposition =
                static_cast<SessionDispositionCode>(session_disposition);
            if (!IsKnownSessionDisposition(session_disposition))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        if (reader.u16(rejection_code)) {
            payload.rejection_code =
                static_cast<RejectionCode>(rejection_code);
            if (!IsKnownRejectionCode(rejection_code))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.string(payload.code);
        reader.string(payload.message);
    });
}

PayloadCodecResult EncodePayload(
    const InvocationProgressPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.u64(payload.invocation_id);
        writer.u64(payload.attempt_id);
        writer.u64(payload.workset_id);
        writer.u64(payload.item_id);
        writer.u32(payload.item_ordinal);
        writer.u64(payload.ordinal);
        writer.blob(payload.progress);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    InvocationProgressPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        reader.u64(payload.invocation_id);
        reader.u64(payload.attempt_id);
        reader.u64(payload.workset_id);
        reader.u64(payload.item_id);
        reader.u32(payload.item_ordinal);
        reader.u64(payload.ordinal);
        reader.blob(payload.progress);
    });
}

PayloadCodecResult EncodePayload(
    const InvocationTerminalPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.u64(payload.invocation_id);
        writer.u64(payload.attempt_id);
        if (!IsKnownInvocationTerminalStatus(static_cast<std::uint8_t>(payload.status)))
            writer.fail(PayloadError::InvalidEnumValue);
        writer.u8(static_cast<std::uint8_t>(payload.status));
        if (!IsKnownSessionDisposition(
                static_cast<std::uint8_t>(payload.session_disposition))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u8(static_cast<std::uint8_t>(payload.session_disposition));
        writer.u64(payload.state_epoch);
        if (!IsKnownRejectionCode(
                static_cast<std::uint16_t>(payload.rejection_code))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u16(static_cast<std::uint16_t>(payload.rejection_code));
        writer.string(payload.error_code);
        writer.string(payload.message);
        writer.blob(payload.result);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    InvocationTerminalPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        std::uint8_t status = 0;
        std::uint8_t session_disposition = 0;
        std::uint16_t rejection_code = 0;
        reader.u64(payload.invocation_id);
        reader.u64(payload.attempt_id);
        if (reader.u8(status)) {
            payload.status = static_cast<InvocationTerminalStatus>(status);
            if (!IsKnownInvocationTerminalStatus(status))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        if (reader.u8(session_disposition)) {
            payload.session_disposition =
                static_cast<SessionDispositionCode>(session_disposition);
            if (!IsKnownSessionDisposition(session_disposition))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.u64(payload.state_epoch);
        if (reader.u16(rejection_code)) {
            payload.rejection_code =
                static_cast<RejectionCode>(rejection_code);
            if (!IsKnownRejectionCode(rejection_code))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.string(payload.error_code);
        reader.string(payload.message);
        reader.blob(payload.result);
    });
}

PayloadCodecResult EncodePayload(
    const WorksetStatePayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.u64(payload.outbound_sequence);
        writer.u64(payload.workset_id);
        if (!IsKnownWorksetState(static_cast<std::uint8_t>(payload.state)))
            writer.fail(PayloadError::InvalidEnumValue);
        writer.u8(static_cast<std::uint8_t>(payload.state));
        writer.u32(payload.next_item_ordinal);
        if (!IsKnownRejectionCode(
                static_cast<std::uint16_t>(payload.rejection_code))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u16(static_cast<std::uint16_t>(payload.rejection_code));
        writer.string(payload.message);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    WorksetStatePayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        std::uint8_t state = 0;
        std::uint16_t rejection = 0;
        reader.u64(payload.outbound_sequence);
        reader.u64(payload.workset_id);
        if (reader.u8(state)) {
            payload.state = static_cast<WorksetStateCode>(state);
            if (!IsKnownWorksetState(state))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.u32(payload.next_item_ordinal);
        if (reader.u16(rejection)) {
            payload.rejection_code = static_cast<RejectionCode>(rejection);
            if (!IsKnownRejectionCode(rejection))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.string(payload.message);
    });
}

PayloadCodecResult EncodePayload(
    const WorksetResidenceSnapshotV1& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        if (payload.format_version != 1
            || payload.has_resident_workset != (payload.workset_id != 0)
            || !IsKnownWorksetState(static_cast<std::uint8_t>(payload.state))
            || (payload.has_resident_workset
                && payload.cancellation_sidecar_sha256.size() != 64)
            || (!payload.has_resident_workset
                && !payload.cancellation_sidecar_sha256.empty())) {
            writer.fail(PayloadError::InvalidValue);
        }
        writer.u32(payload.format_version);
        writer.boolean(payload.has_resident_workset);
        writer.u64(payload.workset_id);
        writer.u8(static_cast<std::uint8_t>(payload.state));
        writer.string(payload.cancellation_sidecar_sha256);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    WorksetResidenceSnapshotV1& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        std::uint8_t state = 0;
        reader.u32(payload.format_version);
        reader.boolean(payload.has_resident_workset);
        reader.u64(payload.workset_id);
        if (reader.u8(state)) {
            payload.state = static_cast<WorksetStateCode>(state);
        }
        reader.string(payload.cancellation_sidecar_sha256);
        if (payload.format_version != 1
            || payload.has_resident_workset != (payload.workset_id != 0)
            || !IsKnownWorksetState(state)
            || (payload.has_resident_workset
                && payload.cancellation_sidecar_sha256.size() != 64)
            || (!payload.has_resident_workset
                && !payload.cancellation_sidecar_sha256.empty())) {
            reader.fail(PayloadError::InvalidValue);
        }
    });
}

PayloadCodecResult EncodePayload(
    const WorksetItemStartedPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.u64(payload.outbound_sequence);
        writer.u64(payload.workset_id);
        writer.u64(payload.item_id);
        writer.u32(payload.item_ordinal);
        writer.u64(payload.invocation_id);
        writer.u64(payload.attempt_id);
        writer.u64(payload.session_id);
        writer.u64(payload.state_epoch);
        writer.string(payload.baseline_sha256);
        writer.string(payload.baseline_lineage);
        writer.boolean(payload.baseline_restored);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    WorksetItemStartedPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        reader.u64(payload.outbound_sequence);
        reader.u64(payload.workset_id);
        reader.u64(payload.item_id);
        reader.u32(payload.item_ordinal);
        reader.u64(payload.invocation_id);
        reader.u64(payload.attempt_id);
        reader.u64(payload.session_id);
        reader.u64(payload.state_epoch);
        reader.string(payload.baseline_sha256);
        reader.string(payload.baseline_lineage);
        reader.boolean(payload.baseline_restored);
    });
}

PayloadCodecResult EncodePayload(
    const WorksetItemTerminalPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.u64(payload.outbound_sequence);
        writer.u64(payload.workset_id);
        writer.u64(payload.item_id);
        writer.u32(payload.item_ordinal);
        writer.u64(payload.invocation_id);
        writer.u64(payload.attempt_id);
        writer.u64(payload.terminal_id);
        writer.u64(payload.terminal_order);
        if (!IsKnownInvocationTerminalStatus(
                static_cast<std::uint8_t>(payload.status))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u8(static_cast<std::uint8_t>(payload.status));
        if (!IsKnownSessionDisposition(
                static_cast<std::uint8_t>(payload.session_disposition))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u8(static_cast<std::uint8_t>(payload.session_disposition));
        writer.u64(payload.state_epoch);
        writer.boolean(payload.unstarted);
        if (!IsKnownRejectionCode(
                static_cast<std::uint16_t>(payload.rejection_code))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u16(static_cast<std::uint16_t>(payload.rejection_code));
        writer.string(payload.error_code);
        writer.string(payload.message);
        writer.blob(payload.result);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    WorksetItemTerminalPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        std::uint8_t status = 0;
        std::uint8_t disposition = 0;
        std::uint16_t rejection = 0;
        reader.u64(payload.outbound_sequence);
        reader.u64(payload.workset_id);
        reader.u64(payload.item_id);
        reader.u32(payload.item_ordinal);
        reader.u64(payload.invocation_id);
        reader.u64(payload.attempt_id);
        reader.u64(payload.terminal_id);
        reader.u64(payload.terminal_order);
        if (reader.u8(status)) {
            payload.status = static_cast<InvocationTerminalStatus>(status);
            if (!IsKnownInvocationTerminalStatus(status))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        if (reader.u8(disposition)) {
            payload.session_disposition =
                static_cast<SessionDispositionCode>(disposition);
            if (!IsKnownSessionDisposition(disposition))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.u64(payload.state_epoch);
        reader.boolean(payload.unstarted);
        if (reader.u16(rejection)) {
            payload.rejection_code = static_cast<RejectionCode>(rejection);
            if (!IsKnownRejectionCode(rejection))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.string(payload.error_code);
        reader.string(payload.message);
        reader.blob(payload.result);
    });
}

PayloadCodecResult EncodePayload(
    const WorksetCreditsPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.u64(payload.outbound_sequence);
        writer.u32(payload.available_item_credits);
        writer.u32(payload.active_and_staged_items);
        writer.u32(payload.retained_terminals);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    WorksetCreditsPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        reader.u64(payload.outbound_sequence);
        reader.u32(payload.available_item_credits);
        reader.u32(payload.active_and_staged_items);
        reader.u32(payload.retained_terminals);
    });
}

PayloadCodecResult EncodePayload(
    const WorksetSummaryPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.u64(payload.outbound_sequence);
        writer.u64(payload.workset_id);
        writer.u32(payload.item_count);
        writer.u32(payload.completed_count);
        writer.u32(payload.unstarted_count);
        writer.u32(payload.initially_suppressed_count);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    WorksetSummaryPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        reader.u64(payload.outbound_sequence);
        reader.u64(payload.workset_id);
        reader.u32(payload.item_count);
        reader.u32(payload.completed_count);
        reader.u32(payload.unstarted_count);
        reader.u32(payload.initially_suppressed_count);
    });
}

PayloadCodecResult EncodePayload(
    const HostEventPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.u64(payload.session_id);
        writer.u64(payload.state_epoch);
        writer.u64(payload.sequence);
        writer.string(payload.name);
        writer.blob(payload.event_data);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    HostEventPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        reader.u64(payload.session_id);
        reader.u64(payload.state_epoch);
        reader.u64(payload.sequence);
        reader.string(payload.name);
        reader.blob(payload.event_data);
    });
}

PayloadCodecResult EncodePayload(
    const RuntimeDiagnosticPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        if (!IsKnownRejectionCode(
                static_cast<std::uint16_t>(payload.rejection_code))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u16(static_cast<std::uint16_t>(payload.rejection_code));
        writer.u64(payload.command_sequence);
        writer.u64(payload.invocation_id);
        writer.string(payload.message);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    RuntimeDiagnosticPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        std::uint16_t rejection_code = 0;
        if (reader.u16(rejection_code)) {
            payload.rejection_code =
                static_cast<RejectionCode>(rejection_code);
            if (!IsKnownRejectionCode(rejection_code))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.u64(payload.command_sequence);
        reader.u64(payload.invocation_id);
        reader.string(payload.message);
    });
}

PayloadCodecResult EncodePayload(
    const ControlExecutionPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        if (!IsKnownExecutionControl(
                static_cast<std::uint8_t>(payload.control))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u8(static_cast<std::uint8_t>(payload.control));
        writer.u64(payload.session_id);
        writer.u64(payload.expected_state_epoch);
        writer.u32(payload.count);
        writer.u32(payload.timeout_ms);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    ControlExecutionPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        std::uint8_t control = 0;
        if (reader.u8(control)) {
            payload.control = static_cast<ExecutionControlKind>(control);
            if (!IsKnownExecutionControl(control))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.u64(payload.session_id);
        reader.u64(payload.expected_state_epoch);
        reader.u32(payload.count);
        reader.u32(payload.timeout_ms);
    });
}

PayloadCodecResult EncodePayload(
    const ExecutionResultPayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.u64(payload.command_sequence);
        if (!IsKnownExecutionControl(
                static_cast<std::uint8_t>(payload.control))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u8(static_cast<std::uint8_t>(payload.control));
        if (!IsKnownCommandStatus(static_cast<std::uint8_t>(payload.status)))
            writer.fail(PayloadError::InvalidEnumValue);
        writer.u8(static_cast<std::uint8_t>(payload.status));
        writer.u64(payload.session_id);
        writer.u64(payload.state_epoch);
        writer.u64(payload.operation_id);
        if (!IsKnownExecutionActivity(
                static_cast<std::uint8_t>(payload.activity))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u8(static_cast<std::uint8_t>(payload.activity));
        writer.boolean(payload.has_terminal_status);
        if (!IsKnownExecutionTerminalStatus(
                static_cast<std::uint8_t>(payload.terminal_status))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u8(static_cast<std::uint8_t>(payload.terminal_status));
        writer.u64(payload.completed_count);
        writer.u32(payload.program_counter);
        if (!IsKnownRejectionCode(
                static_cast<std::uint16_t>(payload.rejection_code))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u16(static_cast<std::uint16_t>(payload.rejection_code));
        writer.string(payload.error_code);
        writer.string(payload.message);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    ExecutionResultPayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        std::uint8_t control = 0;
        std::uint8_t status = 0;
        std::uint8_t activity = 0;
        std::uint8_t terminal_status = 0;
        std::uint16_t rejection_code = 0;
        reader.u64(payload.command_sequence);
        if (reader.u8(control)) {
            payload.control = static_cast<ExecutionControlKind>(control);
            if (!IsKnownExecutionControl(control))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        if (reader.u8(status)) {
            payload.status = static_cast<CommandStatus>(status);
            if (!IsKnownCommandStatus(status))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.u64(payload.session_id);
        reader.u64(payload.state_epoch);
        reader.u64(payload.operation_id);
        if (reader.u8(activity)) {
            payload.activity = static_cast<ExecutionActivityCode>(activity);
            if (!IsKnownExecutionActivity(activity))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.boolean(payload.has_terminal_status);
        if (reader.u8(terminal_status)) {
            payload.terminal_status =
                static_cast<ExecutionTerminalStatusCode>(terminal_status);
            if (!IsKnownExecutionTerminalStatus(terminal_status))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.u64(payload.completed_count);
        reader.u32(payload.program_counter);
        if (reader.u16(rejection_code)) {
            payload.rejection_code =
                static_cast<RejectionCode>(rejection_code);
            if (!IsKnownRejectionCode(rejection_code))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.string(payload.error_code);
        reader.string(payload.message);
    });
}

PayloadCodecResult EncodePayload(
    const ExecutionStatePayload& value,
    std::vector<std::uint8_t>& output)
{
    return EncodePayloadImpl(value, output, [](PayloadWriter& writer, const auto& payload) {
        writer.u64(payload.session_id);
        writer.u64(payload.state_epoch);
        writer.u64(payload.operation_id);
        if (!IsKnownExecutionActivity(
                static_cast<std::uint8_t>(payload.activity))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u8(static_cast<std::uint8_t>(payload.activity));
        writer.boolean(payload.has_active_control);
        if (!IsKnownExecutionControl(
                static_cast<std::uint8_t>(payload.active_control))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u8(static_cast<std::uint8_t>(payload.active_control));
        writer.u64(payload.completed_count);
        writer.u32(payload.program_counter);
        if (!IsKnownRejectionCode(
                static_cast<std::uint16_t>(payload.rejection_code))) {
            writer.fail(PayloadError::InvalidEnumValue);
        }
        writer.u16(static_cast<std::uint16_t>(payload.rejection_code));
        writer.string(payload.code);
        writer.string(payload.message);
    });
}

PayloadCodecResult DecodePayload(
    std::span<const std::uint8_t> input,
    ExecutionStatePayload& output)
{
    return DecodePayloadImpl(input, output, [](PayloadReader& reader, auto& payload) {
        std::uint8_t activity = 0;
        std::uint8_t active_control = 0;
        std::uint16_t rejection_code = 0;
        reader.u64(payload.session_id);
        reader.u64(payload.state_epoch);
        reader.u64(payload.operation_id);
        if (reader.u8(activity)) {
            payload.activity = static_cast<ExecutionActivityCode>(activity);
            if (!IsKnownExecutionActivity(activity))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.boolean(payload.has_active_control);
        if (reader.u8(active_control)) {
            payload.active_control =
                static_cast<ExecutionControlKind>(active_control);
            if (!IsKnownExecutionControl(active_control))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.u64(payload.completed_count);
        reader.u32(payload.program_counter);
        if (reader.u16(rejection_code)) {
            payload.rejection_code =
                static_cast<RejectionCode>(rejection_code);
            if (!IsKnownRejectionCode(rejection_code))
                reader.fail(PayloadError::InvalidEnumValue);
        }
        reader.string(payload.code);
        reader.string(payload.message);
    });
}

} // namespace savor::wrms
