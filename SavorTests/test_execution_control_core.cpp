#include "common/FakeExecutionBackend.h"

#include <gtest/gtest.h>

#include <memory>

namespace savor::runtime {
namespace {

template <typename T>
concept HasLegacyPauseRequest = requires(T& value) {
    value.RequestPause();
};

template <typename T>
concept HasLegacyResumeRequest = requires(T& value) {
    value.Resume();
};

static_assert(!HasLegacyPauseRequest<IExecutionBackendPort>);
static_assert(!HasLegacyResumeRequest<IExecutionBackendPort>);

TEST(ExecutionControlCoreArchitecture, UsesOneGenerationTaggedControlSurface)
{
    auto control =
        std::make_shared<test_support::FakeExecutionBackendControl>();
    test_support::FakeExecutionBackend backend(control);

    ASSERT_TRUE(backend.SubmitControlCommand({
        ExecutionControlGeneration(41),
        BackendControlCommandKind::Pause}).ok);
    ASSERT_TRUE(backend.SubmitControlCommand({
        ExecutionControlGeneration(42),
        BackendControlCommandKind::Resume}).ok);

    const auto calls = control->Calls();
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0], "pause");
    EXPECT_EQ(calls[1], "resume");
    const BackendExecutionSnapshot snapshot = control->Snapshot();
    EXPECT_EQ(
        snapshot.applied_control_generation,
        ExecutionControlGeneration(42));
    EXPECT_EQ(snapshot.core_state, BackendCoreState::Running);
    EXPECT_FALSE(snapshot.pause_confirmed);
}

TEST(ExecutionControlCoreArchitecture, ForegroundIntentCarriesControlOwnership)
{
    const ForegroundStopWait route{
        .suppress_immediate_reentry = true,
        .execution_control_generation = 19,
        .execution_operation_id = 7,
    };
    EXPECT_EQ(route.execution_control_generation, 19u);
    EXPECT_EQ(route.execution_operation_id, 7u);
}

} // namespace
} // namespace savor::runtime
