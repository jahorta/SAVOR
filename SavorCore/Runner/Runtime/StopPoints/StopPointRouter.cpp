#include "StopPointRouter.h"
#include "../../../Utils/Log.h"

#include <algorithm>
#include <cassert>
#include <deque>
#include <format>
#include <iomanip>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace savor::runtime {
namespace {

constexpr std::array kCanonicalStopCpuObservers{
    CanonicalStopCpuObserverDefinition{
        CanonicalStopCpuObserver::CaptureProfile,
        "savor.capture.profile"},
    CanonicalStopCpuObserverDefinition{
        CanonicalStopCpuObserver::DerivedState,
        "savor.derived-state.refresh"},
};

static_assert([] {
    for (std::size_t i = 0; i < kCanonicalStopCpuObservers.size(); ++i)
    {
        if (CanonicalStopCpuObserverId(kCanonicalStopCpuObservers[i].key) == 0 ||
            kCanonicalStopCpuObservers[i].stable_name.empty())
        {
            return false;
        }
        for (std::size_t j = i + 1; j < kCanonicalStopCpuObservers.size(); ++j)
        {
            if (kCanonicalStopCpuObservers[i].key ==
                    kCanonicalStopCpuObservers[j].key ||
                kCanonicalStopCpuObservers[i].stable_name ==
                    kCanonicalStopCpuObservers[j].stable_name)
            {
                return false;
            }
        }
    }
    return true;
}());

struct OneShotGate
{
    std::atomic<bool> fired{false};
};

struct ForegroundOccurrenceGate
{
    std::atomic<std::uint32_t> observed{0};
};

struct SourceDropCounter
{
    StopSourceId source_id;
    std::atomic<std::uint64_t> passive_drops{0};
};

struct SubscriptionRecord
{
    StopSubscriptionDefinition definition;
    std::size_t ordinal = 0;
    std::shared_ptr<OneShotGate> one_shot;
    std::shared_ptr<ForegroundOccurrenceGate> foreground_occurrences;
};

struct GroupRecord
{
    StopSubscriptionGroupId id;
    StopSourceIdentity source;
    std::uint64_t registration_sequence = 0;
    WorksetEpoch acquisition_epoch;
    std::shared_ptr<SourceDropCounter> drop_counter;
    std::vector<SubscriptionRecord> subscriptions;
};

struct DispatchEntry
{
    StopSourceId source_id;
    std::string consumer_name;
    StopSubscriptionGroupId group_id;
    StopSubscriptionId subscription_id;
    StopPointSpec point;
    StopSubscriptionRoute route = PassiveStopObservation{};
    StopSubscriptionLifetime lifetime = StopSubscriptionLifetime::Scoped;
    std::int32_t priority = 0;
    std::uint32_t qualification_id = 0;
    std::vector<std::uint32_t> sample_descriptor_ids;
    IStopPointConsumer* consumer = nullptr;
    std::uint64_t registration_sequence = 0;
    std::size_t subscription_ordinal = 0;
    std::shared_ptr<OneShotGate> one_shot;
    std::shared_ptr<ForegroundOccurrenceGate> foreground_occurrences;
    SourceDropCounter* drop_counter = nullptr;
};

[[nodiscard]] std::string StopConsumerFailureMessage(
    const DispatchEntry& entry,
    const RoutedStopEvent& event,
    std::string_view exception_message)
{
    return std::format(
        "Stop consumer '{}' threw at PC 0x{:08X} "
        "(source={}, group={}, subscription={}): {}",
        entry.consumer_name,
        event.evidence.hit_pc,
        entry.source_id.value(),
        entry.group_id.value(),
        entry.subscription_id.value(),
        exception_message);
}

struct DispatchSnapshot
{
    StopDispatchGeneration generation;
    PhysicalPlanGeneration physical_generation;
    WorksetEpoch workset_epoch;
    PhysicalStopPointPlan physical_plan;
    std::vector<DispatchEntry> entries;
};

struct NativePacket
{
    const DispatchSnapshot* snapshot = nullptr;
    RoutedStopEvent event;
    std::array<std::uint16_t, kMaxStopDeliveriesPerHit> entry_indices{};
    std::uint16_t entry_count = 0;
    std::int32_t terminal_entry = -1;
    std::array<SourceDropCounter*, kMaxStopDeliveriesPerHit> passive_sources{};
    std::uint16_t passive_source_count = 0;
    std::array<std::uint32_t, kMaxCpuObserversPerHit> observer_descriptor_ids{};
    std::uint8_t observer_descriptor_count = 0;
    StopRouteTerminal terminal = StopRouteTerminal::None;
    bool request_break = false;
    bool physical_hit = false;
};

static_assert(std::is_trivially_destructible_v<NativePacket>);

template <std::size_t Capacity>
class SpscPacketQueue final
{
public:
    static_assert(Capacity > 1);

    [[nodiscard]] bool TryPush(NativePacket&& packet) noexcept
    {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = Increment(head);
        if (next == tail_.load(std::memory_order_acquire))
            return false;
        slots_[head] = std::move(packet);
        head_.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool TryPop(NativePacket& packet) noexcept
    {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;
        packet = std::move(slots_[tail]);
        slots_[tail] = {};
        tail_.store(Increment(tail), std::memory_order_release);
        return true;
    }

    void Clear() noexcept
    {
        NativePacket packet;
        while (TryPop(packet))
            packet = {};
    }

private:
    [[nodiscard]] static constexpr std::size_t Increment(std::size_t value) noexcept
    {
        return (value + 1) % (Capacity + 1);
    }

    std::array<NativePacket, Capacity + 1> slots_{};
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
};

class NativeEmergencySlot final
{
public:
    [[nodiscard]] bool TryPublish(NativePacket&& packet) noexcept
    {
        if (occupied_.load(std::memory_order_acquire))
            return false;
        packet_ = std::move(packet);
        occupied_.store(true, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool TryConsume(NativePacket& packet) noexcept
    {
        if (!occupied_.load(std::memory_order_acquire))
            return false;
        packet = std::move(packet_);
        packet_ = {};
        occupied_.store(false, std::memory_order_release);
        return true;
    }

    void Clear() noexcept
    {
        NativePacket packet;
        (void)TryConsume(packet);
    }

private:
    NativePacket packet_;
    std::atomic<bool> occupied_{false};
};

[[nodiscard]] bool IsPassive(
    const StopSubscriptionRoute& route) noexcept
{
    return std::holds_alternative<PassiveStopObservation>(route);
}

[[nodiscard]] bool IsForeground(
    const StopSubscriptionRoute& route) noexcept
{
    return std::holds_alternative<ForegroundStopWait>(route);
}

[[nodiscard]] bool IsInterruption(
    const StopSubscriptionRoute& route) noexcept
{
    return std::holds_alternative<TrustedStopInterruptionRequest>(route);
}

[[nodiscard]] bool IsLossless(
    const StopSubscriptionRoute& route) noexcept
{
    if (const auto* passive =
            std::get_if<PassiveStopObservation>(&route))
    {
        return passive->lossless;
    }
    return true;
}

[[nodiscard]] std::uint32_t CpuObserverDescriptor(
    const StopSubscriptionRoute& route) noexcept
{
    if (const auto* passive =
            std::get_if<PassiveStopObservation>(&route))
    {
        return passive->cpu_observer_descriptor_id;
    }
    return 0;
}

[[nodiscard]] std::string_view InterruptionHandlerKey(
    const StopSubscriptionRoute& route) noexcept
{
    if (const auto* interruption =
            std::get_if<TrustedStopInterruptionRequest>(&route))
    {
        return interruption->handler_key;
    }
    return {};
}

[[nodiscard]] int RouteStage(
    const StopSubscriptionRoute& route) noexcept
{
    return IsPassive(route) ? 0 : 1;
}

[[nodiscard]] bool AccessMatches(
    StopMemoryAccess access,
    bool write) noexcept
{
    return access == StopMemoryAccess::Access ||
        (write && access == StopMemoryAccess::Write) ||
        (!write && access == StopMemoryAccess::Read);
}

[[nodiscard]] bool RangeEnd(
    std::uint32_t address,
    std::uint32_t size,
    std::uint32_t& end) noexcept
{
    if (size == 0)
        return false;
    const std::uint64_t wide =
        static_cast<std::uint64_t>(address) + size - 1;
    if (wide > std::numeric_limits<std::uint32_t>::max())
        return false;
    end = static_cast<std::uint32_t>(wide);
    return true;
}

[[nodiscard]] bool RangesOverlap(
    std::uint32_t lhs_address,
    std::uint32_t lhs_size,
    std::uint32_t rhs_address,
    std::uint32_t rhs_size) noexcept
{
    std::uint32_t lhs_end = 0;
    std::uint32_t rhs_end = 0;
    return RangeEnd(lhs_address, lhs_size, lhs_end) &&
        RangeEnd(rhs_address, rhs_size, rhs_end) &&
        lhs_address <= rhs_end &&
        rhs_address <= lhs_end;
}

[[nodiscard]] bool PointMatches(
    const StopPointSpec& specification,
    const StopPointCpuContext& context) noexcept
{
    return std::visit(
        [&](const auto& point) -> bool {
            using Point = std::decay_t<decltype(point)>;
            if constexpr (std::is_same_v<Point, PcStopPointSpec>)
            {
                return context.path == NativeStopPath::Jit &&
                    point.pc == context.pc;
            }
            else if constexpr (std::is_same_v<Point, MemoryStopPointSpec>)
            {
                return context.path == NativeStopPath::Memcheck &&
                    AccessMatches(point.access, context.write) &&
                    RangesOverlap(
                        point.address,
                        point.size,
                        context.address,
                        context.size);
            }
            else
            {
                return context.path == NativeStopPath::Synthetic &&
                    point.identity == context.synthetic_identity;
            }
        },
        specification);
}

[[nodiscard]] bool SnapshotPhysicalPlanMatches(
    const DispatchSnapshot& snapshot,
    const StopPointCpuContext& context) noexcept
{
    if (context.path == NativeStopPath::Jit)
    {
        return std::ranges::binary_search(
            snapshot.physical_plan.pcs,
            context.pc,
            {},
            &PhysicalPcStop::pc);
    }
    if (context.path == NativeStopPath::Memcheck)
    {
        return std::ranges::any_of(
            snapshot.physical_plan.memory,
            [&](const PhysicalMemoryStop& memory) {
                const std::uint64_t access_end =
                    static_cast<std::uint64_t>(context.address) +
                    context.size - 1;
                return context.size != 0 &&
                    access_end <=
                        std::numeric_limits<std::uint32_t>::max() &&
                    context.address <= memory.end &&
                    memory.start <=
                        static_cast<std::uint32_t>(access_end) &&
                    (context.write ? memory.write : memory.read);
            });
    }
    return context.path == NativeStopPath::Synthetic;
}

[[nodiscard]] bool PointMatchesEvidence(
    const StopPointSpec& specification,
    const RoutedStopEvidence& evidence) noexcept
{
    if (specification.index() != evidence.point.index())
        return false;
    return std::visit(
        [&](const auto& expected) -> bool {
            using Point = std::decay_t<decltype(expected)>;
            const auto* actual = std::get_if<Point>(&evidence.point);
            if (!actual)
                return false;
            if constexpr (std::is_same_v<Point, MemoryStopPointSpec>)
            {
                const bool actual_write =
                    actual->access == StopMemoryAccess::Write ||
                    actual->access == StopMemoryAccess::Access;
                return AccessMatches(expected.access, actual_write) &&
                    RangesOverlap(
                        expected.address,
                        expected.size,
                        actual->address,
                        actual->size);
            }
            else
            {
                return expected == *actual;
            }
        },
        specification);
}

[[nodiscard]] StopPointError Error(
    StopPointErrorCode code,
    std::string message)
{
    return {code, std::move(message)};
}

[[nodiscard]] StopPointError PhysicalError(
    const PhysicalStopBackendReceipt& receipt)
{
    return Error(
        receipt.integrity == PhysicalStopIntegrity::Unknown
            ? StopPointErrorCode::PhysicalIntegrityUnknown
            : StopPointErrorCode::PhysicalReconcileFailed,
        receipt.message.empty()
            ? "Physical stop-point reconciliation failed"
            : receipt.message);
}

[[nodiscard]] StopDispatchGeneration NextDispatchGeneration(
    StopDispatchGeneration current) noexcept
{
    if (current.value() == std::numeric_limits<std::uint64_t>::max())
        return {};
    return StopDispatchGeneration(current.value() + 1);
}

[[nodiscard]] bool HasSample(
    const RoutedStopEvent& event,
    std::uint32_t descriptor_id) noexcept
{
    for (std::size_t i = 0; i < event.sample_count; ++i)
    {
        if (event.samples[i].descriptor_id == descriptor_id)
            return true;
    }
    return false;
}

[[nodiscard]] NativeStopPath ToNativePath(
    savor::probe::NativeStopOrigin origin) noexcept
{
    switch (origin)
    {
    case savor::probe::NativeStopOrigin::Jit:
        return NativeStopPath::Jit;
    case savor::probe::NativeStopOrigin::Memcheck:
        return NativeStopPath::Memcheck;
    }
    return NativeStopPath::Synthetic;
}

} // namespace

std::span<const CanonicalStopCpuObserverDefinition>
CanonicalStopCpuObserverDefinitions() noexcept
{
    return kCanonicalStopCpuObservers;
}

bool StopCpuObserverDispatcher::Register(
    CanonicalStopCpuObserver key,
    IStopPointCpuObserver& observer,
    std::string* error_out)
{
    const auto definitions = CanonicalStopCpuObserverDefinitions();
    const bool known = std::ranges::any_of(
        definitions,
        [key](const CanonicalStopCpuObserverDefinition& definition) {
            return definition.key == key;
        });
    if (frozen_ || !known || CanonicalStopCpuObserverId(key) == 0 ||
        std::ranges::any_of(entries_, [key](const Entry& entry) {
            return entry.key == key;
        }))
    {
        if (error_out)
        {
            *error_out = frozen_
                ? "CPU-observer dispatcher is already frozen"
                : !known
                    ? "CPU-observer descriptor is not canonical"
                    : "CPU-observer descriptor is duplicated";
        }
        return false;
    }
    entries_.push_back({key, &observer});
    return true;
}

bool StopCpuObserverDispatcher::Freeze(std::string* error_out)
{
    if (frozen_)
    {
        if (error_out)
            *error_out = "CPU-observer dispatcher is already frozen";
        return false;
    }
    std::ranges::sort(entries_, {}, [](const Entry& entry) {
        return CanonicalStopCpuObserverId(entry.key);
    });
    frozen_ = true;
    return true;
}

StopCpuObservationResult StopCpuObserverDispatcher::ObserveRoutedHit(
    std::uint32_t descriptor_id,
    const RoutedStopEvent& event) noexcept
{
    if (!frozen_ || descriptor_id == 0)
        return StopCpuObservationResult::Failed;
    const auto found = std::ranges::lower_bound(
        entries_,
        descriptor_id,
        {},
        [](const Entry& entry) {
            return CanonicalStopCpuObserverId(entry.key);
        });
    if (found == entries_.end() ||
        CanonicalStopCpuObserverId(found->key) != descriptor_id ||
        found->observer == nullptr)
    {
        return StopCpuObservationResult::Failed;
    }
    return found->observer->ObserveRoutedHit(descriptor_id, event);
}

bool StopCpuObserverDispatcher::RecognizesDescriptor(
    std::uint32_t descriptor_id) const noexcept
{
    if (!frozen_ || descriptor_id == 0)
        return false;
    const auto found = std::ranges::lower_bound(
        entries_,
        descriptor_id,
        {},
        [](const Entry& entry) {
            return CanonicalStopCpuObserverId(entry.key);
        });
    return found != entries_.end() && found->observer != nullptr &&
        CanonicalStopCpuObserverId(found->key) == descriptor_id;
}

struct StopPointLeaseControl
{
    std::mutex mutex;
    StopPointRouter* router = nullptr;
};

struct StopPointRouter::Impl
{
    std::map<std::uint64_t, GroupRecord> groups;
    std::map<std::uint64_t, std::shared_ptr<SourceDropCounter>>
        source_drop_counters;
    std::map<std::uint64_t, std::uint64_t> released_registration_sequences;
    std::optional<RoutedStopEvent> current_point;
    std::deque<StopRouteReceipt> history;
    SpscPacketQueue<kStopPointNativeIngressCapacity> ingress;
    NativeEmergencySlot emergency;
    std::atomic<const DispatchSnapshot*> dispatch{nullptr};
    std::vector<std::unique_ptr<const DispatchSnapshot>> snapshot_archive;
    std::uint64_t next_registration_sequence = 1;
    std::atomic<std::uint64_t> next_stop_sequence{1};
    std::atomic<std::uint64_t> next_sample_snapshot{1};
    bool overflow_reported = false;

    Impl()
    {
        snapshot_archive.reserve(32);
    }
};

namespace {

[[nodiscard]] bool GroupHasForegroundWait(const GroupRecord& group)
{
    return std::ranges::any_of(group.subscriptions, [](const SubscriptionRecord& subscription) {
        return IsForeground(subscription.definition.route);
    });
}

[[nodiscard]] bool DefinitionHasForegroundWait(
    const StopSubscriptionGroupDefinition& definition)
{
    return std::ranges::any_of(definition.subscriptions, [](const auto& subscription) {
        return IsForeground(subscription.route);
    });
}

[[nodiscard]] StopPointError ValidateDefinition(
    const StopSubscriptionGroupDefinition& definition,
    IStopPointCpuEvaluator* evaluator,
    IStopPointCpuObserver* observer)
{
    if (!definition.id)
        return Error(StopPointErrorCode::InvalidArgument, "Subscription group ID must be nonzero");
    if (!definition.source.id)
        return Error(StopPointErrorCode::InvalidArgument, "Stop source ID must be nonzero");
    if (definition.source.stable_name.empty())
        return Error(StopPointErrorCode::InvalidArgument, "Stop source stable name is required");
    if (definition.subscriptions.empty())
        return Error(StopPointErrorCode::InvalidArgument, "A subscription group cannot be empty");
    if (definition.subscriptions.size() > kMaxLogicalStopSubscriptions)
    {
        return Error(
            StopPointErrorCode::InvalidArgument,
            "A subscription group exceeds the bounded logical capacity");
    }

    std::set<std::uint64_t> subscription_ids;
    std::set<std::uint32_t> sample_ids;
    for (const StopSubscriptionDefinition& subscription : definition.subscriptions)
    {
        if (!subscription.id)
            return Error(StopPointErrorCode::InvalidArgument, "Subscription ID must be nonzero");
        if (!subscription_ids.insert(subscription.id.value()).second)
            return Error(StopPointErrorCode::InvalidArgument, "Subscription IDs must be unique in a group");
        if (subscription.consumer == nullptr)
            return Error(StopPointErrorCode::InvalidArgument, "Every stop subscription requires a consumer");
        if ((subscription.qualification_id != 0 ||
                !subscription.sample_descriptor_ids.empty()) &&
            evaluator == nullptr)
        {
            return Error(
                StopPointErrorCode::InvalidArgument,
                "Qualified or sampled subscriptions require a CPU evaluator");
        }
        const std::uint32_t cpu_observer_descriptor_id =
            CpuObserverDescriptor(subscription.route);
        if (cpu_observer_descriptor_id != 0 &&
            (observer == nullptr ||
             !observer->RecognizesDescriptor(
                 cpu_observer_descriptor_id)))
        {
            return Error(
                StopPointErrorCode::InvalidArgument,
                "CPU observer descriptor is not registered by the frozen trusted observer port");
        }
        if (const auto* foreground =
                std::get_if<ForegroundStopWait>(&subscription.route))
        {
            if (foreground->required_occurrences == 0)
            {
                return Error(
                    StopPointErrorCode::InvalidArgument,
                    "Foreground occurrence count must be nonzero");
            }
            if (foreground->sample_expectation &&
                (foreground->sample_expectation->descriptor_id == 0 ||
                 !std::ranges::contains(
                     subscription.sample_descriptor_ids,
                     foreground->sample_expectation->descriptor_id)))
            {
                return Error(
                    StopPointErrorCode::InvalidArgument,
                    "Foreground sample expectation requires its routed sample descriptor");
            }
        }
        if (subscription.sample_descriptor_ids.size() > kMaxRoutedHitSamples)
        {
            return Error(
                StopPointErrorCode::InvalidArgument,
                "A subscription exceeds the hit-time sample capacity");
        }
        for (const std::uint32_t id : subscription.sample_descriptor_ids)
        {
            if (id == 0)
                return Error(StopPointErrorCode::InvalidArgument, "Sample descriptor IDs must be nonzero");
            sample_ids.insert(id);
        }
        if (sample_ids.size() > kMaxRoutedHitSamples)
        {
            return Error(
                StopPointErrorCode::InvalidArgument,
                "A subscription group exceeds the unique hit-time sample capacity");
        }
        if (const auto* interruption =
                std::get_if<TrustedStopInterruptionRequest>(
                    &subscription.route);
            interruption && interruption->handler_key.empty())
        {
            return Error(
                StopPointErrorCode::InvalidPolicy,
                "Trusted interruption requests require a non-empty handler key");
        }
        const StopPointError point_error = std::visit(
            [](const auto& point) -> StopPointError {
                using Point = std::decay_t<decltype(point)>;
                if constexpr (std::is_same_v<Point, PcStopPointSpec>)
                {
                    if (point.pc == 0)
                        return Error(StopPointErrorCode::InvalidArgument, "PC stop address must be nonzero");
                }
                else if constexpr (std::is_same_v<Point, MemoryStopPointSpec>)
                {
                    std::uint32_t end = 0;
                    if (point.address == 0 || !RangeEnd(point.address, point.size, end))
                    {
                        return Error(
                            StopPointErrorCode::InvalidArgument,
                            "Memory stop range is invalid");
                    }
                }
                else
                {
                    if (point.identity == 0)
                    {
                        return Error(
                            StopPointErrorCode::InvalidArgument,
                            "Synthetic stop identity must be nonzero");
                    }
                }
                return {};
            },
            subscription.point);
        if (point_error)
            return point_error;
    }
    return {};
}

[[nodiscard]] GroupRecord MakeGroupRecord(
    StopSubscriptionGroupDefinition definition,
    std::uint64_t registration_sequence,
    WorksetEpoch acquisition_epoch,
    std::shared_ptr<SourceDropCounter> drop_counter,
    const std::optional<RoutedStopEvent>& current_point)
{
    GroupRecord record;
    record.id = definition.id;
    record.source = std::move(definition.source);
    record.registration_sequence = registration_sequence;
    record.acquisition_epoch = acquisition_epoch;
    record.drop_counter = std::move(drop_counter);
    record.subscriptions.reserve(definition.subscriptions.size());
    for (std::size_t i = 0; i < definition.subscriptions.size(); ++i)
    {
        SubscriptionRecord subscription;
        subscription.definition = std::move(definition.subscriptions[i]);
        subscription.ordinal = i;
        if (subscription.definition.lifetime == StopSubscriptionLifetime::OneShot)
            subscription.one_shot = std::make_shared<OneShotGate>();
        if (std::holds_alternative<ForegroundStopWait>(
                subscription.definition.route))
        {
            subscription.foreground_occurrences =
                std::make_shared<ForegroundOccurrenceGate>();
        }
        record.subscriptions.push_back(std::move(subscription));
    }
    return record;
}

[[nodiscard]] PhysicalStopPointPlan BuildPhysicalPlan(
    const std::map<std::uint64_t, GroupRecord>& groups)
{
    PhysicalStopPointPlan plan;
    for (const auto& [_, group] : groups)
    {
        for (const SubscriptionRecord& subscription : group.subscriptions)
        {
            if (subscription.one_shot &&
                subscription.one_shot->fired.load(std::memory_order_acquire))
            {
                continue;
            }
            std::visit(
                [&](const auto& point) {
                    using Point = std::decay_t<decltype(point)>;
                    if constexpr (std::is_same_v<Point, PcStopPointSpec>)
                    {
                        plan.pcs.push_back({point.pc});
                    }
                    else if constexpr (std::is_same_v<Point, MemoryStopPointSpec>)
                    {
                        std::uint32_t end = 0;
                        if (!RangeEnd(point.address, point.size, end))
                            return;
                        plan.memory.push_back({
                            point.address,
                            end,
                            point.access == StopMemoryAccess::Read ||
                                point.access == StopMemoryAccess::Access,
                            point.access == StopMemoryAccess::Write ||
                                point.access == StopMemoryAccess::Access,
                        });
                    }
                },
                subscription.definition.point);
        }
    }

    std::ranges::sort(plan.pcs, {}, &PhysicalPcStop::pc);
    plan.pcs.erase(
        std::unique(plan.pcs.begin(), plan.pcs.end()),
        plan.pcs.end());

    std::ranges::sort(plan.memory, [](const auto& lhs, const auto& rhs) {
        if (lhs.start != rhs.start)
            return lhs.start < rhs.start;
        return lhs.end < rhs.end;
    });
    std::vector<PhysicalMemoryStop> coalesced;
    coalesced.reserve(plan.memory.size());
    for (const PhysicalMemoryStop& memory : plan.memory)
    {
        if (coalesced.empty() || memory.start > coalesced.back().end)
        {
            coalesced.push_back(memory);
            continue;
        }
        PhysicalMemoryStop& existing = coalesced.back();
        existing.end = std::max(existing.end, memory.end);
        existing.read = existing.read || memory.read;
        existing.write = existing.write || memory.write;
    }
    plan.memory = std::move(coalesced);
    return plan;
}

[[nodiscard]] std::unique_ptr<DispatchSnapshot> BuildSnapshot(
    const std::map<std::uint64_t, GroupRecord>& groups,
    WorksetEpoch epoch,
    StopDispatchGeneration generation,
    PhysicalPlanGeneration physical_generation)
{
    auto snapshot = std::make_unique<DispatchSnapshot>();
    snapshot->generation = generation;
    snapshot->physical_generation = physical_generation;
    snapshot->workset_epoch = epoch;
    snapshot->physical_plan = BuildPhysicalPlan(groups);
    for (const auto& [_, group] : groups)
    {
        for (const SubscriptionRecord& subscription : group.subscriptions)
        {
            if (subscription.one_shot &&
                subscription.one_shot->fired.load(std::memory_order_acquire))
            {
                continue;
            }
            const StopSubscriptionDefinition& definition = subscription.definition;
            const std::string consumer_name =
                group.source.diagnostic_label.empty()
                ? group.source.stable_name
                : group.source.diagnostic_label + " [" +
                    group.source.stable_name + "]";
            snapshot->entries.push_back({
                group.source.id,
                consumer_name,
                group.id,
                definition.id,
                definition.point,
                definition.route,
                definition.lifetime,
                definition.priority,
                definition.qualification_id,
                definition.sample_descriptor_ids,
                definition.consumer,
                group.registration_sequence,
                subscription.ordinal,
                subscription.one_shot,
                subscription.foreground_occurrences,
                group.drop_counter.get(),
            });
        }
    }

    std::stable_sort(snapshot->entries.begin(), snapshot->entries.end(), [](const auto& lhs, const auto& rhs) {
        const int lhs_stage = RouteStage(lhs.route);
        const int rhs_stage = RouteStage(rhs.route);
        if (lhs_stage != rhs_stage)
            return lhs_stage < rhs_stage;
        if (lhs.priority != rhs.priority)
            return lhs.priority > rhs.priority;
        if (lhs.registration_sequence != rhs.registration_sequence)
            return lhs.registration_sequence < rhs.registration_sequence;
        return lhs.subscription_ordinal < rhs.subscription_ordinal;
    });
    return snapshot;
}

[[nodiscard]] StopPointError ValidateSnapshotMatchCapacity(
    const DispatchSnapshot& snapshot)
{
    const auto validate_context = [&](const StopPointCpuContext& context) {
        std::size_t matches = 0;
        std::set<std::uint32_t> samples;
        std::set<std::uint32_t> observers;
        for (const DispatchEntry& entry : snapshot.entries)
        {
            if (!PointMatches(entry.point, context))
                continue;
            ++matches;
            samples.insert(
                entry.sample_descriptor_ids.begin(),
                entry.sample_descriptor_ids.end());
            const std::uint32_t observer =
                CpuObserverDescriptor(entry.route);
            if (observer != 0)
                observers.insert(observer);
        }
        if (matches > kMaxStopDeliveriesPerHit)
        {
            return Error(
                StopPointErrorCode::InvalidArgument,
                "A physical hit can exceed the fixed delivery capacity");
        }
        if (samples.size() > kMaxRoutedHitSamples)
        {
            return Error(
                StopPointErrorCode::InvalidArgument,
                "A physical hit can exceed the fixed sample capacity");
        }
        if (observers.size() > kMaxCpuObserversPerHit)
        {
            return Error(
                StopPointErrorCode::InvalidArgument,
                "A physical hit can exceed the trusted observer capacity");
        }
        return StopPointError{};
    };

    for (const DispatchEntry& entry : snapshot.entries)
    {
        StopPointError error;
        std::visit(
            [&](const auto& point) {
                using Point = std::decay_t<decltype(point)>;
                if constexpr (std::is_same_v<Point, PcStopPointSpec>)
                {
                    StopPointCpuContext context;
                    context.path = NativeStopPath::Jit;
                    context.pc = point.pc;
                    error = validate_context(context);
                }
                else if constexpr (std::is_same_v<Point, MemoryStopPointSpec>)
                {
                    const auto check_access = [&](bool write) {
                        StopPointCpuContext context;
                        context.path = NativeStopPath::Memcheck;
                        context.address = point.address;
                        context.size = 1;
                        context.write = write;
                        return validate_context(context);
                    };
                    if (point.access != StopMemoryAccess::Write)
                        error = check_access(false);
                    if (!error && point.access != StopMemoryAccess::Read)
                        error = check_access(true);
                }
                else
                {
                    StopPointCpuContext context;
                    context.path = NativeStopPath::Synthetic;
                    context.synthetic_identity = point.identity;
                    error = validate_context(context);
                }
            },
            entry.point);
        if (error)
            return error;
    }
    return {};
}

[[nodiscard]] StopRouteReceipt FailureRoute(
    StopRouteTerminal terminal,
    StopPointError error,
    bool remain_stopped = true)
{
    StopRouteReceipt receipt;
    receipt.terminal = terminal;
    receipt.error = std::move(error);
    receipt.core_must_remain_stopped = remain_stopped;
    return receipt;
}

void AppendHistory(
    StopPointRouter::Impl& impl,
    const StopRouteReceipt& receipt)
{
    if (impl.history.size() == kStopPointRoutingHistoryCapacity)
        impl.history.pop_front();
    impl.history.push_back(receipt);
}

[[nodiscard]] bool CurrentPointMatchesDefinition(
    const RoutedStopEvent& current,
    const StopSubscriptionGroupDefinition& definition)
{
    if (current.identity.workset_epoch.value() == 0)
        return false;
    for (const StopSubscriptionDefinition& subscription : definition.subscriptions)
    {
        if (subscription.qualification_id != 0)
            continue;
        if (!PointMatchesEvidence(subscription.point, current.evidence))
            continue;
        const bool has_samples = std::ranges::all_of(
            subscription.sample_descriptor_ids,
            [&](std::uint32_t descriptor_id) {
                return HasSample(current, descriptor_id);
            });
        if (has_samples)
            return true;
    }
    return false;
}

} // namespace

StopSubscriptionGroupHandle::StopSubscriptionGroupHandle(
    std::weak_ptr<StopPointLeaseControl> control,
    StopSubscriptionGroupLease lease) noexcept
    : control_(std::move(control)),
      lease_(lease)
{
}

StopSubscriptionGroupHandle::~StopSubscriptionGroupHandle()
{
    ReleaseNoThrow();
}

StopSubscriptionGroupHandle::StopSubscriptionGroupHandle(
    StopSubscriptionGroupHandle&& other) noexcept
    : control_(std::move(other.control_)),
      lease_(other.lease_)
{
    other.lease_ = {};
}

StopSubscriptionGroupHandle& StopSubscriptionGroupHandle::operator=(
    StopSubscriptionGroupHandle&& other) noexcept
{
    if (this == &other)
        return *this;
    ReleaseNoThrow();
    control_ = std::move(other.control_);
    lease_ = other.lease_;
    other.lease_ = {};
    return *this;
}

StopGroupReceipt StopSubscriptionGroupHandle::Replace(
    StopSubscriptionGroupDefinition definition)
{
    const auto control = control_.lock();
    if (!control)
    {
        return {
            false,
            lease_,
            {},
            {},
            Error(StopPointErrorCode::RuntimeStopping, "Stop-point router no longer exists"),
        };
    }
    std::lock_guard lock(control->mutex);
    if (!control->router)
    {
        return {
            false,
            lease_,
            {},
            {},
            Error(StopPointErrorCode::RuntimeStopping, "Stop-point router is stopping"),
        };
    }
    return control->router->ReplaceFromHandle(lease_, std::move(definition));
}

StopReleaseReceipt StopSubscriptionGroupHandle::Release()
{
    if (!lease_.active)
    {
        return {
            true,
            true,
            lease_.group_id,
            {},
            {},
            {},
        };
    }
    const auto control = control_.lock();
    if (!control)
    {
        lease_.active = false;
        return {
            true,
            true,
            lease_.group_id,
            {},
            {},
            {},
        };
    }
    std::lock_guard lock(control->mutex);
    if (!control->router)
    {
        lease_.active = false;
        return {
            true,
            true,
            lease_.group_id,
            {},
            {},
            {},
        };
    }
    return control->router->ReleaseFromHandle(lease_);
}

void StopSubscriptionGroupHandle::ReleaseNoThrow() noexcept
{
    if (!lease_.active)
        return;
    try
    {
        const auto control = control_.lock();
        if (!control)
        {
            lease_.active = false;
            return;
        }
        std::lock_guard lock(control->mutex);
        if (!control->router)
        {
            lease_.active = false;
            return;
        }
        control->router->ReleaseFromHandleNoThrow(lease_);
    }
    catch (...)
    {
    }
}

StopPointRouter::StopPointRouter(
    PhysicalStopPointManager& physical_manager,
    IStopPointCpuEvaluator* cpu_evaluator,
    IStopPointCpuObserver* cpu_observer,
    HostActivityTracker* host_activity)
    : impl_(std::make_unique<Impl>()),
      physical_manager_(physical_manager),
      cpu_evaluator_(cpu_evaluator),
      cpu_observer_(cpu_observer),
      host_activity_(host_activity),
      lease_control_(std::make_shared<StopPointLeaseControl>()),
      owner_thread_(std::this_thread::get_id())
{
    lease_control_->router = this;
}

StopPointRouter::~StopPointRouter()
{
    ingress_enabled_.store(false, std::memory_order_release);
    if (initialized_ && owner_thread_ == std::this_thread::get_id() && !stopped_)
        (void)StopIngressDrainAndCleanup();
    std::lock_guard lock(lease_control_->mutex);
    lease_control_->router = nullptr;
}

namespace {

[[nodiscard]] StopPointLifecycleReceipt LifecycleFailure(
    const StopPointRouter& router,
    StopPointError error,
    PhysicalStopIntegrity integrity = PhysicalStopIntegrity::Preserved)
{
    return {
        false,
        router.workset_epoch(),
        router.dispatch_generation(),
        {},
        integrity,
        0,
        std::move(error),
    };
}

[[nodiscard]] StopPointLifecycleReceipt LifecycleSuccess(
    const StopPointRouter& router,
    PhysicalPlanGeneration physical_generation,
    std::size_t drained = 0)
{
    return {
        true,
        router.workset_epoch(),
        router.dispatch_generation(),
        physical_generation,
        PhysicalStopIntegrity::Preserved,
        drained,
        {},
    };
}

} // namespace

StopPointLifecycleReceipt StopPointRouter::Initialize(WorksetEpoch first_epoch)
{
    if (owner_thread_ != std::this_thread::get_id())
    {
        return LifecycleFailure(
            *this,
            Error(
                StopPointErrorCode::WrongThread,
                "Stop-point router initialization used the wrong thread"));
    }
    if (initialized_)
    {
        if (first_epoch == workset_epoch_ && !stopped_)
            return LifecycleSuccess(*this, physical_manager_.generation());
        return LifecycleFailure(
            *this,
            Error(StopPointErrorCode::InvalidArgument, "Stop-point router is already initialized"));
    }
    if (!first_epoch)
    {
        return LifecycleFailure(
            *this,
            Error(StopPointErrorCode::InvalidArgument, "The first WorksetEpoch must be nonzero"));
    }

    workset_epoch_ = first_epoch;
    dispatch_generation_ = StopDispatchGeneration(1);

    PhysicalStopBackendReceipt bind = physical_manager_.BindNativeStopSink(*this);
    if (!bind.ok)
    {
        workset_epoch_ = {};
        dispatch_generation_ = {};
        return LifecycleFailure(*this, PhysicalError(bind), bind.integrity);
    }

    const PhysicalPlanGeneration first_physical_generation =
        physical_manager_.next_generation();
    if (!first_physical_generation)
    {
        (void)physical_manager_.UnbindNativeStopSink();
        workset_epoch_ = {};
        dispatch_generation_ = {};
        return LifecycleFailure(
            *this,
            Error(
                StopPointErrorCode::PhysicalIntegrityUnknown,
                "Physical stop-point generation exhausted"),
            PhysicalStopIntegrity::Unknown);
    }
    auto snapshot = BuildSnapshot(
        impl_->groups,
        first_epoch,
        dispatch_generation_,
        first_physical_generation);
    const DispatchSnapshot* snapshot_pointer = snapshot.get();
    impl_->snapshot_archive.push_back(std::move(snapshot));
    PhysicalStopBackendReceipt apply = physical_manager_.ApplyExactPlan(
        snapshot_pointer->physical_plan,
        [&] {
            impl_->dispatch.store(
                snapshot_pointer,
                std::memory_order_release);
        },
        true);
    if (!apply.ok)
    {
        (void)physical_manager_.UnbindNativeStopSink();
        impl_->dispatch.store(nullptr, std::memory_order_release);
        workset_epoch_ = {};
        dispatch_generation_ = {};
        return LifecycleFailure(*this, PhysicalError(apply), apply.integrity);
    }

    initialized_ = true;
    stopped_ = false;
    stopping_ = false;
    ingress_enabled_.store(true, std::memory_order_release);
    return LifecycleSuccess(*this, apply.generation);
}

namespace {

[[nodiscard]] StopPointError CheckControlThread(
    const StopPointRouter& router,
    std::thread::id owner,
    bool initialized,
    bool stopping)
{
    if (!initialized)
        return Error(StopPointErrorCode::InvalidArgument, "Stop-point router is not initialized");
    if (owner != std::this_thread::get_id())
        return Error(StopPointErrorCode::WrongThread, "Stop-point control operation used the wrong thread");
    if (stopping)
        return Error(StopPointErrorCode::RuntimeStopping, "Stop-point router is stopping");
    (void)router;
    return {};
}

} // namespace


namespace {

[[nodiscard]] bool ControlPointsOverlap(
    const StopPointSpec& lhs,
    const StopPointSpec& rhs) noexcept
{
    if (const auto* lhs_pc = std::get_if<PcStopPointSpec>(&lhs))
    {
        const auto* rhs_pc = std::get_if<PcStopPointSpec>(&rhs);
        return rhs_pc != nullptr && lhs_pc->pc == rhs_pc->pc;
    }
    if (const auto* lhs_memory =
            std::get_if<MemoryStopPointSpec>(&lhs))
    {
        const auto* rhs_memory =
            std::get_if<MemoryStopPointSpec>(&rhs);
        if (rhs_memory == nullptr ||
            !RangesOverlap(
                lhs_memory->address,
                lhs_memory->size,
                rhs_memory->address,
                rhs_memory->size))
        {
            return false;
        }
        return lhs_memory->access == StopMemoryAccess::Access ||
            rhs_memory->access == StopMemoryAccess::Access ||
            lhs_memory->access == rhs_memory->access;
    }
    const auto* lhs_synthetic =
        std::get_if<SyntheticStopPointSpec>(&lhs);
    const auto* rhs_synthetic =
        std::get_if<SyntheticStopPointSpec>(&rhs);
    return lhs_synthetic != nullptr && rhs_synthetic != nullptr &&
        lhs_synthetic->identity == rhs_synthetic->identity;
}

[[nodiscard]] StopPointError ValidateCandidate(
    const std::map<std::uint64_t, GroupRecord>& groups)
{
    std::optional<std::uint64_t> foreground_group;
    struct ControlOwner
    {
        std::uint64_t group_id = 0;
        bool foreground = false;
        StopPointSpec point;
    };
    std::vector<ControlOwner> control_owners;
    std::set<std::uint64_t> subscription_ids;
    std::size_t entry_count = 0;
    for (const auto& [group_id, group] : groups)
    {
        if (GroupHasForegroundWait(group))
        {
            if (foreground_group && *foreground_group != group_id)
            {
                return Error(
                    StopPointErrorCode::ForegroundWaitAlreadyRegistered,
                    "Only one foreground-wait group may be active");
            }
            foreground_group = group_id;
        }
        for (const SubscriptionRecord& subscription : group.subscriptions)
        {
            if (!subscription_ids.insert(subscription.definition.id.value()).second)
            {
                return Error(
                    StopPointErrorCode::InvalidArgument,
                    "Subscription IDs must be unique across active groups");
            }
            const bool foreground =
                IsForeground(subscription.definition.route);
            const bool interruption =
                IsInterruption(subscription.definition.route);
            if (foreground || interruption)
            {
                for (const ControlOwner& existing : control_owners)
                {
                    if (!ControlPointsOverlap(
                            existing.point,
                            subscription.definition.point))
                    {
                        continue;
                    }
                    if (existing.foreground != foreground ||
                        !foreground)
                    {
                        return Error(
                            StopPointErrorCode::ControlOwnershipConflict,
                            "A stop point cannot be owned by both foreground waiting and trusted interruption, or by multiple interruption requests");
                    }
                }
                control_owners.push_back({
                    group_id,
                    foreground,
                    subscription.definition.point});
            }
            ++entry_count;
        }
    }
    if (entry_count > kMaxLogicalStopSubscriptions)
    {
        return Error(
            StopPointErrorCode::InvalidArgument,
            "Active subscriptions exceed the bounded logical capacity");
    }
    return {};
}

[[nodiscard]] bool ArchiveSnapshot(
    StopPointRouter::Impl& impl,
    std::unique_ptr<const DispatchSnapshot> snapshot)
{
    try
    {
        impl.snapshot_archive.push_back(std::move(snapshot));
        return true;
    }
    catch (...)
    {
        return false;
    }
}

struct CandidateApplyReceipt
{
    bool ok = false;
    StopDispatchGeneration dispatch_generation;
    PhysicalPlanGeneration physical_generation;
    PhysicalStopIntegrity integrity = PhysicalStopIntegrity::Preserved;
    StopPointError error;
};

[[nodiscard]] CandidateApplyReceipt ApplyCandidate(
    StopPointRouter::Impl& impl,
    PhysicalStopPointManager& physical_manager,
    std::map<std::uint64_t, GroupRecord> candidate,
    WorksetEpoch epoch,
    StopDispatchGeneration& current_generation,
    bool force_physical_reconcile)
{
    if (StopPointError error = ValidateCandidate(candidate))
        return {false, current_generation, physical_manager.generation(), PhysicalStopIntegrity::Preserved, std::move(error)};

    const StopDispatchGeneration next =
        NextDispatchGeneration(current_generation);
    if (!next)
    {
        return {
            false,
            current_generation,
            physical_manager.generation(),
            PhysicalStopIntegrity::Unknown,
            Error(StopPointErrorCode::PhysicalIntegrityUnknown, "Stop dispatch generation exhausted"),
        };
    }

    std::unique_ptr<DispatchSnapshot> snapshot;
    std::shared_ptr<std::map<std::uint64_t, GroupRecord>> candidate_state;
    try
    {
        snapshot = BuildSnapshot(candidate, epoch, next, {});
        if (StopPointError capacity =
                ValidateSnapshotMatchCapacity(*snapshot))
        {
            return {
                false,
                current_generation,
                physical_manager.generation(),
                PhysicalStopIntegrity::Preserved,
                std::move(capacity)};
        }
        const bool unchanged_physical =
            !force_physical_reconcile &&
            physical_manager.has_exact_plan() &&
            snapshot->physical_plan == physical_manager.current_plan();
        snapshot->physical_generation = unchanged_physical
            ? physical_manager.generation()
            : physical_manager.next_generation();
        if (!snapshot->physical_generation)
        {
            return {
                false,
                current_generation,
                physical_manager.generation(),
                PhysicalStopIntegrity::Unknown,
                Error(
                    StopPointErrorCode::PhysicalIntegrityUnknown,
                    "Physical stop-point generation exhausted"),
            };
        }
        candidate_state =
            std::make_shared<std::map<std::uint64_t, GroupRecord>>(std::move(candidate));
    }
    catch (const std::exception& ex)
    {
        return {
            false,
            current_generation,
            physical_manager.generation(),
            PhysicalStopIntegrity::Preserved,
            Error(
                StopPointErrorCode::InvalidArgument,
                std::string("Failed preparing stop dispatch: ") + ex.what()),
        };
    }
    catch (...)
    {
        return {
            false,
            current_generation,
            physical_manager.generation(),
            PhysicalStopIntegrity::Preserved,
            Error(StopPointErrorCode::InvalidArgument, "Failed preparing stop dispatch"),
        };
    }

    const DispatchSnapshot* snapshot_pointer = snapshot.get();
    if (!ArchiveSnapshot(impl, std::move(snapshot)))
    {
        return {
            false,
            current_generation,
            physical_manager.generation(),
            PhysicalStopIntegrity::Preserved,
            Error(StopPointErrorCode::InvalidArgument, "Failed retaining immutable stop dispatch"),
        };
    }

    PhysicalStopBackendReceipt physical = physical_manager.ApplyExactPlan(
        snapshot_pointer->physical_plan,
        [&] {
            impl.groups.swap(*candidate_state);
            current_generation = next;
            impl.dispatch.store(snapshot_pointer, std::memory_order_release);
        },
        force_physical_reconcile);
    if (!physical.ok)
    {
        return {
            false,
            current_generation,
            physical.generation,
            physical.integrity,
            PhysicalError(physical),
        };
    }
    return {
        true,
        current_generation,
        physical.generation,
        physical.integrity,
        {},
    };
}

} // namespace


StopPointLifecycleReceipt StopPointRouter::QuiesceForWorksetBaselineRestore()
{
    if (StopPointError error =
            CheckControlThread(*this, owner_thread_, initialized_, stopping_))
    {
        return LifecycleFailure(*this, std::move(error));
    }
    ingress_enabled_.store(false, std::memory_order_release);
    WaitForNativeIngressQuiescence();
    const std::vector<StopRouteReceipt> drained = DrainIngress();
    return LifecycleSuccess(
        *this,
        physical_manager_.generation(),
        drained.size());
}

StopPointLifecycleReceipt
StopPointRouter::ReconcileAfterWorksetBaselineRestore()
{
    if (StopPointError error =
            CheckControlThread(*this, owner_thread_, initialized_, stopping_))
    {
        return LifecycleFailure(*this, std::move(error));
    }
    if (ingress_enabled_.load(std::memory_order_acquire))
    {
        return LifecycleFailure(
            *this,
            Error(
                StopPointErrorCode::InvalidState,
                "Workset baseline reconciliation requires quiesced ingress"));
    }

    CandidateApplyReceipt applied = ApplyCandidate(
        *impl_,
        physical_manager_,
        impl_->groups,
        workset_epoch_,
        dispatch_generation_,
        true);
    if (!applied.ok)
    {
        return LifecycleFailure(
            *this,
            std::move(applied.error),
            applied.integrity);
    }

    impl_->current_point.reset();
    authoritative_overflow_.store(false, std::memory_order_release);
    impl_->overflow_reported = false;
    ingress_enabled_.store(true, std::memory_order_release);
    return LifecycleSuccess(
        *this,
        applied.physical_generation);
}

StopPointLifecycleReceipt
StopPointRouter::ResumeAfterFailedWorksetBaselineRestore()
{
    if (StopPointError error =
            CheckControlThread(*this, owner_thread_, initialized_, stopping_))
    {
        return LifecycleFailure(*this, std::move(error));
    }
    if (ingress_enabled_.load(std::memory_order_acquire))
    {
        return LifecycleFailure(
            *this,
            Error(
                StopPointErrorCode::InvalidState,
                "Failed workset baseline restoration did not retain quiesced ingress"));
    }
    PhysicalStopBackendReceipt physical =
        physical_manager_.ValidateExactPlanUnchanged();
    if (!physical.ok)
    {
        return LifecycleFailure(
            *this,
            PhysicalError(physical),
            physical.integrity);
    }
    impl_->current_point.reset();
    authoritative_overflow_.store(false, std::memory_order_release);
    impl_->overflow_reported = false;
    ingress_enabled_.store(true, std::memory_order_release);
    return LifecycleSuccess(*this, physical.generation);
}

StopPointLifecycleReceipt StopPointRouter::ValidateEmptyForMovieCoreStop()
{
    if (StopPointError error =
            CheckControlThread(*this, owner_thread_, initialized_, stopping_))
    {
        return LifecycleFailure(*this, std::move(error));
    }

    const PhysicalStopPointPlan desired = DesiredPhysicalPlan();
    if (!desired.pcs.empty() || !desired.memory.empty())
    {
        return LifecycleFailure(
            *this,
            Error(
                StopPointErrorCode::InvalidState,
                "Movie core stop requires an empty program-owned stop-point plan"));
    }
    PhysicalStopBackendReceipt physical =
        physical_manager_.ValidateExactPlanUnchanged();
    if (!physical.ok)
    {
        return LifecycleFailure(
            *this,
            PhysicalError(physical),
            physical.integrity);
    }
    return LifecycleSuccess(*this, physical.generation);
}

StopPointLifecycleReceipt StopPointRouter::EnterStoppedMovieCoreBoundary()
{
    if (StopPointError error =
            CheckControlThread(*this, owner_thread_, initialized_, stopping_))
    {
        return LifecycleFailure(*this, std::move(error));
    }

    // Core shutdown can enqueue JIT/debugger notifications even though the
    // program-owned plan was empty. Quiesce and advance the generations before
    // a module installs the first breakpoint so no pre-stop packet can be
    // interpreted against that new subscription.
    ingress_enabled_.store(false, std::memory_order_release);
    WaitForNativeIngressQuiescence();
    StopPointLifecycleReceipt revalidated = RevalidateAfterJit();
    if (!revalidated.ok)
        return revalidated;
    const std::vector<StopRouteReceipt> stale = DrainIngress();
    impl_->current_point.reset();
    authoritative_overflow_.store(false, std::memory_order_release);
    impl_->overflow_reported = false;
    ingress_enabled_.store(true, std::memory_order_release);
    return LifecycleSuccess(
        *this,
        revalidated.physical_generation,
        stale.size());
}

StopPointLifecycleReceipt StopPointRouter::RevalidateAfterJit()
{
    if (StopPointError error =
            CheckControlThread(*this, owner_thread_, initialized_, stopping_))
    {
        return LifecycleFailure(*this, std::move(error));
    }
    const StopDispatchGeneration next_dispatch =
        NextDispatchGeneration(dispatch_generation_);
    const PhysicalPlanGeneration next_physical =
        physical_manager_.next_generation();
    if (!next_dispatch || !next_physical)
    {
        ingress_enabled_.store(false, std::memory_order_release);
        return LifecycleFailure(
            *this,
            Error(
                StopPointErrorCode::PhysicalIntegrityUnknown,
                "Stop-point generation exhausted during JIT revalidation"),
            PhysicalStopIntegrity::Unknown);
    }

    std::unique_ptr<DispatchSnapshot> snapshot;
    try
    {
        snapshot = BuildSnapshot(
            impl_->groups,
            workset_epoch_,
            next_dispatch,
            next_physical);
    }
    catch (...)
    {
        return LifecycleFailure(
            *this,
            Error(
                StopPointErrorCode::InvalidArgument,
                "Failed preparing JIT-revalidated stop dispatch"));
    }
    const DispatchSnapshot* snapshot_pointer = snapshot.get();
    if (!ArchiveSnapshot(*impl_, std::move(snapshot)))
    {
        return LifecycleFailure(
            *this,
            Error(
                StopPointErrorCode::InvalidArgument,
                "Failed retaining JIT-revalidated stop dispatch"));
    }

    PhysicalStopBackendReceipt physical = physical_manager_.RevalidateAfterJit(
        [&] {
            dispatch_generation_ = next_dispatch;
            impl_->dispatch.store(
                snapshot_pointer,
                std::memory_order_release);
        });
    if (!physical.ok)
    {
        ingress_enabled_.store(false, std::memory_order_release);
        return LifecycleFailure(*this, PhysicalError(physical), physical.integrity);
    }
    return LifecycleSuccess(*this, physical.generation);
}

StopPointLifecycleReceipt StopPointRouter::ValidateBreakpointChangeNotification()
{
    if (StopPointError error =
            CheckControlThread(*this, owner_thread_, initialized_, stopping_))
    {
        return LifecycleFailure(*this, std::move(error));
    }
    PhysicalStopBackendReceipt physical =
        physical_manager_.ValidateExactPlanUnchanged();
    if (!physical.ok)
    {
        ingress_enabled_.store(false, std::memory_order_release);
        return LifecycleFailure(*this, PhysicalError(physical), physical.integrity);
    }
    return LifecycleSuccess(*this, physical.generation);
}

StopPointError StopPointRouter::SetIngressNotificationCounter(
    std::atomic<std::uint64_t>* counter)
{
    if (owner_thread_ != std::this_thread::get_id())
    {
        return Error(
            StopPointErrorCode::WrongThread,
            "Ingress notification must be configured by the router owner");
    }
    if (ingress_enabled_.load(std::memory_order_acquire))
    {
        return Error(
            StopPointErrorCode::InvalidArgument,
            "Ingress notification cannot change while native ingress is enabled");
    }
    ingress_notification_counter_ = counter;
    return {};
}

StopPointError StopPointRouter::SetIngressNotifier(
    void* context,
    StopPointIngressNotifier notifier)
{
    if (owner_thread_ != std::this_thread::get_id())
    {
        return Error(
            StopPointErrorCode::WrongThread,
            "Ingress notifier must be configured by the router owner");
    }
    if (ingress_enabled_.load(std::memory_order_acquire))
    {
        return Error(
            StopPointErrorCode::InvalidArgument,
            "Ingress notifier cannot change while native ingress is enabled");
    }
    if ((context == nullptr) != (notifier == nullptr))
    {
        return Error(
            StopPointErrorCode::InvalidArgument,
            "Ingress notifier context and function must be set or cleared together");
    }
    ingress_notifier_context_ = context;
    ingress_notifier_ = notifier;
    return {};
}

StopGroupRegistrationResult StopPointRouter::RegisterGroup(
    StopSubscriptionGroupDefinition definition,
    StopGroupRegistrationOptions options)
{
    StopGroupRegistrationResult result;
    if (StopPointError error =
            CheckControlThread(*this, owner_thread_, initialized_, stopping_))
    {
        result.receipt.error = std::move(error);
        return result;
    }
    if (StopPointError error = ValidateDefinition(
            definition,
            cpu_evaluator_,
            cpu_observer_))
    {
        result.receipt.error = std::move(error);
        return result;
    }
    if (impl_->groups.contains(definition.id.value()))
    {
        result.receipt.error = Error(
            StopPointErrorCode::InvalidArgument,
            "Subscription group ID is already active");
        return result;
    }

    const bool current_available =
        impl_->current_point &&
        impl_->current_point->identity.workset_epoch == workset_epoch_ &&
        CurrentPointMatchesDefinition(*impl_->current_point, definition);
    if (options.current_point == StopCurrentPointPolicy::Require &&
        !current_available)
    {
        result.receipt.error = Error(
            StopPointErrorCode::CurrentPointUnavailable,
            "The required current stop point is unavailable");
        return result;
    }
    if (impl_->next_registration_sequence == 0 ||
        impl_->next_registration_sequence ==
            std::numeric_limits<std::uint64_t>::max())
    {
        result.receipt.error = Error(
            StopPointErrorCode::PhysicalIntegrityUnknown,
            "Stop subscription registration sequence exhausted");
        return result;
    }

    const std::uint64_t registration_sequence =
        impl_->next_registration_sequence++;
    const StopSubscriptionGroupId group_id = definition.id;
    const StopSourceId source_id = definition.source.id;
    std::shared_ptr<SourceDropCounter> drop_counter;
    try
    {
        auto& counter = impl_->source_drop_counters[source_id.value()];
        if (!counter)
        {
            counter = std::make_shared<SourceDropCounter>();
            counter->source_id = source_id;
        }
        drop_counter = counter;
    }
    catch (...)
    {
        result.receipt.error = Error(
            StopPointErrorCode::InvalidArgument,
            "Failed allocating passive-drop diagnostics for the source");
        return result;
    }
    auto candidate = impl_->groups;
    candidate.emplace(
        group_id.value(),
        MakeGroupRecord(
            std::move(definition),
            registration_sequence,
            workset_epoch_,
            std::move(drop_counter),
            impl_->current_point));

    CandidateApplyReceipt applied = ApplyCandidate(
        *impl_,
        physical_manager_,
        std::move(candidate),
        workset_epoch_,
        dispatch_generation_,
        false);
    if (!applied.ok)
    {
        result.receipt.error = std::move(applied.error);
        result.receipt.dispatch_generation = dispatch_generation_;
        result.receipt.physical_generation = applied.physical_generation;
        if (applied.integrity == PhysicalStopIntegrity::Unknown)
            ingress_enabled_.store(false, std::memory_order_release);
        return result;
    }

    StopSubscriptionGroupLease lease{
        group_id,
        source_id,
        registration_sequence,
        workset_epoch_,
        true,
    };
    result.receipt = {
        true,
        lease,
        dispatch_generation_,
        applied.physical_generation,
        {},
    };
    result.handle =
        StopSubscriptionGroupHandle(lease_control_, lease);

    if (current_available &&
        options.current_point != StopCurrentPointPolicy::Ignore)
    {
        const auto snapshot = impl_->dispatch.load(std::memory_order_acquire);
        StopRouteReceipt current;
        bool fired_one_shot = false;
        current.identity = impl_->current_point->identity;
        current.event = *impl_->current_point;
        current.terminal = StopRouteTerminal::None;
        if (snapshot)
        {
            for (const DispatchEntry& entry : snapshot->entries)
            {
                if (entry.group_id != group_id ||
                    !PointMatchesEvidence(entry.point, impl_->current_point->evidence) ||
                    entry.qualification_id != 0 ||
                    (IsForeground(entry.route) &&
                     std::get<ForegroundStopWait>(entry.route)
                             .required_occurrences != 1))
                {
                    continue;
                }
                const bool samples_available = std::ranges::all_of(
                    entry.sample_descriptor_ids,
                    [&](std::uint32_t descriptor_id) {
                        return HasSample(*impl_->current_point, descriptor_id);
                    });
                if (!samples_available)
                    continue;

                if (entry.one_shot)
                {
                    bool expected = false;
                    if (!entry.one_shot->fired.compare_exchange_strong(
                            expected,
                            true,
                            std::memory_order_acq_rel))
                    {
                        continue;
                    }
                    fired_one_shot = true;
                }

                StopDelivery delivery{
                    *impl_->current_point,
                    entry.source_id,
                    entry.group_id,
                    entry.subscription_id,
                };
                current.deliveries.push_back(delivery);
                try
                {
                    entry.consumer->OnStopPoint(delivery);
                }
                catch (const std::exception& exception)
                {
                    current.terminal = StopRouteTerminal::RoutingFailure;
                    current.error = Error(
                        StopPointErrorCode::InvalidPolicy,
                        StopConsumerFailureMessage(
                            entry, *impl_->current_point, exception.what()));
                    current.core_must_remain_stopped = true;
                    break;
                }
                catch (...)
                {
                    current.terminal = StopRouteTerminal::RoutingFailure;
                    current.error = Error(
                        StopPointErrorCode::InvalidPolicy,
                        StopConsumerFailureMessage(
                            entry, *impl_->current_point,
                            "unknown non-standard exception"));
                    current.core_must_remain_stopped = true;
                    break;
                }

                if (IsForeground(entry.route))
                {
                    current.terminal = StopRouteTerminal::ForegroundMatched;
                    break;
                }
                if (const auto* interruption =
                        std::get_if<TrustedStopInterruptionRequest>(
                            &entry.route))
                {
                    current.terminal =
                        StopRouteTerminal::InterruptionRequested;
                    current.interruption_handler_request =
                        StopInterruptionHandlerRequest{
                            entry.source_id,
                            entry.group_id,
                            entry.subscription_id,
                            interruption->handler_key,
                        };
                    current.core_must_remain_stopped = true;
                    break;
                }
            }
        }
        if (current.deliveries.empty())
        {
            current.terminal = StopRouteTerminal::Unclaimed;
            current.error = Error(
                StopPointErrorCode::CurrentPointUnavailable,
                "No current-point subscription could consume the retained evidence");
        }
        if (fired_one_shot)
        {
            current.event->requires_physical_reconcile_before_resume = true;
            current.core_must_remain_stopped = true;
        }
        AppendHistory(*impl_, current);
        result.current_point = std::move(current);
        if (fired_one_shot)
        {
            const std::vector<StopRouteReceipt> reconciled =
                DrainIngress();
            const auto failure = std::ranges::find_if(
                reconciled,
                [](const StopRouteReceipt& receipt) {
                    return receipt.terminal == StopRouteTerminal::RoutingFailure ||
                        receipt.terminal == StopRouteTerminal::Overflow;
                });
            if (failure != reconciled.end())
            {
                result.current_point->terminal =
                    StopRouteTerminal::RoutingFailure;
                result.current_point->error = failure->error;
                result.current_point->core_must_remain_stopped = true;
            }
        }
    }
    return result;
}

StopRouteReceipt StopPointRouter::AcceptCurrentPoint(
    const StopSubscriptionGroupLease& lease)
{
    if (StopPointError error =
            CheckControlThread(*this, owner_thread_, initialized_, stopping_))
    {
        return FailureRoute(
            StopRouteTerminal::RoutingFailure,
            std::move(error));
    }
    if (!lease.active)
    {
        return FailureRoute(
            StopRouteTerminal::RoutingFailure,
            Error(
                StopPointErrorCode::GroupNotFound,
                "Cannot accept the current point for an inactive lease"));
    }
    const auto group = impl_->groups.find(lease.group_id.value());
    if (group == impl_->groups.end() ||
        group->second.source.id != lease.source_id ||
        group->second.registration_sequence != lease.registration_sequence)
    {
        return FailureRoute(
            StopRouteTerminal::RoutingFailure,
            Error(
                StopPointErrorCode::SourceMismatch,
                "Current-point request does not own the active group"));
    }
    if (!impl_->current_point ||
        impl_->current_point->identity.workset_epoch != workset_epoch_)
    {
        return FailureRoute(
            StopRouteTerminal::Unclaimed,
            Error(
                StopPointErrorCode::CurrentPointUnavailable,
                "No current stop point is retained"));
    }

    StopRouteReceipt current;
    current.identity = impl_->current_point->identity;
    current.event = *impl_->current_point;
    const DispatchSnapshot* snapshot =
        impl_->dispatch.load(std::memory_order_acquire);
    bool fired_one_shot = false;
    if (snapshot)
    {
        for (const DispatchEntry& entry : snapshot->entries)
        {
            if (entry.group_id != lease.group_id ||
                !PointMatchesEvidence(
                    entry.point,
                    impl_->current_point->evidence) ||
                entry.qualification_id != 0 ||
                (IsForeground(entry.route) &&
                 std::get<ForegroundStopWait>(entry.route)
                         .required_occurrences != 1) ||
                !std::ranges::all_of(
                    entry.sample_descriptor_ids,
                    [&](std::uint32_t descriptor_id) {
                        return HasSample(
                            *impl_->current_point,
                            descriptor_id);
                    }))
            {
                continue;
            }
            if (entry.one_shot)
            {
                bool expected = false;
                if (!entry.one_shot->fired.compare_exchange_strong(
                        expected,
                        true,
                        std::memory_order_acq_rel))
                {
                    continue;
                }
                fired_one_shot = true;
            }

            StopDelivery delivery{
                *impl_->current_point,
                entry.source_id,
                entry.group_id,
                entry.subscription_id,
            };
            current.deliveries.push_back(delivery);
            try
            {
                entry.consumer->OnStopPoint(delivery);
            }
            catch (const std::exception& exception)
            {
                current.terminal = StopRouteTerminal::RoutingFailure;
                current.error = Error(
                    StopPointErrorCode::InvalidPolicy,
                    StopConsumerFailureMessage(
                        entry, *impl_->current_point, exception.what()));
                current.core_must_remain_stopped = true;
                break;
            }
            catch (...)
            {
                current.terminal = StopRouteTerminal::RoutingFailure;
                current.error = Error(
                    StopPointErrorCode::InvalidPolicy,
                    StopConsumerFailureMessage(
                        entry, *impl_->current_point,
                        "unknown non-standard exception"));
                current.core_must_remain_stopped = true;
                break;
            }

            if (IsForeground(entry.route))
            {
                current.terminal = StopRouteTerminal::ForegroundMatched;
                break;
            }
            if (const auto* interruption =
                    std::get_if<TrustedStopInterruptionRequest>(
                        &entry.route))
            {
                current.terminal =
                    StopRouteTerminal::InterruptionRequested;
                current.interruption_handler_request =
                    StopInterruptionHandlerRequest{
                        entry.source_id,
                        entry.group_id,
                        entry.subscription_id,
                        interruption->handler_key,
                    };
                current.core_must_remain_stopped = true;
                break;
            }
        }
    }
    if (current.deliveries.empty())
    {
        current.terminal = StopRouteTerminal::Unclaimed;
        current.error = Error(
            StopPointErrorCode::CurrentPointUnavailable,
            "No current-point subscription could consume the retained evidence");
    }
    if (fired_one_shot)
    {
        current.event->requires_physical_reconcile_before_resume = true;
        current.core_must_remain_stopped = true;
    }
    AppendHistory(*impl_, current);

    if (fired_one_shot)
    {
        for (const StopRouteReceipt& reconciled : DrainIngress())
        {
            if (reconciled.terminal == StopRouteTerminal::RoutingFailure ||
                reconciled.terminal == StopRouteTerminal::Overflow)
            {
                current.terminal = StopRouteTerminal::RoutingFailure;
                current.error = reconciled.error;
                current.core_must_remain_stopped = true;
                break;
            }
        }
    }
    return current;
}

StopPointError StopPointRouter::DepartCurrentPoint()
{
    if (StopPointError error =
            CheckControlThread(*this, owner_thread_, initialized_, stopping_))
    {
        return error;
    }
    impl_->current_point.reset();
    return {};
}

StopGroupReceipt StopPointRouter::ReplaceGroup(
    StopSubscriptionGroupLease& lease,
    StopSubscriptionGroupDefinition definition)
{
    return ReplaceFromHandle(lease, std::move(definition));
}

StopGroupReceipt StopPointRouter::ReplaceFromHandle(
    StopSubscriptionGroupLease& lease,
    StopSubscriptionGroupDefinition definition)
{
    StopGroupReceipt result;
    result.lease = lease;
    result.dispatch_generation = dispatch_generation_;
    result.physical_generation = physical_manager_.generation();
    if (StopPointError error =
            CheckControlThread(*this, owner_thread_, initialized_, stopping_))
    {
        result.error = std::move(error);
        return result;
    }
    if (!lease.active)
    {
        result.error = Error(
            StopPointErrorCode::GroupNotFound,
            "Cannot replace an inactive subscription-group lease");
        return result;
    }
    const auto existing = impl_->groups.find(lease.group_id.value());
    if (existing == impl_->groups.end())
    {
        result.error = Error(
            StopPointErrorCode::GroupNotFound,
            "Subscription group is no longer active");
        return result;
    }
    if (existing->second.source.id != lease.source_id ||
        existing->second.registration_sequence != lease.registration_sequence)
    {
        result.error = Error(
            StopPointErrorCode::SourceMismatch,
            "Subscription-group lease does not own the active group");
        return result;
    }
    if (definition.id != lease.group_id ||
        definition.source.id != lease.source_id)
    {
        result.error = Error(
            StopPointErrorCode::SourceMismatch,
            "Replacement must preserve group and source identity");
        return result;
    }
    if (StopPointError error = ValidateDefinition(
            definition,
            cpu_evaluator_,
            cpu_observer_))
    {
        result.error = std::move(error);
        return result;
    }

    auto candidate = impl_->groups;
    candidate[lease.group_id.value()] = MakeGroupRecord(
        std::move(definition),
        lease.registration_sequence,
        workset_epoch_,
        existing->second.drop_counter,
        impl_->current_point);
    CandidateApplyReceipt applied = ApplyCandidate(
        *impl_,
        physical_manager_,
        std::move(candidate),
        workset_epoch_,
        dispatch_generation_,
        false);
    if (!applied.ok)
    {
        result.error = std::move(applied.error);
        result.dispatch_generation = dispatch_generation_;
        result.physical_generation = applied.physical_generation;
        if (applied.integrity == PhysicalStopIntegrity::Unknown)
            ingress_enabled_.store(false, std::memory_order_release);
        return result;
    }

    lease.acquisition_epoch = workset_epoch_;
    result.ok = true;
    result.lease = lease;
    result.dispatch_generation = dispatch_generation_;
    result.physical_generation = applied.physical_generation;
    return result;
}

StopReleaseReceipt StopPointRouter::ReleaseGroup(
    StopSubscriptionGroupLease& lease)
{
    return ReleaseFromHandle(lease);
}

StopReleaseReceipt StopPointRouter::ReleaseFromHandle(
    StopSubscriptionGroupLease& lease)
{
    StopReleaseReceipt result;
    result.group_id = lease.group_id;
    result.dispatch_generation = dispatch_generation_;
    result.physical_generation = physical_manager_.generation();
    if (!lease.active)
    {
        result.ok = true;
        result.already_released = true;
        return result;
    }
    const auto released =
        impl_->released_registration_sequences.find(lease.group_id.value());
    if ((stopping_ || stopped_) &&
        released != impl_->released_registration_sequences.end() &&
        released->second == lease.registration_sequence)
    {
        lease.active = false;
        result.ok = true;
        result.already_released = true;
        return result;
    }
    if (StopPointError error =
            CheckControlThread(*this, owner_thread_, initialized_, stopping_))
    {
        result.error = std::move(error);
        return result;
    }

    const auto existing = impl_->groups.find(lease.group_id.value());
    if (existing == impl_->groups.end())
    {
        const auto released =
            impl_->released_registration_sequences.find(lease.group_id.value());
        if (released != impl_->released_registration_sequences.end() &&
            released->second == lease.registration_sequence)
        {
            lease.active = false;
            result.ok = true;
            result.already_released = true;
            return result;
        }
        result.error = Error(
            StopPointErrorCode::GroupNotFound,
            "Subscription group is not active");
        return result;
    }
    if (existing->second.source.id != lease.source_id ||
        existing->second.registration_sequence != lease.registration_sequence)
    {
        result.error = Error(
            StopPointErrorCode::SourceMismatch,
            "Subscription-group lease does not own the active group");
        return result;
    }

    auto candidate = impl_->groups;
    candidate.erase(lease.group_id.value());
    CandidateApplyReceipt applied = ApplyCandidate(
        *impl_,
        physical_manager_,
        std::move(candidate),
        workset_epoch_,
        dispatch_generation_,
        false);
    if (!applied.ok)
    {
        result.error = std::move(applied.error);
        result.dispatch_generation = dispatch_generation_;
        result.physical_generation = applied.physical_generation;
        if (applied.integrity == PhysicalStopIntegrity::Unknown)
            ingress_enabled_.store(false, std::memory_order_release);
        return result;
    }

    // Applying the replacement dispatch makes this group unreachable to new
    // native hits. Wait for callbacks that protected the prior immutable
    // snapshot before returning the lease so its CPU observer may safely
    // unpublish and destroy item-local storage.
    WaitForNativeIngressQuiescence();

    impl_->released_registration_sequences[lease.group_id.value()] =
        lease.registration_sequence;
    lease.active = false;
    result.ok = true;
    result.dispatch_generation = dispatch_generation_;
    result.physical_generation = applied.physical_generation;
    return result;
}

void StopPointRouter::ReleaseFromHandleNoThrow(
    StopSubscriptionGroupLease& lease) noexcept
{
    StopReleaseReceipt receipt = ReleaseFromHandle(lease);
    if (!receipt.ok)
    {
        ingress_enabled_.store(false, std::memory_order_release);
        authoritative_overflow_.store(true, std::memory_order_release);
    }
}

namespace {

[[nodiscard]] std::uint64_t NextNonzero(
    std::atomic<std::uint64_t>& counter) noexcept
{
    const std::uint64_t value = counter.fetch_add(1, std::memory_order_relaxed);
    if (value == 0 || value == std::numeric_limits<std::uint64_t>::max())
        return 0;
    return value;
}

[[nodiscard]] NativePacket BuildNativePacket(
    const DispatchSnapshot* snapshot,
    const StopPointCpuContext& context,
    RoutedStopEvidence evidence,
    IStopPointCpuEvaluator* evaluator,
    IStopPointCpuObserver* observer,
    std::atomic<std::uint64_t>& next_sequence,
    std::atomic<std::uint64_t>& next_sample_snapshot) noexcept
{
    NativePacket packet;
    packet.snapshot = snapshot;
    if (!packet.snapshot)
        return packet;
    packet.physical_hit =
        SnapshotPhysicalPlanMatches(*packet.snapshot, context);
    if (!packet.physical_hit)
        return packet;

    const std::uint64_t sequence = NextNonzero(next_sequence);
    const std::uint64_t sample_snapshot =
        NextNonzero(next_sample_snapshot);
    if (sequence == 0 || sample_snapshot == 0)
    {
        packet.terminal = StopRouteTerminal::Overflow;
        packet.request_break = true;
        packet.event.authoritative = true;
        return packet;
    }

    packet.event.identity = {
        RoutedStopSequence(sequence),
        StopSampleSnapshotId(sample_snapshot),
        packet.snapshot->workset_epoch,
        packet.snapshot->generation,
        packet.snapshot->physical_generation,
    };
    packet.event.evidence = std::move(evidence);

    std::array<std::uint16_t, kMaxStopDeliveriesPerHit> candidates{};
    std::size_t candidate_count = 0;
    for (std::size_t i = 0;
        i < packet.snapshot->entries.size();
        ++i)
    {
        const DispatchEntry& entry = packet.snapshot->entries[i];
        if (!PointMatches(entry.point, context))
            continue;
        if (entry.qualification_id != 0 &&
            (!evaluator ||
                !evaluator->Qualify(entry.qualification_id, context)))
        {
            continue;
        }
        if (candidate_count >= candidates.size())
        {
            packet.terminal = StopRouteTerminal::Overflow;
            packet.request_break = true;
            packet.event.authoritative = true;
            return packet;
        }
        const std::uint32_t observer_descriptor =
            CpuObserverDescriptor(entry.route);
        if (observer_descriptor != 0)
        {
            bool observer_present = false;
            for (std::size_t observer_index = 0;
                observer_index < packet.observer_descriptor_count;
                ++observer_index)
            {
                if (packet.observer_descriptor_ids[observer_index] ==
                    observer_descriptor)
                {
                    observer_present = true;
                    break;
                }
            }
            if (!observer_present)
            {
                if (packet.observer_descriptor_count >=
                    packet.observer_descriptor_ids.size())
                {
                    packet.terminal = StopRouteTerminal::Overflow;
                    packet.request_break = true;
                    packet.event.authoritative = true;
                    return packet;
                }
                packet.observer_descriptor_ids[
                    packet.observer_descriptor_count++] =
                    observer_descriptor;
            }
        }
        candidates[candidate_count++] = static_cast<std::uint16_t>(i);
    }
    if (candidate_count == 0)
        return packet;

    for (std::size_t candidate = 0; candidate < candidate_count; ++candidate)
    {
        const DispatchEntry& entry =
            packet.snapshot->entries[candidates[candidate]];
        for (const std::uint32_t descriptor_id :
            entry.sample_descriptor_ids)
        {
            bool present = false;
            for (std::size_t i = 0; i < packet.event.sample_count; ++i)
            {
                if (packet.event.samples[i].descriptor_id == descriptor_id)
                {
                    present = true;
                    break;
                }
            }
            if (present)
                continue;
            if (packet.event.sample_count >= packet.event.samples.size())
            {
                packet.terminal = StopRouteTerminal::Overflow;
                packet.request_break = true;
                packet.event.authoritative = true;
                return packet;
            }
            RoutedHitSample sample{
                descriptor_id,
                0,
                false,
            };
            if (evaluator)
            {
                sample = evaluator->Sample(descriptor_id, context);
                sample.descriptor_id = descriptor_id;
            }
            packet.event.samples[packet.event.sample_count++] = sample;
        }
    }

    for (std::size_t candidate = 0; candidate < candidate_count; ++candidate)
    {
        const std::uint16_t entry_index = candidates[candidate];
        const DispatchEntry& entry =
            packet.snapshot->entries[entry_index];
        if (entry.one_shot)
        {
            bool expected = false;
            if (!entry.one_shot->fired.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel))
            {
                continue;
            }
            packet.event.requires_physical_reconcile_before_resume = true;
        }

        if (IsPassive(entry.route) &&
            entry.drop_counter != nullptr)
        {
            bool already_present = false;
            for (std::size_t source_index = 0;
                source_index < packet.passive_source_count;
                ++source_index)
            {
                if (packet.passive_sources[source_index] ==
                    entry.drop_counter)
                {
                    already_present = true;
                    break;
                }
            }
            if (!already_present)
            {
                packet.passive_sources[
                    packet.passive_source_count++] =
                    entry.drop_counter;
            }
        }

        if (IsForeground(entry.route))
        {
            const auto& foreground =
                std::get<ForegroundStopWait>(entry.route);
            std::uint32_t occurrence = 1;
            if (entry.foreground_occurrences)
            {
                occurrence = entry.foreground_occurrences->observed.fetch_add(
                    1,
                    std::memory_order_acq_rel) + 1;
            }
            packet.event.matched_occurrence_count = occurrence;
            if (foreground.sample_expectation)
            {
                const auto expected = *foreground.sample_expectation;
                const auto begin = packet.event.samples.begin();
                const auto end = begin + packet.event.sample_count;
                const auto found = std::ranges::find(
                    std::ranges::subrange(begin, end),
                    expected.descriptor_id,
                    &RoutedHitSample::descriptor_id);
                const bool available = found != end && found->available;
                if (!available || found->value != expected.expected_value)
                {
                    packet.event.sample_expectation_failed = true;
                    packet.event.expected_sample_descriptor_id =
                        expected.descriptor_id;
                    packet.event.expected_sample_value =
                        expected.expected_value;
                    packet.event.actual_sample_available = available;
                    packet.event.actual_sample_value =
                        available ? found->value : 0;
                    packet.entry_indices[packet.entry_count++] = entry_index;
                    packet.event.authoritative = true;
                    packet.terminal = StopRouteTerminal::RoutingFailure;
                    packet.terminal_entry = entry_index;
                    packet.request_break = true;
                    break;
                }
            }
            if (occurrence < foreground.required_occurrences)
                continue;
            packet.entry_indices[packet.entry_count++] = entry_index;
            packet.event.active_foreground_wait = true;
            packet.event.authoritative = true;
            packet.terminal = StopRouteTerminal::ForegroundMatched;
            packet.terminal_entry = entry_index;
            packet.request_break = true;
            break;
        }
        packet.entry_indices[packet.entry_count++] = entry_index;
        packet.event.authoritative =
            packet.event.authoritative || IsLossless(entry.route);
        if (IsInterruption(entry.route))
        {
            packet.event.authoritative = true;
            packet.terminal =
                StopRouteTerminal::InterruptionRequested;
            packet.terminal_entry = entry_index;
            packet.request_break = true;
            break;
        }
    }
    for (std::size_t observer_index = 0;
        observer_index < packet.observer_descriptor_count;
        ++observer_index)
    {
        const StopCpuObservationResult observed = observer == nullptr
            ? StopCpuObservationResult::Failed
            : observer->ObserveRoutedHit(
                  packet.observer_descriptor_ids[observer_index],
                  packet.event);
        if (observed == StopCpuObservationResult::Failed)
        {
            packet.event.authoritative = true;
            packet.terminal = StopRouteTerminal::RoutingFailure;
            packet.terminal_entry = -1;
            packet.request_break = true;
            break;
        }
        if (observed ==
            StopCpuObservationResult::ObservedRequiresReconcile)
        {
            packet.event.requires_physical_reconcile_before_resume = true;
            packet.event.authoritative = true;
            packet.request_break = true;
        }
    }
    return packet;
}

class NativeIngressGuard final
{
public:
    NativeIngressGuard(
        std::atomic<std::size_t>& inflight) noexcept
        : inflight_(inflight)
    {
        inflight_.fetch_add(1, std::memory_order_acq_rel);
    }

    ~NativeIngressGuard()
    {
        if (inflight_.fetch_sub(1, std::memory_order_acq_rel) == 1)
            inflight_.notify_all();
    }

private:
    std::atomic<std::size_t>& inflight_;
};

class NativeSnapshotHazardLease final
{
public:
    explicit NativeSnapshotHazardLease(
        std::atomic<const void*>& hazard) noexcept
        : hazard_(hazard)
    {
    }

    ~NativeSnapshotHazardLease()
    {
        hazard_.store(nullptr, std::memory_order_release);
    }

    [[nodiscard]] const DispatchSnapshot* Protect(
        const std::atomic<const DispatchSnapshot*>& current) noexcept
    {
        const DispatchSnapshot* snapshot = nullptr;
        do
        {
            snapshot = current.load(std::memory_order_acquire);
            hazard_.store(snapshot, std::memory_order_release);
        } while (snapshot != current.load(std::memory_order_acquire));
        return snapshot;
    }

private:
    std::atomic<const void*>& hazard_;
};

class NativeSingleReaderLease final
{
public:
    explicit NativeSingleReaderLease(std::atomic_flag& active) noexcept
        : active_(active),
          acquired_(!active_.test_and_set(std::memory_order_acquire))
    {
    }

    ~NativeSingleReaderLease()
    {
        if (acquired_)
            active_.clear(std::memory_order_release);
    }

    [[nodiscard]] bool acquired() const noexcept
    {
        return acquired_;
    }

private:
    std::atomic_flag& active_;
    bool acquired_ = false;
};

} // namespace

savor::probe::NativeStopDecision StopPointRouter::RouteNative(
    const StopPointCpuContext& context,
    RoutedStopEvidence evidence) noexcept
{
    HostActivityTracker::Scope host_activity =
        host_activity_ ? host_activity_->Track()
                       : HostActivityTracker::Scope{};
    NativeIngressGuard guard(native_inflight_);
    if (!ingress_enabled_.load(std::memory_order_acquire))
    {
        return {
            initialized_,
            authoritative_overflow_.load(std::memory_order_acquire),
        };
    }

    NativeSingleReaderLease reader(native_reader_active_);
    if (!reader.acquired())
    {
        authoritative_overflow_.store(true, std::memory_order_release);
        ingress_enabled_.store(false, std::memory_order_release);
        return {true, true};
    }

    NativeSnapshotHazardLease snapshot_hazard(native_snapshot_hazard_);
    const DispatchSnapshot* snapshot =
        snapshot_hazard.Protect(impl_->dispatch);
    if (!snapshot)
        return {true, false};

    NativePacket packet = BuildNativePacket(
        snapshot,
        context,
        std::move(evidence),
        cpu_evaluator_,
        cpu_observer_,
        impl_->next_stop_sequence,
        impl_->next_sample_snapshot);
    if (!packet.physical_hit)
        return {};
    if (packet.entry_count == 0 &&
        packet.event.matched_occurrence_count != 0 &&
        !packet.request_break)
    {
        return {};
    }
    if (packet.entry_count == 0 &&
        packet.terminal != StopRouteTerminal::Overflow)
    {
        packet.terminal = StopRouteTerminal::Unclaimed;
    }

    const bool request_break = packet.request_break;
    const bool authoritative = packet.event.authoritative ||
        packet.terminal == StopRouteTerminal::Overflow;
    const std::uint64_t routed_sequence =
        packet.event.identity.sequence.value();
    if (packet.terminal == StopRouteTerminal::Overflow)
    {
        authoritative_overflow_.store(true, std::memory_order_release);
        ingress_enabled_.store(false, std::memory_order_release);
    }
    if (!impl_->ingress.TryPush(std::move(packet)))
    {
        if (authoritative)
        {
            authoritative_overflow_.store(true, std::memory_order_release);
            ingress_enabled_.store(false, std::memory_order_release);
            packet.entry_count = 0;
            packet.terminal_entry = -1;
            packet.terminal = StopRouteTerminal::Overflow;
            packet.request_break = true;
            packet.event.authoritative = true;
            if (impl_->emergency.TryPublish(std::move(packet)))
            {
                NotifyIngressPublication();
            }
            return {true, true, routed_sequence};
        }
        passive_drop_count_.fetch_add(1, std::memory_order_relaxed);
        for (std::size_t source_index = 0;
            source_index < packet.passive_source_count;
            ++source_index)
        {
            packet.passive_sources[source_index]->passive_drops.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        return {};
    }
    NotifyIngressPublication();
    return {request_break, false, routed_sequence};
}

void StopPointRouter::NotifyIngressPublication() noexcept
{
    if (ingress_notification_counter_ != nullptr)
    {
        ingress_notification_counter_->fetch_add(
            1,
            std::memory_order_release);
        ingress_notification_counter_->notify_one();
    }
    if (ingress_notifier_ != nullptr)
        ingress_notifier_(ingress_notifier_context_);
}

savor::probe::NativeStopDecision StopPointRouter::OnPcStop(
    const savor::probe::NativePcStop& stop) noexcept
{
    const NativeStopPath path = ToNativePath(stop.origin);
    StopPointCpuContext context{
        path,
        stop.pc,
        0,
        0,
        0,
        false,
        false,
        0,
        stop.power_pc,
        stop.system,
    };
    RoutedStopEvidence evidence{
        path,
        PcStopPointSpec{stop.pc},
        stop.pc,
        0,
        false,
    };
    return RouteNative(context, std::move(evidence));
}

savor::probe::NativeStopDecision StopPointRouter::OnMemoryStop(
    const savor::probe::NativeMemoryStop& stop) noexcept
{
    const NativeStopPath path = NativeStopPath::Memcheck;
    StopPointCpuContext context{
        path,
        stop.pc,
        stop.address,
        stop.size,
        stop.value,
        stop.write,
        stop.post_write,
        0,
        nullptr,
        stop.system,
    };
    RoutedStopEvidence evidence{
        path,
        MemoryStopPointSpec{
            stop.address,
            stop.size,
            stop.write
                ? StopMemoryAccess::Write
                : StopMemoryAccess::Read,
        },
        stop.pc,
        stop.value,
        stop.post_write,
    };
    return RouteNative(context, std::move(evidence));
}

void StopPointRouter::WaitForNativeIngressQuiescence()
{
    std::size_t inflight =
        native_inflight_.load(std::memory_order_acquire);
    while (inflight != 0)
    {
        native_inflight_.wait(inflight, std::memory_order_acquire);
        inflight = native_inflight_.load(std::memory_order_acquire);
    }
}

namespace {

[[nodiscard]] StopRouteReceipt ProcessPacket(
    StopPointRouter::Impl& impl,
    NativePacket packet,
    StopDispatchGeneration current_generation,
    WorksetEpoch current_epoch,
    PhysicalPlanGeneration current_physical_generation)
{
    StopRouteReceipt receipt;
    receipt.identity = packet.event.identity;
    receipt.event = packet.event;

    const DispatchEntry* ingress_terminal_entry =
        packet.snapshot != nullptr && packet.terminal_entry >= 0 &&
            static_cast<std::size_t>(packet.terminal_entry) <
                packet.snapshot->entries.size()
        ? &packet.snapshot->entries[packet.terminal_entry]
        : nullptr;
    SCLOGDX(
        SC_TAGS("stop.router", "stop.router.hit"),
        "path=%u preclassified_terminal=%u consumer=%s matched_entries=%u passive_sources=%u observer_descriptors=%u request_break=%u authoritative=%u routed_sequence=%llu epoch=%llu dispatch_generation=%llu physical_generation=%llu hit_pc=0x%08X",
        static_cast<unsigned>(packet.event.evidence.path),
        static_cast<unsigned>(packet.terminal),
        ingress_terminal_entry
            ? ingress_terminal_entry->consumer_name.c_str()
            : "<none>",
        static_cast<unsigned>(packet.entry_count),
        static_cast<unsigned>(packet.passive_source_count),
        static_cast<unsigned>(packet.observer_descriptor_count),
        packet.request_break ? 1u : 0u,
        packet.event.authoritative ? 1u : 0u,
        packet.event.identity.sequence.value(),
        packet.event.identity.workset_epoch.value(),
        packet.event.identity.dispatch_generation.value(),
        packet.event.identity.physical_generation.value(),
        packet.event.evidence.hit_pc);
    if (!packet.snapshot ||
        packet.event.identity.dispatch_generation != current_generation ||
        packet.event.identity.workset_epoch != current_epoch ||
        packet.event.identity.physical_generation !=
            current_physical_generation)
    {
        receipt.terminal = StopRouteTerminal::Stale;
        receipt.error = Error(
            packet.event.identity.workset_epoch != current_epoch
                ? StopPointErrorCode::StaleEpoch
                : StopPointErrorCode::StaleDispatchGeneration,
            "Native stop event belongs to a stale dispatch snapshot");
        receipt.core_must_remain_stopped = packet.event.authoritative;
        AppendHistory(impl, receipt);
        return receipt;
    }

    receipt.terminal = packet.terminal;
    if (packet.terminal_entry >= 0 &&
        static_cast<std::size_t>(packet.terminal_entry) <
            packet.snapshot->entries.size())
    {
        const DispatchEntry& terminal_entry =
            packet.snapshot->entries[
                static_cast<std::size_t>(packet.terminal_entry)];
        if (const auto* foreground =
                std::get_if<ForegroundStopWait>(&terminal_entry.route))
        {
            receipt.execution_operation_id =
                foreground->execution_operation_id;
        }
        else if (const auto* interruption =
                     std::get_if<TrustedStopInterruptionRequest>(
                         &terminal_entry.route))
        {
            receipt.execution_operation_id =
                interruption->execution_operation_id;
        }
    }
    if (receipt.terminal == StopRouteTerminal::Overflow)
    {
        impl.overflow_reported = true;
        receipt.error = Error(
            StopPointErrorCode::IngressOverflow,
            "Native stop routing exceeded its fixed delivery capacity");
    }
    else if (receipt.terminal == StopRouteTerminal::RoutingFailure &&
             packet.event.sample_expectation_failed)
    {
        receipt.error = Error(
            StopPointErrorCode::InvalidPolicy,
            "Foreground occurrence sample mismatch at occurrence " +
                std::to_string(packet.event.matched_occurrence_count) +
                ": descriptor=" +
                std::to_string(packet.event.expected_sample_descriptor_id) +
                ", expected=" +
                std::to_string(packet.event.expected_sample_value) +
                ", actual=" +
                (packet.event.actual_sample_available
                    ? std::to_string(packet.event.actual_sample_value)
                    : std::string("unavailable")));
    }
    receipt.core_must_remain_stopped =
        packet.event.requires_physical_reconcile_before_resume ||
        (packet.request_break &&
            packet.terminal != StopRouteTerminal::ForegroundMatched);
    for (std::size_t i = 0; i < packet.entry_count; ++i)
    {
        const std::size_t entry_index = packet.entry_indices[i];
        if (entry_index >= packet.snapshot->entries.size())
        {
            receipt.terminal = StopRouteTerminal::RoutingFailure;
            receipt.error = Error(
                StopPointErrorCode::StaleDispatchGeneration,
                "Native stop delivery referenced an invalid dispatch entry");
            receipt.core_must_remain_stopped = true;
            break;
        }
        const DispatchEntry& entry =
            packet.snapshot->entries[entry_index];
        StopDelivery delivery{
            packet.event,
            entry.source_id,
            entry.group_id,
            entry.subscription_id,
        };
        receipt.deliveries.push_back(delivery);
        try
        {
            entry.consumer->OnStopPoint(delivery);
        }
        catch (const std::exception& exception)
        {
            receipt.terminal = StopRouteTerminal::RoutingFailure;
            receipt.error = Error(
                StopPointErrorCode::InvalidPolicy,
                StopConsumerFailureMessage(
                    entry, packet.event, exception.what()));
            receipt.core_must_remain_stopped = true;
            break;
        }
        catch (...)
        {
            receipt.terminal = StopRouteTerminal::RoutingFailure;
            receipt.error = Error(
                StopPointErrorCode::InvalidPolicy,
                StopConsumerFailureMessage(
                    entry, packet.event, "unknown non-standard exception"));
            receipt.core_must_remain_stopped = true;
            break;
        }
    }

    if (receipt.terminal ==
            StopRouteTerminal::InterruptionRequested &&
        packet.terminal_entry >= 0 &&
        static_cast<std::size_t>(packet.terminal_entry) <
            packet.snapshot->entries.size())
    {
        const DispatchEntry& entry =
            packet.snapshot->entries[packet.terminal_entry];
        const auto* interruption =
            std::get_if<TrustedStopInterruptionRequest>(&entry.route);
        if (interruption != nullptr)
        {
            receipt.interruption_handler_request =
            StopInterruptionHandlerRequest{
                entry.source_id,
                entry.group_id,
                entry.subscription_id,
                interruption->handler_key,
            };
        }
    }
    if (receipt.terminal == StopRouteTerminal::RoutingFailure && !receipt.error)
    {
        receipt.error = Error(
            StopPointErrorCode::InvalidPolicy,
            "A routed observer failed");
        receipt.core_must_remain_stopped = true;
    }
    if (receipt.terminal == StopRouteTerminal::None &&
        receipt.deliveries.empty())
    {
        receipt.terminal = StopRouteTerminal::Unclaimed;
    }

    if (packet.event.requires_physical_reconcile_before_resume ||
        (packet.request_break &&
            (receipt.terminal == StopRouteTerminal::ForegroundMatched ||
            receipt.terminal == StopRouteTerminal::RoutingFailure ||
            receipt.terminal ==
                StopRouteTerminal::InterruptionRequested ||
            receipt.terminal == StopRouteTerminal::Overflow)))
    {
        impl.current_point = packet.event;
    }
    else
    {
        impl.current_point.reset();
    }
    if (receipt.terminal == StopRouteTerminal::ForegroundMatched ||
        receipt.terminal == StopRouteTerminal::RoutingFailure ||
        receipt.terminal == StopRouteTerminal::Overflow)
    {
        const DispatchEntry* terminal_entry =
            packet.terminal_entry >= 0 &&
            static_cast<std::size_t>(packet.terminal_entry) <
                packet.snapshot->entries.size()
            ? &packet.snapshot->entries[packet.terminal_entry]
            : nullptr;
        SCLOGDX(
            SC_TAGS("stop.router", "stop.delivery"),
            "terminal=%u consumer=%s source=%llu group=%llu subscription=%llu routed_sequence=%llu epoch=%llu hit_pc=0x%08X error=%s",
            static_cast<unsigned>(receipt.terminal),
            terminal_entry ? terminal_entry->consumer_name.c_str() : "<none>",
            terminal_entry ? terminal_entry->source_id.value() : 0,
            terminal_entry ? terminal_entry->group_id.value() : 0,
            terminal_entry ? terminal_entry->subscription_id.value() : 0,
            receipt.identity.sequence.value(),
            receipt.identity.workset_epoch.value(),
            packet.event.evidence.hit_pc,
            receipt.error.message.empty() ? "<none>" : receipt.error.message.c_str());
    }
    AppendHistory(impl, receipt);
    return receipt;
}

struct FiredOneShotRemoval
{
    std::map<std::uint64_t, GroupRecord> candidate;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ended_groups;
    bool changed = false;
};

[[nodiscard]] FiredOneShotRemoval RemoveFiredOneShotsCandidate(
    const std::map<std::uint64_t, GroupRecord>& groups)
{
    FiredOneShotRemoval result;
    result.candidate = groups;
    for (auto group_iterator = result.candidate.begin();
        group_iterator != result.candidate.end();)
    {
        GroupRecord& group = group_iterator->second;
        const std::size_t before = group.subscriptions.size();
        std::erase_if(group.subscriptions, [](const SubscriptionRecord& subscription) {
            return subscription.one_shot &&
                subscription.one_shot->fired.load(std::memory_order_acquire);
        });
        result.changed = result.changed ||
            group.subscriptions.size() != before;
        if (group.subscriptions.empty())
        {
            result.ended_groups.emplace_back(
                group.id.value(),
                group.registration_sequence);
            group_iterator = result.candidate.erase(group_iterator);
        }
        else
        {
            ++group_iterator;
        }
    }
    return result;
}

void RetireInactiveSnapshots(StopPointRouter::Impl& impl)
{
    const DispatchSnapshot* current =
        impl.dispatch.load(std::memory_order_acquire);
    std::erase_if(
        impl.snapshot_archive,
        [&](const std::unique_ptr<const DispatchSnapshot>& snapshot) {
            return snapshot.get() != current;
        });
}

} // namespace

std::vector<StopRouteReceipt> StopPointRouter::DrainIngress()
{
    std::vector<StopRouteReceipt> receipts;
    if (!initialized_ || owner_thread_ != std::this_thread::get_id())
    {
        receipts.push_back(FailureRoute(
            StopRouteTerminal::RoutingFailure,
            Error(
                !initialized_
                    ? StopPointErrorCode::InvalidArgument
                    : StopPointErrorCode::WrongThread,
                !initialized_
                    ? "Stop-point router is not initialized"
                    : "Stop ingress must be drained by the control thread")));
        return receipts;
    }

    HostActivityTracker::Scope host_activity;
    bool host_activity_started = false;
    const auto ensure_host_activity = [&]() noexcept {
        if (host_activity_started)
            return;
        host_activity_started = true;
        if (host_activity_)
            host_activity = host_activity_->Track();
    };

    bool snapshot_changed = false;
    do
    {
        snapshot_changed = false;
        WaitForNativeIngressQuiescence();
        NativePacket packet;
        while (impl_->ingress.TryPop(packet))
        {
            ensure_host_activity();
            receipts.push_back(ProcessPacket(
                *impl_,
                std::move(packet),
                dispatch_generation_,
                workset_epoch_,
                physical_manager_.generation()));
            packet = {};
        }
        if (impl_->emergency.TryConsume(packet))
        {
            ensure_host_activity();
            receipts.push_back(ProcessPacket(
                *impl_,
                std::move(packet),
                dispatch_generation_,
                workset_epoch_,
                physical_manager_.generation()));
        }

        FiredOneShotRemoval one_shots =
            RemoveFiredOneShotsCandidate(impl_->groups);
        if (one_shots.changed)
        {
            ensure_host_activity();
            CandidateApplyReceipt applied = ApplyCandidate(
                *impl_,
                physical_manager_,
                std::move(one_shots.candidate),
                workset_epoch_,
                dispatch_generation_,
                false);
            if (!applied.ok)
            {
                StopRouteReceipt failure = FailureRoute(
                    StopRouteTerminal::RoutingFailure,
                    std::move(applied.error));
                AppendHistory(*impl_, failure);
                receipts.push_back(std::move(failure));
                ingress_enabled_.store(false, std::memory_order_release);
            }
            else
            {
                for (const auto& [group_id, sequence] :
                    one_shots.ended_groups)
                {
                    impl_->released_registration_sequences[group_id] =
                        sequence;
                }
                snapshot_changed = true;
            }
        }
    } while (snapshot_changed);

    WaitForNativeIngressQuiescence();
    assert(
        native_snapshot_hazard_.load(std::memory_order_acquire) == nullptr ||
        native_snapshot_hazard_.load(std::memory_order_acquire) ==
            impl_->dispatch.load(std::memory_order_acquire));
    RetireInactiveSnapshots(*impl_);

    if (authoritative_overflow_.load(std::memory_order_acquire) &&
        !impl_->overflow_reported)
    {
        ensure_host_activity();
        impl_->overflow_reported = true;
        StopRouteReceipt overflow = FailureRoute(
            StopRouteTerminal::Overflow,
            Error(
                StopPointErrorCode::IngressOverflow,
                "The fixed native stop ingress overflowed for an authoritative event"));
        AppendHistory(*impl_, overflow);
        receipts.push_back(std::move(overflow));
    }
    return receipts;
}

std::vector<StopRouteReceipt> StopPointRouter::RoutingHistory() const
{
    return {impl_->history.begin(), impl_->history.end()};
}

std::vector<StopIngressDropDiagnostic>
StopPointRouter::PassiveDropDiagnostics() const
{
    std::vector<StopIngressDropDiagnostic> diagnostics;
    diagnostics.reserve(impl_->source_drop_counters.size());
    for (const auto& [_, counter] : impl_->source_drop_counters)
    {
        diagnostics.push_back({
            counter->source_id,
            counter->passive_drops.load(std::memory_order_acquire),
        });
    }
    return diagnostics;
}

PhysicalStopPointPlan StopPointRouter::DesiredPhysicalPlan() const
{
    const auto snapshot = impl_->dispatch.load(std::memory_order_acquire);
    return snapshot ? snapshot->physical_plan : PhysicalStopPointPlan{};
}

std::string StopPointRouter::DescribeUnroutedPause(
    std::uint32_t observed_pc) const
{
    const PhysicalStopPointPlan desired = DesiredPhysicalPlan();
    const PhysicalStopPointPlan& manager_plan = physical_manager_.current_plan();
    std::ostringstream out;
    const auto append_plan = [&](std::string_view label,
                                 const PhysicalStopPointPlan& plan) {
        out << label << " pc_count=" << plan.pcs.size() << " pcs=[";
        for (std::size_t index = 0; index < plan.pcs.size(); ++index)
        {
            if (index != 0)
                out << ',';
            out << "0x" << std::hex << std::setw(8) << std::setfill('0')
                << plan.pcs[index].pc << std::dec;
        }
        out << "] memcheck_count=" << plan.memory.size() << " memchecks=[";
        for (std::size_t index = 0; index < plan.memory.size(); ++index)
        {
            if (index != 0)
                out << ',';
            const PhysicalMemoryStop& memory = plan.memory[index];
            out << "0x" << std::hex << std::setw(8) << std::setfill('0')
                << memory.start << "-0x" << std::setw(8) << memory.end
                << std::dec << ":r" << (memory.read ? 1 : 0)
                << "w" << (memory.write ? 1 : 0);
        }
        out << ']';
    };

    out << "router epoch=" << workset_epoch_.value()
        << " dispatch_generation=" << dispatch_generation_.value()
        << " physical_generation=" << physical_manager_.generation().value()
        << " ingress_enabled="
        << (ingress_enabled_.load(std::memory_order_acquire) ? 1 : 0)
        << " authoritative_overflow="
        << (authoritative_overflow_.load(std::memory_order_acquire) ? 1 : 0)
        << " native_inflight="
        << native_inflight_.load(std::memory_order_acquire)
        << " passive_drops="
        << passive_drop_count_.load(std::memory_order_acquire)
        << " sink_bound=" << (physical_manager_.sink_bound() ? 1 : 0)
        << " exact_plan=" << (physical_manager_.has_exact_plan() ? 1 : 0)
        << " integrity_unknown="
        << (physical_manager_.integrity_unknown() ? 1 : 0) << '\n';
    append_plan("desired_plan", desired);
    out << '\n';
    append_plan("manager_plan", manager_plan);
    out << " desired_matches_manager=" << (desired == manager_plan ? 1 : 0)
        << '\n';

    if (impl_->current_point)
    {
        out << "retained_point sequence="
            << impl_->current_point->identity.sequence.value()
            << " hit_pc=0x" << std::hex << std::setw(8) << std::setfill('0')
            << impl_->current_point->evidence.hit_pc << std::dec
            << " authoritative="
            << (impl_->current_point->authoritative ? 1 : 0) << '\n';
    }
    else
    {
        out << "retained_point=<none>\n";
    }

    constexpr std::size_t history_limit = 8;
    const std::size_t first = impl_->history.size() > history_limit
        ? impl_->history.size() - history_limit
        : 0;
    out << "recent_routes count=" << (impl_->history.size() - first);
    for (std::size_t index = first; index < impl_->history.size(); ++index)
    {
        const StopRouteReceipt& receipt = impl_->history[index];
        out << "\n  terminal=" << static_cast<unsigned>(receipt.terminal)
            << " sequence=" << receipt.identity.sequence.value()
            << " epoch=" << receipt.identity.workset_epoch.value()
            << " dispatch_generation="
            << receipt.identity.dispatch_generation.value()
            << " physical_generation="
            << receipt.identity.physical_generation.value()
            << " hit_pc=0x" << std::hex << std::setw(8)
            << std::setfill('0')
            << (receipt.event ? receipt.event->evidence.hit_pc : 0)
            << std::dec
            << " authoritative="
            << (receipt.event && receipt.event->authoritative ? 1 : 0)
            << " deliveries=" << receipt.deliveries.size()
            << " core_must_remain_stopped="
            << (receipt.core_must_remain_stopped ? 1 : 0)
            << " operation=" << receipt.execution_operation_id
            << " stop_transition=" << receipt.stop_transition_id
            << " error="
            << (receipt.error.message.empty()
                    ? "<none>"
                    : receipt.error.message);
    }
    out << '\n' << physical_manager_.DescribePhysicalStopPoints(observed_pc);
    return out.str();
}

StopPointError StopPointRouter::EstablishPausedCurrentPoint(std::uint32_t pc)
{
    if (StopPointError error =
            CheckControlThread(*this, owner_thread_, initialized_, stopping_))
    {
        return error;
    }
    if (pc == 0)
    {
        return Error(
            StopPointErrorCode::InvalidArgument,
            "A paused current point requires a nonzero PC");
    }
    if (impl_->current_point)
    {
        if (impl_->current_point->identity.workset_epoch == workset_epoch_ &&
            impl_->current_point->evidence.hit_pc == pc)
        {
            return {};
        }
        return Error(
            StopPointErrorCode::CurrentPointUnavailable,
            "A different current stop point is already retained");
    }

    const DispatchSnapshot* const snapshot =
        impl_->dispatch.load(std::memory_order_acquire);
    if (!snapshot || snapshot->workset_epoch != workset_epoch_)
    {
        return Error(
            StopPointErrorCode::CurrentPointUnavailable,
            "A paused current point requires the active workset dispatch");
    }
    const std::uint64_t sequence = NextNonzero(impl_->next_stop_sequence);
    const std::uint64_t sample_snapshot =
        NextNonzero(impl_->next_sample_snapshot);
    if (sequence == 0 || sample_snapshot == 0)
    {
        authoritative_overflow_.store(true, std::memory_order_release);
        ingress_enabled_.store(false, std::memory_order_release);
        return Error(
            StopPointErrorCode::PhysicalIntegrityUnknown,
            "Paused current-point identity space is exhausted");
    }

    impl_->current_point = RoutedStopEvent{
        .identity = {
            RoutedStopSequence(sequence),
            StopSampleSnapshotId(sample_snapshot),
            snapshot->workset_epoch,
            snapshot->generation,
            snapshot->physical_generation,
        },
        .evidence = {
            .path = NativeStopPath::Synthetic,
            .point = PcStopPointSpec{pc},
            .hit_pc = pc,
        },
        .authoritative = true,
    };
    return {};
}

StopRouteReceipt StopPointRouter::InjectSyntheticStop(
    SyntheticStopPointSpec point,
    bool authoritative)
{
    if (StopPointError error =
            CheckControlThread(*this, owner_thread_, initialized_, stopping_))
    {
        return FailureRoute(
            StopRouteTerminal::RoutingFailure,
            std::move(error));
    }
    if (point.identity == 0)
    {
        return FailureRoute(
            StopRouteTerminal::RoutingFailure,
            Error(
                StopPointErrorCode::InvalidArgument,
                "Synthetic stop identity must be nonzero"));
    }

    const auto snapshot = impl_->dispatch.load(std::memory_order_acquire);
    StopPointCpuContext context{
        NativeStopPath::Synthetic,
        0,
        0,
        0,
        0,
        false,
        false,
        point.identity,
        nullptr,
        nullptr,
    };
    RoutedStopEvidence evidence{
        NativeStopPath::Synthetic,
        point,
        0,
        0,
        false,
    };
    NativePacket packet = BuildNativePacket(
        snapshot,
        context,
        std::move(evidence),
        cpu_evaluator_,
        cpu_observer_,
        impl_->next_stop_sequence,
        impl_->next_sample_snapshot);
    packet.event.authoritative =
        packet.event.authoritative || authoritative;
    if (packet.entry_count == 0)
    {
        packet.terminal = authoritative
            ? StopRouteTerminal::Unclaimed
            : StopRouteTerminal::None;
        packet.request_break = authoritative;
    }
    return ProcessPacket(
        *impl_,
        std::move(packet),
        dispatch_generation_,
        workset_epoch_,
        physical_manager_.generation());
}

StopPointLifecycleReceipt StopPointRouter::StopIngressDrainAndCleanup()
{
    if (stopped_)
        return LifecycleSuccess(*this, physical_manager_.generation());
    if (!initialized_)
    {
        return LifecycleFailure(
            *this,
            Error(StopPointErrorCode::InvalidArgument, "Stop-point router is not initialized"));
    }
    if (owner_thread_ != std::this_thread::get_id())
    {
        return LifecycleFailure(
            *this,
            Error(StopPointErrorCode::WrongThread, "Stop-point cleanup used the wrong thread"));
    }

    ingress_enabled_.store(false, std::memory_order_release);
    WaitForNativeIngressQuiescence();
    const std::vector<StopRouteReceipt> drained = DrainIngress();
    stopping_ = true;

    for (const auto& [group_id, group] : impl_->groups)
    {
        impl_->released_registration_sequences[group_id] =
            group.registration_sequence;
    }

    const StopDispatchGeneration next =
        NextDispatchGeneration(dispatch_generation_);
    auto empty_snapshot = std::make_unique<DispatchSnapshot>();
    empty_snapshot->generation = next;
    empty_snapshot->physical_generation =
        physical_manager_.next_generation();
    empty_snapshot->workset_epoch = workset_epoch_;
    const DispatchSnapshot* empty_snapshot_pointer = nullptr;
    const bool archived_empty = next &&
        ArchiveSnapshot(*impl_, std::move(empty_snapshot));
    if (archived_empty)
        empty_snapshot_pointer = impl_->snapshot_archive.back().get();

    PhysicalStopBackendReceipt clear = physical_manager_.ClearOwnedStopPoints(
        [&] {
            impl_->groups.clear();
            if (archived_empty)
            {
                dispatch_generation_ = next;
                impl_->dispatch.store(
                    empty_snapshot_pointer,
                    std::memory_order_release);
            }
            else
            {
                impl_->dispatch.store(nullptr, std::memory_order_release);
            }
        });
    PhysicalStopBackendReceipt unbind =
        physical_manager_.UnbindNativeStopSink();

    impl_->ingress.Clear();
    impl_->emergency.Clear();
    ingress_notification_counter_ = nullptr;
    ingress_notifier_context_ = nullptr;
    ingress_notifier_ = nullptr;
    impl_->current_point.reset();
    stopped_ = clear.ok && unbind.ok;
    if (stopped_)
    {
        RetireInactiveSnapshots(*impl_);
        stopping_ = false;
        return LifecycleSuccess(
            *this,
            clear.generation,
            drained.size());
    }

    const PhysicalStopBackendReceipt& failed =
        !clear.ok ? clear : unbind;
    return LifecycleFailure(*this, PhysicalError(failed), failed.integrity);
}

} // namespace savor::runtime
