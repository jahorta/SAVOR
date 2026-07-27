#include "ProbeWatchpointRegistry.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <sstream>

namespace savor::probe {
namespace {

std::optional<std::uint32_t> range_end(std::uint32_t address, std::uint32_t size)
{
    if (size == 0)
        return std::nullopt;
    const auto end = static_cast<std::uint64_t>(address) + size - 1;
    if (end > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    return static_cast<std::uint32_t>(end);
}

bool covers(
    std::uint32_t outer_start,
    std::uint32_t outer_end,
    std::uint32_t inner_start,
    std::uint32_t inner_end)
{
    return outer_start <= inner_start && outer_end >= inner_end;
}

bool overlaps(
    std::uint32_t lhs_start,
    std::uint32_t lhs_end,
    std::uint32_t rhs_start,
    std::uint32_t rhs_end)
{
    return lhs_start <= rhs_end && rhs_start <= lhs_end;
}

void add_access(WatchpointPhysicalRange& range, MemoryAccess access)
{
    range.read = range.read || access == MemoryAccess::Read || access == MemoryAccess::Access;
    range.write = range.write || access == MemoryAccess::Write || access == MemoryAccess::Access;
}

std::string format_failure(const WatchpointBindingFailure& failure)
{
    std::ostringstream out;
    out << "watchpoint binding failed for "
        << (failure.owner.kind == WatchpointOwnerKind::Profile ? "profile" : "control")
        << '[' << failure.owner.index << "] at 0x" << std::hex << failure.address;
    if (failure.reason == WatchpointBindingFailureReason::PartialSameStartForeignOverlap)
        out << ": partial same-start foreign overlap";
    else
        out << ": invalid range";
    return out.str();
}

} // namespace

std::string_view watchpoint_binding_source_name(WatchpointBindingSource source)
{
    switch (source) {
    case WatchpointBindingSource::Static: return "static";
    case WatchpointBindingSource::Dynamic: return "dynamic";
    case WatchpointBindingSource::Control: return "control";
    case WatchpointBindingSource::Foreign: return "foreign";
    }
    return "unknown";
}

WatchpointPlan plan_watchpoint_bindings(
    std::span<const WatchpointRequest> requests,
    std::span<const WatchpointForeignRange> foreign_ranges)
{
    WatchpointPlan result;
    std::vector<const WatchpointRequest*> owned_requests;
    owned_requests.reserve(requests.size());

    for (const auto& request : requests) {
        const auto end = range_end(request.address, request.size);
        if (!end.has_value()) {
            result.failures.push_back(WatchpointBindingFailure{
                request.owner,
                request.address,
                request.size,
                WatchpointBindingFailureReason::InvalidRange,
                request.binding_generation,
            });
            continue;
        }

        const WatchpointForeignRange* best_cover = nullptr;
        for (const auto& foreign : foreign_ranges) {
            if (!covers(foreign.start, foreign.end, request.address, *end)) {
                continue;
            }
            if (!best_cover
                || static_cast<std::uint64_t>(foreign.end) - foreign.start
                    < static_cast<std::uint64_t>(best_cover->end) - best_cover->start) {
                best_cover = &foreign;
            }
        }
        if (best_cover) {
            result.bindings.push_back(WatchpointBinding{
                request.owner,
                request.address,
                request.size,
                best_cover->start,
                best_cover->end,
                WatchpointBindingSource::Foreign,
                request.binding_generation,
            });
            continue;
        }

        const bool partial_same_start = std::ranges::any_of(
            foreign_ranges,
            [&](const WatchpointForeignRange& foreign) {
                return foreign.start == request.address
                    && overlaps(foreign.start, foreign.end, request.address, *end);
            });
        if (partial_same_start) {
            result.failures.push_back(WatchpointBindingFailure{
                request.owner,
                request.address,
                request.size,
                WatchpointBindingFailureReason::PartialSameStartForeignOverlap,
                request.binding_generation,
            });
            continue;
        }
        owned_requests.push_back(&request);
    }

    std::ranges::sort(owned_requests, {}, [](const WatchpointRequest* request) {
        return request->address;
    });
    for (const auto* request : owned_requests) {
        const auto end = *range_end(request->address, request->size);
        if (result.physical_ranges.empty()
            || !overlaps(
                result.physical_ranges.back().start,
                result.physical_ranges.back().end,
                request->address,
                end)) {
            WatchpointPhysicalRange physical;
            physical.start = request->address;
            physical.end = end;
            add_access(physical, request->access);
            result.physical_ranges.push_back(physical);
            continue;
        }
        auto& physical = result.physical_ranges.back();
        physical.end = std::max(physical.end, end);
        add_access(physical, request->access);
    }

    for (const auto* request : owned_requests) {
        const auto end = *range_end(request->address, request->size);
        const auto physical = std::ranges::find_if(
            result.physical_ranges,
            [&](const WatchpointPhysicalRange& candidate) {
                return covers(candidate.start, candidate.end, request->address, end);
            });
        if (physical == result.physical_ranges.end())
            continue;
        result.bindings.push_back(WatchpointBinding{
            request->owner,
            request->address,
            request->size,
            physical->start,
            physical->end,
            request->source,
            request->binding_generation,
        });
    }

    std::ranges::sort(result.bindings, {}, [](const WatchpointBinding& binding) {
        return (static_cast<std::uint64_t>(binding.owner.kind) << 32) | binding.owner.index;
    });
    return result;
}

void ProbeWatchpointRegistry::reserve(std::size_t logical_capacity)
{
    requests_.reserve(logical_capacity);
    owned_physical_.reserve(logical_capacity);
    bindings_.reserve(logical_capacity);
    failures_.reserve(logical_capacity);
}

bool ProbeWatchpointRegistry::upsert(WatchpointRequest request)
{
    if (!range_end(request.address, request.size).has_value())
        return false;
    request.binding_generation = next_binding_generation_++;
    const auto existing = std::ranges::find(requests_, request.owner, &WatchpointRequest::owner);
    if (existing == requests_.end())
        requests_.push_back(request);
    else
        *existing = request;
    return true;
}

bool ProbeWatchpointRegistry::release(WatchpointOwner owner)
{
    const auto existing = std::ranges::find(requests_, owner, &WatchpointRequest::owner);
    if (existing == requests_.end())
        return false;
    requests_.erase(existing);
    return true;
}

bool ProbeWatchpointRegistry::release_kind(WatchpointOwnerKind kind)
{
    const auto old_size = requests_.size();
    std::erase_if(requests_, [kind](const WatchpointRequest& request) {
        return request.owner.kind == kind;
    });
    return requests_.size() != old_size;
}

void ProbeWatchpointRegistry::clear_requests()
{
    requests_.clear();
}

bool ProbeWatchpointRegistry::reconcile(
    std::span<const WatchpointForeignRange> unmanaged_ranges,
    std::string* error_out)
{
    if (!unmanaged_ranges.empty()) {
        if (error_out)
            *error_out = "unmanaged Dolphin memchecks are not adopted";
        return false;
    }

    auto plan = plan_watchpoint_bindings(requests_, {});
    if (!plan.failures.empty()) {
        if (error_out)
            *error_out = format_failure(plan.failures.front());
        return false;
    }
    owned_physical_ = std::move(plan.physical_ranges);
    bindings_ = std::move(plan.bindings);
    failures_.clear();
    return true;
}

void ProbeWatchpointRegistry::release_all()
{
    requests_.clear();
    owned_physical_.clear();
    bindings_.clear();
    failures_.clear();
}

std::optional<WatchpointBinding> ProbeWatchpointRegistry::binding_for(
    WatchpointOwner owner) const
{
    const auto binding = std::ranges::find(bindings_, owner, &WatchpointBinding::owner);
    if (binding == bindings_.end())
        return std::nullopt;
    return *binding;
}

std::optional<WatchpointBindingFailure> ProbeWatchpointRegistry::failure_for(
    WatchpointOwner owner) const
{
    const auto failure = std::ranges::find(failures_, owner, &WatchpointBindingFailure::owner);
    if (failure == failures_.end())
        return std::nullopt;
    return *failure;
}

} // namespace savor::probe
