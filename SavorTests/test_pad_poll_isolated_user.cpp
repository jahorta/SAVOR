#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Core/Input/InputPlan.h"
#include "Core/InputCommon/GCPadStatus.h"
#include "Runner/InputMacro/InputMacroPlan.h"

namespace {

enum class LegacyTraceEvent : std::uint8_t
{
    PublishRequestedInput,
    CaptureRequestEpoch,
    ArmExactSourceReentrySuppression,
    DisableSharedPhysicalStopSite,
    ExecuteSourceInstruction,
    AwaitRoutedStop,
    ExecuteReachedInstruction,
    ReadRequestReceipt,
    PublishNeutralInput,
    CaptureBaseline,
    ObserveCurrentValue,
    AdvanceOneNeutralFrame,
};

std::vector<LegacyTraceEvent> DescribeLegacyInteraction(
    bool starts_at_source_stop,
    bool hold_input_through_reached_instruction,
    bool release_input)
{
    std::vector<LegacyTraceEvent> trace{
        LegacyTraceEvent::PublishRequestedInput,
        LegacyTraceEvent::CaptureRequestEpoch,
    };
    if (starts_at_source_stop)
    {
        trace.push_back(
            LegacyTraceEvent::ArmExactSourceReentrySuppression);
        trace.push_back(LegacyTraceEvent::ExecuteSourceInstruction);
    }
    trace.push_back(LegacyTraceEvent::AwaitRoutedStop);
    if (hold_input_through_reached_instruction)
        trace.push_back(LegacyTraceEvent::ExecuteReachedInstruction);
    trace.push_back(LegacyTraceEvent::ReadRequestReceipt);
    if (release_input)
        trace.push_back(LegacyTraceEvent::PublishNeutralInput);
    return trace;
}

struct LegacyTapeAttempt
{
    savor::InputPlan plan;
    std::uint32_t workflow_retry_count = 0;
    std::uint32_t max_unacknowledged_replays = 2;
    bool safe_mode = false;
    std::string label;
};

LegacyTapeAttempt DescribeLegacyTapeAttempt(
    savor::InputPlan plan,
    std::uint32_t workflow_retry_count)
{
    return {
        .plan = std::move(plan),
        .workflow_retry_count = workflow_retry_count,
        .max_unacknowledged_replays = 2,
        .safe_mode = workflow_retry_count > 0,
        .label =
            "battle_turn_attempt_" +
            std::to_string(workflow_retry_count),
    };
}

std::vector<LegacyTraceEvent> DescribeMemoryChangePolling(
    std::uint32_t observation_count)
{
    std::vector<LegacyTraceEvent> trace{
        LegacyTraceEvent::CaptureBaseline,
    };
    for (std::uint32_t index = 0; index < observation_count; ++index)
    {
        trace.push_back(LegacyTraceEvent::ObserveCurrentValue);
        if (index + 1 < observation_count)
            trace.push_back(LegacyTraceEvent::AdvanceOneNeutralFrame);
    }
    return trace;
}

TEST(LegacyExecutionTrace, PadStatusConversionPreservesNeutralAndAButton)
{
    GCPadStatus neutral{};
    const savor::GCInputFrame neutral_frame =
        savor::FromGCPadStatus(neutral);
    EXPECT_EQ(neutral_frame.buttons & PAD_BUTTON_A, 0u);

    GCPadStatus pressed{};
    pressed.button = PAD_BUTTON_A;
    const savor::GCInputFrame pressed_frame =
        savor::FromGCPadStatus(pressed);
    EXPECT_NE(pressed_frame.buttons & PAD_BUTTON_A, 0u);
}

TEST(
    LegacyExecutionTrace,
    SourceStepOffUsesExactReentrySuppressionWithoutDisablingSharedSite)
{
    const auto trace = DescribeLegacyInteraction(
        true,
        false,
        false);

    EXPECT_EQ(
        std::count(
            trace.begin(),
            trace.end(),
            LegacyTraceEvent::ArmExactSourceReentrySuppression),
        1);
    EXPECT_EQ(
        std::count(
            trace.begin(),
            trace.end(),
            LegacyTraceEvent::ExecuteSourceInstruction),
        1);
    EXPECT_EQ(
        std::count(
            trace.begin(),
            trace.end(),
            LegacyTraceEvent::DisableSharedPhysicalStopSite),
        0);
}

TEST(
    LegacyExecutionTrace,
    PublishesInputBeforeSourceStepAndReadsReceiptBeforeNeutralRelease)
{
    const auto trace = DescribeLegacyInteraction(
        true,
        true,
        true);

    EXPECT_EQ(
        trace,
        (std::vector<LegacyTraceEvent>{
            LegacyTraceEvent::PublishRequestedInput,
            LegacyTraceEvent::CaptureRequestEpoch,
            LegacyTraceEvent::ArmExactSourceReentrySuppression,
            LegacyTraceEvent::ExecuteSourceInstruction,
            LegacyTraceEvent::AwaitRoutedStop,
            LegacyTraceEvent::ExecuteReachedInstruction,
            LegacyTraceEvent::ReadRequestReceipt,
            LegacyTraceEvent::PublishNeutralInput,
        }));
}

TEST(
    LegacyExecutionTrace,
    TapeRetryRetainsThePlanAndEnablesSafeModeAfterFirstAttempt)
{
    savor::GCInputFrame press_a{};
    press_a.A();
    savor::GCInputFrame press_b{};
    press_b.B();
    const savor::InputPlan plan{press_a, press_b};

    const auto first = DescribeLegacyTapeAttempt(plan, 0);
    const auto retry = DescribeLegacyTapeAttempt(plan, 1);

    EXPECT_EQ(first.plan, plan);
    EXPECT_EQ(retry.plan, plan);
    EXPECT_EQ(first.max_unacknowledged_replays, 2u);
    EXPECT_EQ(retry.max_unacknowledged_replays, 2u);
    EXPECT_FALSE(first.safe_mode);
    EXPECT_TRUE(retry.safe_mode);
    EXPECT_EQ(first.label, "battle_turn_attempt_0");
    EXPECT_EQ(retry.label, "battle_turn_attempt_1");
}

TEST(
    LegacyExecutionTrace,
    MacroPlanCapturesNamedBaselineBeforeAnyAdvancement)
{
    constexpr std::uint32_t kAddress = 0x803469A8u;
    constexpr char kBaseline[] = "battle.fake_attack.rng.0";
    const savor::inputmacro::InputMacroPlan plan{{
        {
            .label = "capture",
            .action =
                savor::inputmacro::CaptureU32BaselineAction{
                    kBaseline,
                    kAddress,
                },
        },
        {
            .label = "advance",
            .action =
                savor::inputmacro::NeutralFramesAction{1},
        },
        {
            .label = "observe-change",
            .action =
                savor::inputmacro::WaitU32ChangeAction{
                    kBaseline,
                    kAddress,
                    0,
                },
        },
    }};

    ASSERT_EQ(plan.steps.size(), 3u);
    EXPECT_EQ(
        savor::inputmacro::ActionKind(plan.steps[0].action),
        savor::inputmacro::InputMacroActionKind::CaptureU32Baseline);
    EXPECT_EQ(
        savor::inputmacro::ActionKind(plan.steps[1].action),
        savor::inputmacro::InputMacroActionKind::NeutralFrames);
    EXPECT_EQ(
        savor::inputmacro::ActionKind(plan.steps[2].action),
        savor::inputmacro::InputMacroActionKind::WaitU32Change);

    const auto& capture =
        std::get<savor::inputmacro::CaptureU32BaselineAction>(
            plan.steps[0].action);
    const auto& observation =
        std::get<savor::inputmacro::WaitU32ChangeAction>(
            plan.steps[2].action);
    EXPECT_EQ(capture.baseline_id, observation.baseline_id);
    EXPECT_EQ(capture.address, observation.address);
}

TEST(
    LegacyExecutionTrace,
    MemoryChangePollingPlacesExactlyOneNeutralFrameBetweenReads)
{
    const auto trace = DescribeMemoryChangePolling(3);

    EXPECT_EQ(
        trace,
        (std::vector<LegacyTraceEvent>{
            LegacyTraceEvent::CaptureBaseline,
            LegacyTraceEvent::ObserveCurrentValue,
            LegacyTraceEvent::AdvanceOneNeutralFrame,
            LegacyTraceEvent::ObserveCurrentValue,
            LegacyTraceEvent::AdvanceOneNeutralFrame,
            LegacyTraceEvent::ObserveCurrentValue,
        }));
}

} // namespace
