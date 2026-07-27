#pragma once

#include "../RuntimeTypes.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace savor::runtime {

struct StopSourceIdTag;
struct StopSubscriptionGroupIdTag;
struct StopSubscriptionIdTag;
struct RoutedStopSequenceTag;
struct StopSampleSnapshotIdTag;
struct StopDispatchGenerationTag;
struct PhysicalPlanGenerationTag;

using StopSourceId = StrongId<StopSourceIdTag>;
using StopSubscriptionGroupId = StrongId<StopSubscriptionGroupIdTag>;
using StopSubscriptionId = StrongId<StopSubscriptionIdTag>;
using RoutedStopSequence = StrongId<RoutedStopSequenceTag>;
using StopSampleSnapshotId = StrongId<StopSampleSnapshotIdTag>;
using StopDispatchGeneration = StrongId<StopDispatchGenerationTag>;
using PhysicalPlanGeneration = StrongId<PhysicalPlanGenerationTag>;

static_assert(!std::is_same_v<RoutedStopSequence, StopDispatchGeneration>);
static_assert(!std::is_convertible_v<StateEpoch, StopDispatchGeneration>);

enum class StopMemoryAccess : std::uint8_t
{
    Read,
    Write,
    Access,
};

struct PcStopPointSpec
{
    std::uint32_t pc = 0;

    friend bool operator==(const PcStopPointSpec&, const PcStopPointSpec&) = default;
};

struct MemoryStopPointSpec
{
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    StopMemoryAccess access = StopMemoryAccess::Write;

    friend bool operator==(const MemoryStopPointSpec&, const MemoryStopPointSpec&) = default;
};

struct SyntheticStopPointSpec
{
    std::uint64_t identity = 0;

    friend bool operator==(const SyntheticStopPointSpec&, const SyntheticStopPointSpec&) = default;
};

using StopPointSpec = std::variant<
    PcStopPointSpec,
    MemoryStopPointSpec,
    SyntheticStopPointSpec>;

enum class StopDeliveryMode : std::uint8_t
{
    Observe,
    Progress,
    Guard,
    Intercept,
    Wake,
};

enum class StopRoutingPolicy : std::uint8_t
{
    Pass,
    Consume,
    RequestInterruptionHandler,
    Fail,
};

enum class StopEpochPolicy : std::uint8_t
{
    EpochAgnostic,
    EndOnEpochChange,
    RebindAfterRestore,
};

enum class StopSubscriptionLifetime : std::uint8_t
{
    Scoped,
    OneShot,
};

struct StopSourceIdentity
{
    StopSourceId id;
    std::string stable_name;
    std::string diagnostic_label;
};

enum class NativeStopPath : std::uint8_t
{
    Jit,
    Memcheck,
    Synthetic,
};

struct RoutedStopIdentity
{
    RoutedStopSequence sequence;
    StopSampleSnapshotId sample_snapshot;
    StateEpoch state_epoch;
    StopDispatchGeneration dispatch_generation;
    PhysicalPlanGeneration physical_generation;

    friend bool operator==(
        const RoutedStopIdentity&,
        const RoutedStopIdentity&) = default;
};

struct RoutedStopEvidence
{
    NativeStopPath path = NativeStopPath::Synthetic;
    StopPointSpec point = SyntheticStopPointSpec{};
    std::uint32_t hit_pc = 0;
    std::uint64_t value = 0;
    bool post_write = false;

    friend bool operator==(
        const RoutedStopEvidence&,
        const RoutedStopEvidence&) = default;
};

inline constexpr std::size_t kMaxRoutedHitSamples = 32;

struct RoutedHitSample
{
    std::uint32_t descriptor_id = 0;
    std::uint64_t value = 0;
    bool available = false;
};

struct RoutedStopEvent
{
    RoutedStopIdentity identity;
    RoutedStopEvidence evidence;
    std::array<RoutedHitSample, kMaxRoutedHitSamples> samples{};
    std::uint8_t sample_count = 0;
    bool active_foreground_wake = false;
    bool authoritative = false;
    bool requires_physical_reconcile_before_resume = false;
};

class IStopPointConsumer;

struct StopSubscriptionDefinition
{
    StopSubscriptionId id;
    StopPointSpec point;
    StopDeliveryMode delivery = StopDeliveryMode::Observe;
    StopRoutingPolicy policy = StopRoutingPolicy::Pass;
    StopSubscriptionLifetime lifetime = StopSubscriptionLifetime::Scoped;
    std::int32_t priority = 0;
    std::uint32_t qualification_id = 0;
    std::vector<std::uint32_t> sample_descriptor_ids;
    std::uint32_t cpu_observer_descriptor_id = 0;
    std::string interruption_handler_key;
    bool lossless = false;
    bool suppress_immediate_reentry = false;
    IStopPointConsumer* consumer = nullptr;
};

struct StopSubscriptionGroupDefinition
{
    StopSubscriptionGroupId id;
    StopSourceIdentity source;
    StopEpochPolicy epoch_policy = StopEpochPolicy::EndOnEpochChange;
    std::vector<StopSubscriptionDefinition> subscriptions;
};

enum class StopPointErrorCode : std::uint16_t
{
    None,
    InvalidArgument,
    InvalidPolicy,
    WrongThread,
    GroupNotFound,
    SourceMismatch,
    ForegroundWakeAlreadyRegistered,
    CurrentPointUnavailable,
    StaleEpoch,
    StaleDispatchGeneration,
    IngressOverflow,
    PhysicalReconcileFailed,
    PhysicalIntegrityUnknown,
    RuntimeStopping,
};

struct StopPointError
{
    StopPointErrorCode code = StopPointErrorCode::None;
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return code != StopPointErrorCode::None;
    }
};

struct StopSubscriptionGroupLease
{
    StopSubscriptionGroupId group_id;
    StopSourceId source_id;
    std::uint64_t registration_sequence = 0;
    StateEpoch acquisition_epoch;
    bool active = false;
};

struct StopGroupReceipt
{
    bool ok = false;
    StopSubscriptionGroupLease lease;
    StopDispatchGeneration dispatch_generation;
    PhysicalPlanGeneration physical_generation;
    StopPointError error;
};

struct StopReleaseReceipt
{
    bool ok = false;
    bool already_released = false;
    StopSubscriptionGroupId group_id;
    StopDispatchGeneration dispatch_generation;
    PhysicalPlanGeneration physical_generation;
    StopPointError error;
};

struct StopInterruptionHandlerRequest
{
    StopSourceId source_id;
    StopSubscriptionGroupId group_id;
    StopSubscriptionId subscription_id;
    std::string interruption_handler_key;
};

enum class StopRouteTerminal : std::uint8_t
{
    None,
    WokeForeground,
    Consumed,
    InterruptionHandlerRequested,
    Failed,
    Unclaimed,
    Stale,
    Overflow,
};

struct StopDelivery
{
    RoutedStopEvent event;
    StopSourceId source_id;
    StopSubscriptionGroupId group_id;
    StopSubscriptionId subscription_id;
    StopDeliveryMode delivery = StopDeliveryMode::Observe;
};

class IStopPointConsumer
{
public:
    virtual ~IStopPointConsumer() = default;
    virtual void OnStopPoint(const StopDelivery& delivery) = 0;
};

struct StopRouteReceipt
{
    StopRouteTerminal terminal = StopRouteTerminal::None;
    RoutedStopIdentity identity;
    std::optional<RoutedStopEvent> event;
    std::optional<StopInterruptionHandlerRequest>
        interruption_handler_request;
    std::vector<StopDelivery> deliveries;
    StopPointError error;
    bool core_must_remain_stopped = false;
};

struct PhysicalPcStop
{
    std::uint32_t pc = 0;

    friend bool operator==(const PhysicalPcStop&, const PhysicalPcStop&) = default;
};

struct PhysicalMemoryStop
{
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    bool read = false;
    bool write = false;

    friend bool operator==(const PhysicalMemoryStop&, const PhysicalMemoryStop&) = default;
};

struct PhysicalStopPointPlan
{
    std::vector<PhysicalPcStop> pcs;
    std::vector<PhysicalMemoryStop> memory;

    friend bool operator==(const PhysicalStopPointPlan&, const PhysicalStopPointPlan&) = default;
};

enum class PhysicalStopIntegrity : std::uint8_t
{
    Preserved,
    Unknown,
};

struct PhysicalStopBackendReceipt
{
    bool ok = false;
    PhysicalStopIntegrity integrity = PhysicalStopIntegrity::Preserved;
    PhysicalPlanGeneration generation;
    PhysicalStopPointPlan actual;
    std::string message;
};

} // namespace savor::runtime
