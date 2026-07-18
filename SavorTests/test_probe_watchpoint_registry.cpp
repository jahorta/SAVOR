#include <gtest/gtest.h>

#include "ProbeWatchpointRegistry.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace {

using namespace savor::probe;

WatchpointRequest request(
    WatchpointOwnerKind owner_kind,
    std::uint32_t owner_index,
    std::uint32_t address,
    std::uint32_t size,
    MemoryAccess access = MemoryAccess::Write,
    WatchpointBindingSource source = WatchpointBindingSource::Static)
{
    return WatchpointRequest{
        WatchpointOwner{ owner_kind, owner_index },
        address,
        size,
        access,
        source,
    };
}

TEST(SavorProbeWatchpoints, StaticBindingCreatesOnlyTheRequestedPhysicalRange)
{
    const std::array requests{ request(
        WatchpointOwnerKind::Profile, 4, 0x803469A8u, 4) };
    const auto plan = plan_watchpoint_bindings(requests, {});

    ASSERT_TRUE(plan.failures.empty());
    ASSERT_EQ(plan.physical_ranges.size(), 1u);
    EXPECT_EQ(plan.physical_ranges[0].start, 0x803469A8u);
    EXPECT_EQ(plan.physical_ranges[0].end, 0x803469ABu);
    EXPECT_FALSE(plan.physical_ranges[0].read);
    EXPECT_TRUE(plan.physical_ranges[0].write);
    ASSERT_EQ(plan.bindings.size(), 1u);
    EXPECT_EQ(plan.bindings[0].source, WatchpointBindingSource::Static);
}

TEST(SavorProbeWatchpoints, DynamicRebindReplacesTheLogicalAddressWithoutLeavingTheOldOne)
{
    ProbeWatchpointRegistry registry;
    registry.reserve(2);
    const WatchpointOwner owner{ WatchpointOwnerKind::Profile, 7 };
    ASSERT_TRUE(registry.upsert(request(
        owner.kind, owner.index, 0x80100000u, 4,
        MemoryAccess::Write, WatchpointBindingSource::Dynamic)));
    ASSERT_TRUE(registry.upsert(request(
        owner.kind, owner.index, 0x80200000u, 4,
        MemoryAccess::Write, WatchpointBindingSource::Dynamic)));

    ASSERT_EQ(registry.requests().size(), 1u);
    EXPECT_EQ(registry.requests()[0].address, 0x80200000u);
    EXPECT_GT(registry.requests()[0].binding_generation, 1u);
    EXPECT_TRUE(registry.release(owner));
    EXPECT_TRUE(registry.requests().empty());
}

TEST(SavorProbeWatchpoints, FailedDynamicRootCanReleaseThePreviousLogicalLease)
{
    ProbeWatchpointRegistry registry;
    const WatchpointOwner owner{ WatchpointOwnerKind::Profile, 1 };
    ASSERT_TRUE(registry.upsert(request(
        owner.kind, owner.index, 0x803469A8u, 4,
        MemoryAccess::Write, WatchpointBindingSource::Dynamic)));

    ASSERT_TRUE(registry.release(owner));
    EXPECT_TRUE(registry.requests().empty());
    EXPECT_FALSE(registry.release(owner));
}

TEST(SavorProbeWatchpoints, SharedProfileAndControlRequestsUseOnePhysicalRange)
{
    const std::array requests{
        request(WatchpointOwnerKind::Profile, 0, 0x803469A8u, 4),
        request(WatchpointOwnerKind::Control, 3, 0x803469A8u, 4,
            MemoryAccess::Write, WatchpointBindingSource::Control),
    };
    const auto plan = plan_watchpoint_bindings(requests, {});

    ASSERT_TRUE(plan.failures.empty());
    ASSERT_EQ(plan.physical_ranges.size(), 1u);
    EXPECT_EQ(plan.bindings.size(), 2u);
    EXPECT_EQ(plan.bindings[0].physical_start, plan.bindings[1].physical_start);
    EXPECT_EQ(plan.bindings[0].physical_end, plan.bindings[1].physical_end);
    const auto control = std::ranges::find_if(plan.bindings, [](const auto& binding) {
        return binding.owner.kind == WatchpointOwnerKind::Control;
    });
    ASSERT_NE(control, plan.bindings.end());
    EXPECT_EQ(control->source, WatchpointBindingSource::Control);
}

TEST(SavorProbeWatchpoints, OverlappingOwnedRangesCoalesceAndUnionAccess)
{
    const std::array requests{
        request(WatchpointOwnerKind::Profile, 0, 0x80001000u, 4, MemoryAccess::Read),
        request(WatchpointOwnerKind::Profile, 1, 0x80001002u, 8, MemoryAccess::Write),
    };
    const auto plan = plan_watchpoint_bindings(requests, {});

    ASSERT_TRUE(plan.failures.empty());
    ASSERT_EQ(plan.physical_ranges.size(), 1u);
    EXPECT_EQ(plan.physical_ranges[0].start, 0x80001000u);
    EXPECT_EQ(plan.physical_ranges[0].end, 0x80001009u);
    EXPECT_TRUE(plan.physical_ranges[0].read);
    EXPECT_TRUE(plan.physical_ranges[0].write);
}

TEST(SavorProbeWatchpoints, FullyCoveringForeignCheckIsPreservedAndReused)
{
    const std::array requests{ request(
        WatchpointOwnerKind::Profile, 0, 0x803469A8u, 4) };
    const std::array foreign{ WatchpointForeignRange{ 0x803469A0u, 0x803469BFu } };
    const auto plan = plan_watchpoint_bindings(requests, foreign);

    EXPECT_TRUE(plan.failures.empty());
    EXPECT_TRUE(plan.physical_ranges.empty());
    ASSERT_EQ(plan.bindings.size(), 1u);
    EXPECT_EQ(plan.bindings[0].source, WatchpointBindingSource::Foreign);
    EXPECT_EQ(plan.bindings[0].physical_start, foreign[0].start);
    EXPECT_EQ(plan.bindings[0].physical_end, foreign[0].end);
}

TEST(SavorProbeWatchpoints, PartialSameStartForeignOverlapIsRejected)
{
    const std::array requests{ request(
        WatchpointOwnerKind::Profile, 0, 0x803469A8u, 8) };
    const std::array foreign{ WatchpointForeignRange{ 0x803469A8u, 0x803469ABu } };
    const auto plan = plan_watchpoint_bindings(requests, foreign);

    EXPECT_TRUE(plan.physical_ranges.empty());
    EXPECT_TRUE(plan.bindings.empty());
    ASSERT_EQ(plan.failures.size(), 1u);
    EXPECT_EQ(
        plan.failures[0].reason,
        WatchpointBindingFailureReason::PartialSameStartForeignOverlap);
}

TEST(SavorProbeWatchpoints, PartialForeignOverlapAtAnotherStartUsesAnOwnedCheck)
{
    const std::array requests{ request(
        WatchpointOwnerKind::Profile, 0, 0x803469A8u, 8, MemoryAccess::Write) };
    const std::array foreign{ WatchpointForeignRange{ 0x803469A0u, 0x803469ABu } };
    const auto plan = plan_watchpoint_bindings(requests, foreign);

    ASSERT_TRUE(plan.failures.empty());
    ASSERT_EQ(plan.physical_ranges.size(), 1u);
    EXPECT_EQ(plan.physical_ranges[0].start, 0x803469A8u);
    EXPECT_EQ(plan.physical_ranges[0].end, 0x803469AFu);
    ASSERT_EQ(plan.bindings.size(), 1u);
    EXPECT_EQ(plan.bindings[0].source, WatchpointBindingSource::Static);
}

TEST(SavorProbeWatchpoints, InvalidRangeIsRejectedWithoutARegistration)
{
    const std::array requests{ request(
        WatchpointOwnerKind::Profile, 0, 0xFFFFFFFEu, 4) };
    const auto plan = plan_watchpoint_bindings(requests, {});

    EXPECT_TRUE(plan.physical_ranges.empty());
    ASSERT_EQ(plan.failures.size(), 1u);
    EXPECT_EQ(plan.failures[0].reason, WatchpointBindingFailureReason::InvalidRange);
}

} // namespace
