#pragma once

#include "../../RuntimeTypes.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace savor::runtime {

struct TelemetrySequenceTag;
using TelemetrySequence = StrongId<TelemetrySequenceTag>;

enum class TelemetrySeverity : std::uint8_t
{
    Trace,
    Info,
    Warning,
    Error,
};

enum class TelemetryLossPolicy : std::uint8_t
{
    LossyCoalescing,
    Required,
};

struct TelemetryEvent
{
    TelemetrySequence sequence;
    std::string source;
    std::string kind;
    TelemetrySeverity severity = TelemetrySeverity::Info;
    std::optional<StateEpoch> epoch;
    std::string payload;
    bool record_progress = false;
    TelemetryLossPolicy loss_policy =
        TelemetryLossPolicy::LossyCoalescing;
};

struct TelemetryEnqueueReceipt
{
    bool ok = false;
    TelemetrySequence sequence;
    bool coalesced = false;
    bool dropped = false;
    bool authoritative_overflow = false;
    std::string message;
};

struct TelemetrySnapshot
{
    std::size_t queued = 0;
    std::uint64_t coalesced = 0;
    std::uint64_t dropped = 0;
    std::uint64_t authoritative_overflows = 0;
};

class TelemetryBus final
{
public:
    explicit TelemetryBus(std::size_t capacity = 4096);

    [[nodiscard]] TelemetryEnqueueReceipt Enqueue(TelemetryEvent event);
    [[nodiscard]] std::vector<TelemetryEvent> Drain(
        std::size_t maximum = static_cast<std::size_t>(-1));
    [[nodiscard]] TelemetrySnapshot snapshot() const noexcept;

private:
    static constexpr std::size_t kMaximumSourceLength = 128;
    static constexpr std::size_t kMaximumKindLength = 128;
    static constexpr std::size_t kMaximumPayloadLength = 16 * 1024;

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<TelemetryEvent> queue_;
    std::uint64_t next_sequence_ = 1;
    std::uint64_t coalesced_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t authoritative_overflows_ = 0;
};

} // namespace savor::runtime
