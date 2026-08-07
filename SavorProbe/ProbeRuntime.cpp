#include "ProbeRuntime.h"
#include "AddressProgramEvaluator.h"
#include "CaptureRetention.h"
#include "ProbeDispatchPolicy.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <new>
#include <ranges>
#include <sstream>
#include <type_traits>
#include <unordered_set>

#include <Windows.h>
#include <bcrypt.h>
#include <xxhash.h>

#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#pragma comment(lib, "bcrypt.lib")

namespace savor::probe {

namespace {

std::uint64_t monotonic_now_ns()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t routed_or_local_sequence(
    std::atomic<std::uint64_t>& next_sequence,
    std::uint64_t routed_sequence)
{
    if (routed_sequence == 0)
        return next_sequence.fetch_add(1, std::memory_order_relaxed);

    auto next = next_sequence.load(std::memory_order_relaxed);
    while (next <= routed_sequence
        && !next_sequence.compare_exchange_weak(
            next,
            routed_sequence + 1,
            std::memory_order_relaxed)) {
    }
    return routed_sequence;
}

bool ranges_overlap(
    std::uint32_t lhs_address,
    std::uint32_t lhs_size,
    std::uint32_t rhs_address,
    std::uint32_t rhs_size)
{
    if (lhs_size == 0 || rhs_size == 0)
        return false;
    const auto lhs_end = static_cast<std::uint64_t>(lhs_address) + lhs_size;
    const auto rhs_end = static_cast<std::uint64_t>(rhs_address) + rhs_size;
    return lhs_address < rhs_end && rhs_address < lhs_end;
}

bool memory_access_matches(MemoryAccess access, bool write)
{
    return access == MemoryAccess::Access
        || (write && access == MemoryAccess::Write)
        || (!write && access == MemoryAccess::Read);
}

template <typename T>
bool payload_append(RawProbeEvent& event, T value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    if (event.payload_size > event.payload.size()
        || event.payload.size() - event.payload_size < sizeof(T)) {
        return false;
    }
    std::memcpy(event.payload.data() + event.payload_size, &value, sizeof(T));
    event.payload_size += static_cast<std::uint16_t>(sizeof(T));
    return true;
}

std::uint64_t raw_event_hash(const RawProbeEvent& event)
{
    std::uint64_t hash = XXH64(event.fields.data(), event.field_count * sizeof(RawField), 0);
    return XXH64(event.payload.data(), event.payload_size, hash);
}

} // namespace

struct ProbeRuntime::ProbeState {
    std::atomic<std::uint64_t> hits{ 0 };
    std::atomic<std::uint64_t> sampled{ 0 };
    std::atomic<std::uint64_t> sample_failures{ 0 };
    std::atomic<std::uint64_t> predicate_rejections{ 0 };
    std::atomic<std::uint64_t> window_rejections{ 0 };
    std::atomic<std::uint64_t> sampling_rejections{ 0 };
    std::atomic<std::uint64_t> capture_deliveries{ 0 };
    std::atomic<std::uint64_t> progress_deliveries{ 0 };
    std::atomic<std::uint64_t> control_publications{ 0 };
    std::atomic<std::uint64_t> dropped{ 0 };
    std::atomic<std::uint64_t> progress_coalesced{ 0 };
    std::atomic<std::uint64_t> bytes{ 0 };
    std::atomic<std::uint64_t> traces{ 0 };
    std::atomic<std::uint64_t> trace_failures{ 0 };
    std::atomic<std::uint64_t> physical_watchpoint_hits{ 0 };
    std::atomic<std::uint64_t> binding_generation{ 0 };
    std::atomic<std::uint64_t> binding_failures{ 0 };
    std::atomic<std::uint32_t> binding_failure_reason{ 0 };
    std::atomic<std::uint32_t> derived_address{ 0 };
    std::atomic<std::uint32_t> derived_size{ 0 };
    std::atomic<std::uint32_t> physical_address{ 0 };
    std::atomic<std::uint32_t> physical_size{ 0 };
    std::atomic<std::uint8_t> binding_source{ 0 };
    std::atomic<bool> active{ true };
    std::atomic<bool> group_enabled{ true };
    std::atomic<bool> exhausted{ false };
    std::atomic<bool> address_resolved{ false };
    std::uint64_t last_hash = 0;
    std::uint64_t lease_hits = 0;
    bool has_last_hash = false;
    std::uint32_t effective_address = 0;
    RawAddressTrace root_trace{};
    bool root_trace_available = false;
    std::unique_ptr<LatestRawProbeEvent> coalesced_progress;
};

ProbeRuntime::ProbeRuntime() = default;

ProbeRuntime::~ProbeRuntime()
{
    stop();
}

bool ValidateProfilePcAccess(
    const Profile& profile,
    std::span<const std::uint32_t> denied_profile_pcs,
    std::string* error_out)
{
    if (denied_profile_pcs.empty())
        return true;

    const std::unordered_set<std::uint32_t> denied{
        denied_profile_pcs.begin(), denied_profile_pcs.end() };
    for (const auto& probe : profile.probes) {
        const bool denied_pc_probe = probe.kind == ProbeKind::Pc
            && denied.contains(probe.address);
        const bool denied_activation = probe.activate_on_pc.has_value()
            && denied.contains(*probe.activate_on_pc);
        if (denied_pc_probe || denied_activation) {
            if (error_out)
                *error_out = "capture profile references a reserved runtime breakpoint";
            return false;
        }
    }
    return true;
}

bool ProbeRuntime::validate_profile(const Profile& profile, std::string* error_out) const
{
    if (profile.schema != "savor.capture.profile/1") {
        if (error_out) *error_out = "unsupported probe profile schema " + profile.schema;
        return false;
    }
    if (profile.probes.size() > 4096) {
        if (error_out) *error_out = "probe profile exceeds 4096 probes";
        return false;
    }
    if (profile.expected_module_sha256.size() != 64) {
        if (error_out) *error_out = "effective probe profile requires a 64-character module hash";
        return false;
    }
    std::unordered_set<std::string> ids;
    for (const auto& probe : profile.probes) {
        if (probe.id.empty() || !ids.insert(probe.id).second) {
            if (error_out) *error_out = "probe ids must be nonempty and unique";
            return false;
        }
        if (probe.predicate.size() > 64 || probe.samples.size() > kMaxRawFields) {
            if (error_out) *error_out = "probe '" + probe.id + "' exceeds a bounded runtime limit";
            return false;
        }
    }
    return true;
}

bool ProbeRuntime::start(
    Core::System& system,
    Profile profile,
    SessionOptions options,
    std::string* error_out)
{
    stop();
    if (!validate_profile(profile, error_out))
        return false;
    if (!ValidateProfilePcAccess(profile, options.denied_profile_pcs, error_out))
        return false;
    std::string hash_error;
    const auto current_hash = current_module_sha256(&hash_error);
    if (current_hash.empty() || current_hash != profile.expected_module_sha256) {
        if (error_out) {
            *error_out = current_hash.empty()
                ? hash_error
                : "host module SHA-256 does not match capture profile";
        }
        return false;
    }

    const auto max_by_bytes = static_cast<std::uint32_t>(std::max<std::uint64_t>(
        16, profile.limits.queue_bytes / sizeof(RawProbeEvent)));
    const auto capture_capacity = std::clamp(
        std::min(profile.limits.max_events, max_by_bytes), 16u, 4096u);
    if (!capture_queue_.configure(capture_capacity)
        || !progress_queue_.configure(std::clamp(profile.limits.progress_events, 16u, 4096u))) {
        if (error_out) *error_out = "failed allocating probe event queues";
        return false;
    }

    system_ = &system;
    profile_ = std::move(profile);
    rebuild_dispatch_indices();
    options_ = std::move(options);
    options_.metadata.dolphin_source_commit = std::string(kSupportedDolphinSourceCommit);
    probe_states_ = std::make_unique<ProbeState[]>(profile_.probes.size());
    window_states_ = std::make_unique<ProbeWindowState[]>(profile_.windows.size());
    watchpoint_registry_.reserve(profile_.probes.size());
    for (std::size_t i = 0; i < profile_.probes.size(); ++i) {
        probe_states_[i].active = !profile_.probes[i].activate_on_pc.has_value();
        probe_states_[i].effective_address = profile_.probes[i].address;
        probe_states_[i].address_resolved = profile_.probes[i].kind == ProbeKind::Memory
            && !profile_.probes[i].activate_on_pc.has_value();
        if (has_subscription(profile_.probes[i].subscriptions, Subscription::Progress)) {
            probe_states_[i].coalesced_progress = std::make_unique<LatestRawProbeEvent>();
            if (!probe_states_[i].coalesced_progress->configure()) {
                if (error_out) *error_out = "failed allocating progress coalescing slots";
                system_ = nullptr;
                return false;
            }
        }
    }
    for (std::size_t i = 0; i < profile_.windows.size(); ++i)
        window_states_[i].reset(profile_.windows[i].initially_open);

    capture_drops_.store(0, std::memory_order_relaxed);
    progress_drops_.store(0, std::memory_order_relaxed);
    next_capture_sequence_.store(1, std::memory_order_relaxed);
    next_record_sequence_.store(1, std::memory_order_relaxed);
    next_snapshot_id_.store(1, std::memory_order_relaxed);
    frame_index_.store(0, std::memory_order_relaxed);
    frame_clock_active_.store(std::ranges::any_of(profile_.probes, [](const auto& probe) {
        return probe.kind == ProbeKind::Pc && probe.frame_clock;
    }), std::memory_order_relaxed);
    guest_workset_epoch_.store(0, std::memory_order_relaxed);
    flight_triggered_.store(false, std::memory_order_relaxed);
    flight_post_remaining_.store(0, std::memory_order_relaxed);
    capture_gap_first_sequence_.store(0, std::memory_order_relaxed);
    capture_gap_last_sequence_.store(0, std::memory_order_relaxed);
    watchpoints_dirty_.store(false, std::memory_order_relaxed);
    watchpoint_reconcile_pending_.store(false, std::memory_order_relaxed);
    incomplete_reason_.fill('\0');
    capture_complete_.store(true, std::memory_order_relaxed);
    capture_enabled_.store(!options_.capture_path.empty(), std::memory_order_relaxed);

    if (!options_.capture_path.empty()
        && !writer_.open(options_.capture_path, options_.metadata, options_.writer_options, error_out)) {
        system_ = nullptr;
        return false;
    }
    if (!arm_profile_sites(error_out)) {
        restore_foreign_sites();
        if (writer_.is_open())
            writer_.close(nullptr);
        system_ = nullptr;
        return false;
    }

    recorder_stop_.store(false, std::memory_order_release);
    active_.store(true, std::memory_order_release);
    recorder_thread_ = std::thread([this] { recorder_main(); });
    return true;
}

void ProbeRuntime::stop()
{
    if (!active_.exchange(false, std::memory_order_acq_rel) && !recorder_thread_.joinable())
        return;
    restore_foreign_sites();
    recorder_stop_.store(true, std::memory_order_release);
    wake_recorder();
    if (recorder_thread_.joinable())
        recorder_thread_.join();
    if (writer_.is_open()) {
        if (!capture_complete_.load(std::memory_order_acquire))
            writer_.mark_incomplete(capture_incomplete_reason());
        writer_.close(nullptr);
    }
    system_ = nullptr;
    profile_ = {};
    options_ = {};
    probe_states_.reset();
    window_states_.reset();
    pc_probe_indices_.clear();
    memory_probe_indices_.clear();
    activation_probe_indices_.clear();
}

void ProbeRuntime::rebuild_dispatch_indices()
{
    pc_probe_indices_.clear();
    memory_probe_indices_.clear();
    activation_probe_indices_.clear();
    pc_probe_indices_.reserve(profile_.probes.size());
    memory_probe_indices_.reserve(profile_.probes.size());
    activation_probe_indices_.reserve(profile_.probes.size());
    for (std::uint32_t i = 0; i < profile_.probes.size(); ++i) {
        const auto& probe = profile_.probes[i];
        if (probe.kind == ProbeKind::Pc)
            pc_probe_indices_.push_back(i);
        if (probe.kind == ProbeKind::Memory)
            memory_probe_indices_.push_back(i);
        if (probe.activate_on_pc.has_value())
            activation_probe_indices_.push_back(i);
    }
}

bool ProbeRuntime::arm_profile_sites(std::string* error_out)
{
    if (!system_)
        return false;

    watchpoint_registry_.clear_requests();
    for (std::uint32_t i = 0; i < profile_.probes.size(); ++i) {
        const auto& probe = profile_.probes[i];
        const auto& state = probe_states_[i];
        if (probe.kind != ProbeKind::Memory || !state.active || !state.group_enabled
            || state.exhausted || !state.address_resolved) {
            continue;
        }
        if (!watchpoint_registry_.upsert(WatchpointRequest{
                WatchpointOwner{ WatchpointOwnerKind::Profile, i },
                state.effective_address,
                probe.size,
                probe.memory_access,
                probe.activate_on_pc.has_value()
                    ? WatchpointBindingSource::Dynamic
                    : WatchpointBindingSource::Static,
            })) {
            if (error_out)
                *error_out = "invalid profile watchpoint range for probe '" + probe.id + "'";
            return false;
        }
    }
    return reconcile_watchpoints(error_out);
}

void ProbeRuntime::restore_foreign_sites()
{
    watchpoint_registry_.release_all();
    watchpoints_dirty_.store(false, std::memory_order_release);
    watchpoint_reconcile_pending_.store(false, std::memory_order_release);
}

bool ProbeRuntime::reconcile_watchpoints(std::string* error_out)
{
    if (!system_)
        return false;
    const bool ok = watchpoint_registry_.reconcile({}, error_out);
    for (const auto& failure : watchpoint_registry_.failures()) {
        if (failure.owner.kind == WatchpointOwnerKind::Profile
            && failure.owner.index < profile_.probes.size()) {
            probe_states_[failure.owner.index].binding_failure_reason.store(
                static_cast<std::uint32_t>(failure.reason), std::memory_order_relaxed);
        }
    }
    refresh_watchpoint_binding_metrics();
    watchpoints_dirty_.store(false, std::memory_order_release);
    watchpoint_reconcile_pending_.store(false, std::memory_order_release);
    return ok;
}

void ProbeRuntime::refresh_watchpoint_binding_metrics()
{
    for (std::uint32_t i = 0; i < profile_.probes.size(); ++i) {
        const auto& probe = profile_.probes[i];
        auto& state = probe_states_[i];
        if (probe.kind != ProbeKind::Memory)
            continue;
        state.derived_address.store(
            state.address_resolved ? state.effective_address : 0,
            std::memory_order_relaxed);
        state.derived_size.store(
            state.address_resolved ? probe.size : 0,
            std::memory_order_relaxed);
        const auto binding = watchpoint_registry_.binding_for(
            WatchpointOwner{ WatchpointOwnerKind::Profile, i });
        if (!binding.has_value()) {
            state.physical_address.store(0, std::memory_order_relaxed);
            state.physical_size.store(0, std::memory_order_relaxed);
            state.binding_source.store(0, std::memory_order_relaxed);
            continue;
        }
        state.binding_generation.store(binding->binding_generation, std::memory_order_relaxed);
        state.physical_address.store(binding->physical_start, std::memory_order_relaxed);
        state.physical_size.store(
            binding->physical_end - binding->physical_start + 1,
            std::memory_order_relaxed);
        state.binding_source.store(
            static_cast<std::uint8_t>(binding->source), std::memory_order_relaxed);
    }
}

bool ProbeRuntime::set_group_enabled(std::string_view group, bool enabled)
{
    if (!active())
        return false;
    bool found = false;
    for (std::size_t i = 0; i < profile_.probes.size(); ++i) {
        if (profile_.probes[i].group == group) {
            probe_states_[i].group_enabled = enabled;
            if (profile_.probes[i].kind == ProbeKind::Memory) {
                const WatchpointOwner owner{ WatchpointOwnerKind::Profile,
                    static_cast<std::uint32_t>(i) };
                if (enabled && probe_states_[i].active && !probe_states_[i].exhausted
                    && probe_states_[i].address_resolved) {
                    watchpoint_registry_.upsert(WatchpointRequest{
                        owner,
                        probe_states_[i].effective_address,
                        profile_.probes[i].size,
                        profile_.probes[i].memory_access,
                        profile_.probes[i].activate_on_pc.has_value()
                            ? WatchpointBindingSource::Dynamic
                            : WatchpointBindingSource::Static,
                    });
                } else {
                    watchpoint_registry_.release(owner);
                }
            }
            found = true;
        }
    }
    if (found) {
        std::string error;
        if (!reconcile_watchpoints(&error))
            mark_capture_incomplete("watchpoint group rebind failed");
        watchpoint_reconcile_pending_.store(true, std::memory_order_release);
    }
    return found;
}

bool ProbeRuntime::replace_profile(
    Profile profile,
    std::string profile_json,
    std::string* error_out)
{
    if (!validate_profile(profile, error_out))
        return false;
    if (!active() || !system_) {
        if (error_out) *error_out = "probe runtime is not started";
        return false;
    }
    if (Core::GetState(*system_) != Core::State::Paused) {
        if (error_out) *error_out = "capture profile reload requires a paused CPU";
        return false;
    }
    if (profile.revision <= profile_.revision) {
        if (error_out) *error_out = "capture profile reload revision must increase";
        return false;
    }
    if (!ValidateProfilePcAccess(profile, options_.denied_profile_pcs, error_out))
        return false;
    std::string hash_error;
    const auto module_hash = current_module_sha256(&hash_error);
    if (module_hash.empty() || module_hash != profile.expected_module_sha256) {
        if (error_out) *error_out = module_hash.empty()
            ? hash_error
            : "reloaded profile module SHA-256 does not match";
        return false;
    }

    std::unique_ptr<ProbeState[]> new_probe_states;
    std::unique_ptr<ProbeWindowState[]> new_window_states;
    try {
        new_probe_states = std::make_unique<ProbeState[]>(profile.probes.size());
        new_window_states = std::make_unique<ProbeWindowState[]>(profile.windows.size());
    } catch (const std::bad_alloc&) {
        if (error_out) *error_out = "failed allocating capture profile reload state";
        return false;
    }
    for (std::size_t i = 0; i < profile.probes.size(); ++i) {
        new_probe_states[i].active = !profile.probes[i].activate_on_pc.has_value();
        new_probe_states[i].effective_address = profile.probes[i].address;
        new_probe_states[i].address_resolved = profile.probes[i].kind == ProbeKind::Memory
            && !profile.probes[i].activate_on_pc.has_value();
        if (has_subscription(profile.probes[i].subscriptions, Subscription::Progress)) {
            try {
                new_probe_states[i].coalesced_progress =
                    std::make_unique<LatestRawProbeEvent>();
            } catch (const std::bad_alloc&) {
                if (error_out) *error_out = "failed allocating progress coalescing state";
                return false;
            }
            if (!new_probe_states[i].coalesced_progress->configure()) {
                if (error_out) *error_out = "failed allocating progress coalescing slots";
                return false;
            }
        }
    }
    for (std::size_t i = 0; i < profile.windows.size(); ++i)
        new_window_states[i].reset(profile.windows[i].initially_open);

    emit_marker("profile.reload.begin", profile.revision);
    active_.store(false, std::memory_order_release);
    restore_foreign_sites();
    recorder_stop_.store(true, std::memory_order_release);
    wake_recorder();
    if (recorder_thread_.joinable())
        recorder_thread_.join();

    profile_ = std::move(profile);
    rebuild_dispatch_indices();
    watchpoint_registry_.reserve(profile_.probes.size());
    options_.metadata.profile_json = std::move(profile_json);
    probe_states_ = std::move(new_probe_states);
    window_states_ = std::move(new_window_states);
    frame_clock_active_.store(std::ranges::any_of(profile_.probes, [](const auto& probe) {
        return probe.kind == ProbeKind::Pc && probe.frame_clock;
    }), std::memory_order_release);
    flight_triggered_.store(false, std::memory_order_release);
    flight_post_remaining_.store(0, std::memory_order_release);

    if (!arm_profile_sites(error_out)) {
        mark_capture_incomplete("capture profile reload arm failed");
        if (writer_.is_open())
            writer_.close(nullptr);
        system_ = nullptr;
        return false;
    }
    watchpoint_reconcile_pending_.store(true, std::memory_order_release);

    if (writer_.is_open()) {
        capture_format::Event event;
        event.capture_sequence = next_capture_sequence_.fetch_add(1, std::memory_order_relaxed);
        event.record_sequence = next_record_sequence_.fetch_add(1, std::memory_order_relaxed);
        event.monotonic_ns = monotonic_now_ns();
        event.frame_index = frame_index_.load(std::memory_order_relaxed);
        event.guest_workset_epoch = guest_workset_epoch_.load(std::memory_order_relaxed);
        event.profile_revision = profile_.revision;
        event.snapshot_id = next_snapshot_id_.fetch_add(1, std::memory_order_relaxed);
        event.kind = capture_format::EventKind::Marker;
        event.probe_id = "profile.reload";
        capture_format::Field definition;
        definition.name = "profile_json";
        definition.type = capture_format::FieldType::Text;
        definition.bytes.assign(
            options_.metadata.profile_json.begin(), options_.metadata.profile_json.end());
        event.fields.push_back(std::move(definition));
        std::string append_error;
        if (!writer_.append(event, &append_error)) {
            mark_capture_incomplete("capture profile reload record failed");
            if (error_out) *error_out = append_error;
            writer_.close(nullptr);
            system_ = nullptr;
            return false;
        }
    }

    recorder_stop_.store(false, std::memory_order_release);
    active_.store(true, std::memory_order_release);
    recorder_thread_ = std::thread([this] { recorder_main(); });
    emit_marker("profile.reload.end", profile_.revision);
    return true;
}

bool ProbeRuntime::prepare_for_core_shutdown(std::string*)
{
    if (!active())
        return true;
    restore_foreign_sites();
    for (std::size_t i = 0; i < profile_.probes.size(); ++i) {
        auto& state = probe_states_[i];
        state.active = !profile_.probes[i].activate_on_pc.has_value();
        state.effective_address = profile_.probes[i].address;
        state.address_resolved = profile_.probes[i].kind == ProbeKind::Memory
            && !profile_.probes[i].activate_on_pc.has_value();
        state.exhausted = false;
        state.has_last_hash = false;
    }
    return true;
}

bool ProbeRuntime::resume_after_core_boot(std::string* error_out)
{
    if (!active())
        return true;
    const bool armed = arm_profile_sites(error_out);
    if (armed)
        watchpoint_reconcile_pending_.store(true, std::memory_order_release);
    return armed;
}

void ProbeRuntime::set_guest_workset_epoch(std::uint64_t epoch)
{
    guest_workset_epoch_.store(epoch, std::memory_order_release);
}

std::vector<ProfileStopRequirement>
ProbeRuntime::profile_stop_requirements() const
{
    std::vector<ProfileStopRequirement> requirements;
    if (!probe_states_)
        return requirements;

    requirements.reserve(
        profile_.probes.size() + activation_probe_indices_.size());
    for (std::uint32_t i = 0; i < profile_.probes.size(); ++i) {
        const auto& probe = profile_.probes[i];
        const auto& state = probe_states_[i];
        if (probe.activate_on_pc.has_value()) {
            requirements.push_back(ProfileStopRequirement{
                .probe_index = i,
                .kind = ProfileStopRequirementKind::ActivationPc,
                .address = *probe.activate_on_pc,
                .memory_access = probe.memory_access,
                .subscriptions = probe.subscriptions,
                .active = true,
                .exhausted = false,
                .group_enabled = state.group_enabled,
                .address_resolved = true,
            });
        }
        if (probe.kind == ProbeKind::Pc) {
            requirements.push_back(ProfileStopRequirement{
                .probe_index = i,
                .kind = ProfileStopRequirementKind::Pc,
                .address = probe.address,
                .memory_access = probe.memory_access,
                .subscriptions = probe.subscriptions,
                .active = state.active,
                .exhausted = state.exhausted,
                .group_enabled = state.group_enabled,
                .address_resolved = true,
            });
        } else if (probe.kind == ProbeKind::Memory) {
            requirements.push_back(ProfileStopRequirement{
                .probe_index = i,
                .kind = ProfileStopRequirementKind::Memory,
                .address = state.effective_address,
                .size = probe.size,
                .memory_access = probe.memory_access,
                .subscriptions = probe.subscriptions,
                .active = state.active,
                .exhausted = state.exhausted,
                .group_enabled = state.group_enabled,
                .address_resolved = state.address_resolved,
            });
        }
    }
    return requirements;
}

void ProbeRuntime::set_frame_index(std::uint64_t frame)
{
    frame_index_.store(frame, std::memory_order_release);
    for (std::size_t i = 0; i < profile_.windows.size(); ++i) {
        const auto& definition = profile_.windows[i];
        window_states_[i].on_frame(definition, frame);
    }
}

bool ProbeRuntime::emit_marker(std::string_view id, std::uint64_t value)
{
    if (!active() || id.size() > kRawPayloadBytes)
        return false;
    RawProbeEvent event{};
    event.capture_sequence = next_capture_sequence_.fetch_add(1, std::memory_order_relaxed);
    event.monotonic_ns = monotonic_now_ns();
    event.frame_index = frame_index_.load(std::memory_order_relaxed);
    event.guest_workset_epoch = guest_workset_epoch_.load(std::memory_order_relaxed);
    event.profile_revision = profile_.revision;
    event.snapshot_id = next_snapshot_id_.fetch_add(1, std::memory_order_relaxed);
    event.probe_index = kSyntheticMarkerProbeIndex;
    event.kind = static_cast<std::uint8_t>(ProbeKind::Marker);
    event.value = value;
    event.payload_size = static_cast<std::uint16_t>(id.size());
    std::memcpy(event.payload.data(), id.data(), id.size());
    for (std::size_t i = 0; i < profile_.windows.size(); ++i) {
        const auto& definition = profile_.windows[i];
        window_states_[i].on_marker(definition, id, event.frame_index);
    }
    if (capture_enabled_.load(std::memory_order_relaxed) && !capture_queue_.try_push(event)) {
        mark_capture_incomplete("capture queue overflow");
        return false;
    }
    wake_recorder();
    return true;
}

bool ProbeRuntime::trigger_flight_recorder(std::uint32_t post_events)
{
    flight_post_remaining_.store(post_events, std::memory_order_release);
    flight_triggered_.store(true, std::memory_order_release);
    wake_recorder();
    return active();
}

void ProbeRuntime::process_routed_pc(
    PowerPC::PowerPCManager& power_pc,
    std::uint32_t hit_pc,
    const ProbeRoutedHitContext& context)
{
    if (!active_.load(std::memory_order_acquire))
        return;
    guest_workset_epoch_.store(
        context.guest_workset_epoch,
        std::memory_order_release);
    if (watchpoints_dirty_.load(std::memory_order_acquire))
        watchpoint_reconcile_pending_.store(true, std::memory_order_release);
    const auto pc = hit_pc;
    const auto capture_sequence = routed_or_local_sequence(
        next_capture_sequence_,
        context.routed_sequence);
    const auto snapshot_id = routed_or_local_sequence(
        next_snapshot_id_,
        context.sample_snapshot_id);
    const auto stamp_routed_identity = [&](RawProbeEvent& event) {
        event.capture_sequence = capture_sequence;
        event.snapshot_id = snapshot_id;
        event.guest_workset_epoch = context.guest_workset_epoch;
    };
    bool frame_clock_hit = false;
    for (const auto i : pc_probe_indices_) {
        const auto& probe = profile_.probes[i];
        const auto& state = probe_states_[i];
        if (probe.kind == ProbeKind::Pc && probe.frame_clock && probe.address == pc
            && state.active && state.group_enabled && !state.exhausted) {
            frame_clock_hit = true;
            break;
        }
    }
    if (frame_clock_hit) {
        const auto frame = frame_index_.fetch_add(1, std::memory_order_acq_rel) + 1;
        set_frame_index(frame);
        if (capture_enabled_.load(std::memory_order_relaxed)) {
            RawProbeEvent marker{};
            marker.capture_sequence = capture_sequence;
            marker.monotonic_ns = monotonic_now_ns();
            marker.frame_index = frame;
            marker.guest_workset_epoch = context.guest_workset_epoch;
            marker.profile_revision = profile_.revision;
            marker.snapshot_id = snapshot_id;
            marker.probe_index = kSyntheticMarkerProbeIndex;
            marker.kind = static_cast<std::uint8_t>(ProbeKind::Marker);
            constexpr std::string_view id = "frame.end";
            marker.payload_size = static_cast<std::uint16_t>(id.size());
            std::memcpy(marker.payload.data(), id.data(), id.size());
            if (!capture_queue_.try_push(marker))
                mark_capture_incomplete("capture queue overflow");
            else
                wake_recorder();
        }
    }
    bool requested_control = false;
    RawProbeEvent control_event{};
    const bool active_foreground_wake =
        context.active_foreground_wake;

    bool watchpoint_binding_changed = false;
    for (const auto i : activation_probe_indices_) {
        const auto& probe = profile_.probes[i];
        auto& state = probe_states_[i];
        if (probe.activate_on_pc.has_value() && *probe.activate_on_pc == pc) {
            state.active = true;
            state.exhausted = false;
            state.lease_hits = 0;
            if (probe.kind == ProbeKind::Memory && !probe.address_program.empty()) {
                RawAddressTrace trace;
                const auto address = evaluate_address_program(
                    probe.address_program, *system_, power_pc, &trace);
                state.address_resolved = address.has_value();
                state.effective_address = address.value_or(0);
                state.root_trace = trace;
                state.root_trace_available = probe.address_trace == AddressTracePolicy::Always
                    || (probe.address_trace == AddressTracePolicy::OnFailure && !trace.success);
                if (state.root_trace_available) {
                    RawProbeEvent resolution{};
                    resolution.capture_sequence = capture_sequence;
                    resolution.monotonic_ns = monotonic_now_ns();
                    resolution.frame_index = frame_index_.load(std::memory_order_relaxed);
                    resolution.guest_workset_epoch = context.guest_workset_epoch;
                    resolution.profile_revision = profile_.revision;
                    resolution.snapshot_id = snapshot_id;
                    resolution.probe_index = i;
                    resolution.pc = pc;
                    resolution.address = state.effective_address;
                    resolution.kind = static_cast<std::uint8_t>(ProbeKind::Marker);
                    resolution.flags = kRawEventAddressResolution;
                    if (append_address_trace(resolution, UINT16_MAX, trace)
                        && capture_enabled_.load(std::memory_order_relaxed)
                        && has_subscription(probe.subscriptions, Subscription::Capture)) {
                        if (!capture_queue_.try_push(resolution))
                            mark_capture_incomplete("capture queue overflow", resolution.capture_sequence);
                        else
                            wake_recorder();
                    }
                    state.traces.fetch_add(1, std::memory_order_relaxed);
                    if (!trace.success)
                        state.trace_failures.fetch_add(1, std::memory_order_relaxed);
                    state.root_trace_available = false;
                }
                if (!address.has_value()) {
                    state.active = false;
                    state.binding_failures.fetch_add(1, std::memory_order_relaxed);
                    state.binding_failure_reason.store(
                        static_cast<std::uint32_t>(
                            WatchpointBindingFailureReason::AddressDerivationFailed),
                        std::memory_order_relaxed);
                    watchpoint_registry_.release(
                        WatchpointOwner{ WatchpointOwnerKind::Profile, i });
                    watchpoint_binding_changed = true;
                    mark_capture_incomplete("dynamic watchpoint address derivation failed");
                }
            } else if (probe.kind == ProbeKind::Memory) {
                state.address_resolved = true;
                state.effective_address = probe.address;
            }
            if (probe.kind == ProbeKind::Memory && state.address_resolved
                && state.group_enabled) {
                if (!watchpoint_registry_.upsert(WatchpointRequest{
                        WatchpointOwner{ WatchpointOwnerKind::Profile, i },
                        state.effective_address,
                        probe.size,
                        probe.memory_access,
                        WatchpointBindingSource::Dynamic,
                    })) {
                    state.active = false;
                    state.address_resolved = false;
                    watchpoint_registry_.release(
                        WatchpointOwner{ WatchpointOwnerKind::Profile, i });
                    state.binding_failures.fetch_add(1, std::memory_order_relaxed);
                    state.binding_failure_reason.store(
                        static_cast<std::uint32_t>(WatchpointBindingFailureReason::InvalidRange),
                        std::memory_order_relaxed);
                    mark_capture_incomplete("dynamic watchpoint range is invalid");
                }
                watchpoint_binding_changed = true;
            }
        }
    }
    if (watchpoint_binding_changed) {
        watchpoints_dirty_.store(true, std::memory_order_release);
        watchpoint_reconcile_pending_.store(true, std::memory_order_release);
    }
    for (const auto i : pc_probe_indices_) {
        const auto& probe = profile_.probes[i];
        auto& state = probe_states_[i];
        if (probe.kind != ProbeKind::Pc || probe.address != pc || !state.active
            || !state.group_enabled || state.exhausted) {
            continue;
        }
        const auto hit_count = state.hits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (probe.max_hits.has_value() && hit_count > *probe.max_hits) {
            state.exhausted = true;
            watchpoints_dirty_.store(true, std::memory_order_release);
            watchpoint_reconcile_pending_.store(true, std::memory_order_release);
            continue;
        }
        if (probe.max_hits.has_value() && hit_count == *probe.max_hits) {
            state.exhausted = true;
            watchpoints_dirty_.store(true, std::memory_order_release);
            watchpoint_reconcile_pending_.store(true, std::memory_order_release);
        }
        update_windows_before_probe(probe);
        RawProbeEvent event{};
        if (!sample_probe(probe, state, *system_, pc, 0, 0, 0, false, event)) {
            state.sample_failures.fetch_add(1, std::memory_order_relaxed);
            update_windows_after_probe(probe, false);
            continue;
        }
        event.probe_index = static_cast<std::uint32_t>(i);
        stamp_routed_identity(event);
        event.hit_count = hit_count;
        if (!evaluate_predicate(probe, event, power_pc)) {
            state.predicate_rejections.fetch_add(1, std::memory_order_relaxed);
            update_windows_after_probe(probe, false);
            continue;
        }
        if (!window_accepts(probe)) {
            state.window_rejections.fetch_add(1, std::memory_order_relaxed);
            update_windows_after_probe(probe, false);
            continue;
        }
        if (!sampling_accepts(probe, state, event)) {
            state.sampling_rejections.fetch_add(1, std::memory_order_relaxed);
            update_windows_after_probe(probe, false);
            continue;
        }
        state.sampled.fetch_add(1, std::memory_order_relaxed);
        const bool profile_control = subscriber_dispatch_decision(
            probe.subscriptions,
            capture_enabled_.load(std::memory_order_relaxed),
            active_foreground_wake,
            requested_control).control;
        if (profile_control)
            event.flags |= kRawEventControlPublished;
        enqueue_event(probe, event);
        if (profile_control) {
            requested_control = true;
            control_event = event;
        }
        update_windows_after_probe(probe, true);
        if (probe.one_shot) {
            state.exhausted = true;
            watchpoints_dirty_.store(true, std::memory_order_release);
            watchpoint_reconcile_pending_.store(true, std::memory_order_release);
        }
    }
    if (active_foreground_wake) {
        if (!requested_control) {
            requested_control = true;
            control_event.capture_sequence = capture_sequence;
            control_event.monotonic_ns = monotonic_now_ns();
            control_event.frame_index = frame_index_.load(std::memory_order_relaxed);
            control_event.guest_workset_epoch = context.guest_workset_epoch;
            control_event.profile_revision = profile_.revision;
            control_event.snapshot_id = snapshot_id;
            control_event.probe_index = kSyntheticControlProbeIndex;
            control_event.pc = pc;
            control_event.kind = static_cast<std::uint8_t>(ProbeKind::Pc);
            control_event.flags |= kRawEventControlPublished;
        }
    }
    if (requested_control)
        observe_routed_control(control_event, false);
}

void ProbeRuntime::process_routed_memory(
    Core::System& system,
    std::uint32_t pc,
    std::uint32_t address,
    std::uint32_t size,
    std::uint64_t value,
    bool write,
    bool post_write,
    const ProbeRoutedHitContext& context)
{
    if (!active_.load(std::memory_order_acquire) || (write && !post_write))
        return;
    guest_workset_epoch_.store(
        context.guest_workset_epoch,
        std::memory_order_release);
    if (memory_probe_indices_.empty()
        && !context.active_foreground_wake) {
        return;
    }
    const auto capture_sequence = routed_or_local_sequence(
        next_capture_sequence_,
        context.routed_sequence);
    const auto snapshot_id = routed_or_local_sequence(
        next_snapshot_id_,
        context.sample_snapshot_id);
    const auto stamp_routed_identity = [&](RawProbeEvent& event) {
        event.capture_sequence = capture_sequence;
        event.snapshot_id = snapshot_id;
        event.guest_workset_epoch = context.guest_workset_epoch;
    };
    bool requested_control = false;
    RawProbeEvent control_event{};
    const bool active_foreground_wake =
        context.active_foreground_wake;
    const auto& power_pc = system.GetPowerPC();
    for (const auto i : memory_probe_indices_) {
        const auto& probe = profile_.probes[i];
        auto& state = probe_states_[i];
        if (probe.kind != ProbeKind::Memory || !state.active || !state.group_enabled
            || !state.address_resolved || !memory_access_matches(probe.memory_access, write)
            || !ranges_overlap(state.effective_address, probe.size, address, size)) {
            continue;
        }
        state.physical_watchpoint_hits.fetch_add(1, std::memory_order_relaxed);
        if (state.exhausted)
            continue;
        const auto hit_count = state.hits.fetch_add(1, std::memory_order_relaxed) + 1;
        const auto lease_hit_count = ++state.lease_hits;
        if (probe.max_hits.has_value() && lease_hit_count > *probe.max_hits) {
            state.exhausted = true;
            watchpoint_registry_.release(
                WatchpointOwner{ WatchpointOwnerKind::Profile, i });
            watchpoints_dirty_.store(true, std::memory_order_release);
            watchpoint_reconcile_pending_.store(true, std::memory_order_release);
            continue;
        }
        if (probe.max_hits.has_value() && lease_hit_count == *probe.max_hits) {
            state.exhausted = true;
            watchpoint_registry_.release(
                WatchpointOwner{ WatchpointOwnerKind::Profile, i });
            watchpoints_dirty_.store(true, std::memory_order_release);
            watchpoint_reconcile_pending_.store(true, std::memory_order_release);
        }
        update_windows_before_probe(probe);
        RawProbeEvent event{};
        if (!sample_probe(probe, state, system, pc, address, size, value, write, event)) {
            state.sample_failures.fetch_add(1, std::memory_order_relaxed);
            update_windows_after_probe(probe, false);
            continue;
        }
        event.probe_index = static_cast<std::uint32_t>(i);
        stamp_routed_identity(event);
        event.hit_count = hit_count;
        if (!evaluate_predicate(probe, event, power_pc)) {
            state.predicate_rejections.fetch_add(1, std::memory_order_relaxed);
            update_windows_after_probe(probe, false);
            continue;
        }
        if (!window_accepts(probe)) {
            state.window_rejections.fetch_add(1, std::memory_order_relaxed);
            update_windows_after_probe(probe, false);
            continue;
        }
        if (!sampling_accepts(probe, state, event)) {
            state.sampling_rejections.fetch_add(1, std::memory_order_relaxed);
            update_windows_after_probe(probe, false);
            continue;
        }
        state.sampled.fetch_add(1, std::memory_order_relaxed);
        const bool profile_control = subscriber_dispatch_decision(
            probe.subscriptions,
            capture_enabled_.load(std::memory_order_relaxed),
            active_foreground_wake,
            requested_control).control;
        if (profile_control)
            event.flags |= kRawEventControlPublished;
        enqueue_event(probe, event);
        if (profile_control) {
            requested_control = true;
            control_event = event;
        }
        update_windows_after_probe(probe, true);
        if (probe.one_shot) {
            state.exhausted = true;
            watchpoint_registry_.release(
                WatchpointOwner{ WatchpointOwnerKind::Profile, i });
            watchpoints_dirty_.store(true, std::memory_order_release);
            watchpoint_reconcile_pending_.store(true, std::memory_order_release);
        }
    }
    if (active_foreground_wake && !requested_control) {
            requested_control = true;
            control_event.capture_sequence = capture_sequence;
            control_event.monotonic_ns = monotonic_now_ns();
            control_event.frame_index = frame_index_.load(std::memory_order_relaxed);
            control_event.guest_workset_epoch = context.guest_workset_epoch;
            control_event.profile_revision = profile_.revision;
            control_event.snapshot_id = snapshot_id;
            control_event.probe_index = kSyntheticControlProbeIndex;
            control_event.pc = pc;
            control_event.address = address;
            control_event.size = size;
            control_event.value = value;
            control_event.kind = static_cast<std::uint8_t>(ProbeKind::Memory);
            control_event.flags |= kRawEventControlPublished;
    }
    if (requested_control)
        observe_routed_control(control_event, write);
}

bool ProbeRuntime::sample_probe(
    const ProbeDefinition& probe,
    ProbeState& state,
    Core::System& system,
    std::uint32_t pc,
    std::uint32_t address,
    std::uint32_t size,
    std::uint64_t value,
    bool,
    RawProbeEvent& event)
{
    const auto& power_pc = system.GetPowerPC();
    const auto& ppc_state = power_pc.GetPPCState();
    event.capture_sequence = next_capture_sequence_.fetch_add(1, std::memory_order_relaxed);
    event.monotonic_ns = monotonic_now_ns();
    event.frame_index = frame_index_.load(std::memory_order_relaxed);
    event.guest_workset_epoch = guest_workset_epoch_.load(std::memory_order_relaxed);
    event.profile_revision = profile_.revision;
    event.pc = pc;
    event.address = address;
    event.size = size;
    event.value = value;
    event.kind = static_cast<std::uint8_t>(probe.kind);

    for (std::size_t sample_index = 0;
        sample_index < probe.samples.size() && event.field_count < event.fields.size();
        ++sample_index) {
        const auto& sample = probe.samples[sample_index];
        auto& field = event.fields[event.field_count++];
        field.sample_index = static_cast<std::uint16_t>(sample_index);
        field.kind = static_cast<std::uint8_t>(sample.kind);
        field.status = RawFieldStatus::Present;
        std::uint32_t sample_address = sample.address;
        switch (sample.kind) {
        case SampleKind::Gpr:
            if (sample.base_register >= 32)
                field.status = RawFieldStatus::Invalid;
            else
                field.value = ppc_state.gpr[sample.base_register];
            break;
        case SampleKind::Constant:
            field.value = sample.constant;
            break;
        case SampleKind::RegisterMemory:
            if (sample.base_register >= 32) {
                field.status = RawFieldStatus::Invalid;
                break;
            }
            sample_address = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(ppc_state.gpr[sample.base_register]) + sample.offset);
            if (!read_guest_value(system, sample_address, sample.width, field.value))
                field.status = RawFieldStatus::Missing;
            break;
        case SampleKind::AddressProgram: {
            RawAddressTrace trace;
            const auto evaluated = evaluate_address_program(
                sample.address_program, system, power_pc, &trace);
            if (!evaluated.has_value()
                || !read_guest_value(system, *evaluated, sample.width, field.value)) {
                field.status = RawFieldStatus::Missing;
            }
            if (sample.trace == AddressTracePolicy::Always
                || (sample.trace == AddressTracePolicy::OnFailure && !trace.success)) {
                if (!append_address_trace(event, static_cast<std::uint16_t>(sample_index), trace))
                    field.status = RawFieldStatus::Truncated;
                else
                    state.traces.fetch_add(1, std::memory_order_relaxed);
                if (!trace.success)
                    state.trace_failures.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }
        case SampleKind::Memory:
            if (!read_guest_value(system, sample_address, sample.width, field.value))
                field.status = RawFieldStatus::Missing;
            break;
        case SampleKind::LinkedList: {
            RawAddressTrace trace;
            std::optional<std::uint32_t> root_address;
            if (sample.address_provided)
                root_address = sample.address;
            else
                root_address = evaluate_address_program(
                    sample.address_program, system, power_pc, &trace);
            if (!sample.address_program.empty()
                && (sample.trace == AddressTracePolicy::Always
                    || (sample.trace == AddressTracePolicy::OnFailure && !trace.success))) {
                if (!append_address_trace(event, static_cast<std::uint16_t>(sample_index), trace))
                    field.status = RawFieldStatus::Truncated;
                else
                    state.traces.fetch_add(1, std::memory_order_relaxed);
                if (!trace.success)
                    state.trace_failures.fetch_add(1, std::memory_order_relaxed);
            }
            if (!root_address.has_value()) {
                field.status = RawFieldStatus::Missing;
                break;
            }
            field.value = *root_address;
            const auto payload_begin = event.payload_size;
            field.payload_offset = payload_begin;
            std::uint64_t first_node_value = 0;
            std::array<std::uint32_t, 256> seen{};
            std::uint16_t node_count = 0;
            std::uint8_t flags = 0;
            if (!read_guest_value(system, *root_address, SampleWidth::U32, first_node_value))
                flags |= 0x4;
            if (!payload_append(event, static_cast<std::uint32_t>(first_node_value))
                || !payload_append(event, node_count)
                || !payload_append(event, flags)) {
                field.status = RawFieldStatus::Truncated;
                break;
            }
            auto node = static_cast<std::uint32_t>(first_node_value);
            while (node != 0 && node_count < sample.max_nodes) {
                bool cycle = false;
                for (std::uint16_t i = 0; i < node_count; ++i) {
                    if (seen[i] == node) {
                        cycle = true;
                        break;
                    }
                }
                if (cycle) {
                    flags |= 0x2;
                    break;
                }
                seen[node_count] = node;
                if (!payload_append(event, node)) {
                    flags |= 0x1;
                    break;
                }
                for (const auto& list_field : sample.linked_list_fields) {
                    std::uint64_t list_value = 0;
                    const auto field_address = static_cast<std::uint32_t>(
                        static_cast<std::int64_t>(node) + list_field.offset);
                    const std::uint8_t present = read_guest_value(
                        system, field_address, list_field.width, list_value) ? 1 : 0;
                    if (!present)
                        flags |= 0x4;
                    if (!payload_append(event, present) || !payload_append(event, list_value)) {
                        flags |= 0x1;
                        node = 0;
                        break;
                    }
                }
                if (node == 0)
                    break;
                ++node_count;
                std::uint64_t next = 0;
                const auto next_address = static_cast<std::uint32_t>(
                    static_cast<std::int64_t>(node) + sample.next_offset);
                if (!read_guest_value(system, next_address, SampleWidth::U32, next)) {
                    flags |= 0x4;
                    break;
                }
                node = static_cast<std::uint32_t>(next);
            }
            if (node != 0 && node_count >= sample.max_nodes)
                flags |= 0x1;
            std::memcpy(event.payload.data() + payload_begin + sizeof(std::uint32_t),
                &node_count, sizeof(node_count));
            std::memcpy(event.payload.data() + payload_begin + sizeof(std::uint32_t) + sizeof(node_count),
                &flags, sizeof(flags));
            field.payload_size = event.payload_size - payload_begin;
            if ((flags & 0x1) != 0)
                field.status = RawFieldStatus::Truncated;
            break;
        }
        case SampleKind::StackTrace: {
            const auto payload_begin = event.payload_size;
            field.payload_offset = payload_begin;
            std::array<std::uint32_t, 64> seen{};
            std::uint8_t frame_count = 0;
            std::uint8_t flags = 0;
            if (!payload_append(event, frame_count) || !payload_append(event, flags)) {
                field.status = RawFieldStatus::Truncated;
                break;
            }
            auto stack_pointer = ppc_state.gpr[1];
            const auto current_return_pc = ppc_state.spr[SPR_LR];
            if (sample.max_frames != 0 && current_return_pc != 0) {
                const std::uint32_t register_frame_pointer = 0;
                const auto current_callsite_pc = current_return_pc >= 4
                    ? current_return_pc - 4
                    : 0;
                if (!payload_append(event, register_frame_pointer)
                    || !payload_append(event, stack_pointer)
                    || !payload_append(event, current_return_pc)
                    || !payload_append(event, current_callsite_pc)) {
                    flags |= 0x1;
                } else {
                    ++frame_count;
                }
            }
            std::uint8_t seen_count = 0;
            while (stack_pointer != 0 && frame_count < sample.max_frames) {
                bool cycle = false;
                for (std::uint8_t i = 0; i < seen_count; ++i) {
                    if (seen[i] == stack_pointer) {
                        cycle = true;
                        break;
                    }
                }
                if (cycle) {
                    flags |= 0x2;
                    break;
                }
                seen[seen_count++] = stack_pointer;
                std::uint64_t next_value = 0;
                std::uint64_t return_value = 0;
                if (!read_guest_value(system, stack_pointer, SampleWidth::U32, next_value)
                    || !read_guest_value(system, stack_pointer + 4, SampleWidth::U32, return_value)) {
                    flags |= 0x4;
                    break;
                }
                const auto next = static_cast<std::uint32_t>(next_value);
                const auto return_pc = static_cast<std::uint32_t>(return_value);
                const auto callsite_pc = return_pc >= 4 ? return_pc - 4 : 0;
                if (!payload_append(event, stack_pointer)
                    || !payload_append(event, next)
                    || !payload_append(event, return_pc)
                    || !payload_append(event, callsite_pc)) {
                    flags |= 0x1;
                    break;
                }
                ++frame_count;
                if (next == 0 || next == stack_pointer || (next & 3u) != 0)
                    break;
                stack_pointer = next;
            }
            if (stack_pointer != 0 && frame_count >= sample.max_frames)
                flags |= 0x1;
            std::memcpy(event.payload.data() + payload_begin, &frame_count, sizeof(frame_count));
            std::memcpy(
                event.payload.data() + payload_begin + sizeof(frame_count), &flags, sizeof(flags));
            field.payload_size = event.payload_size - payload_begin;
            if ((flags & 0x1) != 0)
                field.status = RawFieldStatus::Truncated;
            break;
        }
        }
    }
    return true;
}

bool ProbeRuntime::evaluate_predicate(
    const ProbeDefinition& probe,
    const RawProbeEvent& event,
    const PowerPC::PowerPCManager& power_pc) const
{
    if (probe.predicate.empty())
        return true;
    std::array<std::uint64_t, 64> stack{};
    std::size_t top = 0;
    const auto push = [&](std::uint64_t value) {
        if (top >= stack.size()) return false;
        stack[top++] = value;
        return true;
    };
    const auto pop = [&](std::uint64_t& value) {
        if (top == 0) return false;
        value = stack[--top];
        return true;
    };
    const auto& state = power_pc.GetPPCState();
    for (const auto& instruction : probe.predicate) {
        std::uint64_t lhs = 0;
        std::uint64_t rhs = 0;
        switch (instruction.op) {
        case PredicateOp::PushConstant: if (!push(instruction.operand)) return false; break;
        case PredicateOp::PushPc: if (!push(event.pc)) return false; break;
        case PredicateOp::PushLr:
        case PredicateOp::PushCaller: if (!push(state.spr[SPR_LR])) return false; break;
        case PredicateOp::PushGpr:
            if (instruction.operand >= 32 || !push(state.gpr[instruction.operand])) return false;
            break;
        case PredicateOp::PushHitCount: if (!push(event.hit_count)) return false; break;
        case PredicateOp::PushSample:
            if (instruction.operand >= event.field_count
                || !push(event.fields[instruction.operand].value)) return false;
            break;
        case PredicateOp::LogicalNot:
            if (!pop(lhs) || !push(!lhs)) return false;
            break;
        case PredicateOp::MaskEqual:
            if (!pop(lhs) || !push((lhs & instruction.operand2) == instruction.operand)) return false;
            break;
        default:
            if (!pop(rhs) || !pop(lhs)) return false;
            switch (instruction.op) {
            case PredicateOp::Equal: if (!push(lhs == rhs)) return false; break;
            case PredicateOp::NotEqual: if (!push(lhs != rhs)) return false; break;
            case PredicateOp::Less: if (!push(lhs < rhs)) return false; break;
            case PredicateOp::LessEqual: if (!push(lhs <= rhs)) return false; break;
            case PredicateOp::Greater: if (!push(lhs > rhs)) return false; break;
            case PredicateOp::GreaterEqual: if (!push(lhs >= rhs)) return false; break;
            case PredicateOp::LogicalAnd: if (!push(lhs && rhs)) return false; break;
            case PredicateOp::LogicalOr: if (!push(lhs || rhs)) return false; break;
            default: return false;
            }
            break;
        }
    }
    return top == 1 && stack[0] != 0;
}

bool ProbeRuntime::sampling_accepts(
    const ProbeDefinition& probe,
    ProbeState& state,
    const RawProbeEvent& event)
{
    const auto hit = event.hit_count;
    switch (probe.sampling.mode) {
    case SamplingMode::EveryHit: return true;
    case SamplingMode::EveryN: return probe.sampling.n != 0 && (hit - 1) % probe.sampling.n == 0;
    case SamplingMode::FirstN: return hit <= probe.sampling.n;
    case SamplingMode::ChangedOnly: {
        const auto hash = raw_event_hash(event);
        const bool changed = !state.has_last_hash || state.last_hash != hash;
        state.last_hash = hash;
        state.has_last_hash = true;
        return changed;
    }
    case SamplingMode::HitRanges:
        return std::ranges::any_of(probe.sampling.hit_ranges, [hit](const auto& range) {
            return hit >= range.first && hit <= range.second;
        });
    case SamplingMode::LastN:
        return true;
    case SamplingMode::Burst:
        return probe.sampling.burst_period != 0
            && ((hit - 1) % probe.sampling.burst_period) < probe.sampling.burst_length;
    }
    return true;
}

bool ProbeRuntime::window_accepts(const ProbeDefinition& probe) const
{
    if (probe.window_id.empty())
        return true;
    for (std::size_t i = 0; i < profile_.windows.size(); ++i) {
        if (profile_.windows[i].id == probe.window_id)
            return window_states_[i].is_open();
    }
    return false;
}

void ProbeRuntime::update_windows_before_probe(const ProbeDefinition& probe)
{
    const auto frame = frame_index_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < profile_.windows.size(); ++i) {
        const auto& definition = profile_.windows[i];
        window_states_[i].before_probe(definition, probe.id, frame);
    }
}

void ProbeRuntime::update_windows_after_probe(const ProbeDefinition& probe, bool accepted)
{
    const auto frame = frame_index_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < profile_.windows.size(); ++i) {
        const auto& definition = profile_.windows[i];
        const bool member = probe.window_id == definition.id;
        window_states_[i].after_probe(
            definition, probe.id, member, accepted, probe.frame_clock, frame);
    }
}

bool ProbeRuntime::enqueue_event(const ProbeDefinition& probe, const RawProbeEvent& event)
{
    bool queued = true;
    auto& state = probe_states_[event.probe_index];
    const auto dispatch = subscriber_dispatch_decision(
        probe.subscriptions,
        capture_enabled_.load(std::memory_order_relaxed),
        false);
    const bool capture_subscriber = has_subscription(probe.subscriptions, Subscription::Capture)
        || has_subscription(probe.subscriptions, Subscription::Progress);
    if (dispatch.capture) {
        if (capture_queue_.try_push(event)) {
            state.capture_deliveries.fetch_add(1, std::memory_order_relaxed);
        } else {
            capture_drops_.fetch_add(1, std::memory_order_relaxed);
            state.dropped.fetch_add(1, std::memory_order_relaxed);
            mark_capture_incomplete("capture queue overflow", event.capture_sequence);
            queued = false;
        }
    } else if (capture_subscriber && !options_.capture_path.empty()
        && !capture_complete_.load(std::memory_order_relaxed)) {
        capture_drops_.fetch_add(1, std::memory_order_relaxed);
        state.dropped.fetch_add(1, std::memory_order_relaxed);
        capture_gap_last_sequence_.store(event.capture_sequence, std::memory_order_relaxed);
    }
    if (dispatch.progress) {
        if (progress_queue_.try_push(event)) {
            state.progress_deliveries.fetch_add(1, std::memory_order_relaxed);
        } else if (state.coalesced_progress) {
            bool replaced = false;
            if (!state.coalesced_progress->publish(event, &replaced)) {
                progress_drops_.fetch_add(1, std::memory_order_relaxed);
                state.dropped.fetch_add(1, std::memory_order_relaxed);
                queued = false;
                wake_recorder();
                return queued;
            }
            state.progress_coalesced.fetch_add(1, std::memory_order_release);
            if (replaced) {
                progress_drops_.fetch_add(1, std::memory_order_relaxed);
                state.dropped.fetch_add(1, std::memory_order_relaxed);
            }
            queued = false;
        } else {
            progress_drops_.fetch_add(1, std::memory_order_relaxed);
            state.dropped.fetch_add(1, std::memory_order_relaxed);
            queued = false;
        }
    }
    state.bytes.fetch_add(event.payload_size + event.field_count * sizeof(RawField), std::memory_order_relaxed);
    wake_recorder();
    return queued;
}

void ProbeRuntime::observe_routed_control(
    const RawProbeEvent& event,
    bool)
{
    for (std::size_t i = 0; i < profile_.windows.size(); ++i) {
        const auto& definition = profile_.windows[i];
        window_states_[i].on_control(definition, event.frame_index);
    }
    if (event.probe_index < profile_.probes.size())
        probe_states_[event.probe_index].control_publications.fetch_add(
            1,
            std::memory_order_relaxed);
    if (event.probe_index == kSyntheticControlProbeIndex
        && capture_enabled_.load(std::memory_order_relaxed)) {
        if (!capture_queue_.try_push(event))
            mark_capture_incomplete(
                "capture queue overflow",
                event.capture_sequence);
        else
            wake_recorder();
    }
}

void ProbeRuntime::wake_recorder()
{
    recorder_generation_.fetch_add(1, std::memory_order_release);
    WakeByAddressSingle(&recorder_generation_);
}

void ProbeRuntime::mark_capture_incomplete(const char* reason, std::uint64_t capture_sequence)
{
    capture_enabled_.store(false, std::memory_order_release);
    if (capture_complete_.exchange(false, std::memory_order_acq_rel)) {
        std::strncpy(incomplete_reason_.data(), reason, incomplete_reason_.size() - 1);
        incomplete_reason_.back() = '\0';
        capture_gap_first_sequence_.store(capture_sequence, std::memory_order_relaxed);
    }
    if (capture_sequence != 0)
        capture_gap_last_sequence_.store(capture_sequence, std::memory_order_relaxed);
    wake_recorder();
}

std::string ProbeRuntime::capture_incomplete_reason() const
{
    return std::string(incomplete_reason_.data());
}

capture_format::Event ProbeRuntime::decode_event(const RawProbeEvent& raw) const
{
    capture_format::Event event;
    event.capture_sequence = raw.capture_sequence;
    event.monotonic_ns = raw.monotonic_ns;
    event.frame_index = raw.frame_index;
    event.guest_workset_epoch = raw.guest_workset_epoch;
    event.profile_revision = raw.profile_revision;
    event.snapshot_id = raw.snapshot_id;
    event.pc = raw.pc;
    event.address = raw.address;
    event.size = raw.size;
    event.value = raw.value;
    if (raw.probe_index == kSyntheticControlProbeIndex) {
        event.kind = capture_format::EventKind::Control;
        event.probe_id = "control.hit";
    } else if (raw.probe_index == kSyntheticMarkerProbeIndex) {
        event.kind = capture_format::EventKind::Marker;
        event.probe_id.assign(reinterpret_cast<const char*>(raw.payload.data()), raw.payload_size);
    } else if (raw.probe_index < profile_.probes.size()) {
        const auto& probe = profile_.probes[raw.probe_index];
        const bool address_resolution = (raw.flags & kRawEventAddressResolution) != 0;
        event.probe_id = address_resolution ? probe.id + ".address_resolution" : probe.id;
        event.kind = address_resolution ? capture_format::EventKind::Marker
            : probe.kind == ProbeKind::Memory
            ? capture_format::EventKind::Memory
            : (has_subscription(probe.subscriptions, Subscription::Progress)
                && !has_subscription(probe.subscriptions, Subscription::Capture)
                ? capture_format::EventKind::Progress
                : capture_format::EventKind::Pc);
        event.fields.push_back(capture_format::Field{
            "hit_count", capture_format::FieldType::Unsigned,
            capture_format::FieldStatus::Present, raw.hit_count, {},
        });
        for (std::uint16_t i = 0; i < raw.field_count; ++i) {
            const auto& raw_field = raw.fields[i];
            const SampleDefinition* sample = raw_field.sample_index < probe.samples.size()
                ? &probe.samples[raw_field.sample_index] : nullptr;
            if (!sample && raw_field.kind != kRawFieldAddressTrace)
                continue;
            capture_format::Field field;
            field.name = raw_field.kind == kRawFieldAddressTrace
                ? (sample ? sample->name + ".address_trace" : "address_root.trace")
                : sample->name;
            field.status = static_cast<capture_format::FieldStatus>(raw_field.status);
            field.value = raw_field.value;
            const auto payload_end = std::min<std::size_t>(
                raw.payload_size, raw_field.payload_offset + raw_field.payload_size);
            std::size_t payload_offset = raw_field.payload_offset;
            const auto read_payload = [&]<typename T>(T& value) {
                if (payload_offset > payload_end || payload_end - payload_offset < sizeof(T))
                    return false;
                std::memcpy(&value, raw.payload.data() + payload_offset, sizeof(T));
                payload_offset += sizeof(T);
                return true;
            };
            if (raw_field.kind == kRawFieldAddressTrace) {
                field.type = capture_format::FieldType::AddressTrace;
                capture_format::AddressTraceValue trace;
                std::uint8_t count = 0;
                std::uint8_t success = 0;
                std::uint8_t failure_operation = 0;
                std::uint8_t failure = 0;
                if (!read_payload(count) || !read_payload(success)
                    || !read_payload(failure_operation) || !read_payload(failure)
                    || !read_payload(trace.final_address) || count > 64) {
                    field.status = capture_format::FieldStatus::Invalid;
                } else {
                    trace.success = success != 0;
                    if (failure != static_cast<std::uint8_t>(AddressProgramFailure::None)) {
                        trace.failure_operation = failure_operation;
                        trace.failure = std::string(address_program_failure_name(
                            static_cast<AddressProgramFailure>(failure)));
                    }
                    for (std::uint8_t operation_index = 0; operation_index < count; ++operation_index) {
                        capture_format::AddressTraceOperation operation;
                        std::uint8_t has_dereference = 0;
                        std::uint8_t operation_failure = 0;
                        std::uint64_t dereferenced = 0;
                        if (!read_payload(operation.index) || !read_payload(operation.operation)
                            || !read_payload(has_dereference) || !read_payload(operation_failure)
                            || !read_payload(operation.address_before)
                            || !read_payload(operation.address_after)
                            || !read_payload(dereferenced)) {
                            field.status = capture_format::FieldStatus::Invalid;
                            break;
                        }
                        if (has_dereference) operation.dereferenced_value = dereferenced;
                        if (operation_failure != static_cast<std::uint8_t>(AddressProgramFailure::None))
                            operation.failure = std::string(address_program_failure_name(
                                static_cast<AddressProgramFailure>(operation_failure)));
                        trace.operations.push_back(std::move(operation));
                    }
                    if (field.status != capture_format::FieldStatus::Invalid
                        && payload_offset != payload_end) {
                        field.status = capture_format::FieldStatus::Invalid;
                    }
                    field.address_trace = std::move(trace);
                }
            } else if (sample->kind == SampleKind::LinkedList) {
                field.type = capture_format::FieldType::LinkedList;
                capture_format::LinkedListValue list;
                list.root_address = static_cast<std::uint32_t>(raw_field.value);
                std::uint16_t node_count = 0;
                std::uint8_t flags = 0;
                if (!read_payload(list.head) || !read_payload(node_count) || !read_payload(flags)) {
                    field.status = capture_format::FieldStatus::Invalid;
                } else {
                    list.read_ok = (flags & 0x4) == 0;
                    list.truncated = (flags & 0x1) != 0;
                    list.cycle_detected = (flags & 0x2) != 0;
                    for (std::uint16_t node_index = 0; node_index < node_count; ++node_index) {
                        capture_format::LinkedListNode node;
                        if (!read_payload(node.address)) {
                            field.status = capture_format::FieldStatus::Invalid;
                            break;
                        }
                        for (const auto& definition : sample->linked_list_fields) {
                            std::uint8_t present = 0;
                            capture_format::LinkedListMember member;
                            member.name = definition.name;
                            member.width = static_cast<std::uint8_t>(definition.width);
                            if (!read_payload(present) || !read_payload(member.value)) {
                                field.status = capture_format::FieldStatus::Invalid;
                                break;
                            }
                            member.status = present
                                ? capture_format::FieldStatus::Present
                                : capture_format::FieldStatus::Missing;
                            node.fields.push_back(std::move(member));
                        }
                        list.nodes.push_back(std::move(node));
                    }
                    field.linked_list = std::move(list);
                }
            } else if (sample->kind == SampleKind::StackTrace) {
                field.type = capture_format::FieldType::StackTrace;
                capture_format::StackTraceValue stack;
                std::uint8_t frame_count = 0;
                std::uint8_t flags = 0;
                if (!read_payload(frame_count) || !read_payload(flags) || frame_count > 64) {
                    field.status = capture_format::FieldStatus::Invalid;
                } else {
                    stack.read_ok = (flags & 0x4) == 0;
                    stack.truncated = (flags & 0x1) != 0;
                    stack.cycle_detected = (flags & 0x2) != 0;
                    for (std::uint8_t frame_index = 0; frame_index < frame_count; ++frame_index) {
                        capture_format::StackFrame frame;
                        if (!read_payload(frame.stack_pointer)
                            || !read_payload(frame.next_stack_pointer)
                            || !read_payload(frame.return_pc)
                            || !read_payload(frame.callsite_pc)) {
                            field.status = capture_format::FieldStatus::Invalid;
                            break;
                        }
                        stack.frames.push_back(frame);
                    }
                    field.stack_trace = std::move(stack);
                }
            } else {
                field.type = capture_format::FieldType::Unsigned;
            }
            event.fields.push_back(std::move(field));
        }
    }
    return event;
}

void ProbeRuntime::recorder_main()
{
    CaptureRetention retention(profile_);

    bool writer_marked_incomplete = false;
    const auto append_capture = [this](capture_format::Event event) {
        if (!writer_.is_open())
            return;
        if (event.record_sequence == 0)
            event.record_sequence = next_record_sequence_.fetch_add(1, std::memory_order_relaxed);
        std::string error;
        if (!writer_.append(event, &error))
            mark_capture_incomplete("capture writer append failed");
    };
    const auto append_raw = [&](const RawProbeEvent& raw) {
        append_capture(decode_event(raw));
    };
    const auto drain_coalesced_progress = [&]() {
        bool drained = false;
        for (std::size_t i = 0; i < profile_.probes.size(); ++i) {
            auto& state = probe_states_[i];
            if (!state.coalesced_progress) {
                continue;
            }
            RawProbeEvent copy{};
            if (!state.coalesced_progress->try_take(copy))
                continue;
            drained = true;
            state.progress_deliveries.fetch_add(1, std::memory_order_relaxed);
            if (options_.progress_callback)
                options_.progress_callback(decode_event(copy), profile_.probes[i].progress_record);
        }
        return drained;
    };
    for (;;) {
        bool did_work = false;
        if (flight_triggered_.load(std::memory_order_acquire)) {
            const auto post_override = flight_post_remaining_.exchange(0, std::memory_order_acq_rel);
            retention.trigger_all(
                post_override,
                next_capture_sequence_.load(std::memory_order_relaxed),
                append_raw);
            flight_triggered_.store(false, std::memory_order_release);
            did_work = true;
        }
        RawProbeEvent raw{};
        while (capture_queue_.try_pop(raw)) {
            did_work = true;
            retention.accept(raw, append_raw);
        }
        while (progress_queue_.try_pop(raw)) {
            did_work = true;
            if (options_.progress_callback) {
                const bool record = raw.probe_index < profile_.probes.size()
                    ? profile_.probes[raw.probe_index].progress_record
                    : true;
                options_.progress_callback(decode_event(raw), record);
            }
        }
        did_work = drain_coalesced_progress() || did_work;
        if (!capture_complete_.load(std::memory_order_acquire)
            && writer_.is_open() && !writer_marked_incomplete) {
            writer_.mark_incomplete(capture_incomplete_reason());
            writer_marked_incomplete = true;
        }
        if (recorder_stop_.load(std::memory_order_acquire)
            && capture_queue_.approximate_size() == 0
            && progress_queue_.approximate_size() == 0
            && !std::ranges::any_of(profile_.probes, [this](const auto& probe) {
                const auto index = static_cast<std::size_t>(&probe - profile_.probes.data());
                return probe_states_[index].coalesced_progress
                    && probe_states_[index].coalesced_progress->has_pending();
            })) {
            break;
        }
        if (!did_work) {
            auto generation = recorder_generation_.load(std::memory_order_acquire);
            WaitOnAddress(&recorder_generation_, &generation, sizeof(generation), 100);
        }
    }

    retention.finish(append_raw);

    const auto gap_first = capture_gap_first_sequence_.load(std::memory_order_relaxed);
    if (gap_first != 0 && writer_.is_open()) {
        capture_format::Event gap;
        gap.capture_sequence = gap_first;
        gap.frame_index = frame_index_.load(std::memory_order_relaxed);
        gap.guest_workset_epoch = guest_workset_epoch_.load(std::memory_order_relaxed);
        gap.profile_revision = profile_.revision;
        gap.kind = capture_format::EventKind::Gap;
        gap.probe_id = "probe.capture.gap";
        gap.fields.push_back(capture_format::Field{ .name = "first_capture_sequence", .value = gap_first });
        gap.fields.push_back(capture_format::Field{
            .name = "last_capture_sequence",
            .value = capture_gap_last_sequence_.load(std::memory_order_relaxed) });
        gap.fields.push_back(capture_format::Field{ .name = "lost_events", .value = capture_drops_.load(std::memory_order_relaxed) });
        capture_format::Field reason;
        reason.name = "reason";
        reason.type = capture_format::FieldType::Text;
        const auto text = capture_incomplete_reason();
        reason.bytes.assign(text.begin(), text.end());
        gap.fields.push_back(std::move(reason));
        append_capture(std::move(gap));
    }

    for (const auto& metric : metrics()) {
        capture_format::Event event;
        event.capture_sequence = next_capture_sequence_.fetch_add(1, std::memory_order_relaxed);
        event.frame_index = frame_index_.load(std::memory_order_relaxed);
        event.guest_workset_epoch = guest_workset_epoch_.load(std::memory_order_relaxed);
        event.profile_revision = profile_.revision;
        event.kind = capture_format::EventKind::Metrics;
        event.probe_id = "probe.metrics." + metric.id;
        const auto add = [&](std::string name, std::uint64_t value) {
            event.fields.push_back(capture_format::Field{ .name = std::move(name), .value = value });
        };
        add("hits", metric.hits);
        add("sampled", metric.sampled);
        add("sample_failures", metric.sample_failures);
        add("predicate_rejections", metric.predicate_rejections);
        add("window_rejections", metric.window_rejections);
        add("sampling_rejections", metric.sampling_rejections);
        add("capture_deliveries", metric.capture_deliveries);
        add("progress_deliveries", metric.progress_deliveries);
        add("control_publications", metric.control_publications);
        add("drops", metric.dropped);
        add("progress_coalesced", metric.progress_coalesced);
        add("bytes", metric.bytes);
        add("traces", metric.traces);
        add("trace_failures", metric.trace_failures);
        add("physical_watchpoint_hits", metric.physical_watchpoint_hits);
        add("binding_generation", metric.binding_generation);
        add("binding_failures", metric.binding_failures);
        add("binding_failure_reason", metric.binding_failure_reason);
        add("derived_address", metric.derived_address);
        add("derived_size", metric.derived_size);
        add("physical_address", metric.physical_address);
        add("physical_size", metric.physical_size);
        capture_format::Field binding_source;
        binding_source.name = "binding_source";
        binding_source.type = capture_format::FieldType::Text;
        binding_source.bytes.assign(
            metric.binding_source.begin(), metric.binding_source.end());
        event.fields.push_back(std::move(binding_source));
        append_capture(std::move(event));
    }
    for (const auto& state : retention.flight_metrics()) {
        capture_format::Event event;
        event.capture_sequence = next_capture_sequence_.fetch_add(1, std::memory_order_relaxed);
        event.frame_index = frame_index_.load(std::memory_order_relaxed);
        event.guest_workset_epoch = guest_workset_epoch_.load(std::memory_order_relaxed);
        event.profile_revision = profile_.revision;
        event.kind = capture_format::EventKind::Metrics;
        event.probe_id = "flight.metrics." + state.id;
        event.fields.push_back(capture_format::Field{ .name = "triggers", .value = state.triggers });
        event.fields.push_back(capture_format::Field{ .name = "retriggers", .value = state.retriggers });
        event.fields.push_back(capture_format::Field{ .name = "completed_windows", .value = state.completed_windows });
        event.fields.push_back(capture_format::Field{ .name = "post_remaining", .value = state.post_remaining });
        event.fields.push_back(capture_format::Field{ .name = "active", .value = state.active ? 1u : 0u });
        append_capture(std::move(event));
    }
    if (writer_.is_open()) {
        capture_format::Event session;
        session.capture_sequence = next_capture_sequence_.fetch_add(1, std::memory_order_relaxed);
        session.frame_index = frame_index_.load(std::memory_order_relaxed);
        session.guest_workset_epoch = guest_workset_epoch_.load(std::memory_order_relaxed);
        session.profile_revision = profile_.revision;
        session.kind = capture_format::EventKind::Metrics;
        session.probe_id = "probe.session.metrics";
        session.fields.push_back(capture_format::Field{ .name = "complete", .value = capture_complete_.load(std::memory_order_relaxed) ? 1u : 0u });
        session.fields.push_back(capture_format::Field{ .name = "capture_drops", .value = capture_drops_.load(std::memory_order_relaxed) });
        session.fields.push_back(capture_format::Field{ .name = "progress_drops", .value = progress_drops_.load(std::memory_order_relaxed) });
        session.fields.push_back(capture_format::Field{ .name = "profile_revision", .value = profile_.revision });
        session.fields.push_back(capture_format::Field{ .name = "guest_workset_epoch", .value = guest_workset_epoch_.load(std::memory_order_relaxed) });
        append_capture(std::move(session));
    }
}

std::optional<std::uint32_t> ProbeRuntime::evaluate_address_program(
    std::span<const std::uint8_t> program,
    Core::System& system,
    const PowerPC::PowerPCManager& power_pc,
    RawAddressTrace* trace) const
{
    const auto& registers = power_pc.GetPPCState().gpr;
    return evaluate_address_program_fixed(
        program,
        std::span<const std::uint32_t, 32>(registers),
        static_cast<bool>(options_.base_key_resolver),
        [this](std::uint16_t key) {
            return options_.base_key_resolver
                ? options_.base_key_resolver(key)
                : std::optional<std::uint32_t>{};
        },
        [this, &system](std::uint32_t address, SampleWidth width, std::uint64_t& value) {
            return read_guest_value(system, address, width, value);
        },
        trace);
}

bool ProbeRuntime::append_address_trace(
    RawProbeEvent& event,
    std::uint16_t sample_index,
    const RawAddressTrace& trace) const
{
    if (event.field_count >= event.fields.size())
        return false;
    const auto payload_begin = event.payload_size;
    if (!payload_append(event, trace.count)
        || !payload_append(event, static_cast<std::uint8_t>(trace.success ? 1 : 0))
        || !payload_append(event, trace.failure_operation)
        || !payload_append(event, static_cast<std::uint8_t>(trace.failure))
        || !payload_append(event, trace.final_address)) {
        event.payload_size = static_cast<std::uint16_t>(payload_begin);
        return false;
    }
    for (std::uint8_t i = 0; i < trace.count; ++i) {
        const auto& step = trace.operations[i];
        if (!payload_append(event, step.index)
            || !payload_append(event, step.operation)
            || !payload_append(event, static_cast<std::uint8_t>(step.has_dereferenced_value ? 1 : 0))
            || !payload_append(event, static_cast<std::uint8_t>(step.failure))
            || !payload_append(event, step.address_before)
            || !payload_append(event, step.address_after)
            || !payload_append(event, step.dereferenced_value)) {
            event.payload_size = static_cast<std::uint16_t>(payload_begin);
            return false;
        }
    }
    auto& field = event.fields[event.field_count++];
    field.sample_index = sample_index;
    field.kind = kRawFieldAddressTrace;
    field.status = trace.success ? RawFieldStatus::Present : RawFieldStatus::Missing;
    field.value = trace.final_address;
    field.payload_offset = static_cast<std::uint32_t>(payload_begin);
    field.payload_size = event.payload_size - payload_begin;
    return true;
}

bool ProbeRuntime::read_guest_value(
    Core::System& system,
    std::uint32_t address,
    SampleWidth width,
    std::uint64_t& value) const
{
    auto& memory = system.GetMemory();
    if (!memory.GetPointerForRange(address, static_cast<std::size_t>(width)))
        return false;
    switch (width) {
    case SampleWidth::U8: value = memory.Read_U8(address); return true;
    case SampleWidth::U16: value = memory.Read_U16(address); return true;
    case SampleWidth::U32: value = memory.Read_U32(address); return true;
    case SampleWidth::U64: value = memory.Read_U64(address); return true;
    }
    return false;
}

std::vector<ProbeMetrics> ProbeRuntime::metrics() const
{
    std::vector<ProbeMetrics> result;
    result.reserve(profile_.probes.size());
    for (std::size_t i = 0; i < profile_.probes.size(); ++i) {
        const auto& state = probe_states_[i];
        ProbeMetrics metric;
        metric.id = profile_.probes[i].id;
        metric.hits = state.hits.load(std::memory_order_relaxed);
        metric.sampled = state.sampled.load(std::memory_order_relaxed);
        metric.sample_failures = state.sample_failures.load(std::memory_order_relaxed);
        metric.predicate_rejections = state.predicate_rejections.load(std::memory_order_relaxed);
        metric.window_rejections = state.window_rejections.load(std::memory_order_relaxed);
        metric.sampling_rejections = state.sampling_rejections.load(std::memory_order_relaxed);
        metric.capture_deliveries = state.capture_deliveries.load(std::memory_order_relaxed);
        metric.progress_deliveries = state.progress_deliveries.load(std::memory_order_relaxed);
        metric.control_publications = state.control_publications.load(std::memory_order_relaxed);
        metric.dropped = state.dropped.load(std::memory_order_relaxed);
        metric.progress_coalesced = state.progress_coalesced.load(std::memory_order_relaxed);
        metric.bytes = state.bytes.load(std::memory_order_relaxed);
        metric.traces = state.traces.load(std::memory_order_relaxed);
        metric.trace_failures = state.trace_failures.load(std::memory_order_relaxed);
        metric.physical_watchpoint_hits =
            state.physical_watchpoint_hits.load(std::memory_order_relaxed);
        metric.binding_generation = state.binding_generation.load(std::memory_order_relaxed);
        metric.binding_failures = state.binding_failures.load(std::memory_order_relaxed);
        metric.binding_failure_reason =
            state.binding_failure_reason.load(std::memory_order_relaxed);
        metric.derived_address = state.derived_address.load(std::memory_order_relaxed);
        metric.derived_size = state.derived_size.load(std::memory_order_relaxed);
        metric.physical_address = state.physical_address.load(std::memory_order_relaxed);
        metric.physical_size = state.physical_size.load(std::memory_order_relaxed);
        const auto source = static_cast<WatchpointBindingSource>(
            state.binding_source.load(std::memory_order_relaxed));
        metric.binding_source = state.binding_source.load(std::memory_order_relaxed) == 0
            ? "unbound"
            : std::string(watchpoint_binding_source_name(source));
        result.push_back(std::move(metric));
    }
    return result;
}

std::string current_module_sha256(std::string* error_out)
{
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        if (error_out) *error_out = "failed resolving current module path";
        return {};
    }
    std::ifstream stream(std::filesystem::path(path.data()), std::ios::binary);
    if (!stream) {
        if (error_out) *error_out = "failed opening current module for SHA-256";
        return {};
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD result_size = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0
        || BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &result_size, 0) < 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        if (error_out) *error_out = "failed initializing SHA-256 provider";
        return {};
    }
    std::vector<std::uint8_t> object(object_size);
    std::array<std::uint8_t, 32> digest{};
    if (BCryptCreateHash(
            algorithm, &hash, object.data(), static_cast<ULONG>(object.size()), nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        if (error_out) *error_out = "failed creating SHA-256 hash";
        return {};
    }
    std::array<char, 64 * 1024> buffer{};
    while (stream) {
        stream.read(buffer.data(), buffer.size());
        const auto count = stream.gcount();
        if (count > 0 && BCryptHashData(hash,
            reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) < 0) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            if (error_out) *error_out = "failed updating SHA-256 hash";
            return {};
        }
    }
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        if (error_out) *error_out = "failed finalizing SHA-256 hash";
        return {};
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : digest)
        out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}

} // namespace savor::probe
