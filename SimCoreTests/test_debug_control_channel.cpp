#include "gtest/gtest.h"

#include "Runner/Debug/DebugControlChannel.h"

#include <thread>
#include <chrono>

TEST(DebugControlChannel, RunToBreakpointCanBeInterruptedByFrameStep) {
    simcore::debug::LocalDebugControlServer server(
        42,
        7,
        "tok",
        "ipc://vm/tok",
        "ipc://dolphin/tok");

    simcore::debug::ControlCommand run_cmd{};
    run_cmd.env.session_id = 42;
    run_cmd.env.endpoint = simcore::debug::EndpointType::Dolphin;
    run_cmd.env.command = simcore::debug::CommandType::RunToBreakpoint;
    auto rr = server.Send("ipc://dolphin/tok", "tok", run_cmd);
    EXPECT_TRUE(rr.ok);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    simcore::debug::ControlCommand step_cmd{};
    step_cmd.env.session_id = 42;
    step_cmd.env.endpoint = simcore::debug::EndpointType::Dolphin;
    step_cmd.env.command = simcore::debug::CommandType::StepFrame;
    rr = server.Send("ipc://dolphin/tok", "tok", step_cmd);
    EXPECT_TRUE(rr.ok);

    auto snap = server.Snapshot();
    ASSERT_TRUE(snap.ok);
    EXPECT_EQ(snap.value.emu_state, "EMU_PAUSED");
    EXPECT_EQ(snap.value.ux_mode, "FRAME_STEP_DEFAULT");
    EXPECT_GE(snap.value.frame_index, 1);
}

TEST(DebugControlChannel, RejectsBadToken) {
    simcore::debug::LocalDebugControlServer server(
        42,
        7,
        "tok",
        "ipc://vm/tok",
        "ipc://dolphin/tok");

    simcore::debug::ControlCommand cmd{};
    cmd.env.session_id = 42;
    cmd.env.endpoint = simcore::debug::EndpointType::VM;
    cmd.env.command = simcore::debug::CommandType::StepVmInstruction;

    auto rr = server.Send("ipc://vm/tok", "wrong", cmd);
    EXPECT_FALSE(rr.ok);
    EXPECT_EQ(rr.error.message, "InvalidSessionToken");
}

TEST(DebugControlChannel, SetModeFrameStepInterruptsRunToBreakpoint) {
    simcore::debug::LocalDebugControlServer server(
        77,
        11,
        "tok",
        "ipc://vm/tok",
        "ipc://dolphin/tok");

    simcore::debug::ControlCommand run_cmd{};
    run_cmd.env.session_id = 77;
    run_cmd.env.endpoint = simcore::debug::EndpointType::Dolphin;
    run_cmd.env.command = simcore::debug::CommandType::RunToBreakpoint;
    auto rr = server.Send("ipc://dolphin/tok", "tok", run_cmd);
    ASSERT_TRUE(rr.ok);

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    simcore::debug::ControlCommand mode_cmd{};
    mode_cmd.env.session_id = 77;
    mode_cmd.env.endpoint = simcore::debug::EndpointType::Dolphin;
    mode_cmd.env.command = simcore::debug::CommandType::SetModeFrameStep;
    rr = server.Send("ipc://dolphin/tok", "tok", mode_cmd);
    ASSERT_TRUE(rr.ok);

    auto snap = server.Snapshot();
    ASSERT_TRUE(snap.ok);
    EXPECT_EQ(snap.value.emu_state, "EMU_PAUSED");
    EXPECT_EQ(snap.value.ux_mode, "FRAME_STEP_DEFAULT");
}
