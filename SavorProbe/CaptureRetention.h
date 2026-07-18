#pragma once

#include "ProbeEvent.h"
#include "ProbeProfile.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace savor::probe {

struct FlightRecorderMetrics {
    std::string id;
    std::uint64_t triggers = 0;
    std::uint64_t retriggers = 0;
    std::uint64_t completed_windows = 0;
    std::uint32_t post_remaining = 0;
    bool active = false;
};

// Recorder-thread policy for delayed LastN and flight-recorder capture. This
// class never runs on Dolphin's CPU thread, so its bounded deques may allocate.
class CaptureRetention {
public:
    using Emit = std::function<void(const RawProbeEvent&)>;

    explicit CaptureRetention(const Profile& profile);

    void accept(const RawProbeEvent& event, const Emit& emit);
    void trigger_all(
        std::uint32_t post_override,
        std::uint64_t trigger_sequence,
        const Emit& emit);
    void finish(const Emit& emit);
    std::vector<FlightRecorderMetrics> flight_metrics() const;

private:
    struct FlightState {
        FlightRecorderDefinition definition;
        std::deque<RawProbeEvent> pre_events;
        bool active = false;
        std::uint64_t trigger_sequence = 0;
        std::uint32_t post_remaining = 0;
        std::uint64_t triggers = 0;
        std::uint64_t retriggers = 0;
        std::uint64_t completed_windows = 0;
    };

    std::string_view probe_id(const RawProbeEvent& event) const;
    bool is_member(const FlightState& state, std::string_view id) const;
    bool is_trigger(const FlightState& state, const RawProbeEvent& event) const;
    void activate(
        FlightState& state,
        std::uint64_t trigger_sequence,
        bool trigger_is_member,
        std::uint32_t post_override,
        const Emit& emit);

    const Profile& profile_;
    std::vector<std::deque<RawProbeEvent>> last_n_;
    std::vector<FlightState> flight_states_;
};

} // namespace savor::probe
