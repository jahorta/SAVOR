#include "gtest/gtest.h"

#include "../SavorProbe/NativeStopHooks.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <latch>
#include <string>
#include <thread>
#include <utility>

namespace {

using savor::probe::BindNativeStopSink;
using savor::probe::BoundNativeStopSink;
using savor::probe::DispatchBoundNativeMemoryStop;
using savor::probe::DispatchBoundNativePcStop;
using savor::probe::INativeStopSink;
using savor::probe::InstallNativeStopHooks;
using savor::probe::NativeMemoryStop;
using savor::probe::NativePcStop;
using savor::probe::NativeStopDecision;
using savor::probe::NativeStopOrigin;
using savor::probe::NativeStopHooksInstalled;
using savor::probe::NativeStopRequiresBreak;
using savor::probe::UnbindNativeStopSink;
using savor::probe::UninstallNativeStopHooks;

class RecordingNativeStopSink final : public INativeStopSink
{
public:
    NativeStopDecision OnPcStop(const NativePcStop& stop) noexcept override
    {
        ++pc_calls;
        last_pc = stop;
        if (unbind_during_pc)
            reentrant_unbind_result = UnbindNativeStopSink(*this);
        if (bind_during_pc)
            reentrant_bind_result = BindNativeStopSink(*this);
        return pc_decision;
    }

    NativeStopDecision OnMemoryStop(const NativeMemoryStop& stop) noexcept override
    {
        ++memory_calls;
        last_memory = stop;
        return memory_decision;
    }

    NativeStopDecision pc_decision;
    NativeStopDecision memory_decision;
    NativePcStop last_pc;
    NativeMemoryStop last_memory;
    std::uint32_t pc_calls = 0;
    std::uint32_t memory_calls = 0;
    bool unbind_during_pc = false;
    bool reentrant_unbind_result = true;
    bool bind_during_pc = false;
    bool reentrant_bind_result = true;
};

class BlockingNativeStopSink final : public INativeStopSink
{
public:
    NativeStopDecision OnPcStop(const NativePcStop&) noexcept override
    {
        entered.count_down();
        release.wait();
        return {};
    }

    NativeStopDecision OnMemoryStop(const NativeMemoryStop&) noexcept override
    {
        return {};
    }

    std::latch entered{ 1 };
    std::latch release{ 1 };
};

TEST(NativeStopHookAdapter, InstallationLifecycleIsIdempotent)
{
    ASSERT_EQ(BoundNativeStopSink(), nullptr);
    UninstallNativeStopHooks();
    ASSERT_FALSE(NativeStopHooksInstalled());

    std::string error;
    ASSERT_TRUE(InstallNativeStopHooks(&error)) << error;
    EXPECT_TRUE(NativeStopHooksInstalled());
    EXPECT_TRUE(InstallNativeStopHooks(&error)) << error;
    EXPECT_TRUE(NativeStopHooksInstalled());

    UninstallNativeStopHooks();
    EXPECT_FALSE(NativeStopHooksInstalled());
    UninstallNativeStopHooks();
    EXPECT_FALSE(NativeStopHooksInstalled());
}

TEST(NativeStopHookAdapter, BindsOneExactSinkAndRejectsCompetingOwners)
{
    ASSERT_EQ(BoundNativeStopSink(), nullptr);

    RecordingNativeStopSink first;
    RecordingNativeStopSink second;
    std::string error;

    EXPECT_TRUE(BindNativeStopSink(first, &error));
    EXPECT_EQ(BoundNativeStopSink(), &first);
    EXPECT_TRUE(BindNativeStopSink(first, &error));

    EXPECT_FALSE(BindNativeStopSink(second, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(UnbindNativeStopSink(second));
    EXPECT_EQ(BoundNativeStopSink(), &first);

    EXPECT_TRUE(UnbindNativeStopSink(first));
    EXPECT_EQ(BoundNativeStopSink(), nullptr);
    EXPECT_FALSE(UnbindNativeStopSink(first));
}

TEST(NativeStopHookAdapter, PublishesTypedPcAndMemoryEvidence)
{
    ASSERT_EQ(BoundNativeStopSink(), nullptr);

    RecordingNativeStopSink sink;
    sink.pc_decision.request_break = true;
    sink.memory_decision.authoritative_overflow = true;
    ASSERT_TRUE(BindNativeStopSink(sink));

    auto* const power_pc =
        reinterpret_cast<PowerPC::PowerPCManager*>(static_cast<std::uintptr_t>(0x1234));
    const NativePcStop pc_stop{
        NativeStopOrigin::Jit,
        0x8025ECD8u,
        power_pc,
    };
    const auto pc_decision = DispatchBoundNativePcStop(pc_stop);
    EXPECT_EQ(sink.pc_calls, 1u);
    EXPECT_EQ(sink.last_pc.origin, NativeStopOrigin::Jit);
    EXPECT_EQ(sink.last_pc.pc, 0x8025ECD8u);
    EXPECT_EQ(sink.last_pc.power_pc, power_pc);
    EXPECT_TRUE(pc_decision.request_break);
    EXPECT_TRUE(NativeStopRequiresBreak(pc_decision));

    auto* const system =
        reinterpret_cast<Core::System*>(static_cast<std::uintptr_t>(0x5678));
    const NativeMemoryStop memory_stop{
        NativeStopOrigin::Memcheck,
        0x80002000u,
        0x803469A8u,
        8u,
        0x123456789abcdef0ull,
        true,
        true,
        system,
    };
    const auto memory_decision = DispatchBoundNativeMemoryStop(memory_stop);
    EXPECT_EQ(sink.memory_calls, 1u);
    EXPECT_EQ(sink.last_memory.origin, NativeStopOrigin::Memcheck);
    EXPECT_EQ(sink.last_memory.pc, 0x80002000u);
    EXPECT_EQ(sink.last_memory.address, 0x803469A8u);
    EXPECT_EQ(sink.last_memory.size, 8u);
    EXPECT_EQ(sink.last_memory.value, 0x123456789abcdef0ull);
    EXPECT_TRUE(sink.last_memory.write);
    EXPECT_TRUE(sink.last_memory.post_write);
    EXPECT_EQ(sink.last_memory.system, system);
    EXPECT_TRUE(memory_decision.authoritative_overflow);
    EXPECT_TRUE(NativeStopRequiresBreak(memory_decision));

    EXPECT_TRUE(UnbindNativeStopSink(sink));
}

TEST(NativeStopHookAdapter, IsPassiveWithoutASinkAndRejectsReentrantUnbind)
{
    static_assert(noexcept(DispatchBoundNativePcStop(NativePcStop{})));
    static_assert(noexcept(DispatchBoundNativeMemoryStop(NativeMemoryStop{})));
    ASSERT_EQ(BoundNativeStopSink(), nullptr);
    EXPECT_FALSE(NativeStopRequiresBreak(DispatchBoundNativePcStop({})));
    EXPECT_FALSE(NativeStopRequiresBreak(DispatchBoundNativeMemoryStop({})));

    RecordingNativeStopSink sink;
    sink.unbind_during_pc = true;
    sink.bind_during_pc = true;
    ASSERT_TRUE(BindNativeStopSink(sink));
    (void)DispatchBoundNativePcStop({});
    EXPECT_FALSE(sink.reentrant_unbind_result);
    EXPECT_FALSE(sink.reentrant_bind_result);
    EXPECT_EQ(BoundNativeStopSink(), &sink);
    EXPECT_TRUE(UnbindNativeStopSink(sink));
}

TEST(NativeStopHookAdapter, UnbindWaitsForInFlightDispatch)
{
    using namespace std::chrono_literals;

    ASSERT_EQ(BoundNativeStopSink(), nullptr);
    BlockingNativeStopSink sink;
    ASSERT_TRUE(BindNativeStopSink(sink));

    auto dispatch = std::async(std::launch::async, [&sink] {
        (void)DispatchBoundNativePcStop({});
    });
    sink.entered.wait();

    std::atomic<bool> unbind_started{ false };
    auto unbind = std::async(std::launch::async, [&] {
        unbind_started.store(true, std::memory_order_release);
        return UnbindNativeStopSink(sink);
    });
    while (!unbind_started.load(std::memory_order_acquire))
        std::this_thread::yield();

    for (std::uint32_t attempt = 0;
         attempt < 100000 && BoundNativeStopSink() != nullptr;
         ++attempt) {
        std::this_thread::yield();
    }
    if (BoundNativeStopSink() != nullptr) {
        sink.release.count_down();
        dispatch.wait();
        (void)unbind.get();
        FAIL() << "unbind did not detach the sink before waiting for in-flight dispatch";
        return;
    }

    EXPECT_EQ(unbind.wait_for(0ms), std::future_status::timeout);
    sink.release.count_down();
    dispatch.get();
    EXPECT_TRUE(unbind.get());
    EXPECT_EQ(BoundNativeStopSink(), nullptr);
}

} // namespace
