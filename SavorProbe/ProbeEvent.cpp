#include "ProbeEvent.h"

#include <algorithm>
#include <new>

namespace savor::probe {

bool BoundedEventQueue::configure(std::uint32_t capacity)
{
    if (capacity < 2)
        return false;
    try {
        slots_ = std::make_unique<Slot[]>(capacity);
    } catch (const std::bad_alloc&) {
        return false;
    }
    capacity_ = capacity;
    reset();
    return true;
}

void BoundedEventQueue::reset()
{
    enqueue_position_.store(0, std::memory_order_relaxed);
    dequeue_position_.store(0, std::memory_order_relaxed);
    for (std::uint64_t i = 0; i < capacity_; ++i)
        slots_[i].sequence.store(i, std::memory_order_relaxed);
}

bool BoundedEventQueue::try_push(const RawProbeEvent& event)
{
    if (!slots_ || capacity_ < 2)
        return false;
    auto position = enqueue_position_.load(std::memory_order_relaxed);
    for (;;) {
        auto& slot = slots_[position % capacity_];
        const auto sequence = slot.sequence.load(std::memory_order_acquire);
        const auto difference = static_cast<std::int64_t>(sequence)
            - static_cast<std::int64_t>(position);
        if (difference == 0) {
            if (enqueue_position_.compare_exchange_weak(
                    position, position + 1, std::memory_order_relaxed)) {
                slot.event = event;
                slot.sequence.store(position + 1, std::memory_order_release);
                return true;
            }
        } else if (difference < 0) {
            return false;
        } else {
            position = enqueue_position_.load(std::memory_order_relaxed);
        }
    }
}

bool BoundedEventQueue::try_pop(RawProbeEvent& event)
{
    if (!slots_ || capacity_ < 2)
        return false;
    const auto position = dequeue_position_.load(std::memory_order_relaxed);
    auto& slot = slots_[position % capacity_];
    const auto sequence = slot.sequence.load(std::memory_order_acquire);
    if (static_cast<std::int64_t>(sequence) - static_cast<std::int64_t>(position + 1) != 0)
        return false;
    event = slot.event;
    slot.sequence.store(position + capacity_, std::memory_order_release);
    dequeue_position_.store(position + 1, std::memory_order_relaxed);
    return true;
}

std::uint32_t BoundedEventQueue::approximate_size() const
{
    if (capacity_ == 0)
        return 0;
    const auto enqueued = enqueue_position_.load(std::memory_order_acquire);
    const auto dequeued = dequeue_position_.load(std::memory_order_acquire);
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        enqueued - dequeued, capacity_));
}

bool LatestRawProbeEvent::configure()
{
    try {
        slots_ = std::make_unique<RawProbeEvent[]>(kSlotCount);
    } catch (const std::bad_alloc&) {
        return false;
    }
    reset();
    return true;
}

void LatestRawProbeEvent::reset()
{
    next_generation_ = 0;
    published_token_.store(0, std::memory_order_relaxed);
    consumed_token_.store(0, std::memory_order_relaxed);
    reader_slot_.store(kNoSlot, std::memory_order_relaxed);
}

bool LatestRawProbeEvent::publish(
    const RawProbeEvent& event,
    bool* replaced_pending)
{
    if (!slots_)
        return false;
    const auto previous = published_token_.load(std::memory_order_acquire);
    const auto consumed = consumed_token_.load(std::memory_order_acquire);
    if (replaced_pending)
        *replaced_pending = previous != 0 && previous != consumed;

    const auto active_slot = previous == 0
        ? kNoSlot
        : static_cast<std::uint32_t>(previous & 0x3u);
    const auto reader_slot = reader_slot_.load(std::memory_order_acquire);
    std::uint32_t selected = kNoSlot;
    for (std::uint32_t index = 0; index < kSlotCount; ++index) {
        if (index != active_slot && index != reader_slot) {
            selected = index;
            break;
        }
    }
    if (selected == kNoSlot)
        return false;

    slots_[selected] = event;
    const auto token = (++next_generation_ << 2) | selected;
    published_token_.store(token, std::memory_order_release);
    return true;
}

bool LatestRawProbeEvent::try_take(RawProbeEvent& event)
{
    if (!slots_)
        return false;
    for (;;) {
        const auto token = published_token_.load(std::memory_order_acquire);
        if (token == 0 || token == consumed_token_.load(std::memory_order_acquire))
            return false;
        const auto slot = static_cast<std::uint32_t>(token & 0x3u);
        reader_slot_.store(slot, std::memory_order_release);
        if (published_token_.load(std::memory_order_acquire) != token) {
            reader_slot_.store(kNoSlot, std::memory_order_release);
            continue;
        }
        event = slots_[slot];
        reader_slot_.store(kNoSlot, std::memory_order_release);
        if (published_token_.load(std::memory_order_acquire) != token)
            continue;
        consumed_token_.store(token, std::memory_order_release);
        return true;
    }
}

bool LatestRawProbeEvent::has_pending() const
{
    const auto published = published_token_.load(std::memory_order_acquire);
    return published != 0
        && published != consumed_token_.load(std::memory_order_acquire);
}

} // namespace savor::probe
