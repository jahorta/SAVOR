#include <gtest/gtest.h>

#include "Runner/Runtime/DerivedState/DerivedStateService.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceReducers.h"
#include "Runner/Runtime/Services/Memory/IGuestMemoryBackendPort.h"
#include "Runner/Runtime/Services/Memory/IHitTimeGuestMemoryBackendPort.h"
#include "Runner/Runtime/StopPoints/StopPointRouter.h"
#include "Runner/Runtime/Worksets/WorksetWireCodec.h"
#include "Core/Memory/Soa/SoaAddrRegistry.h"
#include "Core/Memory/Soa/SoaStructs.h"
#include "common/FakePhysicalStopBackend.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace {

using namespace savor::runtime;
using namespace savor::runtime::derived;
using namespace savor::test_support;

class DerivedMemoryBackend final
    : public IGuestMemoryBackendPort,
      public IHitTimeGuestMemoryBackendPort
{
public:
    bool IsPaused() const noexcept override { return paused; }

    GuestBytesResult Read(std::uint32_t address, std::size_t size) const override
    {
        ++actor_reads;
        std::vector<std::uint8_t> bytes;
        bytes.reserve(size);
        for (std::size_t index = 0; index < size; ++index)
        {
            const auto found = memory.find(
                address + static_cast<std::uint32_t>(index));
            if (found == memory.end())
            {
                return {
                    BackendResult::Failure(
                        BackendErrorCode::OperationFailed,
                        "unmapped derived-state test memory"),
                    {}};
            }
            bytes.push_back(found->second);
        }
        return {BackendResult::Success(), std::move(bytes)};
    }

    HitTimeGuestReadReceipt ReadHitTimeBytes(
        std::uint32_t address,
        std::span<std::uint8_t> destination) const noexcept override
    {
        ++hit_time_reads;
        last_hit_time_thread = std::this_thread::get_id();
        if (destination.empty() ||
            destination.size() - 1 >
                std::numeric_limits<std::uint32_t>::max() - address)
        {
            return {
                false,
                HitTimeGuestReadError::InvalidArgument,
                address,
                destination.size()};
        }
        for (std::size_t index = 0; index < destination.size(); ++index)
        {
            const auto found = memory.find(
                address + static_cast<std::uint32_t>(index));
            if (found == memory.end())
            {
                return {
                    false,
                    HitTimeGuestReadError::UnmappedRange,
                    address,
                    destination.size()};
            }
            destination[index] = found->second;
        }
        return {
            true,
            HitTimeGuestReadError::None,
            address,
            destination.size()};
    }

    BackendResult Write(
        std::uint32_t,
        const std::vector<std::uint8_t>&) override
    {
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "derived-state test memory is read-only");
    }

    BackendResult InvalidateExecutableRange(
        std::uint32_t,
        std::size_t) override
    {
        return BackendResult::Success();
    }

    void Fill(std::uint32_t address, std::size_t size, std::uint8_t value = 0)
    {
        for (std::size_t index = 0; index < size; ++index)
            memory[address + static_cast<std::uint32_t>(index)] = value;
    }

    void PutU8(std::uint32_t address, std::uint8_t value)
    {
        memory[address] = value;
    }

    void PutBeU16(std::uint32_t address, std::uint16_t value)
    {
        memory[address] = static_cast<std::uint8_t>(value >> 8);
        memory[address + 1] = static_cast<std::uint8_t>(value);
    }

    void PutBeI16(std::uint32_t address, std::int16_t value)
    {
        PutBeU16(address, static_cast<std::uint16_t>(value));
    }

    void PutBeU32(std::uint32_t address, std::uint32_t value)
    {
        memory[address] = static_cast<std::uint8_t>(value >> 24);
        memory[address + 1] = static_cast<std::uint8_t>(value >> 16);
        memory[address + 2] = static_cast<std::uint8_t>(value >> 8);
        memory[address + 3] = static_cast<std::uint8_t>(value);
    }

    bool paused = true;
    mutable std::size_t actor_reads = 0;
    mutable std::size_t hit_time_reads = 0;
    mutable std::thread::id last_hit_time_thread;

private:
    std::map<std::uint32_t, std::uint8_t> memory;
};

class ForegroundConsumer final : public IStopPointConsumer
{
public:
    void OnStopPoint(const StopDelivery&) override {}
};

StopSubscriptionGroupDefinition ForegroundPoint(
    ForegroundConsumer& consumer,
    std::uint32_t pc = 0x80071740u)
{
    return {
        .id = StopSubscriptionGroupId(0x7001),
        .source = {
            .id = StopSourceId(0x7001),
            .stable_name = "test.derived.foreground",
            .diagnostic_label = "derived current point",
        },
        .subscriptions = {{
            .id = StopSubscriptionId(1),
            .point = PcStopPointSpec{pc},
            .route = ForegroundStopWait{},
            .consumer = &consumer,
        }},
    };
}

DerivedStateQueryV1 SameEvent(
    WorkerWorksetItemId item,
    const RoutedStopIdentity& identity)
{
    return {
        .freshness = DerivedStateFreshness::SameRoutedEvent,
        .workset_epoch = identity.workset_epoch,
        .item_id = item,
        .routed_stop = identity,
    };
}

DerivedStateQueryV1 Latest(WorkerWorksetItemId item, WorksetEpoch epoch)
{
    return {
        .freshness = DerivedStateFreshness::LatestInItem,
        .workset_epoch = epoch,
        .item_id = item,
    };
}

program::ProgramValueGraph U16Value(std::uint16_t value)
{
    constexpr program::ProgramValueId root(1);
    return {
        .root = root,
        .values = {{
            .id = root,
            .type = program::TypeRef::Builtin(program::BuiltinType::U16),
            .payload = value,
        }},
    };
}

std::optional<std::uint32_t> U32Value(
    const program::ProgramValueGraph& graph)
{
    const auto found = std::ranges::find(
        graph.values, graph.root, &program::ProgramValue::id);
    if (found == graph.values.end())
        return std::nullopt;
    const auto* value = std::get_if<std::uint32_t>(&found->payload);
    return value ? std::optional(*value) : std::nullopt;
}

std::optional<std::uint32_t> ReduceU32(
    std::string_view reducer,
    std::span<const program::ProgramValueGraph> inputs,
    std::string* diagnostic = nullptr)
{
    const auto output = program::capabilities::InvokeSourceReducer(
        program::capabilities::BattleDerivedReducerIdentity(reducer),
        inputs,
        diagnostic);
    return output ? U32Value(*output) : std::nullopt;
}

TEST(DerivedStateService, BattleCoreRefreshesAtomicallyAndIsItemLocal)
{
    constexpr WorksetEpoch epoch(41);
    constexpr WorkerWorksetItemId item(73);
    constexpr std::uint32_t battle_state = 0x80500000u;

    DerivedMemoryBackend memory_backend;
    memory_backend.PutU8(
        addr::AddrRegistry::base(addr::battle::CurrentTurn),
        2);
    memory_backend.PutBeU32(
        addr::AddrRegistry::base(addr::battle::MainInstancePtr),
        battle_state);
    const auto inventory = battle_state + static_cast<std::uint32_t>(
        offsetof(soa::BattleState, useable_items));
    memory_backend.Fill(inventory, sizeof(soa::BattleState::useable_items));
    memory_backend.PutBeU16(inventory, 9);
    memory_backend.PutU8(inventory + 2, 3);
    memory_backend.PutBeU16(inventory + sizeof(soa::ItemSlot), 2);
    memory_backend.PutU8(inventory + sizeof(soa::ItemSlot) + 2, 4);
    memory_backend.PutBeU16(inventory + 2 * sizeof(soa::ItemSlot), 9);
    memory_backend.PutU8(inventory + 2 * sizeof(soa::ItemSlot) + 2, 5);

    const auto drops = battle_state + static_cast<std::uint32_t>(
        offsetof(soa::BattleState, item_drops));
    memory_backend.Fill(drops, sizeof(soa::BattleState::item_drops));
    memory_backend.PutBeI16(drops, 2);
    memory_backend.PutBeI16(drops + 2, 17);
    memory_backend.PutBeI16(
        drops + sizeof(soa::BattleItemDropSlot), 3);
    memory_backend.PutBeI16(
        drops + sizeof(soa::BattleItemDropSlot) + 2, 17);

    const auto turn_order = addr::AddrRegistry::base(
        addr::battle::TurnOrderTable);
    memory_backend.Fill(turn_order, 12, 0xFFu);
    memory_backend.PutU8(turn_order, 4);
    memory_backend.PutU8(turn_order + 1, 0);
    memory_backend.PutU8(turn_order + 2, 7);

    GuestMemory memory(memory_backend);
    memory.InitializeWorksetEpoch(epoch);
    DerivedStateService service(memory, memory_backend);
    StopCpuObserverDispatcher cpu_observers;
    ASSERT_TRUE(cpu_observers.Register(
        CanonicalStopCpuObserver::DerivedState,
        service));
    ASSERT_TRUE(cpu_observers.Freeze());
    auto physical_control = std::make_shared<FakePhysicalStopBackendControl>();
    FakePhysicalStopBackend physical(physical_control);
    PhysicalStopPointManager manager(physical);
    StopPointRouter router(manager, nullptr, &cpu_observers);
    ASSERT_TRUE(router.Initialize(epoch).ok);
    ASSERT_TRUE(service.BindRouter(router, epoch).ok);

    ForegroundConsumer foreground_consumer;
    auto foreground = router.RegisterGroup(
        ForegroundPoint(foreground_consumer));
    ASSERT_TRUE(foreground.receipt.ok) << foreground.receipt.error.message;
    (void)physical.InjectJitPcStop(0x80071740u);
    const auto entry_routes = router.DrainIngress();
    ASSERT_EQ(entry_routes.size(), 1u);

    const std::array battle_blocks{std::string(kBattleCoreBlockId)};
    const auto binding = ResolveWorksetDerivedStateBindingV1(battle_blocks);
    const auto activated = service.ActivateItem(binding, epoch, item);
    ASSERT_TRUE(activated.ok) << activated.message;

    const auto entry = service.QueryBattleTurnEntry(
        SameEvent(item, entry_routes.front().identity));
    ASSERT_TRUE(entry.receipt.ok) << entry.receipt.message;
    ASSERT_TRUE(entry.snapshot.has_value());
    EXPECT_EQ(entry.snapshot->current_turn, 2u);
    EXPECT_EQ(entry.snapshot->inventory,
        (std::vector<BattleItemTotalV1>{{2, 4}, {9, 8}}));
    const auto entry_value =
        program::capabilities::EncodeBattleDerivedSnapshotValue(
            *entry.snapshot);
    const auto item_nine = U16Value(9);
    const auto item_absent = U16Value(88);
    const std::array inventory_nine{entry_value, item_nine};
    const std::array inventory_absent{entry_value, item_absent};
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.inventory_count", inventory_nine), 8u);
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.inventory_count", inventory_absent), 0u);

    std::thread::id native_thread;
    std::thread native([&] {
        native_thread = std::this_thread::get_id();
        EXPECT_FALSE(physical.InjectJitPcStop(0x800715ECu).request_break);
    });
    native.join();
    EXPECT_EQ(memory_backend.last_hit_time_thread, native_thread);
    const auto order_routes = router.DrainIngress();
    ASSERT_EQ(order_routes.size(), 1u);
    const auto order = service.QueryBattleTurnOrder(
        SameEvent(item, order_routes.front().identity));
    ASSERT_TRUE(order.receipt.ok) << order.receipt.message;
    ASSERT_TRUE(order.snapshot.has_value());
    EXPECT_EQ(order.snapshot->active_slots,
        (std::vector<std::uint8_t>{4, 0, 7}));
    const auto order_value =
        program::capabilities::EncodeBattleDerivedSnapshotValue(
            *order.snapshot);
    const std::array order_input{order_value};
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.current_turn", order_input), 2u);
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.player_count", order_input), 1u);
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.enemy_count", order_input), 2u);
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.player_min_position", order_input), 1u);
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.player_max_position", order_input), 1u);
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.enemy_min_position", order_input), 0u);
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.enemy_max_position", order_input), 2u);

    const auto actor_reads_before_reward = memory_backend.actor_reads;
    memory_backend.paused = false;
    const auto end_turn = physical.InjectJitPcStop(0x800702A0u);
    EXPECT_FALSE(end_turn.request_break);
    memory_backend.PutBeI16(drops, 99);
    const auto reward_routes = router.DrainIngress();
    ASSERT_EQ(reward_routes.size(), 1u);
    const auto rewards = service.QueryBattleRewards(
        SameEvent(item, reward_routes.front().identity));
    ASSERT_TRUE(rewards.receipt.ok) << rewards.receipt.message;
    ASSERT_TRUE(rewards.snapshot.has_value());
    EXPECT_EQ(rewards.snapshot->drops,
        (std::vector<BattleItemTotalV1>{{17, 5}}));
    EXPECT_EQ(memory_backend.actor_reads, actor_reads_before_reward);
    EXPECT_GT(memory_backend.hit_time_reads, 0u);
    const auto reward_value =
        program::capabilities::EncodeBattleDerivedSnapshotValue(
            *rewards.snapshot);
    const auto item_seventeen = U16Value(17);
    const std::array reward_input{reward_value, item_seventeen};
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.drop_count", reward_input), 5u);

    BattleTurnOrderSnapshotV1 enemies_only = *order.snapshot;
    enemies_only.active_slots = {4, 7};
    const auto enemies_only_value =
        program::capabilities::EncodeBattleDerivedSnapshotValue(enemies_only);
    const std::array enemies_only_input{enemies_only_value};
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.player_min_position",
        enemies_only_input), 12u);
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.player_max_position",
        enemies_only_input), 0u);
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.enemy_min_position",
        enemies_only_input), 0u);
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.enemy_max_position",
        enemies_only_input), 1u);

    BattleTurnOrderSnapshotV1 players_only = *order.snapshot;
    players_only.active_slots = {0, 1};
    const auto players_only_value =
        program::capabilities::EncodeBattleDerivedSnapshotValue(players_only);
    const std::array players_only_input{players_only_value};
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.player_min_position",
        players_only_input), 0u);
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.player_max_position",
        players_only_input), 1u);
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.enemy_min_position",
        players_only_input), 12u);
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.enemy_max_position",
        players_only_input), 0u);

    BattleTurnOrderSnapshotV1 empty_order = *order.snapshot;
    empty_order.active_slots.clear();
    const auto empty_order_value =
        program::capabilities::EncodeBattleDerivedSnapshotValue(empty_order);
    const std::array empty_order_input{empty_order_value};
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.player_min_position",
        empty_order_input), 12u);
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.player_max_position",
        empty_order_input), 0u);
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.enemy_min_position",
        empty_order_input), 12u);
    EXPECT_EQ(ReduceU32(
        "soa.battle.derived.enemy_max_position",
        empty_order_input), 0u);

    memory_backend.PutU8(turn_order, 4);
    memory_backend.PutU8(turn_order + 1, 4);
    (void)physical.InjectJitPcStop(0x800715ECu);
    const auto malformed_routes = router.DrainIngress();
    ASSERT_EQ(malformed_routes.size(), 1u);
    EXPECT_EQ(
        malformed_routes.front().terminal,
        StopRouteTerminal::RoutingFailure);
    const auto retained = service.QueryBattleTurnOrder(Latest(item, epoch));
    ASSERT_TRUE(retained.receipt.ok) << retained.receipt.message;
    ASSERT_TRUE(retained.snapshot.has_value());
    EXPECT_EQ(retained.snapshot, order.snapshot);

    auto stale_identity = order_routes.front().identity;
    stale_identity.sequence = RoutedStopSequence(
        stale_identity.sequence.value() + 1);
    EXPECT_FALSE(service.QueryBattleTurnOrder(
        SameEvent(item, stale_identity)).receipt.ok);

    ASSERT_TRUE(service.CloseItem().ok);
    EXPECT_FALSE(service.active());
    EXPECT_FALSE(service.QueryBattleTurnEntry(
        SameEvent(item, entry_routes.front().identity)).receipt.ok);
    ASSERT_TRUE(foreground.handle.Release().ok);
    const auto cleanup = router.StopIngressDrainAndCleanup();
    EXPECT_TRUE(cleanup.ok) << cleanup.error.message;
}

std::string RunTurnInputsAcquisitionFailure(
    bool map_pointer,
    std::uint32_t pointer,
    bool map_payload)
{
    constexpr WorksetEpoch epoch(91);
    constexpr WorkerWorksetItemId item(92);
    DerivedMemoryBackend memory_backend;
    memory_backend.PutU8(
        addr::AddrRegistry::base(addr::battle::CurrentTurn), 1);
    if (map_pointer)
    {
        memory_backend.PutBeU32(
            addr::AddrRegistry::base(addr::battle::MainInstancePtr),
            pointer);
    }
    if (map_payload)
    {
        memory_backend.Fill(
            pointer + static_cast<std::uint32_t>(
                offsetof(soa::BattleState, useable_items)),
            sizeof(soa::BattleState::useable_items));
    }

    GuestMemory memory(memory_backend);
    memory.InitializeWorksetEpoch(epoch);
    DerivedStateService service(memory, memory_backend);
    StopCpuObserverDispatcher dispatcher;
    EXPECT_TRUE(dispatcher.Register(
        CanonicalStopCpuObserver::DerivedState, service));
    EXPECT_TRUE(dispatcher.Freeze());
    auto control = std::make_shared<FakePhysicalStopBackendControl>();
    FakePhysicalStopBackend physical(control);
    PhysicalStopPointManager manager(physical);
    StopPointRouter router(manager, nullptr, &dispatcher);
    EXPECT_TRUE(router.Initialize(epoch).ok);
    EXPECT_TRUE(service.BindRouter(router, epoch).ok);
    const std::array battle_blocks{std::string(kBattleCoreBlockId)};
    EXPECT_TRUE(service.ActivateItem(
        ResolveWorksetDerivedStateBindingV1(battle_blocks),
        epoch,
        item).ok);

    EXPECT_TRUE(physical.InjectJitPcStop(0x80071740u).request_break);
    const auto routed = router.DrainIngress();
    EXPECT_EQ(routed.size(), 1u);
    const std::string message = routed.empty()
        ? std::string{}
        : routed.front().error.message;
    EXPECT_TRUE(service.CloseItem().ok);
    EXPECT_TRUE(router.StopIngressDrainAndCleanup().ok);
    return message;
}

TEST(DerivedStateService, DistinguishesBattleStateAcquisitionFailures)
{
    const auto unreadable_pointer =
        RunTurnInputsAcquisitionFailure(false, 0, false);
    EXPECT_NE(
        unreadable_pointer.find("BattleState pointer read"),
        std::string::npos);
    EXPECT_NE(unreadable_pointer.find("unmapped range"), std::string::npos);

    const auto null_pointer =
        RunTurnInputsAcquisitionFailure(true, 0, false);
    EXPECT_NE(
        null_pointer.find("null BattleState pointer"),
        std::string::npos);
    EXPECT_EQ(null_pointer.find("unmapped range"), std::string::npos);

    const auto unreadable_payload =
        RunTurnInputsAcquisitionFailure(true, 0x80510000u, false);
    EXPECT_NE(
        unreadable_payload.find("BattleState payload read"),
        std::string::npos);
    EXPECT_NE(unreadable_payload.find("unmapped range"), std::string::npos);
    EXPECT_NE(unreadable_payload.find("0x8051"), std::string::npos);
}

TEST(DerivedStateService, RetainedCurrentPointRequiresPausedActorMemory)
{
    constexpr WorksetEpoch epoch(93);
    constexpr WorkerWorksetItemId item(94);
    constexpr std::uint32_t battle_state = 0x80520000u;
    DerivedMemoryBackend memory_backend;
    memory_backend.PutU8(
        addr::AddrRegistry::base(addr::battle::CurrentTurn), 1);
    memory_backend.PutBeU32(
        addr::AddrRegistry::base(addr::battle::MainInstancePtr), battle_state);
    memory_backend.Fill(
        battle_state + static_cast<std::uint32_t>(
            offsetof(soa::BattleState, useable_items)),
        sizeof(soa::BattleState::useable_items));
    GuestMemory memory(memory_backend);
    memory.InitializeWorksetEpoch(epoch);
    DerivedStateService service(memory, memory_backend);
    StopCpuObserverDispatcher dispatcher;
    ASSERT_TRUE(dispatcher.Register(
        CanonicalStopCpuObserver::DerivedState, service));
    ASSERT_TRUE(dispatcher.Freeze());
    auto control = std::make_shared<FakePhysicalStopBackendControl>();
    FakePhysicalStopBackend physical(control);
    PhysicalStopPointManager manager(physical);
    StopPointRouter router(manager, nullptr, &dispatcher);
    ASSERT_TRUE(router.Initialize(epoch).ok);
    ASSERT_TRUE(service.BindRouter(router, epoch).ok);

    ForegroundConsumer foreground_consumer;
    auto foreground = router.RegisterGroup(
        ForegroundPoint(foreground_consumer));
    ASSERT_TRUE(foreground.receipt.ok);
    EXPECT_TRUE(physical.InjectJitPcStop(0x80071740u).request_break);
    ASSERT_EQ(router.DrainIngress().size(), 1u);

    memory_backend.paused = false;
    const std::array battle_blocks{std::string(kBattleCoreBlockId)};
    const auto activated = service.ActivateItem(
        ResolveWorksetDerivedStateBindingV1(battle_blocks),
        epoch,
        item);
    EXPECT_FALSE(activated.ok);
    EXPECT_NE(activated.message.find("paused"), std::string::npos);
    EXPECT_FALSE(service.active());
    ASSERT_TRUE(foreground.handle.Release().ok);
    EXPECT_TRUE(router.StopIngressDrainAndCleanup().ok);
}

TEST(DerivedStateService, RetainedNonInitializationPointIsNotRefreshed)
{
    constexpr WorksetEpoch epoch(95);
    constexpr WorkerWorksetItemId item(96);
    DerivedMemoryBackend memory_backend;
    memory_backend.PutU8(
        addr::AddrRegistry::base(addr::battle::CurrentTurn), 2);
    const auto turn_order = addr::AddrRegistry::base(
        addr::battle::TurnOrderTable);
    memory_backend.Fill(turn_order, 12, 0xFFu);
    memory_backend.PutU8(turn_order, 4);
    GuestMemory memory(memory_backend);
    memory.InitializeWorksetEpoch(epoch);
    DerivedStateService service(memory, memory_backend);
    StopCpuObserverDispatcher dispatcher;
    ASSERT_TRUE(dispatcher.Register(
        CanonicalStopCpuObserver::DerivedState, service));
    ASSERT_TRUE(dispatcher.Freeze());
    auto control = std::make_shared<FakePhysicalStopBackendControl>();
    FakePhysicalStopBackend physical(control);
    PhysicalStopPointManager manager(physical);
    StopPointRouter router(manager, nullptr, &dispatcher);
    ASSERT_TRUE(router.Initialize(epoch).ok);
    ASSERT_TRUE(service.BindRouter(router, epoch).ok);

    ForegroundConsumer foreground_consumer;
    auto foreground = router.RegisterGroup(
        ForegroundPoint(foreground_consumer, 0x800715ECu));
    ASSERT_TRUE(foreground.receipt.ok);
    EXPECT_TRUE(physical.InjectJitPcStop(0x800715ECu).request_break);
    ASSERT_EQ(router.DrainIngress().size(), 1u);

    const std::array battle_blocks{std::string(kBattleCoreBlockId)};
    const auto activated = service.ActivateItem(
        ResolveWorksetDerivedStateBindingV1(battle_blocks),
        epoch,
        item);
    ASSERT_TRUE(activated.ok) << activated.message;
    EXPECT_FALSE(service.QueryBattleTurnOrder(
        Latest(item, epoch)).receipt.ok);
    ASSERT_TRUE(service.CloseItem().ok);
    ASSERT_TRUE(foreground.handle.Release().ok);
    EXPECT_TRUE(router.StopIngressDrainAndCleanup().ok);
}

TEST(DerivedStateService, FixedCpuEvidenceQueueFailsClosedOnOverflow)
{
    constexpr WorksetEpoch epoch(97);
    constexpr WorkerWorksetItemId item(98);
    DerivedMemoryBackend memory_backend;
    memory_backend.PutU8(
        addr::AddrRegistry::base(addr::battle::CurrentTurn), 2);
    const auto turn_order = addr::AddrRegistry::base(
        addr::battle::TurnOrderTable);
    memory_backend.Fill(turn_order, 12, 0xFFu);
    GuestMemory memory(memory_backend);
    memory.InitializeWorksetEpoch(epoch);
    DerivedStateService service(memory, memory_backend);
    StopCpuObserverDispatcher dispatcher;
    ASSERT_TRUE(dispatcher.Register(
        CanonicalStopCpuObserver::DerivedState, service));
    ASSERT_TRUE(dispatcher.Freeze());
    auto control = std::make_shared<FakePhysicalStopBackendControl>();
    FakePhysicalStopBackend physical(control);
    PhysicalStopPointManager manager(physical);
    StopPointRouter router(manager, nullptr, &dispatcher);
    ASSERT_TRUE(router.Initialize(epoch).ok);
    ASSERT_TRUE(service.BindRouter(router, epoch).ok);
    const std::array battle_blocks{std::string(kBattleCoreBlockId)};
    ASSERT_TRUE(service.ActivateItem(
        ResolveWorksetDerivedStateBindingV1(battle_blocks),
        epoch,
        item).ok);

    RoutedStopEvent event{
        .identity = {.workset_epoch = epoch},
        .evidence = {
            .path = NativeStopPath::Jit,
            .point = PcStopPointSpec{0x800715ECu},
            .hit_pc = 0x800715ECu,
        },
    };
    for (std::size_t index = 0;
         index < kStopPointNativeIngressCapacity;
         ++index)
    {
        event.identity.sequence = RoutedStopSequence(index + 1);
        EXPECT_EQ(
            service.ObserveRoutedHit(
                CanonicalStopCpuObserverId(
                    CanonicalStopCpuObserver::DerivedState),
                event),
            StopCpuObservationResult::Observed);
    }
    event.identity.sequence = RoutedStopSequence(
        kStopPointNativeIngressCapacity + 1);
    EXPECT_EQ(
        service.ObserveRoutedHit(
            CanonicalStopCpuObserverId(
                CanonicalStopCpuObserver::DerivedState),
            event),
        StopCpuObservationResult::Failed);

    ASSERT_TRUE(service.CloseItem().ok);
    EXPECT_FALSE(service.active());
    EXPECT_TRUE(router.StopIngressDrainAndCleanup().ok);
}

TEST(DerivedStateRegistry, EmptyBindingIsCanonicalAndBattleBlockIsExact)
{
    const WorksetDerivedStateBindingV1 empty;
    EXPECT_TRUE(ValidateWorksetDerivedStateBindingV1(empty));
    EXPECT_EQ(empty.content_sha256,
        "4bf4d7c8b3d9d28238f46029b93c4649ec76fdfdd387a49565b630560ab8240f");

    const std::array battle_blocks{std::string(kBattleCoreBlockId)};
    const auto battle = ResolveWorksetDerivedStateBindingV1(battle_blocks);
    ASSERT_TRUE(ValidateWorksetDerivedStateBindingV1(battle));
    ASSERT_EQ(battle.blocks.size(), 1u);
    const auto* descriptor = ProductionDerivedStateRegistry().FindBlock(
        kBattleCoreBlockId);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(battle.blocks.front().identity, descriptor->identity);
    EXPECT_EQ(
        descriptor->refresh_provider.canonical_id,
        kBattleCoreRefreshProviderId);
    EXPECT_TRUE(static_cast<bool>(descriptor->refresh_provider));
    EXPECT_EQ(descriptor->groups.size(), 3u);
    EXPECT_TRUE(descriptor->groups[0].accepts_current_point_initialization);
    EXPECT_FALSE(descriptor->groups[1].accepts_current_point_initialization);
    EXPECT_FALSE(descriptor->groups[2].accepts_current_point_initialization);

    std::vector<std::uint8_t> encoded;
    ASSERT_TRUE(EncodeWorksetDerivedStateBindingV1(battle, encoded));
    WorksetDerivedStateBindingV1 decoded;
    ASSERT_TRUE(DecodeWorksetDerivedStateBindingV1(encoded, decoded));
    EXPECT_EQ(decoded, battle);
}

TEST(DerivedStateRegistry, RejectsDuplicateStaticDescriptorMembers)
{
    const auto* canonical = ProductionDerivedStateRegistry().FindBlock(
        kBattleCoreBlockId);
    ASSERT_NE(canonical, nullptr);

    auto duplicate_reducer = *canonical;
    duplicate_reducer.identity.canonical_id = "test.derived.duplicate-reducer";
    duplicate_reducer.reducers.push_back(
        duplicate_reducer.reducers.front());
    duplicate_reducer.identity.descriptor_sha256 =
        ComputeDerivedStateBlockDescriptorHashV1(duplicate_reducer);
    DerivedStateRegistry registry;
    std::string error;
    EXPECT_FALSE(registry.Register(std::move(duplicate_reducer), &error));
    EXPECT_EQ(error, "Derived-state reducer identity is empty or duplicated");

    auto duplicate_point = *canonical;
    duplicate_point.identity.canonical_id = "test.derived.duplicate-point";
    duplicate_point.groups.back().triggers.front() =
        duplicate_point.groups.front().triggers.front();
    duplicate_point.identity.descriptor_sha256 =
        ComputeDerivedStateBlockDescriptorHashV1(duplicate_point);
    error.clear();
    EXPECT_FALSE(registry.Register(std::move(duplicate_point), &error));
    EXPECT_EQ(error, "Derived-state semantic trigger is empty or duplicated");
}

} // namespace
