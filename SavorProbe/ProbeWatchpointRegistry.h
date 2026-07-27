#pragma once

#include "ProbeProfile.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace savor::probe {

enum class WatchpointOwnerKind : std::uint8_t {
    Profile = 1,
    Control = 2,
};

struct WatchpointOwner {
    WatchpointOwnerKind kind = WatchpointOwnerKind::Profile;
    std::uint32_t index = 0;

    friend bool operator==(const WatchpointOwner&, const WatchpointOwner&) = default;
};

enum class WatchpointBindingSource : std::uint8_t {
    Static = 1,
    Dynamic = 2,
    Control = 3,
    Foreign = 4,
};

std::string_view watchpoint_binding_source_name(WatchpointBindingSource source);

struct WatchpointRequest {
    WatchpointOwner owner;
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    MemoryAccess access = MemoryAccess::Write;
    WatchpointBindingSource source = WatchpointBindingSource::Static;
    std::uint64_t binding_generation = 0;
};

struct WatchpointForeignRange {
    std::uint32_t start = 0;
    std::uint32_t end = 0;
};

struct WatchpointPhysicalRange {
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    bool read = false;
    bool write = false;
    bool foreign = false;

    friend bool operator==(const WatchpointPhysicalRange&, const WatchpointPhysicalRange&) = default;
};

struct WatchpointBinding {
    WatchpointOwner owner;
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    std::uint32_t physical_start = 0;
    std::uint32_t physical_end = 0;
    WatchpointBindingSource source = WatchpointBindingSource::Static;
    std::uint64_t binding_generation = 0;
};

enum class WatchpointBindingFailureReason : std::uint8_t {
    InvalidRange = 1,
    PartialSameStartForeignOverlap = 2,
    AddressDerivationFailed = 3,
};

struct WatchpointBindingFailure {
    WatchpointOwner owner;
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    WatchpointBindingFailureReason reason = WatchpointBindingFailureReason::InvalidRange;
    std::uint64_t binding_generation = 0;
};

struct WatchpointPlan {
    std::vector<WatchpointPhysicalRange> physical_ranges;
    std::vector<WatchpointBinding> bindings;
    std::vector<WatchpointBindingFailure> failures;
};

WatchpointPlan plan_watchpoint_bindings(
    std::span<const WatchpointRequest> requests,
    std::span<const WatchpointForeignRange> foreign_ranges);

class ProbeWatchpointRegistry {
public:
    void reserve(std::size_t logical_capacity);
    bool upsert(WatchpointRequest request);
    bool release(WatchpointOwner owner);
    bool release_kind(WatchpointOwnerKind kind);
    void clear_requests();

    bool reconcile(
        std::span<const WatchpointForeignRange> unmanaged_ranges = {},
        std::string* error_out = nullptr);
    void release_all();

    std::optional<WatchpointBinding> binding_for(WatchpointOwner owner) const;
    std::optional<WatchpointBindingFailure> failure_for(WatchpointOwner owner) const;
    std::span<const WatchpointRequest> requests() const { return requests_; }
    std::span<const WatchpointPhysicalRange> physical_ranges() const { return owned_physical_; }
    std::span<const WatchpointBinding> bindings() const { return bindings_; }
    std::span<const WatchpointBindingFailure> failures() const { return failures_; }

private:
    std::vector<WatchpointRequest> requests_;
    std::vector<WatchpointPhysicalRange> owned_physical_;
    std::vector<WatchpointBinding> bindings_;
    std::vector<WatchpointBindingFailure> failures_;
    std::uint64_t next_binding_generation_ = 1;
};

} // namespace savor::probe
