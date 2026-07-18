#pragma once

#include "ProbeProfile.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace savor::probe {

inline constexpr std::size_t kMaxRawFields = kMaxProbeSamples;
inline constexpr std::size_t kRawPayloadBytes = 24u * 1024u;
inline constexpr std::size_t kRawEventSlotBytes = 64u * 1024u;
inline constexpr std::uint8_t kRawFieldAddressTrace = 0xfe;
inline constexpr std::uint8_t kRawEventControlPublished = 0x01;
inline constexpr std::uint8_t kRawEventAddressResolution = 0x02;
inline constexpr std::uint32_t kSyntheticControlProbeIndex = UINT32_MAX;
inline constexpr std::uint32_t kSyntheticMarkerProbeIndex = UINT32_MAX - 1;

enum class RawFieldStatus : std::uint8_t {
    Present = 0,
    Missing = 1,
    Truncated = 2,
    Invalid = 3,
};

struct RawField {
    std::uint16_t sample_index = 0;
    std::uint8_t kind = 0;
    RawFieldStatus status = RawFieldStatus::Present;
    std::uint64_t value = 0;
    std::uint32_t payload_offset = 0;
    std::uint32_t payload_size = 0;
};

struct RawAddressTraceStep {
    std::uint8_t index = 0;
    std::uint8_t operation = 0;
    bool has_dereferenced_value = false;
    AddressProgramFailure failure = AddressProgramFailure::None;
    std::uint32_t address_before = 0;
    std::uint32_t address_after = 0;
    std::uint64_t dereferenced_value = 0;
};

struct RawAddressTrace {
    std::uint8_t count = 0;
    bool success = false;
    std::uint8_t failure_operation = 0;
    AddressProgramFailure failure = AddressProgramFailure::None;
    std::uint32_t final_address = 0;
    std::array<RawAddressTraceStep, 64> operations{};
};

struct RawProbeEvent {
    std::uint64_t capture_sequence = 0;
    std::uint64_t monotonic_ns = 0;
    std::uint64_t frame_index = 0;
    std::uint64_t guest_state_epoch = 0;
    std::uint64_t profile_revision = 0;
    std::uint64_t snapshot_id = 0;
    std::uint64_t hit_count = 0;
    std::uint64_t value = 0;
    std::uint32_t probe_index = 0;
    std::uint32_t pc = 0;
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    std::uint16_t field_count = 0;
    std::uint16_t payload_size = 0;
    std::uint8_t kind = 0;
    std::uint8_t flags = 0;
    std::array<RawField, kMaxRawFields> fields{};
    std::array<std::uint8_t, kRawPayloadBytes> payload{};
};

static_assert(sizeof(RawProbeEvent) <= kRawEventSlotBytes);

class BoundedEventQueue {
public:
    bool configure(std::uint32_t capacity);
    void reset();
    bool try_push(const RawProbeEvent& event);
    bool try_pop(RawProbeEvent& event);
    std::uint32_t capacity() const { return capacity_; }
    std::uint32_t approximate_size() const;

private:
    struct Slot {
        std::atomic<std::uint64_t> sequence{ 0 };
        RawProbeEvent event{};
    };

    std::unique_ptr<Slot[]> slots_;
    std::uint32_t capacity_ = 0;
    alignas(64) std::atomic<std::uint64_t> enqueue_position_{ 0 };
    alignas(64) std::atomic<std::uint64_t> dequeue_position_{ 0 };
};

// A progress producer may replace an undelivered event, but it must never write
// the record currently being copied by the recorder thread. Three fixed slots
// leave one writable slot even when the active and reader slots differ.
class LatestRawProbeEvent {
public:
    bool configure();
    void reset();
    bool publish(const RawProbeEvent& event, bool* replaced_pending = nullptr);
    bool try_take(RawProbeEvent& event);
    bool has_pending() const;

private:
    static constexpr std::uint32_t kSlotCount = 3;
    static constexpr std::uint32_t kNoSlot = UINT32_MAX;

    std::unique_ptr<RawProbeEvent[]> slots_;
    std::uint64_t next_generation_ = 0; // single CPU-thread producer
    std::atomic<std::uint64_t> published_token_{ 0 };
    std::atomic<std::uint64_t> consumed_token_{ 0 };
    std::atomic<std::uint32_t> reader_slot_{ kNoSlot };
};

} // namespace savor::probe
