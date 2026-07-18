#include "CaptureRetention.h"

#include <algorithm>
#include <ranges>

namespace savor::probe {

CaptureRetention::CaptureRetention(const Profile& profile)
    : profile_(profile), last_n_(profile.probes.size())
{
    flight_states_.reserve(profile.flight_recorders.size());
    for (const auto& definition : profile.flight_recorders)
        flight_states_.push_back(FlightState{ .definition = definition });
}

std::string_view CaptureRetention::probe_id(const RawProbeEvent& event) const
{
    if (event.probe_index < profile_.probes.size())
        return profile_.probes[event.probe_index].id;
    if (event.probe_index == kSyntheticMarkerProbeIndex) {
        return std::string_view(
            reinterpret_cast<const char*>(event.payload.data()), event.payload_size);
    }
    if (event.probe_index == kSyntheticControlProbeIndex)
        return "control.hit";
    return {};
}

bool CaptureRetention::is_member(const FlightState& state, std::string_view id) const
{
    return std::ranges::find(state.definition.member_probes, id)
        != state.definition.member_probes.end();
}

bool CaptureRetention::is_trigger(
    const FlightState& state,
    const RawProbeEvent& event) const
{
    const auto id = probe_id(event);
    return ((event.flags & kRawEventControlPublished) != 0
            && state.definition.trigger_on_control)
        || std::ranges::find(state.definition.trigger_probes, id)
            != state.definition.trigger_probes.end()
        || (event.probe_index == kSyntheticMarkerProbeIndex
            && std::ranges::find(state.definition.trigger_markers, id)
                != state.definition.trigger_markers.end());
}

void CaptureRetention::activate(
    FlightState& state,
    std::uint64_t trigger_sequence,
    bool trigger_is_member,
    std::uint32_t post_override,
    const Emit& emit)
{
    if (state.active) {
        ++state.retriggers;
    } else {
        while (!state.pre_events.empty()) {
            emit(state.pre_events.front());
            state.pre_events.pop_front();
        }
    }
    state.trigger_sequence = trigger_sequence;
    state.post_remaining = std::max(
        state.post_remaining,
        std::max(state.definition.post_events, post_override));
    state.active = trigger_is_member || state.post_remaining != 0;
    ++state.triggers;
    if (!state.active)
        ++state.completed_windows;
}

void CaptureRetention::accept(const RawProbeEvent& event, const Emit& emit)
{
    const auto id = probe_id(event);
    for (auto& state : flight_states_) {
        if (is_trigger(state, event)) {
            activate(
                state,
                event.capture_sequence,
                is_member(state, id),
                0,
                emit);
        }
    }

    for (auto& state : flight_states_) {
        if (!is_member(state, id))
            continue;
        if (!state.active) {
            state.pre_events.push_back(event);
            while (state.pre_events.size() > state.definition.pre_events)
                state.pre_events.pop_front();
            return;
        }
        emit(event);
        if (event.capture_sequence != state.trigger_sequence && state.post_remaining > 0)
            --state.post_remaining;
        if (state.post_remaining == 0) {
            state.active = false;
            ++state.completed_windows;
        }
        return;
    }

    if (event.probe_index < profile_.probes.size()) {
        const auto& sampling = profile_.probes[event.probe_index].sampling;
        if (sampling.mode == SamplingMode::LastN) {
            auto& retained = last_n_[event.probe_index];
            retained.push_back(event);
            while (retained.size() > sampling.n)
                retained.pop_front();
            return;
        }
    }
    emit(event);
}

void CaptureRetention::trigger_all(
    std::uint32_t post_override,
    std::uint64_t trigger_sequence,
    const Emit& emit)
{
    for (auto& state : flight_states_)
        activate(state, trigger_sequence, false, post_override, emit);
}

void CaptureRetention::finish(const Emit& emit)
{
    std::vector<RawProbeEvent> retained;
    for (auto& events : last_n_) {
        retained.insert(retained.end(), events.begin(), events.end());
        events.clear();
    }
    std::ranges::sort(retained, {}, &RawProbeEvent::capture_sequence);
    for (const auto& event : retained)
        emit(event);
}

std::vector<FlightRecorderMetrics> CaptureRetention::flight_metrics() const
{
    std::vector<FlightRecorderMetrics> metrics;
    metrics.reserve(flight_states_.size());
    for (const auto& state : flight_states_) {
        metrics.push_back(FlightRecorderMetrics{
            .id = state.definition.id,
            .triggers = state.triggers,
            .retriggers = state.retriggers,
            .completed_windows = state.completed_windows,
            .post_remaining = state.post_remaining,
            .active = state.active,
        });
    }
    return metrics;
}

} // namespace savor::probe
