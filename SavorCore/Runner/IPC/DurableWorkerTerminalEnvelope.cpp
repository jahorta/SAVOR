#include "DurableWorkerTerminalEnvelope.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace savor::runtime {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{
    'S', 'W', 'T', 'E', 'R', 'M', '1', '\0'};

bool Fail(std::string message, std::string* error_out) {
    if (error_out != nullptr) {
        *error_out = std::move(message);
    }
    return false;
}

void AppendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void AppendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        bytes.push_back(
            static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
}

void AppendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        bytes.push_back(
            static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
}

bool ReadU16(
    std::span<const std::uint8_t> bytes,
    std::size_t* offset,
    std::uint16_t* value_out) {
    if (offset == nullptr || value_out == nullptr
        || *offset > bytes.size() || bytes.size() - *offset < 2) {
        return false;
    }
    *value_out = static_cast<std::uint16_t>(bytes[*offset])
        | (static_cast<std::uint16_t>(bytes[*offset + 1]) << 8u);
    *offset += 2;
    return true;
}

bool ReadU32(
    std::span<const std::uint8_t> bytes,
    std::size_t* offset,
    std::uint32_t* value_out) {
    if (offset == nullptr || value_out == nullptr
        || *offset > bytes.size() || bytes.size() - *offset < 4) {
        return false;
    }
    std::uint32_t value = 0;
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(
            bytes[*offset + shift / 8])
            << shift;
    }
    *offset += 4;
    *value_out = value;
    return true;
}

bool ReadU64(
    std::span<const std::uint8_t> bytes,
    std::size_t* offset,
    std::uint64_t* value_out) {
    if (offset == nullptr || value_out == nullptr
        || *offset > bytes.size() || bytes.size() - *offset < 8) {
        return false;
    }
    std::uint64_t value = 0;
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(
            bytes[*offset + shift / 8])
            << shift;
    }
    *offset += 8;
    *value_out = value;
    return true;
}

} // namespace

bool EncodeDurableWorkerTerminalEnvelope(
    const DurableWorkerTerminalEnvelope& envelope,
    std::vector<std::uint8_t>* bytes_out,
    std::string* error_out) {
    if (bytes_out == nullptr) {
        return Fail("durable worker terminal output is required", error_out);
    }
    if (envelope.envelope_version
            != kDurableWorkerTerminalEnvelopeVersion
        || envelope.wrms_protocol_version != wrms::ProtocolVersion
        || envelope.process_generation == 0
        || envelope.terminal.workset_id == 0
        || envelope.terminal.item_id == 0
        || envelope.terminal.invocation_id == 0
        || envelope.terminal.attempt_id == 0
        || envelope.terminal.terminal_id == 0
        || envelope.terminal.terminal_order == 0) {
        return Fail("durable worker terminal identity is invalid", error_out);
    }

    std::vector<std::uint8_t> terminal_bytes;
    const auto encoded =
        wrms::EncodePayload(envelope.terminal, terminal_bytes);
    if (!encoded) {
        return Fail("failed encoding WRMS worker terminal payload", error_out);
    }
    if (terminal_bytes.size()
        > (std::numeric_limits<std::uint32_t>::max)()) {
        return Fail("WRMS worker terminal payload is too large", error_out);
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(
        kMagic.size() + 2 + 2 + 8 + 8 + 1 + 4 + terminal_bytes.size());
    bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
    AppendU16(bytes, envelope.envelope_version);
    AppendU16(bytes, envelope.wrms_protocol_version);
    AppendU64(bytes, envelope.worker_id);
    AppendU64(bytes, envelope.process_generation);
    bytes.push_back(envelope.cancellation_reason);
    AppendU32(
        bytes,
        static_cast<std::uint32_t>(terminal_bytes.size()));
    bytes.insert(bytes.end(), terminal_bytes.begin(), terminal_bytes.end());

    *bytes_out = std::move(bytes);
    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

bool DecodeDurableWorkerTerminalEnvelope(
    std::span<const std::uint8_t> bytes,
    DurableWorkerTerminalEnvelope* envelope_out,
    std::string* error_out) {
    if (envelope_out == nullptr) {
        return Fail("durable worker terminal receiver is required", error_out);
    }
    if (bytes.size() < kMagic.size()
        || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        return Fail("durable worker terminal magic is invalid", error_out);
    }

    std::size_t offset = kMagic.size();
    DurableWorkerTerminalEnvelope envelope{};
    std::uint32_t terminal_size = 0;
    if (!ReadU16(bytes, &offset, &envelope.envelope_version)
        || !ReadU16(bytes, &offset, &envelope.wrms_protocol_version)
        || !ReadU64(bytes, &offset, &envelope.worker_id)
        || !ReadU64(bytes, &offset, &envelope.process_generation)
        || offset >= bytes.size()) {
        return Fail("durable worker terminal header is invalid", error_out);
    }
    envelope.cancellation_reason = bytes[offset++];
    if (!ReadU32(bytes, &offset, &terminal_size)
        || envelope.envelope_version
            != kDurableWorkerTerminalEnvelopeVersion
        || envelope.wrms_protocol_version != wrms::ProtocolVersion
        || envelope.process_generation == 0
        || offset > bytes.size()
        || terminal_size != bytes.size() - offset) {
        return Fail("durable worker terminal header is invalid", error_out);
    }

    const auto decoded = wrms::DecodePayload(
        bytes.subspan(offset, terminal_size),
        envelope.terminal);
    if (!decoded || envelope.terminal.workset_id == 0
        || envelope.terminal.item_id == 0
        || envelope.terminal.invocation_id == 0
        || envelope.terminal.attempt_id == 0
        || envelope.terminal.terminal_id == 0
        || envelope.terminal.terminal_order == 0) {
        return Fail("durable WRMS worker terminal payload is invalid", error_out);
    }

    *envelope_out = std::move(envelope);
    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

} // namespace savor::runtime
