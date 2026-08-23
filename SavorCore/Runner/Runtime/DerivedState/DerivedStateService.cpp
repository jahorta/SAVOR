#include "DerivedStateService.h"
#include "../../../Utils/Log.h"

#include "../../../Core/Memory/Soa/SoaAddrRegistry.h"
#include "../../../Core/Memory/Soa/SoaStructs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace savor::runtime::derived {
namespace {

constexpr StopSourceId kSourceId(0x44530001u);
constexpr StopSubscriptionGroupId kInitializationGroupId(0x44530001u);
constexpr StopSubscriptionGroupId kNativeGroupId(0x44530002u);
constexpr std::uint64_t kSubscriptionIdBase = 0x4453000100000000ull;
constexpr std::size_t kEvidenceQueueCapacity = kStopPointNativeIngressCapacity;
constexpr std::size_t kMaximumBattleEvidenceBytes =
    sizeof(soa::BattleState::useable_items);
constexpr std::size_t kMaximumRefreshGroupsPerItem = 64;
constexpr std::size_t kMaximumRefreshGroupsPerHit = 32;

enum class BattleEvidenceKind : std::uint8_t
{
    None,
    TurnEntry,
    TurnOrder,
    Rewards,
};

enum class BattleEvidenceFailure : std::uint8_t
{
    None,
    StaleItem,
    UnknownTrigger,
    CurrentTurnRead,
    MainPointerRead,
    MainPointerNull,
    PayloadAddressOverflow,
    PayloadRead,
};

struct CpuRefreshPlanEntry
{
    std::uint32_t trigger_pc = 0;
    const DerivedStateBlockDescriptor* block = nullptr;
    const DerivedStateRefreshGroupDescriptor* group = nullptr;
    BattleEvidenceKind kind = BattleEvidenceKind::None;
};

struct BattleGroupEvidenceRecord
{
    const DerivedStateBlockDescriptor* block = nullptr;
    const DerivedStateRefreshGroupDescriptor* group = nullptr;
    BattleEvidenceKind kind = BattleEvidenceKind::None;
    BattleEvidenceFailure failure = BattleEvidenceFailure::None;
    HitTimeGuestReadError backend_error = HitTimeGuestReadError::None;
    std::uint32_t failure_address = 0;
    std::size_t failure_size = 0;
    std::uint8_t current_turn = 0;
    std::uint32_t main_pointer = 0;
    std::array<std::uint8_t, kMaximumBattleEvidenceBytes> payload{};
    std::size_t payload_size = 0;
};

struct DerivedEventEvidenceRecord
{
    RoutedStopIdentity routed_stop;
    WorksetEpoch epoch;
    WorkerWorksetItemId item_id;
    std::uint32_t trigger_pc = 0;
    std::array<BattleGroupEvidenceRecord, kMaximumRefreshGroupsPerHit> groups{};
    std::size_t group_count = 0;
};

struct CpuItemPlan
{
    WorksetEpoch epoch;
    WorkerWorksetItemId item_id;
    std::array<CpuRefreshPlanEntry, kMaximumRefreshGroupsPerItem> entries{};
    std::size_t entry_count = 0;
};

template <typename Value, std::size_t Capacity>
class SpscEvidenceQueue final
{
public:
    [[nodiscard]] bool TryPush(const Value& value) noexcept
    {
        const std::size_t write = write_.load(std::memory_order_relaxed);
        const std::size_t read = read_.load(std::memory_order_acquire);
        if (write - read >= Capacity)
            return false;
        values_[write % Capacity] = value;
        write_.store(write + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool TryPop(Value& value) noexcept
    {
        const std::size_t read = read_.load(std::memory_order_relaxed);
        const std::size_t write = write_.load(std::memory_order_acquire);
        if (read == write)
            return false;
        value = values_[read % Capacity];
        read_.store(read + 1, std::memory_order_release);
        return true;
    }

    void Clear() noexcept
    {
        const std::size_t write = write_.load(std::memory_order_acquire);
        read_.store(write, std::memory_order_release);
    }

private:
    std::array<Value, Capacity> values_{};
    std::atomic<std::size_t> read_{0};
    std::atomic<std::size_t> write_{0};
};

std::uint16_t ReadBeU16(const std::uint8_t* bytes)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[0]) << 8u) |
        static_cast<std::uint16_t>(bytes[1]));
}

std::int16_t ReadBeI16(const std::uint8_t* bytes)
{
    return static_cast<std::int16_t>(ReadBeU16(bytes));
}

std::uint32_t ReadBeU32(const std::uint8_t* bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24u) |
        (static_cast<std::uint32_t>(bytes[1]) << 16u) |
        (static_cast<std::uint32_t>(bytes[2]) << 8u) |
        static_cast<std::uint32_t>(bytes[3]);
}

void CheckedAdd(
    std::map<std::uint16_t, std::uint32_t>& totals,
    std::uint16_t item_id,
    std::uint32_t count)
{
    auto& total = totals[item_id];
    if (count > std::numeric_limits<std::uint32_t>::max() - total)
        throw std::runtime_error("Derived Battle item total overflowed u32");
    total += count;
}

std::vector<BattleItemTotalV1> ToSparseTotals(
    const std::map<std::uint16_t, std::uint32_t>& totals)
{
    std::vector<BattleItemTotalV1> result;
    result.reserve(totals.size());
    for (const auto& [item_id, count] : totals)
    {
        if (count != 0)
            result.push_back({item_id, count});
    }
    return result;
}

std::vector<BattleItemTotalV1> ParseInventory(
    std::span<const std::uint8_t> bytes)
{
    if (bytes.size() != sizeof(soa::BattleState::useable_items))
        throw std::runtime_error("Battle inventory evidence has the wrong size");
    std::map<std::uint16_t, std::uint32_t> totals;
    for (std::size_t i = 0; i < 80; ++i)
    {
        const auto* slot = bytes.data() + i * sizeof(soa::ItemSlot);
        const auto item_id = ReadBeU16(slot);
        const auto count = static_cast<std::uint32_t>(slot[2]);
        if (count != 0)
            CheckedAdd(totals, item_id, count);
    }
    return ToSparseTotals(totals);
}

std::vector<std::uint8_t> ParseTurnOrder(
    std::span<const std::uint8_t> bytes)
{
    if (bytes.size() != 12)
        throw std::runtime_error("Battle turn-order evidence has the wrong size");
    std::vector<std::uint8_t> slots;
    std::set<std::uint8_t> seen;
    for (const auto slot : bytes)
    {
        if (slot == 0xFFu)
            break;
        if (slot > 11u || !seen.insert(slot).second)
            throw std::runtime_error(
                "Battle turn order contains an invalid active slot");
        slots.push_back(slot);
    }
    return slots;
}

std::vector<BattleItemTotalV1> ParseRewards(
    std::span<const std::uint8_t> bytes)
{
    if (bytes.size() != sizeof(soa::BattleState::item_drops))
        throw std::runtime_error("Battle reward evidence has the wrong size");
    std::map<std::uint16_t, std::uint32_t> totals;
    for (std::size_t i = 0; i < 8; ++i)
    {
        const auto* slot = bytes.data() +
            i * sizeof(soa::BattleItemDropSlot);
        const auto count = ReadBeI16(slot);
        const auto item_id = ReadBeI16(slot + 2);
        if (count <= 0)
            continue;
        if (item_id < 0)
        {
            throw std::runtime_error(
                "Battle rewards contain a negative item ID with a positive count");
        }
        CheckedAdd(
            totals,
            static_cast<std::uint16_t>(item_id),
            static_cast<std::uint32_t>(count));
    }
    return ToSparseTotals(totals);
}

const DerivedStateRefreshGroupDescriptor& RequireGroup(
    const DerivedStateRegistry& registry,
    std::string_view group_id)
{
    const auto* group = registry.FindGroup(kBattleCoreBlockId, group_id);
    if (!group)
        throw std::runtime_error(
            "The static Battle derived-state group is unavailable");
    return *group;
}

BattleEvidenceKind BattleEvidenceKindFor(
    std::string_view group_id) noexcept
{
    if (group_id == kBattleTurnEntryGroupId)
        return BattleEvidenceKind::TurnEntry;
    if (group_id == kBattleTurnOrderGroupId)
        return BattleEvidenceKind::TurnOrder;
    if (group_id == kBattleRewardsGroupId)
        return BattleEvidenceKind::Rewards;
    return BattleEvidenceKind::None;
}

std::string_view FailureName(BattleEvidenceFailure failure) noexcept
{
    switch (failure)
    {
    case BattleEvidenceFailure::None: return "none";
    case BattleEvidenceFailure::StaleItem: return "stale item";
    case BattleEvidenceFailure::UnknownTrigger: return "unknown trigger";
    case BattleEvidenceFailure::CurrentTurnRead: return "current-turn read";
    case BattleEvidenceFailure::MainPointerRead: return "BattleState pointer read";
    case BattleEvidenceFailure::MainPointerNull: return "null BattleState pointer";
    case BattleEvidenceFailure::PayloadAddressOverflow: return "payload address overflow";
    case BattleEvidenceFailure::PayloadRead: return "BattleState payload read";
    }
    return "unknown";
}

std::string_view HitTimeErrorName(HitTimeGuestReadError error) noexcept
{
    switch (error)
    {
    case HitTimeGuestReadError::None: return "none";
    case HitTimeGuestReadError::InvalidArgument: return "invalid argument";
    case HitTimeGuestReadError::BackendUnavailable: return "backend unavailable";
    case HitTimeGuestReadError::UnmappedRange: return "unmapped range";
    }
    return "unknown";
}

} // namespace

struct DerivedStateService::CpuState
{
    explicit CpuState(IHitTimeGuestMemoryBackendPort& memory)
        : memory(memory)
    {
    }

    void CaptureGroup(
        const CpuRefreshPlanEntry& entry,
        BattleGroupEvidenceRecord& record) const noexcept
    {
        record.block = entry.block;
        record.group = entry.group;
        record.kind = entry.kind;
        record.payload_size = entry.kind == BattleEvidenceKind::TurnEntry
            ? sizeof(soa::BattleState::useable_items)
            : entry.kind == BattleEvidenceKind::TurnOrder
                ? 12
                : entry.kind == BattleEvidenceKind::Rewards
                    ? sizeof(soa::BattleState::item_drops)
                    : 0;
        if (entry.kind == BattleEvidenceKind::None)
        {
            record.failure = BattleEvidenceFailure::UnknownTrigger;
            return;
        }

        const auto read = [&](std::uint32_t address,
                              std::span<std::uint8_t> destination,
                              BattleEvidenceFailure failure) {
            if (record.failure != BattleEvidenceFailure::None)
                return;
            const HitTimeGuestReadReceipt receipt =
                memory.ReadHitTimeBytes(address, destination);
            if (!receipt.ok)
            {
                record.failure = failure;
                record.backend_error = receipt.error;
                record.failure_address = receipt.address;
                record.failure_size = receipt.size;
            }
        };

        std::array<std::uint8_t, 1> current_turn{};
        read(
            addr::AddrRegistry::base(addr::battle::CurrentTurn),
            current_turn,
            BattleEvidenceFailure::CurrentTurnRead);
        record.current_turn = current_turn[0];

        if (entry.kind == BattleEvidenceKind::TurnOrder)
        {
            read(
                addr::AddrRegistry::base(addr::battle::TurnOrderTable),
                std::span(record.payload).first(record.payload_size),
                BattleEvidenceFailure::PayloadRead);
            return;
        }

        std::array<std::uint8_t, 4> pointer{};
        read(
            addr::AddrRegistry::base(addr::battle::MainInstancePtr),
            pointer,
            BattleEvidenceFailure::MainPointerRead);
        if (record.failure == BattleEvidenceFailure::None)
        {
            record.main_pointer = ReadBeU32(pointer.data());
            if (record.main_pointer == 0)
                record.failure = BattleEvidenceFailure::MainPointerNull;
        }
        const std::uint32_t offset = entry.kind == BattleEvidenceKind::TurnEntry
            ? static_cast<std::uint32_t>(
                  offsetof(soa::BattleState, useable_items))
            : static_cast<std::uint32_t>(
                  offsetof(soa::BattleState, item_drops));
        if (record.failure != BattleEvidenceFailure::None)
            return;
        if (record.main_pointer >
            std::numeric_limits<std::uint32_t>::max() - offset)
        {
            record.failure = BattleEvidenceFailure::PayloadAddressOverflow;
            record.failure_address = record.main_pointer;
            record.failure_size = record.payload_size;
            return;
        }
        read(
            record.main_pointer + offset,
            std::span(record.payload).first(record.payload_size),
            BattleEvidenceFailure::PayloadRead);
    }

    [[nodiscard]] StopCpuObservationResult Observe(
        const RoutedStopEvent& event) noexcept
    {
        const CpuItemPlan* plan = active_plan.load(std::memory_order_acquire);
        if (!plan)
            return StopCpuObservationResult::Ignored;

        DerivedEventEvidenceRecord record{
            .routed_stop = event.identity,
            .epoch = plan->epoch,
            .item_id = plan->item_id,
            .trigger_pc = event.evidence.hit_pc,
        };
        const bool epoch_matches =
            event.identity.workset_epoch == plan->epoch;
        bool captured = epoch_matches;
        for (std::size_t index = 0; index < plan->entry_count; ++index)
        {
            const auto& entry = plan->entries[index];
            if (entry.trigger_pc != record.trigger_pc)
                continue;
            if (record.group_count == record.groups.size())
            {
                overflowed.store(true, std::memory_order_release);
                return StopCpuObservationResult::Failed;
            }
            auto& group = record.groups[record.group_count++];
            if (!epoch_matches)
            {
                group.block = entry.block;
                group.group = entry.group;
                group.kind = entry.kind;
                group.failure = BattleEvidenceFailure::StaleItem;
            }
            else
            {
                CaptureGroup(entry, group);
                captured = captured &&
                    group.failure == BattleEvidenceFailure::None;
            }
        }
        if (record.group_count == 0)
            return StopCpuObservationResult::Failed;
        if (!queue.TryPush(record))
        {
            overflowed.store(true, std::memory_order_release);
            return StopCpuObservationResult::Failed;
        }
        return captured
            ? StopCpuObservationResult::Observed
            : StopCpuObservationResult::Failed;
    }

    IHitTimeGuestMemoryBackendPort& memory;
    std::unique_ptr<CpuItemPlan> plan;
    std::atomic<const CpuItemPlan*> active_plan{nullptr};
    SpscEvidenceQueue<DerivedEventEvidenceRecord, kEvidenceQueueCapacity> queue;
    std::atomic<bool> overflowed{false};
};

DerivedStateReceipt DerivedStateReceipt::Success()
{
    return {.ok = true};
}

DerivedStateReceipt DerivedStateReceipt::Failure(
    DerivedStateErrorCode code,
    std::string message)
{
    return {.ok = false, .code = code, .message = std::move(message)};
}

DerivedStateService::DerivedStateService(
    GuestMemory& memory,
    IHitTimeGuestMemoryBackendPort& hit_time_memory,
    const DerivedStateRegistry& registry)
    : memory_(memory),
      hit_time_memory_(hit_time_memory),
      registry_(registry),
      cpu_(std::make_unique<CpuState>(hit_time_memory_)),
      owner_thread_(std::this_thread::get_id())
{
}

DerivedStateService::~DerivedStateService()
{
    (void)CloseItem();
}

bool DerivedStateService::OnOwnerThread() const noexcept
{
    return owner_thread_ == std::this_thread::get_id();
}

DerivedStateReceipt DerivedStateService::BindRouter(
    StopPointRouter& router,
    WorksetEpoch epoch)
{
    if (!OnOwnerThread() || router_ || !epoch ||
        router.workset_epoch() != epoch)
    {
        return DerivedStateReceipt::Failure(
            DerivedStateErrorCode::InvalidState,
            "Derived state could not bind the current session router");
    }
    router_ = &router;
    return DerivedStateReceipt::Success();
}

DerivedStateReceipt DerivedStateService::ActivateItem(
    const WorksetDerivedStateBindingV1& binding,
    WorksetEpoch epoch,
    WorkerWorksetItemId item_id)
{
    if (!OnOwnerThread() || active_ || !router_ || !epoch || !item_id)
    {
        return DerivedStateReceipt::Failure(
            DerivedStateErrorCode::InvalidState,
            "Derived state can only activate one item on its owner thread");
    }
    std::string validation_error;
    if (!ValidateWorksetDerivedStateBindingV1(binding, &validation_error))
    {
        return DerivedStateReceipt::Failure(
            DerivedStateErrorCode::InvalidArgument,
            std::move(validation_error));
    }
    if (router_->workset_epoch() != epoch)
    {
        return DerivedStateReceipt::Failure(
            DerivedStateErrorCode::StaleEpoch,
            "Derived-state activation does not match the router epoch");
    }

    active_ = true;
    epoch_ = epoch;
    item_id_ = item_id;
    for (const auto& block : binding.blocks)
    {
        const auto* descriptor =
            registry_.FindBlock(block.identity.canonical_id);
        if (!descriptor || descriptor->identity != block.identity ||
            block.configuration.size() >
                descriptor->maximum_configuration_bytes)
        {
            (void)CloseItem();
            return DerivedStateReceipt::Failure(
                DerivedStateErrorCode::InvalidArgument,
                "Derived-state activation binding does not match the static registry");
        }
        if (block.identity.canonical_id == kBattleCoreBlockId)
        {
            if (descriptor->refresh_provider.canonical_id !=
                    kBattleCoreRefreshProviderId ||
                descriptor->refresh_provider.revision != 1)
            {
                (void)CloseItem();
                return DerivedStateReceipt::Failure(
                    DerivedStateErrorCode::InvalidArgument,
                    "Battle derived-state refresh provider is not canonical");
            }
            battle_core_active_ = true;
            battle_core_identity_ = block.identity;
        }
    }
    if (!battle_core_active_)
        return DerivedStateReceipt::Success();

    auto cpu_plan = std::make_unique<CpuItemPlan>();
    cpu_plan->epoch = epoch_;
    cpu_plan->item_id = item_id_;
    for (const auto& block_binding : binding.blocks)
    {
        const auto* descriptor =
            registry_.FindBlock(block_binding.identity.canonical_id);
        if (!descriptor ||
            descriptor->refresh_provider.canonical_id !=
                kBattleCoreRefreshProviderId)
        {
            continue;
        }
        for (const auto& group : descriptor->groups)
        {
            const BattleEvidenceKind kind =
                BattleEvidenceKindFor(group.group_id);
            if (kind == BattleEvidenceKind::None)
            {
                (void)CloseItem();
                return DerivedStateReceipt::Failure(
                    DerivedStateErrorCode::InvalidArgument,
                    "Battle derived-state refresh group has no static CPU sampler");
            }
            for (const auto& trigger : group.triggers)
            {
                if (cpu_plan->entry_count == cpu_plan->entries.size())
                {
                    (void)CloseItem();
                    return DerivedStateReceipt::Failure(
                        DerivedStateErrorCode::InvalidArgument,
                        "Derived-state item exceeds the fixed CPU refresh-plan capacity");
                }
                cpu_plan->entries[cpu_plan->entry_count++] = {
                    trigger.pc,
                    descriptor,
                    &group,
                    kind};
            }
        }
    }
    for (std::size_t index = 0; index < cpu_plan->entry_count; ++index)
    {
        std::size_t matching = 0;
        for (std::size_t other = 0; other < cpu_plan->entry_count; ++other)
        {
            matching += cpu_plan->entries[other].trigger_pc ==
                cpu_plan->entries[index].trigger_pc;
        }
        if (matching > kMaximumRefreshGroupsPerHit)
        {
            (void)CloseItem();
            return DerivedStateReceipt::Failure(
                DerivedStateErrorCode::InvalidArgument,
                "Derived-state trigger exceeds the fixed CPU evidence capacity");
        }
    }
    SCLOGDX(
        SC_TAGS("derived_state.refresh_plan", "derived_state.transition"),
        "epoch=%llu item=%llu entries=%zu",
        static_cast<unsigned long long>(epoch_.value()),
        static_cast<unsigned long long>(item_id_.value()),
        cpu_plan->entry_count);
    for (std::size_t index = 0; index < cpu_plan->entry_count; ++index)
    {
        const auto& entry = cpu_plan->entries[index];
        SCLOGDX(
            SC_TAGS("derived_state.refresh_plan", "derived_state.subscription"),
            "epoch=%llu item=%llu ordinal=%zu group=%s trigger_pc=0x%08X",
            static_cast<unsigned long long>(epoch_.value()),
            static_cast<unsigned long long>(item_id_.value()),
            index,
            entry.group ? entry.group->group_id.c_str() : "<missing>",
            entry.trigger_pc);
    }
    cpu_->plan = std::move(cpu_plan);
    cpu_->overflowed.store(false, std::memory_order_release);
    cpu_->queue.Clear();
    cpu_->active_plan.store(cpu_->plan.get(), std::memory_order_release);

    StopSubscriptionGroupDefinition initialization_definition{
        .id = kInitializationGroupId,
        .source = {
            .id = kSourceId,
            .stable_name = "soa.derived-state.battle.core",
            .diagnostic_label = "Battle core derived state",
        },
    };
    StopSubscriptionGroupDefinition native_definition{
        .id = kNativeGroupId,
        .source = initialization_definition.source,
    };
    std::set<std::uint32_t> initialization_trigger_pcs;
    std::set<std::uint32_t> native_trigger_pcs;
    const auto& block = *registry_.FindBlock(kBattleCoreBlockId);
    for (const auto& group : block.groups)
    {
        for (const auto& trigger : group.triggers)
        {
            (group.accepts_current_point_initialization
                 ? initialization_trigger_pcs
                 : native_trigger_pcs)
                .insert(trigger.pc);
        }
    }
    std::uint64_t subscription_ordinal = 1;
    const auto append_subscriptions =
        [&](const std::set<std::uint32_t>& pcs,
            StopSubscriptionGroupDefinition& definition) {
        for (const auto pc : pcs)
        {
            definition.subscriptions.push_back({
                .id = StopSubscriptionId(
                    kSubscriptionIdBase + subscription_ordinal++),
                .point = PcStopPointSpec{pc},
                .route = PassiveStopObservation{
                    .cpu_observer_descriptor_id = CanonicalStopCpuObserverId(
                        CanonicalStopCpuObserver::DerivedState),
                    .lossless = true,
                },
                .lifetime = StopSubscriptionLifetime::Scoped,
                .priority = std::numeric_limits<std::int32_t>::max(),
                .consumer = this,
            });
        }
    };
    append_subscriptions(
        initialization_trigger_pcs, initialization_definition);
    append_subscriptions(native_trigger_pcs, native_definition);

    if (!initialization_definition.subscriptions.empty())
    {
        accepting_current_point_ = true;
        auto registered = router_->RegisterGroup(
            std::move(initialization_definition),
            {.current_point = StopCurrentPointPolicy::AcceptIfAvailable});
        accepting_current_point_ = false;
        if (!registered.receipt.ok ||
            (registered.current_point &&
             registered.current_point->terminal ==
                 StopRouteTerminal::RoutingFailure))
        {
            std::string message = registered.receipt.error.message;
            if (message.empty() && registered.current_point)
                message = registered.current_point->error.message;
            initialization_subscriptions_ = std::move(registered.handle);
            (void)CloseItem();
            return DerivedStateReceipt::Failure(
                DerivedStateErrorCode::RoutingFailure,
                message.empty()
                    ? "Battle derived-state initialization subscription could not activate"
                    : std::move(message));
        }
        initialization_subscriptions_ = std::move(registered.handle);
    }

    if (!native_definition.subscriptions.empty())
    {
        auto registered = router_->RegisterGroup(std::move(native_definition));
        if (!registered.receipt.ok)
        {
            std::string message = registered.receipt.error.message;
            native_subscriptions_ = std::move(registered.handle);
            (void)CloseItem();
            return DerivedStateReceipt::Failure(
                DerivedStateErrorCode::RoutingFailure,
                message.empty()
                    ? "Battle derived-state native subscriptions could not activate"
                    : std::move(message));
        }
        native_subscriptions_ = std::move(registered.handle);
    }
    return DerivedStateReceipt::Success();
}

DerivedStateReceipt DerivedStateService::CloseItem() noexcept
{
    if (!OnOwnerThread())
    {
        return DerivedStateReceipt::Failure(
            DerivedStateErrorCode::InvalidState,
            "Derived state can only close on its owner thread");
    }
    if (native_subscriptions_.active())
    {
        const auto released = native_subscriptions_.Release();
        if (!released.ok)
        {
            return DerivedStateReceipt::Failure(
                DerivedStateErrorCode::RoutingFailure,
                released.error.message.empty()
                    ? "Derived-state subscriptions could not release"
                    : released.error.message);
        }
    }
    if (initialization_subscriptions_.active())
    {
        const auto released = initialization_subscriptions_.Release();
        if (!released.ok)
        {
            return DerivedStateReceipt::Failure(
                DerivedStateErrorCode::RoutingFailure,
                released.error.message.empty()
                    ? "Derived-state initialization subscriptions could not release"
                    : released.error.message);
        }
    }
    cpu_->active_plan.store(nullptr, std::memory_order_release);
    cpu_->queue.Clear();
    cpu_->overflowed.store(false, std::memory_order_release);
    cpu_->plan.reset();
    accepting_current_point_ = false;
    turn_entry_.reset();
    turn_order_.reset();
    rewards_.reset();
    turn_entry_generation_ = 0;
    turn_order_generation_ = 0;
    rewards_generation_ = 0;
    battle_core_identity_ = {};
    battle_core_active_ = false;
    item_id_ = {};
    epoch_ = {};
    active_ = false;
    return DerivedStateReceipt::Success();
}

DerivedStateReceipt DerivedStateService::ValidateQuery(
    const DerivedStateQueryV1& query,
    const DerivedStateSnapshotProvenanceV1* provenance) const
{
    if (!OnOwnerThread() || !active_ || !battle_core_active_)
    {
        return DerivedStateReceipt::Failure(
            DerivedStateErrorCode::InvalidState,
            "Battle derived state is not active for this item");
    }
    if (query.workset_epoch != epoch_ || query.item_id != item_id_)
    {
        return DerivedStateReceipt::Failure(
            DerivedStateErrorCode::StaleEpoch,
            "Derived-state query names a stale item or epoch");
    }
    if (!provenance)
    {
        return DerivedStateReceipt::Failure(
            DerivedStateErrorCode::MissingSnapshot,
            "The requested derived-state group has not refreshed in this item");
    }
    if (query.freshness == DerivedStateFreshness::SameRoutedEvent)
    {
        if (!query.routed_stop.sequence ||
            query.routed_stop != provenance->routed_stop)
        {
            return DerivedStateReceipt::Failure(
                DerivedStateErrorCode::MissingSnapshot,
                "Derived-state query requires the exact routed event generation");
        }
    }
    else if (query.freshness != DerivedStateFreshness::LatestInItem)
    {
        return DerivedStateReceipt::Failure(
            DerivedStateErrorCode::InvalidArgument,
            "Derived-state query freshness is unknown");
    }
    return DerivedStateReceipt::Success();
}

#define SAVOR_DERIVED_QUERY(method_name, member_name, snapshot_type) \
DerivedStateQueryResult<snapshot_type> DerivedStateService::method_name( \
    const DerivedStateQueryV1& query) const \
{ \
    const auto receipt = ValidateQuery( \
        query, member_name ? &member_name->provenance : nullptr); \
    if (!receipt.ok) { \
        SCLOGWX( \
            SC_TAGS("derived_state.query", "derived_state.invariant"), \
            "query=%s epoch=%llu item=%llu freshness=%u available_turn_entry=%llu available_turn_order=%llu available_rewards=%llu reason=%s", \
            #method_name, \
            static_cast<unsigned long long>(query.workset_epoch.value()), \
            static_cast<unsigned long long>(query.item_id.value()), \
            static_cast<unsigned>(query.freshness), \
            static_cast<unsigned long long>(turn_entry_generation_), \
            static_cast<unsigned long long>(turn_order_generation_), \
            static_cast<unsigned long long>(rewards_generation_), \
            receipt.message.c_str()); \
        return {.receipt = receipt}; \
    } \
    return {.receipt = receipt, .snapshot = *member_name}; \
}

SAVOR_DERIVED_QUERY(
    QueryBattleTurnEntry,
    turn_entry_,
    BattleTurnEntrySnapshotV1)
SAVOR_DERIVED_QUERY(
    QueryBattleTurnOrder,
    turn_order_,
    BattleTurnOrderSnapshotV1)
SAVOR_DERIVED_QUERY(
    QueryBattleRewards,
    rewards_,
    BattleRewardsSnapshotV1)

#undef SAVOR_DERIVED_QUERY

DerivedStateSnapshotProvenanceV1 DerivedStateService::MakeProvenance(
    const DerivedStateRefreshGroupDescriptor& group,
    std::uint64_t generation,
    const RoutedStopEvent& event) const
{
    return {
        .workset_epoch = epoch_,
        .item_id = item_id_,
        .block = battle_core_identity_,
        .group_id = group.group_id,
        .group_revision = group.revision,
        .generation = generation,
        .routed_stop = event.identity,
        .trigger_pc = event.evidence.hit_pc,
    };
}

void DerivedStateService::RefreshFromRetainedCurrentPoint(
    const RoutedStopEvent& event)
{
    const auto current_turn = memory_.ReadScalar(
        addr::AddrRegistry::base(addr::battle::CurrentTurn),
        GuestScalarWidth::U8,
        epoch_);
    if (!current_turn.ok)
    {
        throw std::runtime_error(
            "Could not read the current Battle turn at retained point: " +
            current_turn.message);
    }

    const auto pc = event.evidence.hit_pc;
    if (pc == 0x800715ECu)
    {
        const auto bytes = memory_.ReadBytes(
            addr::AddrRegistry::base(addr::battle::TurnOrderTable),
            12,
            epoch_);
        if (!bytes.result.ok || bytes.bytes.size() != 12)
        {
            throw std::runtime_error(
                "Could not read Battle turn order at retained point: " +
                bytes.result.message);
        }
        const auto generation = turn_order_generation_ + 1;
        BattleTurnOrderSnapshotV1 replacement{
            .provenance = MakeProvenance(
                RequireGroup(registry_, kBattleTurnOrderGroupId),
                generation,
                event),
            .current_turn = static_cast<std::uint32_t>(current_turn.value),
            .active_slots = ParseTurnOrder(bytes.bytes),
        };
        turn_order_ = std::move(replacement);
        turn_order_generation_ = generation;
        return;
    }

    if (pc != 0x80071740u && pc != 0x800702A0u &&
        pc != 0x800706D8u)
    {
        throw std::runtime_error(
            "Derived-state retained point names an unknown trigger PC");
    }
    const auto pointer = memory_.ReadScalar(
        addr::AddrRegistry::base(addr::battle::MainInstancePtr),
        GuestScalarWidth::U32,
        epoch_);
    if (!pointer.ok)
    {
        throw std::runtime_error(
            "Could not read BattleState pointer at retained point: " +
            pointer.message);
    }
    if (pointer.value == 0)
        throw std::runtime_error("BattleState pointer is null at retained point");
    const auto main_pointer = static_cast<std::uint32_t>(pointer.value);

    if (pc == 0x80071740u)
    {
        constexpr auto offset = offsetof(soa::BattleState, useable_items);
        constexpr auto size = sizeof(soa::BattleState::useable_items);
        if (main_pointer >
            std::numeric_limits<std::uint32_t>::max() - offset)
        {
            throw std::runtime_error(
                "Battle inventory address overflows at retained point");
        }
        const auto bytes = memory_.ReadBytes(
            main_pointer + static_cast<std::uint32_t>(offset), size, epoch_);
        if (!bytes.result.ok || bytes.bytes.size() != size)
        {
            throw std::runtime_error(
                "Could not read Battle inventory at retained point: " +
                bytes.result.message);
        }
        const auto generation = turn_entry_generation_ + 1;
        BattleTurnEntrySnapshotV1 replacement{
            .provenance = MakeProvenance(
                RequireGroup(registry_, kBattleTurnEntryGroupId),
                generation,
                event),
            .current_turn = static_cast<std::uint32_t>(current_turn.value),
            .inventory = ParseInventory(bytes.bytes),
        };
        turn_entry_ = std::move(replacement);
        turn_entry_generation_ = generation;
        return;
    }

    constexpr auto offset = offsetof(soa::BattleState, item_drops);
    constexpr auto size = sizeof(soa::BattleState::item_drops);
    if (main_pointer >
        std::numeric_limits<std::uint32_t>::max() - offset)
    {
        throw std::runtime_error(
            "Battle rewards address overflows at retained point");
    }
    const auto bytes = memory_.ReadBytes(
        main_pointer + static_cast<std::uint32_t>(offset), size, epoch_);
    if (!bytes.result.ok || bytes.bytes.size() != size)
    {
        throw std::runtime_error(
            "Could not read Battle rewards at retained point: " +
            bytes.result.message);
    }
    const auto generation = rewards_generation_ + 1;
    BattleRewardsSnapshotV1 replacement{
        .provenance = MakeProvenance(
            RequireGroup(registry_, kBattleRewardsGroupId),
            generation,
            event),
        .current_turn = static_cast<std::uint32_t>(current_turn.value),
        .drops = ParseRewards(bytes.bytes),
    };
    rewards_ = std::move(replacement);
    rewards_generation_ = generation;
}

void DerivedStateService::CommitNativeEvidence(const RoutedStopEvent& event)
{
    DerivedEventEvidenceRecord record;
    if (!cpu_->queue.TryPop(record))
    {
        SCLOGWX(
            SC_TAGS("derived_state.capture", "derived_state.invariant"),
            "epoch=%llu item=%llu trigger_pc=0x%08X queue=empty overflowed=%d",
            static_cast<unsigned long long>(epoch_.value()),
            static_cast<unsigned long long>(item_id_.value()),
            event.evidence.hit_pc,
            cpu_->overflowed.load(std::memory_order_acquire) ? 1 : 0);
        if (cpu_->overflowed.exchange(false, std::memory_order_acq_rel))
        {
            throw std::runtime_error(
                "Battle derived-state hit-time evidence queue overflowed");
        }
        throw std::runtime_error(
            "Battle derived-state native hit has no CPU-thread evidence");
    }
    if (record.routed_stop != event.identity || record.epoch != epoch_ ||
        record.item_id != item_id_ ||
        record.trigger_pc != event.evidence.hit_pc)
    {
        throw std::runtime_error(
            "Battle derived-state CPU evidence belongs to a stale routed event");
    }
    if (record.group_count == 0 ||
        record.group_count > record.groups.size())
    {
        throw std::runtime_error(
            "Battle derived-state CPU evidence has an invalid group count");
    }

    SCLOGDX(
        SC_TAGS("derived_state.capture", "derived_state.transition"),
        "epoch=%llu item=%llu trigger_pc=0x%08X captured_groups=%zu queue=committing",
        static_cast<unsigned long long>(epoch_.value()),
        static_cast<unsigned long long>(item_id_.value()),
        event.evidence.hit_pc,
        record.group_count);

    std::optional<BattleTurnEntrySnapshotV1> turn_entry_replacement;
    std::optional<BattleTurnOrderSnapshotV1> turn_order_replacement;
    std::optional<BattleRewardsSnapshotV1> rewards_replacement;
    std::uint64_t turn_entry_generation = turn_entry_generation_;
    std::uint64_t turn_order_generation = turn_order_generation_;
    std::uint64_t rewards_generation = rewards_generation_;
    for (std::size_t index = 0; index < record.group_count; ++index)
    {
        const auto& evidence = record.groups[index];
        if (!evidence.block || !evidence.group ||
            registry_.FindBlock(evidence.block->identity.canonical_id) !=
                evidence.block ||
            !std::ranges::any_of(
                evidence.block->groups,
                [&](const DerivedStateRefreshGroupDescriptor& group) {
                    return &group == evidence.group;
                }) ||
            !std::ranges::any_of(
                evidence.group->triggers,
                [&](const program::SemanticPointDescriptor& trigger) {
                    return trigger.pc == record.trigger_pc;
                }))
        {
            throw std::runtime_error(
                "Battle derived-state CPU evidence names an invalid static refresh group");
        }
        if (evidence.failure != BattleEvidenceFailure::None)
        {
            SCLOGWX(
                SC_TAGS("derived_state.capture", "derived_state.invariant"),
                "epoch=%llu item=%llu trigger_pc=0x%08X group=%s failure=%s address=0x%08X size=%zu backend=%s",
                static_cast<unsigned long long>(epoch_.value()),
                static_cast<unsigned long long>(item_id_.value()),
                record.trigger_pc,
                evidence.group ? evidence.group->group_id.c_str() : "<missing>",
                FailureName(evidence.failure),
                evidence.failure_address,
                evidence.failure_size,
                HitTimeErrorName(evidence.backend_error));
            throw std::runtime_error(std::format(
                "Derived-state CPU acquisition failed for block '{}' group '{}' "
                "at trigger PC 0x{:08X}: {}; address 0x{:08X}, size {}, read result {}",
                evidence.block->identity.canonical_id,
                evidence.group->group_id,
                record.trigger_pc,
                FailureName(evidence.failure),
                evidence.failure_address,
                evidence.failure_size,
                HitTimeErrorName(evidence.backend_error)));
        }

        if (evidence.kind == BattleEvidenceKind::TurnEntry)
        {
            if (turn_entry_replacement)
                throw std::runtime_error("Battle turn-entry refreshed twice in one routed event");
            turn_entry_generation = turn_entry_generation_ + 1;
            turn_entry_replacement = BattleTurnEntrySnapshotV1{
                .provenance = MakeProvenance(
                    *evidence.group, turn_entry_generation, event),
                .current_turn = evidence.current_turn,
                .inventory = ParseInventory(
                    std::span(evidence.payload).first(evidence.payload_size)),
            };
            continue;
        }
        if (evidence.kind == BattleEvidenceKind::TurnOrder)
        {
            if (turn_order_replacement)
                throw std::runtime_error("Battle turn-order refreshed twice in one routed event");
            turn_order_generation = turn_order_generation_ + 1;
            turn_order_replacement = BattleTurnOrderSnapshotV1{
                .provenance = MakeProvenance(
                    *evidence.group, turn_order_generation, event),
                .current_turn = evidence.current_turn,
                .active_slots = ParseTurnOrder(
                    std::span(evidence.payload).first(evidence.payload_size)),
            };
            continue;
        }
        if (evidence.kind == BattleEvidenceKind::Rewards)
        {
            if (rewards_replacement)
                throw std::runtime_error("Battle rewards refreshed twice in one routed event");
            rewards_generation = rewards_generation_ + 1;
            rewards_replacement = BattleRewardsSnapshotV1{
                .provenance = MakeProvenance(
                    *evidence.group, rewards_generation, event),
                .current_turn = evidence.current_turn,
                .drops = ParseRewards(
                    std::span(evidence.payload).first(evidence.payload_size)),
            };
            continue;
        }
        throw std::runtime_error(
            "Battle derived-state CPU evidence has an unknown group kind");
    }

    // Publish only after every group captured at this routed event validates
    // and derives successfully.
    if (turn_entry_replacement)
    {
        turn_entry_ = std::move(turn_entry_replacement);
        turn_entry_generation_ = turn_entry_generation;
    }
    if (turn_order_replacement)
    {
        turn_order_ = std::move(turn_order_replacement);
        turn_order_generation_ = turn_order_generation;
    }
    if (rewards_replacement)
    {
        rewards_ = std::move(rewards_replacement);
        rewards_generation_ = rewards_generation;
    }
}

void DerivedStateService::OnStopPoint(const StopDelivery& delivery)
{
    if (!OnOwnerThread() || !active_ || !battle_core_active_ ||
        delivery.event.identity.workset_epoch != epoch_)
    {
        throw std::runtime_error("Derived-state routed delivery is stale");
    }
    if (accepting_current_point_)
    {
        RefreshFromRetainedCurrentPoint(delivery.event);
        return;
    }
    CommitNativeEvidence(delivery.event);
}

StopCpuObservationResult DerivedStateService::ObserveRoutedHit(
    std::uint32_t descriptor_id,
    const RoutedStopEvent& event) noexcept
{
    if (descriptor_id != CanonicalStopCpuObserverId(
            CanonicalStopCpuObserver::DerivedState))
    {
        return StopCpuObservationResult::Failed;
    }
    return cpu_->Observe(event);
}

} // namespace savor::runtime::derived
