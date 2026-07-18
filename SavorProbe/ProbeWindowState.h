#pragma once

#include "ProbeProfile.h"

#include <atomic>
#include <cstdint>
#include <string_view>

namespace savor::probe {

class ProbeWindowState {
public:
    void reset(bool initially_open)
    {
        open_.store(initially_open, std::memory_order_relaxed);
        opened_frame_.store(0, std::memory_order_relaxed);
        events_.store(0, std::memory_order_relaxed);
        close_after_frame_event_.store(false, std::memory_order_relaxed);
    }

    bool is_open() const { return open_.load(std::memory_order_acquire); }
    std::uint64_t accepted_events() const
    {
        return events_.load(std::memory_order_acquire);
    }

    void on_frame(const WindowDefinition& definition, std::uint64_t frame)
    {
        if (definition.open_frame.has_value() && *definition.open_frame == frame)
            open_at(frame);
        if (definition.close_frame.has_value() && *definition.close_frame == frame)
            close_after_frame_event_.store(true, std::memory_order_release);
    }

    void on_marker(
        const WindowDefinition& definition,
        std::string_view marker,
        std::uint64_t frame)
    {
        if (!definition.open_marker.empty() && definition.open_marker == marker)
            open_at(frame);
        if (!definition.close_marker.empty() && definition.close_marker == marker)
            open_.store(false, std::memory_order_release);
    }

    void on_control(const WindowDefinition& definition, std::uint64_t frame)
    {
        if (definition.open_on_control)
            open_at(frame);
        if (definition.close_on_control)
            open_.store(false, std::memory_order_release);
    }

    void before_probe(
        const WindowDefinition& definition,
        std::string_view probe_id,
        std::uint64_t frame)
    {
        if (!definition.open_probe.empty() && definition.open_probe == probe_id)
            open_at(frame);
    }

    void after_probe(
        const WindowDefinition& definition,
        std::string_view probe_id,
        bool member,
        bool accepted,
        bool frame_clock,
        std::uint64_t frame)
    {
        const auto open = is_open();
        const auto events = open && accepted && member
            ? events_.fetch_add(1, std::memory_order_relaxed) + 1
            : events_.load(std::memory_order_relaxed);
        const auto opened_frame = opened_frame_.load(std::memory_order_relaxed);
        if ((!definition.close_probe.empty() && definition.close_probe == probe_id)
            || (open && definition.event_limit != 0 && accepted && member
                && events >= definition.event_limit)
            || (open && definition.frame_limit != 0 && frame >= opened_frame
                && frame - opened_frame >= definition.frame_limit)) {
            open_.store(false, std::memory_order_release);
        }
        if (frame_clock
            && close_after_frame_event_.exchange(false, std::memory_order_acq_rel)) {
            open_.store(false, std::memory_order_release);
        }
    }

private:
    void open_at(std::uint64_t frame)
    {
        opened_frame_.store(frame, std::memory_order_relaxed);
        events_.store(0, std::memory_order_relaxed);
        open_.store(true, std::memory_order_release);
    }

    std::atomic<bool> open_{ false };
    std::atomic<std::uint64_t> opened_frame_{ 0 };
    std::atomic<std::uint64_t> events_{ 0 };
    std::atomic<bool> close_after_frame_event_{ false };
};

} // namespace savor::probe
