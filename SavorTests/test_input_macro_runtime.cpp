#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../SavorCore/Runner/InputMacro/InputMacroRuntime.h"

namespace {

using namespace savor;
using namespace savor::inputmacro;

constexpr std::uint32_t kAddress = 0x80001000u;
constexpr BPKey kGateA = bp::battle::BattleMacroMainMenuMoveHigher;
constexpr BPKey kGateB = bp::battle::BattleMacroMainMenuMoveLower;
constexpr BPKey kGateAlt = bp::battle::BattleMacroMainMenuMoveHigherAlt;

struct FakeInputMacroHost final : IInputMacroHost {
    bool acquire_ok{true};
    std::deque<BreakpointWaitResult> breakpoint_results;
    std::deque<InputMacroHostStatus> neutral_results;
    std::deque<std::pair<bool, std::uint32_t>> read_results;
    std::deque<MemoryChangeResult> memory_results;

    std::vector<std::string> calls;
    std::vector<BPKey> acquired_keys;
    std::vector<BreakpointWaitAction> breakpoint_requests;
    std::vector<std::uint32_t> neutral_frame_requests;
    std::vector<std::uint32_t> read_addresses;
    struct MemoryRequest {
        std::uint32_t address;
        std::uint32_t baseline;
        std::uint32_t timeout_ms;
    };
    std::vector<MemoryRequest> memory_requests;

    bool acquire_exclusive_session(std::span<const BPKey> provider_keys) override
    {
        calls.emplace_back("acquire");
        acquired_keys.assign(provider_keys.begin(), provider_keys.end());
        return acquire_ok;
    }

    void release_exclusive_session() override { calls.emplace_back("release"); }

    BreakpointWaitResult run_to_breakpoints(const BreakpointWaitAction& action) override
    {
        calls.emplace_back("breakpoint");
        breakpoint_requests.push_back(action);
        if (breakpoint_results.empty()) return {};
        const auto result = breakpoint_results.front();
        breakpoint_results.pop_front();
        return result;
    }

    InputMacroHostStatus step_neutral_frames(std::uint32_t frame_count) override
    {
        calls.emplace_back("frames");
        neutral_frame_requests.push_back(frame_count);
        if (neutral_results.empty()) return InputMacroHostStatus::Succeeded;
        const auto result = neutral_results.front();
        neutral_results.pop_front();
        return result;
    }

    bool read_u32(std::uint32_t address, std::uint32_t& value_out) override
    {
        calls.emplace_back("read");
        read_addresses.push_back(address);
        if (read_results.empty()) return false;
        const auto [ok, value] = read_results.front();
        read_results.pop_front();
        value_out = value;
        return ok;
    }

    MemoryChangeResult wait_for_u32_change(
        std::uint32_t address,
        std::uint32_t baseline,
        std::uint32_t timeout_ms) override
    {
        calls.emplace_back("memory");
        memory_requests.push_back({address, baseline, timeout_ms});
        if (memory_results.empty()) return {};
        const auto result = memory_results.front();
        memory_results.pop_front();
        return result;
    }

    void set_neutral_input() override { calls.emplace_back("neutral"); }
    void clear_macro_memory_watchpoints() override { calls.emplace_back("clear"); }
    void restore_breakpoint_state() override { calls.emplace_back("restore"); }

    std::size_t Count(std::string_view call) const
    {
        return static_cast<std::size_t>(std::count(calls.begin(), calls.end(), call));
    }
};

InputMacroStep GateStep(
    std::string label,
    std::vector<BPKey> keys,
    bool hold = false,
    std::uint16_t buttons = 0)
{
    GCInputFrame input{};
    input.buttons = buttons;
    return InputMacroStep{
        .label = std::move(label),
        .action = BreakpointWaitAction{
            .expected_keys = std::move(keys),
            .input = input,
            .hold_input_through_hit_opcode = hold,
        },
    };
}

InputMacroPlan OneGatePlan(BPKey key = kGateA)
{
    return InputMacroPlan{{GateStep("gate", {key})}};
}

void ExpectCleanupSuffix(const FakeInputMacroHost& host)
{
    ASSERT_GE(host.calls.size(), 4u);
    const auto begin = host.calls.end() - 4;
    EXPECT_EQ(std::vector<std::string>(begin, host.calls.end()),
        (std::vector<std::string>{"neutral", "clear", "release", "restore"}));
}

TEST(InputMacroRuntime, ExecutesSequentialAndAlternativeBreakpointGates)
{
    FakeInputMacroHost host;
    host.breakpoint_results = {
        {.status = InputMacroHostStatus::Succeeded, .hit = true, .hit_key = kGateA, .hit_pc = 0x8007cfd8u, .elapsed_ms = 4},
        {.status = InputMacroHostStatus::Succeeded, .hit = true, .hit_key = kGateAlt, .hit_pc = 0x8007d06cu, .elapsed_ms = 7},
    };
    InputMacroRuntime runtime(host);
    const std::vector<BPKey> declared{kGateA, kGateB, kGateAlt};
    InputMacroPlan plan{{
        GateStep("first", {kGateA}),
        GateStep("alternative", {kGateB, kGateAlt}, true, GC_A),
    }};

    EXPECT_EQ(runtime.Start(std::move(plan), declared).state, InputMacroRuntimeState::Running);
    const auto first = runtime.ExecuteNext();
    EXPECT_TRUE(first.step_completed);
    EXPECT_FALSE(first.terminal());
    EXPECT_EQ(first.hit_key, kGateA);
    EXPECT_EQ(runtime.current_index(), 1u);

    const auto second = runtime.ExecuteNext();
    EXPECT_TRUE(second.step_completed);
    EXPECT_EQ(second.terminal_status, InputMacroTerminalStatus::Completed);
    EXPECT_EQ(second.expected_keys, (std::vector<BPKey>{kGateB, kGateAlt}));
    EXPECT_EQ(second.hit_key, kGateAlt);
    ASSERT_EQ(host.breakpoint_requests.size(), 2u);
    EXPECT_TRUE(host.breakpoint_requests[1].hold_input_through_hit_opcode);
    EXPECT_EQ(host.breakpoint_requests[1].input.buttons, GC_A);
    ExpectCleanupSuffix(host);
}

TEST(InputMacroRuntime, PropagatesBreakpointInputPollReceipt)
{
    FakeInputMacroHost host;
    GCInputFrame input{};
    input.buttons = GC_A;
    host.breakpoint_results.push_back(BreakpointWaitResult{
        .status = InputMacroHostStatus::Succeeded,
        .hit = true,
        .hit_key = kGateA,
        .hit_pc = 0x8007cfd8u,
        .stop_sequence = 7,
        .input_epoch = 42,
        .requested_input = input,
        .input_poll_count = 11,
        .input_acknowledged = true,
        .elapsed_ms = 5,
    });

    InputMacroRuntime runtime(host);
    const std::vector<BPKey> declared{kGateA};
    ASSERT_EQ(runtime.Start(InputMacroPlan{{GateStep("gate", {kGateA}, false, GC_A)}}, declared).failure,
        InputMacroFailure::None);

    const auto result = runtime.ExecuteNext();
    EXPECT_EQ(result.stop_sequence, 7u);
    EXPECT_EQ(result.input_epoch, 42u);
    EXPECT_EQ(result.requested_input, input);
    EXPECT_EQ(result.input_poll_count, 11u);
    EXPECT_TRUE(result.input_acknowledged);
    EXPECT_EQ(result.terminal_status, InputMacroTerminalStatus::Completed);
}

TEST(InputMacroRuntime, RejectsUnexpectedBreakpointFromAlternativeGate)
{
    FakeInputMacroHost host;
    host.breakpoint_results.push_back({
        .status = InputMacroHostStatus::Succeeded,
        .hit = true,
        .hit_key = kGateAlt,
        .hit_pc = 0x8007d06cu,
    });
    InputMacroRuntime runtime(host);
    const std::vector<BPKey> declared{kGateA, kGateAlt};
    ASSERT_EQ(runtime.Start(OneGatePlan(), declared).state, InputMacroRuntimeState::Running);

    const auto result = runtime.ExecuteNext();
    EXPECT_EQ(result.failure, InputMacroFailure::UnexpectedBreakpoint);
    EXPECT_EQ(result.hit_key, kGateAlt);
    EXPECT_EQ(result.hit_pc, 0x8007d06cu);
    EXPECT_EQ(result.terminal_status, InputMacroTerminalStatus::Failed);
    ExpectCleanupSuffix(host);
}

TEST(InputMacroRuntime, ExecutesNeutralFramesAndReportsHostFailure)
{
    FakeInputMacroHost host;
    host.neutral_results = {
        InputMacroHostStatus::Succeeded,
        InputMacroHostStatus::Failed,
    };
    InputMacroRuntime runtime(host);
    const std::vector<BPKey> declared;
    InputMacroPlan plan{{
        {.label = "settle", .action = NeutralFramesAction{3}},
        {.label = "fail", .action = NeutralFramesAction{2}},
    }};
    ASSERT_EQ(runtime.Start(std::move(plan), declared).state, InputMacroRuntimeState::Running);

    EXPECT_TRUE(runtime.ExecuteNext().step_completed);
    const auto failed = runtime.ExecuteNext();
    EXPECT_EQ(failed.failure, InputMacroFailure::HostFailure);
    EXPECT_EQ(failed.label, "fail");
    EXPECT_EQ(host.neutral_frame_requests, (std::vector<std::uint32_t>{3, 2}));
    ExpectCleanupSuffix(host);
}

TEST(InputMacroRuntime, CapturesBaselineAndWaitsForU32Change)
{
    FakeInputMacroHost host;
    host.read_results.push_back({true, 0x12345678u});
    host.memory_results.push_back({
        .status = InputMacroHostStatus::Succeeded,
        .latest_value = 0x87654321u,
        .poll_count = 6,
        .elapsed_ms = 19,
    });
    InputMacroRuntime runtime(host);
    const std::vector<BPKey> declared;
    InputMacroPlan plan{{
        {.label = "before", .action = CaptureU32BaselineAction{"target", kAddress}},
        {.label = "changed", .action = WaitU32ChangeAction{"target", kAddress, 250, 2}},
    }};
    ASSERT_EQ(runtime.Start(std::move(plan), declared).state, InputMacroRuntimeState::Running);

    const auto capture = runtime.ExecuteNext();
    EXPECT_EQ(capture.action_kind, InputMacroActionKind::CaptureU32Baseline);
    EXPECT_EQ(capture.memory_baseline, 0x12345678u);

    const auto changed = runtime.ExecuteNext();
    EXPECT_EQ(changed.terminal_status, InputMacroTerminalStatus::Completed);
    EXPECT_EQ(changed.memory_address, kAddress);
    EXPECT_EQ(changed.memory_baseline, 0x12345678u);
    EXPECT_EQ(changed.memory_latest, 0x87654321u);
    EXPECT_TRUE(changed.memory_changed);
    EXPECT_EQ(changed.memory_poll_count, 6u);
    EXPECT_EQ(changed.elapsed_ms, 19u);
    EXPECT_EQ(changed.diagnostic_cycle_index, 2u);
    ASSERT_EQ(host.memory_requests.size(), 1u);
    EXPECT_EQ(host.memory_requests[0].timeout_ms, 250u);
    ExpectCleanupSuffix(host);
}

TEST(InputMacroRuntime, RejectsMissingOrMismatchedBaselineReferences)
{
    const std::vector<BPKey> declared;
    {
        FakeInputMacroHost host;
        InputMacroRuntime runtime(host);
        InputMacroPlan plan{{
            {.label = "missing", .action = WaitU32ChangeAction{"none", kAddress, 100, 0}},
        }};
        const auto result = runtime.Start(std::move(plan), declared);
        EXPECT_EQ(result.failure, InputMacroFailure::InvalidBaselineReference);
        EXPECT_EQ(host.Count("acquire"), 0u);
    }
    {
        FakeInputMacroHost host;
        InputMacroRuntime runtime(host);
        InputMacroPlan plan{{
            {.label = "capture", .action = CaptureU32BaselineAction{"same", kAddress}},
            {.label = "wrong-address", .action = WaitU32ChangeAction{"same", kAddress + 4, 100, 0}},
        }};
        const auto result = runtime.Start(std::move(plan), declared);
        EXPECT_EQ(result.failure, InputMacroFailure::InvalidBaselineReference);
    }
}

TEST(InputMacroRuntime, ReportsMemoryTimeoutWithTelemetry)
{
    FakeInputMacroHost host;
    host.read_results.push_back({true, 41u});
    host.memory_results.push_back({
        .status = InputMacroHostStatus::TimedOut,
        .latest_value = 41u,
        .poll_count = 9,
        .elapsed_ms = 100,
    });
    InputMacroRuntime runtime(host);
    const std::vector<BPKey> declared;
    InputMacroPlan plan{{
        {.label = "capture", .action = CaptureU32BaselineAction{"value", kAddress}},
        {.label = "timeout", .action = WaitU32ChangeAction{"value", kAddress, 100, 1}},
    }};
    ASSERT_EQ(runtime.Start(std::move(plan), declared).state, InputMacroRuntimeState::Running);
    ASSERT_TRUE(runtime.ExecuteNext().step_completed);

    const auto result = runtime.ExecuteNext();
    EXPECT_EQ(result.failure, InputMacroFailure::MemoryTimeout);
    EXPECT_EQ(result.memory_baseline, 41u);
    EXPECT_EQ(result.memory_latest, 41u);
    EXPECT_EQ(result.memory_poll_count, 9u);
    EXPECT_EQ(result.elapsed_ms, 100u);
    ExpectCleanupSuffix(host);
}

TEST(InputMacroRuntime, ReportsCaptureAndWaitReadFailures)
{
    const std::vector<BPKey> declared;
    {
        FakeInputMacroHost host;
        host.read_results.push_back({false, 0});
        InputMacroRuntime runtime(host);
        InputMacroPlan plan{{
            {.label = "capture", .action = CaptureU32BaselineAction{"value", kAddress}},
        }};
        ASSERT_EQ(runtime.Start(std::move(plan), declared).state, InputMacroRuntimeState::Running);
        EXPECT_EQ(runtime.ExecuteNext().failure, InputMacroFailure::MemoryReadFailed);
        ExpectCleanupSuffix(host);
    }
    {
        FakeInputMacroHost host;
        host.read_results.push_back({true, 7});
        host.memory_results.push_back({
            .status = InputMacroHostStatus::ReadFailed,
            .latest_value = 7,
            .poll_count = 2,
            .elapsed_ms = 3,
        });
        InputMacroRuntime runtime(host);
        InputMacroPlan plan{{
            {.label = "capture", .action = CaptureU32BaselineAction{"value", kAddress}},
            {.label = "read-fail", .action = WaitU32ChangeAction{"value", kAddress, 50, 0}},
        }};
        ASSERT_EQ(runtime.Start(std::move(plan), declared).state, InputMacroRuntimeState::Running);
        ASSERT_TRUE(runtime.ExecuteNext().step_completed);
        EXPECT_EQ(runtime.ExecuteNext().failure, InputMacroFailure::MemoryReadFailed);
        ExpectCleanupSuffix(host);
    }
}

TEST(InputMacroRuntime, ValidatesPlanStructureAddressesAndTimeouts)
{
    const std::vector<BPKey> declared{kGateA};
    auto expect_failure = [&](InputMacroPlan plan, InputMacroFailure failure) {
        FakeInputMacroHost host;
        InputMacroRuntime runtime(host);
        const auto result = runtime.Start(std::move(plan), declared);
        EXPECT_EQ(result.failure, failure);
        EXPECT_EQ(host.Count("acquire"), 0u);
        EXPECT_EQ(host.Count("neutral"), 1u);
        EXPECT_EQ(host.Count("clear"), 1u);
        EXPECT_EQ(host.Count("release"), 0u);
        EXPECT_EQ(host.Count("restore"), 1u);
    };

    expect_failure({}, InputMacroFailure::EmptyPlan);
    expect_failure(InputMacroPlan{{GateStep("empty", {})}}, InputMacroFailure::EmptyExpectedKeys);
    expect_failure(InputMacroPlan{{{.label = "zero", .action = CaptureU32BaselineAction{"b", 0}}}},
        InputMacroFailure::InvalidAddress);
    expect_failure(InputMacroPlan{{{.label = "unaligned", .action = CaptureU32BaselineAction{"b", kAddress + 1}}}},
        InputMacroFailure::InvalidAddress);
    expect_failure(InputMacroPlan{{
        {.label = "capture", .action = CaptureU32BaselineAction{"b", kAddress}},
        {.label = "timeout", .action = WaitU32ChangeAction{"b", kAddress, 0, 0}},
    }}, InputMacroFailure::InvalidTimeout);
}

TEST(InputMacroRuntime, RejectsUndeclaredAndUnauthorizedBreakpointKeys)
{
    {
        FakeInputMacroHost host;
        InputMacroRuntime runtime(host);
        const std::vector<BPKey> declared{kGateA};
        const auto result = runtime.Start(OneGatePlan(kGateB), declared);
        EXPECT_EQ(result.failure, InputMacroFailure::UndeclaredBreakpoint);
        EXPECT_EQ(result.expected_key, kGateB);
    }
    {
        FakeInputMacroHost host;
        InputMacroRuntime runtime(host);
        constexpr BPKey unknown = 65000;
        const std::vector<BPKey> declared{unknown};
        const auto result = runtime.Start(OneGatePlan(unknown), declared);
        EXPECT_EQ(result.failure, InputMacroFailure::UnauthorizedBreakpoint);
        EXPECT_EQ(result.expected_key, unknown);
    }
}

TEST(InputMacroRuntime, AcceptsPlayerVisibleSharedKeysAllowedForMacroControl)
{
    FakeInputMacroHost host;
    constexpr BPKey shared_key = bp::battle::TurnInputs;
    host.breakpoint_results = {{
        .status = InputMacroHostStatus::Succeeded,
        .hit = true,
        .hit_key = shared_key,
        .hit_pc = 0x80071740u,
    }};

    InputMacroRuntime runtime(host);
    const std::vector<BPKey> declared{shared_key};
    ASSERT_EQ(runtime.Start(OneGatePlan(shared_key), declared).failure, InputMacroFailure::None);
    const auto result = runtime.ExecuteNext();

    EXPECT_EQ(result.failure, InputMacroFailure::None);
    EXPECT_EQ(result.terminal_status, InputMacroTerminalStatus::Completed);
    EXPECT_EQ(host.acquired_keys, declared);
}

TEST(InputMacroRuntime, CleansUpExactlyOnceOnCompletionFailureAndCancellation)
{
    const std::vector<BPKey> declared{kGateA};
    {
        FakeInputMacroHost host;
        host.breakpoint_results.push_back({
            .status = InputMacroHostStatus::Succeeded, .hit = true, .hit_key = kGateA});
        InputMacroRuntime runtime(host);
        ASSERT_EQ(runtime.Start(OneGatePlan(), declared).state, InputMacroRuntimeState::Running);
        ASSERT_EQ(runtime.ExecuteNext().state, InputMacroRuntimeState::Completed);
        EXPECT_EQ(host.Count("neutral"), 1u);
        EXPECT_EQ(host.Count("clear"), 1u);
        EXPECT_EQ(host.Count("release"), 1u);
        EXPECT_EQ(host.Count("restore"), 1u);
        EXPECT_EQ(runtime.Cancel().failure, InputMacroFailure::NotRunning);
        EXPECT_EQ(host.Count("release"), 1u);
    }
    {
        FakeInputMacroHost host;
        host.breakpoint_results.push_back({.status = InputMacroHostStatus::TimedOut});
        InputMacroRuntime runtime(host);
        ASSERT_EQ(runtime.Start(OneGatePlan(), declared).state, InputMacroRuntimeState::Running);
        ASSERT_EQ(runtime.ExecuteNext().state, InputMacroRuntimeState::Failed);
        EXPECT_EQ(host.Count("release"), 1u);
        EXPECT_EQ(runtime.ExecuteNext().failure, InputMacroFailure::NotRunning);
        EXPECT_EQ(host.Count("release"), 1u);
    }
    {
        FakeInputMacroHost host;
        InputMacroRuntime runtime(host);
        ASSERT_EQ(runtime.Start(OneGatePlan(), declared).state, InputMacroRuntimeState::Running);
        const auto cancelled = runtime.Cancel();
        EXPECT_EQ(cancelled.terminal_status, InputMacroTerminalStatus::Cancelled);
        EXPECT_EQ(host.Count("release"), 1u);
        EXPECT_EQ(runtime.Cancel().failure, InputMacroFailure::NotRunning);
        EXPECT_EQ(host.Count("release"), 1u);
    }
}

TEST(InputMacroRuntime, CleansUpExactlyOnceOnReplacementAndDestruction)
{
    const std::vector<BPKey> declared{kGateA};
    {
        FakeInputMacroHost host;
        InputMacroRuntime runtime(host);
        ASSERT_EQ(runtime.Start(OneGatePlan(), declared).state, InputMacroRuntimeState::Running);
        ASSERT_EQ(runtime.Replace(OneGatePlan(), declared).state, InputMacroRuntimeState::Running);
        EXPECT_EQ(host.Count("acquire"), 2u);
        EXPECT_EQ(host.Count("release"), 1u);
        runtime.Cancel();
        EXPECT_EQ(host.Count("release"), 2u);
        EXPECT_EQ(host.Count("restore"), 2u);
    }
    {
        FakeInputMacroHost host;
        {
            InputMacroRuntime runtime(host);
            ASSERT_EQ(runtime.Start(OneGatePlan(), declared).state, InputMacroRuntimeState::Running);
        }
        EXPECT_EQ(host.Count("neutral"), 1u);
        EXPECT_EQ(host.Count("clear"), 1u);
        EXPECT_EQ(host.Count("release"), 1u);
        EXPECT_EQ(host.Count("restore"), 1u);
        ExpectCleanupSuffix(host);
    }
}

TEST(InputMacroRuntime, SessionAcquisitionFailureRestoresSafeHostStateWithoutRelease)
{
    FakeInputMacroHost host;
    host.acquire_ok = false;
    InputMacroRuntime runtime(host);
    const std::vector<BPKey> declared{kGateA};

    const auto result = runtime.Start(OneGatePlan(), declared);
    EXPECT_EQ(result.failure, InputMacroFailure::SessionUnavailable);
    EXPECT_EQ(host.calls, (std::vector<std::string>{"acquire", "neutral", "clear", "restore"}));
    EXPECT_EQ(host.Count("release"), 0u);
}

} // namespace
