#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "WrmsProtocol.h"

namespace savor::runtime {

inline constexpr std::uint16_t kDurableWorkerTerminalEnvelopeVersion = 2;

struct DurableWorkerTerminalEnvelope {
    std::uint16_t envelope_version =
        kDurableWorkerTerminalEnvelopeVersion;
    std::uint16_t wrms_protocol_version = 0;
    std::uint64_t worker_id = 0;
    std::uint64_t process_generation = 0;
    std::uint8_t cancellation_reason = 0;
    wrms::WorksetItemTerminalPayload terminal;

    friend bool operator==(
        const DurableWorkerTerminalEnvelope&,
        const DurableWorkerTerminalEnvelope&) = default;
};

bool EncodeDurableWorkerTerminalEnvelope(
    const DurableWorkerTerminalEnvelope& envelope,
    std::vector<std::uint8_t>* bytes_out,
    std::string* error_out = nullptr);

bool DecodeDurableWorkerTerminalEnvelope(
    std::span<const std::uint8_t> bytes,
    DurableWorkerTerminalEnvelope* envelope_out,
    std::string* error_out = nullptr);

} // namespace savor::runtime
