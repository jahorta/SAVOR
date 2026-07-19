#pragma once

#include "ProbeEvent.h"
#include "ProbeProfile.h"
#include "ProbeWatchpointRegistry.h"
#include "ProbeWindowState.h"
#include "../SavorCaptureFormat/CaptureFormat.h"

#include <array>
#include <atomic>
#include <chrono>
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

struct TMemCheck;

namespace PowerPC {
class MMU;
class PowerPCManager;
}

namespace CPU {
class CPUManager;
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
    // activation gates. Trusted control waits are intentionally independent.
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

enum class ControlHitKind : std::uint8_t {
    None = 0,
    Pc = 1,
    Memory = 2,
    Shutdown = 3,
};

struct ControlHit {
    ControlHitKind kind = ControlHitKind::None;
    std::uint64_t generation = 0;
    std::uint64_t capture_sequence = 0;
    std::uint32_t pc = 0;
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    std::uint64_t value = 0;
    std::uint32_t control_id = 0;
    bool write = false;
};

struct ControlMemorySite {
    std::uint32_t id = 0;
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    MemoryAccess access = MemoryAccess::Write;
};

enum class ControlWaitStatus : std::uint8_t {
    Hit,
    Timeout,
    Cancelled,
    Shutdown,
};

struct ControlWaitResult {
    ControlWaitStatus status = ControlWaitStatus::Timeout;
    ControlHit hit;
};

class ProbeRuntime;

class ControlLease {
public:
    ControlLease() = default;
    ~ControlLease();
    ControlLease(const ControlLease&) = delete;
    ControlLease& operator=(const ControlLease&) = delete;
    ControlLease(ControlLease&& other) noexcept;
    ControlLease& operator=(ControlLease&& other) noexcept;

    bool active() const { return runtime_ != nullptr; }
    std::uint64_t generation() const { return generation_; }
    void reset();

private:
    friend class ProbeRuntime;
    ControlLease(ProbeRuntime* runtime, std::uint64_t generation)
        : runtime_(runtime), generation_(generation) {}
    ProbeRuntime* runtime_ = nullptr;
    std::uint64_t generation_ = 0;
};

class ProbeRuntime {
public:
    static ProbeRuntime& instance();

    bool install_native_hooks(std::string* error_out = nullptr);
    void uninstall_native_hooks();
    bool native_hooks_installed() const;

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
    void set_guest_state_epoch(std::uint64_t epoch);
    void set_frame_index(std::uint64_t frame);
    bool uses_frame_clock() const { return frame_clock_active_.load(std::memory_order_acquire); }
    bool emit_marker(std::string_view id, std::uint64_t value = 0);
    bool trigger_flight_recorder(std::uint32_t post_events = 0);

    ControlLease begin_control_wait(
        const std::vector<std::uint32_t>& pcs,
        const std::vector<ControlMemorySite>& memory_sites = {},
        std::optional<std::uint32_t> suppress_once_pc = std::nullopt,
        std::string* error_out = nullptr);
    ControlWaitResult wait_for_control(
        std::uint64_t observed_generation,
        std::chrono::milliseconds timeout) const;
    void cancel_control_wait();

    std::vector<ProbeMetrics> metrics() const;
    bool capture_complete() const { return capture_complete_.load(std::memory_order_acquire); }
    std::string capture_incomplete_reason() const;
    std::uint64_t capture_drop_count() const { return capture_drops_.load(std::memory_order_acquire); }
    std::uint64_t progress_drop_count() const { return progress_drops_.load(std::memory_order_acquire); }

    // Native-hook entry points. These execute on Dolphin's CPU thread.
    bool dispatch_pc(PowerPC::PowerPCManager& power_pc);
    void complete_pc_dispatch(bool control_will_stop);
    bool dispatch_memory(
        Core::System& system,
        std::uint32_t pc,
        std::uint32_t address,
        std::uint32_t size,
        std::uint64_t value,
        bool write,
        bool post_write);
    void observe_cpu_break(CPU::CPUManager& cpu);

    // Called by ControlLease.
    void end_control_wait(std::uint64_t lease_generation);

private:
    ProbeRuntime();
    ~ProbeRuntime();
    ProbeRuntime(const ProbeRuntime&) = delete;
    ProbeRuntime& operator=(const ProbeRuntime&) = delete;

    struct ProbeState;
    struct ForeignBreakpoint;

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
        RawProbeEvent& event);
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
    void publish_control(const RawProbeEvent& event, bool write);
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
    std::atomic<std::uint64_t> guest_state_epoch_{ 0 };
    std::atomic<std::uint64_t> recorder_generation_{ 0 };
    std::atomic<std::uint64_t> control_generation_{ 0 };
    std::atomic<bool> control_wait_active_{ false };
    std::atomic<bool> control_hit_published_{ false };
    std::atomic<std::uint32_t> control_count_{ 0 };
    std::atomic<std::uint32_t> suppress_once_pc_{ 0 };
    std::atomic<bool> control_cancelled_{ false };
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
    std::array<std::uint32_t, 512> control_pcs_{};
    std::array<ControlMemorySite, 64> control_memory_sites_{};
    std::atomic<std::uint32_t> control_memory_count_{ 0 };
    ControlHit control_hit_;
    std::array<char, 160> incomplete_reason_{};
    std::vector<ForeignBreakpoint> foreign_breakpoints_;
    std::vector<std::uint32_t> control_owned_breakpoints_;
    ProbeWatchpointRegistry watchpoint_registry_;
};

std::string current_module_sha256(std::string* error_out = nullptr);

} // namespace savor::probe
