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

TEST(ExecutionControlCoreArchitecture, UsesOneOwnedControlTaskAtATime)
{
    auto control =
        std::make_shared<test_support::FakeExecutionBackendControl>();
    test_support::FakeExecutionBackend backend(control);

    ASSERT_TRUE(backend.SubmitControlTask({
        BackendControlTaskKind::Pause}).ok);
    EXPECT_FALSE(backend.SubmitControlTask({
        BackendControlTaskKind::Resume}).ok);
    EXPECT_FALSE(backend.TakeControlCompletion().has_value());
    ASSERT_TRUE(backend.PumpControlTask().ok);
    const auto pause = backend.TakeControlCompletion();
    ASSERT_TRUE(pause.has_value());
    EXPECT_EQ(pause->kind, BackendControlTaskKind::Pause);
    ASSERT_TRUE(backend.SubmitControlTask({
        BackendControlTaskKind::Resume}).ok);
    ASSERT_TRUE(backend.PumpControlTask().ok);
    const auto resume = backend.TakeControlCompletion();
    ASSERT_TRUE(resume.has_value());

    const auto calls = control->Calls();
    ASSERT_EQ(calls.size(), 4u);
    EXPECT_EQ(calls[0], "pump_control_task");
    EXPECT_EQ(calls[1], "pause");
    EXPECT_EQ(calls[2], "pump_control_task");
    EXPECT_EQ(calls[3], "resume");
    const BackendExecutionSnapshot snapshot = control->Snapshot();
    EXPECT_EQ(snapshot.core_state, BackendCoreState::Running);
    EXPECT_FALSE(snapshot.paused_quiescent);
}

TEST(ExecutionControlCoreArchitecture, ForegroundIntentCarriesControlOwnership)
{
    const ForegroundStopWait route{
        .execution_operation_id = 7,
    };
    EXPECT_EQ(route.execution_operation_id, 7u);
}

} // namespace
} // namespace savor::runtime
