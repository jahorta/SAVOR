#include "TelemetryBus.h"

#include <algorithm>
#include <utility>

namespace savor::runtime {

TelemetryBus::TelemetryBus(std::size_t capacity)
    : capacity_(std::max<std::size_t>(1, capacity))
{
}

TelemetryEnqueueReceipt TelemetryBus::Enqueue(TelemetryEvent event)
{
    if (event.source.empty() || event.source.size() > kMaximumSourceLength ||
        event.kind.empty() || event.kind.size() > kMaximumKindLength ||
        event.payload.size() > kMaximumPayloadLength)
    {
        return {false, {}, false, false, false, "telemetry event exceeds a bounded field"};
    }

    std::lock_guard lock(mutex_);
    event.sequence = TelemetrySequence(next_sequence_++);
    if (queue_.size() < capacity_)
    {
        const TelemetrySequence sequence = event.sequence;
        queue_.push_back(std::move(event));
        return {true, sequence, false, false, false, {}};
    }

    if (event.loss_policy == TelemetryLossPolicy::Required)
    {
        ++authoritative_overflows_;
        return {
            false,
            event.sequence,
            false,
            false,
            true,
            "required telemetry ingress overflow"};
    }

    const auto existing = std::find_if(
        queue_.rbegin(),
        queue_.rend(),
        [&](const TelemetryEvent& queued) {
            return queued.loss_policy == TelemetryLossPolicy::LossyCoalescing &&
                queued.source == event.source &&
                queued.kind == event.kind;
        });
    if (existing != queue_.rend())
    {
        const TelemetrySequence sequence = event.sequence;
        auto position = existing.base();
        --position;
        queue_.erase(position);
        queue_.push_back(std::move(event));
        ++coalesced_;
        return {true, sequence, true, false, false, {}};
    }

    ++dropped_;
    return {true, event.sequence, false, true, false, {}};
}

std::vector<TelemetryEvent> TelemetryBus::Drain(std::size_t maximum)
{
    std::lock_guard lock(mutex_);
    const std::size_t count = std::min(maximum, queue_.size());
    std::vector<TelemetryEvent> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        result.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }
    return result;
}

TelemetrySnapshot TelemetryBus::snapshot() const noexcept
{
    std::lock_guard lock(mutex_);
    return {
        queue_.size(),
        coalesced_,
        dropped_,
        authoritative_overflows_};
}

} // namespace savor::runtime
