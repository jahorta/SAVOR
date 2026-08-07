#include <gtest/gtest.h>

#include "common/FakePhysicalStopBackend.h"
#include "common/ScriptedDolphinBackend.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace savor::runtime;
using namespace savor::test_support;

class RecordingNativeStopSink final : public savor::probe::INativeStopSink
{
public:
    RecordingNativeStopSink()
    {
        pc_stops.reserve(4);
        memory_stops.reserve(4);
    }

    savor::probe::NativeStopDecision OnPcStop(
        const savor::probe::NativePcStop& stop) noexcept override
    {
        pc_stops.push_back(stop);
        return decision;
    }

    savor::probe::NativeStopDecision OnMemoryStop(
        const savor::probe::NativeMemoryStop& stop) noexcept override
    {
        memory_stops.push_back(stop);
        return decision;
    }

    savor::probe::NativeStopDecision decision;
    std::vector<savor::probe::NativePcStop> pc_stops;
    std::vector<savor::probe::NativeMemoryStop> memory_stops;
};

TEST(FakePhysicalStopBackend, GatesApplyAndAtomicCommitWithoutSleeps)
{
    auto control = std::make_shared<FakePhysicalStopBackendControl>();
    FakePhysicalStopBackend backend(control);

    auto operation_entered = std::make_shared<std::latch>(1);
    auto allow_operation = std::make_shared<std::latch>(1);
    auto commit_entered = std::make_shared<std::latch>(1);
    auto allow_commit = std::make_shared<std::latch>(1);
    control->SetApplyGate(FakePhysicalStopOperationGate{
        .operation_entered = operation_entered,
        .allow_operation = allow_operation,
        .commit_entered = commit_entered,
        .allow_commit = allow_commit,
    });

    const PhysicalStopPointPlan requested{
        .pcs = {{0x80001000u}, {0x80002000u}},
        .memory = {{
            .start = 0x803469A8u,
            .end = 0x803469ABu,
            .read = false,
            .write = true,
        }},
    };
    std::atomic<bool> committed{false};
    auto apply = std::async(std::launch::async, [&] {
        return backend.ApplyExactPhysicalStopPlan(
            requested,
            PhysicalPlanGeneration(7),
            [&] { committed.store(true, std::memory_order_release); });
    });

    operation_entered->wait();
    EXPECT_EQ(apply.wait_for(0ms), std::future_status::timeout);
    EXPECT_FALSE(committed.load(std::memory_order_acquire));

    allow_operation->count_down();
    commit_entered->wait();
    EXPECT_EQ(apply.wait_for(0ms), std::future_status::timeout);
    EXPECT_FALSE(committed.load(std::memory_order_acquire));

    allow_commit->count_down();
    const PhysicalStopBackendReceipt receipt = apply.get();
    ASSERT_TRUE(receipt.ok) << receipt.message;
    EXPECT_TRUE(committed.load(std::memory_order_acquire));
    EXPECT_EQ(receipt.generation, PhysicalPlanGeneration(7));
    EXPECT_EQ(receipt.actual, requested);
    EXPECT_EQ(control->ActualPlan(), requested);
    EXPECT_EQ(control->ActualGeneration(), PhysicalPlanGeneration(7));
    EXPECT_FALSE(control->HasOwnerViolation());

    const auto calls = control->Calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].operation, FakePhysicalStopOperation::Apply);
    EXPECT_EQ(calls[0].plan, requested);
    EXPECT_TRUE(calls[0].commit_invoked);
}

TEST(FakePhysicalStopBackend, FailureInjectionPreservesPlanAndSkipsCommit)
{
    auto control = std::make_shared<FakePhysicalStopBackendControl>();
    control->SetApplyOutcome(FakePhysicalStopOutcome{
        .ok = false,
        .integrity = PhysicalStopIntegrity::Unknown,
        .message = "injected physical mutation failure",
    });
    FakePhysicalStopBackend backend(control);

    bool committed = false;
    const PhysicalStopPointPlan requested{
        .pcs = {{0x80001000u}},
    };
    const PhysicalStopBackendReceipt receipt =
        backend.ApplyExactPhysicalStopPlan(
            requested,
            PhysicalPlanGeneration(2),
            [&] { committed = true; });

    EXPECT_FALSE(receipt.ok);
    EXPECT_EQ(receipt.integrity, PhysicalStopIntegrity::Unknown);
    EXPECT_EQ(receipt.message, "injected physical mutation failure");
    EXPECT_FALSE(committed);
    EXPECT_TRUE(control->ActualPlan().pcs.empty());
    EXPECT_FALSE(control->HasOwnerViolation());

    const auto calls = control->Calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_FALSE(calls[0].commit_invoked);
}

TEST(FakePhysicalStopBackend, RecordsHooksAndInjectsNativeAndLifecycleEvents)
{
    auto control = std::make_shared<FakePhysicalStopBackendControl>();
    FakePhysicalStopBackend backend(control);
    RecordingNativeStopSink sink;
    sink.decision = {
        .request_break = true,
        .authoritative_overflow = false,
    };

    const PhysicalStopBackendReceipt bound = backend.BindNativeStopSink(sink);
    ASSERT_TRUE(bound.ok) << bound.message;
    EXPECT_EQ(control->BoundSink(), &sink);

    const auto first_jit = backend.InjectJitPcStop(0x80001000u);
    const auto second_jit = backend.InjectJitPcStop(0x80002000u);
    const auto memory = backend.InjectMemoryStop(
        0x80003000u,
        0x803469A8u,
        4,
        0x12345678u,
        true,
        true);
    EXPECT_TRUE(first_jit.request_break);
    EXPECT_TRUE(second_jit.request_break);
    EXPECT_TRUE(memory.request_break);

    ASSERT_EQ(sink.pc_stops.size(), 2u);
    EXPECT_EQ(
        sink.pc_stops[0].origin,
        savor::probe::NativeStopOrigin::Jit);
    EXPECT_EQ(sink.pc_stops[0].pc, 0x80001000u);
    EXPECT_EQ(sink.pc_stops[1].origin, savor::probe::NativeStopOrigin::Jit);
    EXPECT_EQ(sink.pc_stops[1].pc, 0x80002000u);
    ASSERT_EQ(sink.memory_stops.size(), 1u);
    EXPECT_EQ(sink.memory_stops[0].pc, 0x80003000u);
    EXPECT_EQ(sink.memory_stops[0].address, 0x803469A8u);
    EXPECT_TRUE(sink.memory_stops[0].write);
    EXPECT_TRUE(sink.memory_stops[0].post_write);

    int jit_invalidations = 0;
    WorksetEpoch restored_epoch;
    control->SetJitInvalidationHandler([&] { ++jit_invalidations; });
    control->SetRestoreHandler(
        [&](WorksetEpoch epoch) { restored_epoch = epoch; });
    EXPECT_TRUE(backend.InjectJitInvalidation());
    EXPECT_TRUE(backend.InjectRestore(WorksetEpoch(11)));
    EXPECT_EQ(jit_invalidations, 1);
    EXPECT_EQ(restored_epoch, WorksetEpoch(11));
    EXPECT_EQ(
        control->InjectedRestoreEpochs(),
        std::vector<WorksetEpoch>{WorksetEpoch(11)});

    const PhysicalStopBackendReceipt unbound =
        backend.UnbindNativeStopSink(sink);
    ASSERT_TRUE(unbound.ok) << unbound.message;
    EXPECT_EQ(control->BoundSink(), nullptr);
    EXPECT_FALSE(backend.InjectJitPcStop(0x80004000u).request_break);
    EXPECT_FALSE(control->HasOwnerViolation());
}

TEST(ScriptedDolphinBackend, PrivatelyOwnsAnOptionalPhysicalStopFacet)
{
    auto session_control =
        std::make_shared<ScriptedDolphinBackendControl>();
    auto physical_control =
        std::make_shared<FakePhysicalStopBackendControl>();
    auto physical =
        std::make_unique<FakePhysicalStopBackend>(physical_control);
    auto* const expected_physical = physical.get();

    ScriptedDolphinBackend backend(
        session_control,
        std::move(physical));
    EXPECT_EQ(backend.PhysicalStopPoints(), expected_physical);
    EXPECT_FALSE(session_control->HasOwnerViolation());
    EXPECT_FALSE(physical_control->HasOwnerViolation());
}

} // namespace
