#pragma once

#include "ProbeEvent.h"
#include "ProbeProfile.h"
#include "ProbeWatchpointRegistry.h"
#include "ProbeWindowState.h"
#include "../SavorCaptureFormat/CaptureFormat.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace Core {
class System;
}

namespace PowerPC {
class PowerPCManager;
}

namespace savor::probe {

inline constexpr std::string_view kSupportedDolphinSourceCommit =
    "9843115ad8414970312c954d83145300d7cdbec3";

struct SessionOptions {
    std::filesystem::path capture_path;
    capture_format::FileMetadata metadata;
    capture_format::WriterOptions writer_options;
    std::function<void(const capture_format::Event&, bool record_progress)> progress_callback;
    std::function<std::optional<std::uint32_t>(std::uint16_t)> base_key_resolver;
    // Runtime-owned PCs that capture profiles may not observe or use as
    // activation gates. StopPointRouter owns all wake/control authority.
    std::vector<std::uint32_t> denied_profile_pcs;
};

bool ValidateProfilePcAccess(
    const Profile& profile,
    std::span<const std::uint32_t> denied_profile_pcs,
    std::string* error_out = nullptr);

struct ProbeMetrics {
    std::string id;
    std::uint64_t hits = 0;
    std::uint64_t sampled = 0;
    std::uint64_t sample_failures = 0;
    std::uint64_t predicate_rejections = 0;
    std::uint64_t window_rejections = 0;
    std::uint64_t sampling_rejections = 0;
    std::uint64_t capture_deliveries = 0;
    std::uint64_t progress_deliveries = 0;
    std::uint64_t control_publications = 0;
    std::uint64_t dropped = 0;
    std::uint64_t progress_coalesced = 0;
    std::uint64_t bytes = 0;
    std::uint64_t traces = 0;
    std::uint64_t trace_failures = 0;
    std::uint64_t physical_watchpoint_hits = 0;
    std::uint64_t binding_generation = 0;
    std::uint64_t binding_failures = 0;
    std::uint32_t binding_failure_reason = 0;
    std::uint32_t derived_address = 0;
    std::uint32_t derived_size = 0;
    std::uint32_t physical_address = 0;
    std::uint32_t physical_size = 0;
    std::string binding_source;
};

inline constexpr std::size_t kMaxProbeRoutedHitSamples = 32;

struct ProbeRoutedHitSample {
    std::uint32_t descriptor_id = 0;
    std::uint64_t value = 0;
    bool available = false;
};

// Identity and bounded hit-time evidence supplied by the session-owned stop
// router. ProbeRuntime consumes this context passively; it never converts the
// context into a request to pause or otherwise control the guest.
struct ProbeRoutedHitContext {
    std::uint64_t routed_sequence = 0;
    std::uint64_t sample_snapshot_id = 0;
    std::uint64_t guest_workset_epoch = 0;
    std::array<ProbeRoutedHitSample, kMaxProbeRoutedHitSamples> samples{};
    std::uint8_t sample_count = 0;
    bool active_foreground_wake = false;
};

enum class ProfileStopRequirementKind : std::uint8_t {
    Pc,
    ActivationPc,
    Memory,
};

// A passive snapshot of the logical profile sites currently needed by
// ProbeRuntime. The stop router remains the only owner of physical sites.
struct ProfileStopRequirement {
    std::uint32_t probe_index = 0;
    ProfileStopRequirementKind kind = ProfileStopRequirementKind::Pc;
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    MemoryAccess memory_access = MemoryAccess::Write;
    Subscription subscriptions = Subscription::None;
    bool active = false;
    bool exhausted = false;
    bool group_enabled = true;
    bool address_resolved = false;
    std::vector<std::uint32_t> routed_sample_descriptor_ids;
};

class ProbeRuntime {
public:
    ProbeRuntime();
    ~ProbeRuntime();
    ProbeRuntime(const ProbeRuntime&) = delete;
    ProbeRuntime& operator=(const ProbeRuntime&) = delete;

    bool start(
        Core::System& system,
        Profile profile,
        SessionOptions options,
        std::string* error_out = nullptr);
    void stop();
    bool active() const { return active_.load(std::memory_order_acquire); }

    bool set_group_enabled(std::string_view group, bool enabled);
    bool replace_profile(
        Profile profile,
        std::string profile_json,
        std::string* error_out = nullptr);
    bool prepare_for_core_shutdown(std::string* error_out = nullptr);
    bool resume_after_core_boot(std::string* error_out = nullptr);
    void set_guest_workset_epoch(std::uint64_t epoch);
    void set_frame_index(std::uint64_t frame);
    bool uses_frame_clock() const { return frame_clock_active_.load(std::memory_order_acquire); }
    bool emit_marker(std::string_view id, std::uint64_t value = 0);
    bool trigger_flight_recorder(std::uint32_t post_events = 0);
    [[nodiscard]] bool requires_physical_reconcile_before_resume() const noexcept
    {
        return watchpoint_reconcile_pending_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool consume_physical_reconcile_request() noexcept
    {
        return watchpoint_reconcile_pending_.exchange(false, std::memory_order_acq_rel);
    }
    [[nodiscard]] const Profile& profile() const noexcept
    {
        return profile_;
    }

    std::vector<ProbeMetrics> metrics() const;
    bool capture_complete() const { return capture_complete_.load(std::memory_order_acquire); }
    std::string capture_incomplete_reason() const;
    std::uint64_t capture_drop_count() const { return capture_drops_.load(std::memory_order_acquire); }
    std::uint64_t progress_drop_count() const { return progress_drops_.load(std::memory_order_acquire); }

    // Passive routed profile processing. The supplied identity is retained by
    // capture/progress/control-visible events, and active_foreground_wake only
    // qualifies legacy profile `control` behavior; it grants no wake authority.
    void process_routed_pc(
        PowerPC::PowerPCManager& power_pc,
        std::uint32_t hit_pc,
        const ProbeRoutedHitContext& context);
    void process_routed_memory(
        Core::System& system,
        std::uint32_t pc,
        std::uint32_t address,
        std::uint32_t size,
        std::uint64_t value,
        bool write,
        bool post_write,
        const ProbeRoutedHitContext& context);
    [[nodiscard]] std::vector<ProfileStopRequirement>
    profile_stop_requirements() const;

private:
    struct ProbeState;
    bool validate_profile(const Profile& profile, std::string* error_out) const;
    void rebuild_dispatch_indices();
    bool arm_profile_sites(std::string* error_out);
    void restore_foreign_sites();
    bool reconcile_watchpoints(std::string* error_out = nullptr);
    void refresh_watchpoint_binding_metrics();
    bool sample_probe(
        const ProbeDefinition& probe,
        ProbeState& state,
        Core::System& system,
        std::uint32_t pc,
        std::uint32_t address,
        std::uint32_t size,
        std::uint64_t value,
        bool write,
        RawProbeEvent& event,
        const ProbeRoutedHitContext* routed_context = nullptr);
    bool evaluate_predicate(
        const ProbeDefinition& probe,
        const RawProbeEvent& event,
        const PowerPC::PowerPCManager& power_pc) const;
    bool sampling_accepts(
        const ProbeDefinition& probe,
        ProbeState& state,
        const RawProbeEvent& event);
    bool window_accepts(const ProbeDefinition& probe) const;
    void update_windows_before_probe(const ProbeDefinition& probe);
    void update_windows_after_probe(const ProbeDefinition& probe, bool accepted);
    bool enqueue_event(const ProbeDefinition& probe, const RawProbeEvent& event);
    void observe_routed_control(
        const RawProbeEvent& event,
        bool write);
    void recorder_main();
    capture_format::Event decode_event(const RawProbeEvent& raw) const;
    std::optional<std::uint32_t> evaluate_address_program(
        std::span<const std::uint8_t> program,
        Core::System& system,
        const PowerPC::PowerPCManager& power_pc,
        RawAddressTrace* trace = nullptr) const;
    bool append_address_trace(
        RawProbeEvent& event,
        std::uint16_t sample_index,
        const RawAddressTrace& trace) const;
    bool read_guest_value(
        Core::System& system,
        std::uint32_t address,
        SampleWidth width,
        std::uint64_t& value) const;
    void wake_recorder();
    void mark_capture_incomplete(const char* reason, std::uint64_t capture_sequence = 0);

    std::atomic<bool> active_{ false };
    std::atomic<bool> recorder_stop_{ false };
    std::atomic<bool> capture_enabled_{ false };
    std::atomic<bool> capture_complete_{ true };
    std::atomic<std::uint64_t> capture_drops_{ 0 };
    std::atomic<std::uint64_t> progress_drops_{ 0 };
    std::atomic<std::uint64_t> next_capture_sequence_{ 1 };
    std::atomic<std::uint64_t> next_record_sequence_{ 1 };
    std::atomic<std::uint64_t> next_snapshot_id_{ 1 };
    std::atomic<std::uint64_t> frame_index_{ 0 };
    std::atomic<bool> frame_clock_active_{ false };
    std::atomic<std::uint64_t> guest_workset_epoch_{ 0 };
    std::atomic<std::uint64_t> recorder_generation_{ 0 };
    std::atomic<bool> flight_triggered_{ false };
    std::atomic<std::uint32_t> flight_post_remaining_{ 0 };
    std::atomic<std::uint64_t> capture_gap_first_sequence_{ 0 };
    std::atomic<std::uint64_t> capture_gap_last_sequence_{ 0 };
    std::atomic<bool> watchpoints_dirty_{ false };
    std::atomic<bool> watchpoint_reconcile_pending_{ false };

    Core::System* system_ = nullptr;
    Profile profile_;
    SessionOptions options_;
    std::unique_ptr<ProbeState[]> probe_states_;
    std::unique_ptr<ProbeWindowState[]> window_states_;
    std::vector<std::uint32_t> pc_probe_indices_;
    std::vector<std::uint32_t> memory_probe_indices_;
    std::vector<std::uint32_t> activation_probe_indices_;
    BoundedEventQueue capture_queue_;
    BoundedEventQueue progress_queue_;
    std::thread recorder_thread_;
    capture_format::Writer writer_;
    std::array<char, 160> incomplete_reason_{};
    ProbeWatchpointRegistry watchpoint_registry_;
};

std::string current_module_sha256(std::string* error_out = nullptr);

} // namespace savor::probe
