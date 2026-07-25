#include <ActionMotionInvocationModel.h>
#include <ActionViewMode11Model.h>
#include <BattleFrameSchedulerModel.h>
#include <RngCore.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace savor::predict;

std::uint32_t float_bits(float value) {
    return std::bit_cast<std::uint32_t>(value);
}

ActionViewMode11Vec3Bits vec3_bits(float x, float y, float z) {
    return {
        .x_bits = float_bits(x),
        .y_bits = float_bits(y),
        .z_bits = float_bits(z),
    };
}

std::vector<MovementSlotState> mode11_slots() {
    return {
        MovementSlotState{
            .slot = 0,
            .present = true,
            .is_player = true,
            .alive = true,
            .motion_base_speed = 2.5f,
            .motion_alt_speed = 0.5f,
            .motion_speeds_known = true,
            .motion_turn_speed = 22.0f,
            .motion_turn_speed_known = true,
            .width = 1,
            .depth = 1,
            .start_position = BattleStartPosition{
                .slot = 0,
                .present = true,
                .is_player = true,
                .grid_x = 4,
                .grid_z = 8,
            },
        },
        MovementSlotState{
            .slot = 4,
            .present = true,
            .is_player = false,
            .alive = true,
            .motion_base_speed = 2.0f,
            .motion_alt_speed = 0.5f,
            .motion_speeds_known = true,
            .motion_turn_speed = 22.0f,
            .motion_turn_speed_known = true,
            .width = 1,
            .depth = 1,
            .start_position = BattleStartPosition{
                .slot = 4,
                .present = true,
                .is_player = false,
                .grid_x = 4,
                .grid_z = 2,
            },
        },
    };
}

std::optional<BattleFrameRuntime> mode11_runtime() {
    std::array<std::uint8_t, 81> terrain{};
    return initialize_first_battle_frame_runtime(
        0,
        mode11_slots(),
        terrain);
}

void install_mode11_controller_input(BattleFrameRuntime& runtime) {
    CombatantVisualResource resource;
    resource.binding = {
        .slot = 0,
        .resource_stem = "mode11_fixture",
    };
    resource.records.push_back(CombatantVisualCommandRecord{
        .index = 0,
        .location_code = 0x2a,
        .opcode = 3,
        .gate_fields_known = true,
        .gate_fields = Std0PayloadGateFields{
            .primary_action_key = 5,
            .generic_secondary_key = -1,
            .direct_gate_secondary_key = -1,
        },
    });
    resource.records.push_back(CombatantVisualCommandRecord{
        .index = 1,
        .location_code = -1,
        .opcode = 0,
    });
    resource.includes_sentinel = true;
    runtime.visual.resources[0] = std::move(resource);

    auto& timeline = runtime.visual.timelines[0];
    timeline.installed = true;
    timeline.epoch = 1;
    timeline.instruction = CombatantVisualInstructionSnapshot{
        .slot = 0,
        .runtime_instruction_mode = 5,
        .selected_std_action_key = 5,
        .subtype = -1,
        .target_slot = 4,
        .instruction_flags = 0,
        .knowledge = CombatantVisualInstructionKnowledge::Known,
        .provenance = "mode-0x11 controller test input",
    };
    runtime.visual.timeline_action_ordinals[0] = 0;
    auto* actor = find_frame_combatant(runtime.state, 0);
    ASSERT_NE(actor, nullptr);
    actor->instruction_target_slot_0x4 = 4;
    actor->visual_instruction_mode_0x6 = 5;
    actor->visual_instruction_subtype_0x8 = -1;
    actor->visual_instruction_knowledge =
        CombatantVisualInstructionKnowledge::Known;
    actor->visual_instruction_revision = 1;
    actor->visual_instruction_action_ordinal = 0;
    actor->visual_instruction_provenance =
        "mode-0x11 current worksheet fixture";
}

BattleFrameActionScheduleResult schedule_mode11_action(
    BattleFrameRuntime& runtime) {
    return schedule_first_turn_actor_action(
        runtime,
        BattleFrameScheduleActionInput{
            .action_ordinal = 0,
            .actor_slot = 0,
            .target_slot = 4,
            .enemy_owned = false,
            .selected_worker =
                MovementSelectedWorker::PcDirectAttack_80086308,
            .action_kind = BattleMovementActionKind::BasicAttack,
            .relation_scope = BattleMovementRelationScope::SingleTarget,
            .turn_type = BattleMovementTurnType::Normal,
        });
}

BattleFrameVisualChildTask* mode11_task(BattleFrameRuntime& runtime) {
    const auto found = std::find_if(
        runtime.visual.child_tasks.begin(),
        runtime.visual.child_tasks.end(),
        [](const BattleFrameVisualChildTask& task) {
            return task.payload_mode == 0x11;
        });
    return found == runtime.visual.child_tasks.end() ? nullptr : &*found;
}

int active_node_index(
    const BattleFrameThreadListRuntime& runtime,
    int node_id) {
    for (std::size_t index = 0; index < runtime.nodes.size(); ++index) {
        if (runtime.nodes[index].active
            && runtime.nodes[index].node_id == node_id) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

int count_step(
    const std::vector<BattleFrameStepEvent>& events,
    BattleFrameWorkerStepKind kind) {
    return static_cast<int>(std::count_if(
        events.begin(),
        events.end(),
        [kind](const BattleFrameStepEvent& event) {
            return event.step_kind == kind;
        }));
}

BattleFrameVisualChildTask make_old_mode11_task(int sequence) {
    BattleFrameVisualChildTask task;
    task.sequence = sequence;
    task.action_ordinal = 0;
    task.origin_slot = 0;
    task.target_slot = 4;
    task.command_kind = CombatantVisualCommandKind::SyntheticActionView;
    task.kind = BattleFrameVisualChildKind::ActionViewRecord;
    task.phase = BattleFrameVisualChildPhase::CompletionWait;
    task.synthetic = true;
    task.payload_mode = 0x11;
    task.effective_mode = 0x11;
    task.thread_state_0x19 = 2;
    task.mode11_initialized = true;
    task.mode11_status = ActionViewMode11Status::Matched;
    task.mode11_branch = ActionViewMode11Branch::Interpolation;
    task.mode11_substate = 3;
    task.mode11_counter = 0;
    task.mode11_gate_owned = true;
    task.mode11_gate_cleared = true;
    task.active_record_installed = true;
    task.maximum_visits = 256;
    task.provenance = "old mode-0x11 active-record fixture";
    return task;
}

BattleFrameVisualChildTask make_replacement_task(int sequence) {
    BattleFrameVisualChildTask task;
    task.sequence = sequence;
    task.action_ordinal = 0;
    task.origin_slot = 0;
    task.target_slot = 4;
    task.command_kind = CombatantVisualCommandKind::SystemCamera;
    task.kind = BattleFrameVisualChildKind::ActionViewRecord;
    task.phase = BattleFrameVisualChildPhase::Published;
    task.payload_mode = 2;
    task.effective_mode = 2;
    task.maximum_visits = 100;
    task.status = CombatantVisualModelStatus::Matched;
    task.provenance = "replacement action-view fixture";
    return task;
}

int add_visual_thread(
    BattleFrameRuntime& runtime,
    int sequence,
    BattleFrameThreadInsertionKind insertion,
    std::optional<int> relative_node_id = std::nullopt) {
    const auto created = create_battle_frame_thread(
        runtime.thread_list,
        BattleFrameThreadCreateRequest{
            .kind = BattleFrameThreadNodeKind::AuxiliaryVisualChild,
            .owner_slot = 0,
            .semantic_instance_id =
                static_cast<std::uint64_t>(sequence),
            .callback = BattleFrameThreadCallbackIdentity::VisualSystemCamera,
            .active = true,
            .insertion = insertion,
            .relative_node_id = relative_node_id,
            .semantic_source_id = "test.action_view.active_record",
            .provenance = "active-record replacement ordering fixture",
        });
    EXPECT_EQ(created.status, BattleFrameThreadMutationStatus::Applied);
    return created.node_id;
}

std::optional<BattleFrameRuntime> replacement_runtime(
    bool replacement_before_old) {
    auto runtime = mode11_runtime();
    if (!runtime.has_value()
        || !runtime->persistent_action_view_controller_node_id.has_value()) {
        return std::nullopt;
    }

    auto old_task = make_old_mode11_task(0);
    old_task.thread_node_id = add_visual_thread(
        *runtime,
        old_task.sequence,
        BattleFrameThreadInsertionKind::Append);

    auto replacement = make_replacement_task(1);
    replacement.thread_node_id = add_visual_thread(
        *runtime,
        replacement.sequence,
        replacement_before_old
            ? BattleFrameThreadInsertionKind::AfterNode
            : BattleFrameThreadInsertionKind::Append,
        replacement_before_old
            ? runtime->persistent_action_view_controller_node_id
            : std::nullopt);

    runtime->visual.child_tasks.push_back(std::move(old_task));
    runtime->visual.child_tasks.push_back(std::move(replacement));
    runtime->visual.next_child_sequence = 2;
    runtime->visual.active_record.task_sequence = 0;
    runtime->visual.active_record.revision = 1;
    return runtime;
}

TEST(SavorPredictActionViewMode11, PpcEqualityHandlesZerosAndNaN) {
    EXPECT_TRUE(action_view_mode11_ppc_float_equal(
        0x00000000u,
        0x80000000u));
    EXPECT_TRUE(action_view_mode11_ppc_float_equal(
        float_bits(12.5f),
        float_bits(12.5f)));
    EXPECT_FALSE(action_view_mode11_ppc_float_equal(
        float_bits(12.5f),
        float_bits(12.75f)));
    EXPECT_FALSE(action_view_mode11_ppc_float_equal(
        0x7fc00000u,
        0x7fc00000u));
}

TEST(SavorPredictActionViewMode11, SelectsExactAndProvisionalBranches) {
    const auto equal = select_action_view_mode11_branch(
        ActionViewMode11CameraOperands{
            .current_position = vec3_bits(1.0f, 2.0f, 3.0f),
            .desired_position = vec3_bits(1.0f, 2.0f, 3.0f),
            .current_center = vec3_bits(4.0f, 5.0f, 6.0f),
            .desired_center = vec3_bits(4.0f, 5.0f, 6.0f),
        });
    EXPECT_EQ(equal.status, ActionViewMode11Status::Matched);
    EXPECT_EQ(equal.branch, ActionViewMode11Branch::Equal);
    EXPECT_EQ(equal.counter, 5);
    EXPECT_FALSE(equal.advance_on_setup_visit);

    const auto unequal = select_action_view_mode11_branch(
        ActionViewMode11CameraOperands{
            .current_position = vec3_bits(1.0f, 2.0f, 3.0f),
            .desired_position = vec3_bits(1.0f, 2.0f, 4.0f),
            .current_center = vec3_bits(4.0f, 5.0f, 6.0f),
            .desired_center = vec3_bits(4.0f, 5.0f, 6.0f),
        });
    EXPECT_EQ(unequal.status, ActionViewMode11Status::Matched);
    EXPECT_EQ(unequal.branch, ActionViewMode11Branch::Interpolation);
    EXPECT_EQ(unequal.counter, 15);
    EXPECT_TRUE(unequal.advance_on_setup_visit);

    const auto missing = select_action_view_mode11_branch(std::nullopt);
    EXPECT_EQ(missing.status, ActionViewMode11Status::Provisional);
    EXPECT_EQ(
        missing.branch,
        ActionViewMode11Branch::ProvisionalInterpolation);
}

TEST(SavorPredictActionViewMode11, CounterOrderMatchesSixAndFifteenFrameClears) {
    int interpolation_counter = 15;
    auto interpolation =
        advance_action_view_mode11_counter(interpolation_counter);
    EXPECT_FALSE(interpolation.clear_gate);
    interpolation_counter = interpolation.counter_after;
    for (int frame_delta = 1; frame_delta < 15; ++frame_delta) {
        interpolation =
            advance_action_view_mode11_counter(interpolation_counter);
        EXPECT_FALSE(interpolation.clear_gate) << frame_delta;
        interpolation_counter = interpolation.counter_after;
    }
    interpolation =
        advance_action_view_mode11_counter(interpolation_counter);
    EXPECT_TRUE(interpolation.clear_gate);

    int equal_counter = 5;
    ActionViewMode11CounterStep equal;
    for (int frame_delta = 1; frame_delta < 6; ++frame_delta) {
        equal = advance_action_view_mode11_counter(equal_counter);
        EXPECT_FALSE(equal.clear_gate) << frame_delta;
        equal_counter = equal.counter_after;
    }
    equal = advance_action_view_mode11_counter(equal_counter);
    EXPECT_TRUE(equal.clear_gate);
}

TEST(SavorPredictActionViewMode11, State3WaitsWithoutCallingResolver) {
    const auto result = resolve_action_motion_invocation(
        {},
        ActionMotionInvocationRequest{
            .callback_family =
                ActionMotionPersistentCallbackFamily::
                    ActionMotionBasic_8001B1B0,
            .callback_state = 3,
            .instruction_mode = 5,
            .instruction_flags_0xf0 = 0x02000000u,
        });

    EXPECT_EQ(result.status, ActionMotionInvocationStatus::Matched);
    EXPECT_EQ(result.decision, ActionMotionInvocationDecisionKind::Wait);
    EXPECT_EQ(result.callback_state_before, 3);
    EXPECT_EQ(result.callback_state_after, 3);
    EXPECT_FALSE(result.resolver_called);
}

TEST(SavorPredictActionViewMode11, RoleResolverUsesLowF0BitOnly) {
    const auto ordinary = resolve_action_view_roles_8001d41c({
        .acting_actor_slot = 0,
        .queued_target_slot = 4,
        .target_instruction_flags_0xf0 = 0x04000000u,
        .target_present = true,
    });
    EXPECT_EQ(ordinary.status, ActionViewRoleStatus::Matched);
    EXPECT_FALSE(ordinary.reversed);
    EXPECT_EQ(ordinary.actor_slot, 0);
    EXPECT_EQ(ordinary.secondary_slot, 4);

    const auto reversed = resolve_action_view_roles_8001d41c({
        .acting_actor_slot = 0,
        .queued_target_slot = 4,
        .target_instruction_flags_0xf0 = 0x00000004u,
        .target_present = true,
    });
    EXPECT_EQ(reversed.status, ActionViewRoleStatus::Matched);
    EXPECT_TRUE(reversed.reversed);
    EXPECT_EQ(reversed.actor_slot, 4);
    EXPECT_EQ(reversed.secondary_slot, 0);
}

TEST(SavorPredictActionViewMode11, RoleFlagProducerPublishesFixedSequenceAndFlag) {
    ActionViewRoleFlagProducerState state;
    auto state0 = visit_action_view_role_flag_producer_80019b70({
        .state = state,
        .instruction_mode_0x6 = 5,
    });
    ASSERT_TRUE(state0.requested_instruction_mode.has_value());
    EXPECT_EQ(*state0.requested_instruction_mode, 0x0b);

    auto waiting = visit_action_view_role_flag_producer_80019b70({
        .state = state0.state,
        .instruction_mode_0x6 = 5,
    });
    EXPECT_EQ(
        waiting.state.phase,
        ActionViewRoleFlagProducerPhase::WaitForMode0B);

    auto matched = visit_action_view_role_flag_producer_80019b70({
        .state = waiting.state,
        .instruction_mode_0x6 = 0x0b,
    });
    EXPECT_EQ(
        matched.state.phase,
        ActionViewRoleFlagProducerPhase::AdvanceFixedSequence);

    auto mode5 = visit_action_view_role_flag_producer_80019b70({
        .state = matched.state,
        .instruction_mode_0x6 = 0x0b,
    });
    ASSERT_TRUE(mode5.requested_instruction_mode.has_value());
    EXPECT_EQ(*mode5.requested_instruction_mode, 5);

    auto published = visit_action_view_role_flag_producer_80019b70({
        .state = mode5.state,
        .instruction_mode_0x6 = 5,
    });
    EXPECT_TRUE(published.set_role_flag);
    EXPECT_TRUE(published.state.role_flag_owned);

    auto cleared = visit_action_view_role_flag_producer_80019b70({
        .state = published.state,
        .instruction_mode_0x6 = 5,
        .turn_phase = 5,
    });
    EXPECT_TRUE(cleared.clear_role_flag);
    EXPECT_TRUE(cleared.complete);
}

TEST(SavorPredictActionViewMode11, ControllerPublishesThreadedChildInSameFrame) {
    auto runtime = mode11_runtime();
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(runtime->persistent_action_view_controller_node_id.has_value());
    install_mode11_controller_input(*runtime);
    ASSERT_TRUE(schedule_mode11_action(*runtime).scheduled);

    std::uint32_t rng = 0x12345678u;
    const auto frame = run_first_turn_frame(*runtime, rng);
    auto* task = mode11_task(*runtime);
    ASSERT_NE(task, nullptr);
    EXPECT_GE(task->thread_node_id, 0);
    EXPECT_TRUE(task->mode11_initialized);
    EXPECT_EQ(
        task->mode11_branch,
        ActionViewMode11Branch::ProvisionalInterpolation);
    EXPECT_EQ(task->mode11_counter, 14);
    EXPECT_EQ(task->publication_frame, task->mode11_setup_frame);
    EXPECT_EQ(task->publication_frame, runtime->state.frame_index);
    EXPECT_EQ(
        runtime->visual.active_record.task_sequence,
        std::optional<int>{task->sequence});

    const auto* actor = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(actor, nullptr);
    EXPECT_NE(actor->instruction_flags_0xf0 & 0x02000000u, 0u);

    const int controller_index = active_node_index(
        runtime->thread_list,
        *runtime->persistent_action_view_controller_node_id);
    const int child_index =
        active_node_index(runtime->thread_list, task->thread_node_id);
    EXPECT_EQ(child_index, controller_index + 1);
    EXPECT_EQ(
        count_step(frame.events, BattleFrameWorkerStepKind::VisualMode11Setup),
        1);
    EXPECT_EQ(
        count_step(
            runtime->visual.history,
            BattleFrameWorkerStepKind::VisualMode11Setup),
        1);
}

TEST(
    SavorPredictActionViewMode11,
    ControllerWaitsForEarlierStateZeroInstructionProducer) {
    auto runtime = mode11_runtime();
    ASSERT_TRUE(runtime.has_value());

    CombatantVisualResource resource;
    resource.binding = {
        .slot = 0,
        .resource_stem = "mode11_initial_instruction_fixture",
    };
    resource.action_rows.push_back(CombatantStdActionRow{
        .index = 0,
        .action_id = 1,
        .row_type = 0,
        .callback_index = 9,
        .callback_ordinal = 7,
    });
    runtime->visual.resources[0] = std::move(resource);
    const auto instruction_thread = create_battle_frame_thread(
        runtime->thread_list,
        BattleFrameThreadCreateRequest{
            .kind = BattleFrameThreadNodeKind::CombatantInstruction,
            .owner_slot = 0,
            .callback =
                BattleFrameThreadCallbackIdentity::CombatantInstruction,
            .active = true,
            .semantic_source_id =
                "test.std_resource.publication",
            .provenance =
                "state-0 instruction producer follows the controller",
        });
    ASSERT_EQ(
        instruction_thread.status,
        BattleFrameThreadMutationStatus::Applied);
    ASSERT_TRUE(schedule_mode11_action(*runtime).scheduled);

    std::uint32_t rng = 0x12345678u;
    const auto first_frame = run_first_turn_frame(*runtime, rng);
    const auto deferred = std::find_if(
        first_frame.events.begin(),
        first_frame.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.callback == "FUN_80012F58";
        });
    ASSERT_NE(deferred, first_frame.events.end());
    EXPECT_EQ(deferred->status, BattleFrameEventStatus::Skipped);
    EXPECT_EQ(mode11_task(*runtime), nullptr);

    const auto* actor = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(actor, nullptr);
    EXPECT_NE(
        actor->visual_instruction_knowledge,
        CombatantVisualInstructionKnowledge::Unknown);

    (void)run_first_turn_frame(*runtime, rng);
    EXPECT_NE(mode11_task(*runtime), nullptr);
}

TEST(
    SavorPredictActionViewMode11,
    ActionVisualPublicationWaitsForCurrentSelectorRevision) {
    auto runtime = mode11_runtime();
    ASSERT_TRUE(runtime.has_value());
    auto* actor = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(actor, nullptr);
    actor->visual_instruction_mode_0x6 = 5;
    actor->visual_instruction_subtype_0x8 = -1;
    actor->visual_instruction_knowledge =
        CombatantVisualInstructionKnowledge::Known;
    runtime->visual.action_view_role = BattleFrameActionViewRoleRuntime{
        .valid = true,
        .action_ordinal = 3,
        .acting_actor_slot = 0,
        .queued_target_slot = 4,
    };
    runtime->visual.controller.initialized = true;
    runtime->visual.controller.actor_slot_0x2 = 0;
    runtime->visual.controller.effective_mode_0x2f = 3;
    runtime->visual.controller.selector_state_0x30 = 1;

    EXPECT_TRUE(
        battle_frame_action_visual_publication_pending(*runtime, 3));

    runtime->visual.controller.selector_state_0x30 = 3;
    EXPECT_FALSE(
        battle_frame_action_visual_publication_pending(*runtime, 3));

    runtime->visual.controller.effective_mode_0x2f = 0;
    EXPECT_TRUE(
        battle_frame_action_visual_publication_pending(*runtime, 3));

    runtime->visual.controller.effective_mode_0x2f = 3;
    runtime->visual.controller.actor_slot_0x2 = 4;
    EXPECT_TRUE(
        battle_frame_action_visual_publication_pending(*runtime, 3));
}

TEST(SavorPredictActionViewMode11, EqualBranchClearsGateAfterSixFrames) {
    auto runtime = mode11_runtime();
    ASSERT_TRUE(runtime.has_value());
    install_mode11_controller_input(*runtime);
    runtime->visual.mode11_camera_operands[0] =
        ActionViewMode11CameraOperands{
            .current_position = vec3_bits(1.0f, 2.0f, 3.0f),
            .desired_position = vec3_bits(1.0f, 2.0f, 3.0f),
            .current_center = vec3_bits(4.0f, 5.0f, 6.0f),
            .desired_center = vec3_bits(4.0f, 5.0f, 6.0f),
            .provenance = "exact equal-branch fixture",
        };
    ASSERT_TRUE(schedule_mode11_action(*runtime).scheduled);

    std::uint32_t rng = 0x12345678u;
    (void)run_first_turn_frame(*runtime, rng);
    auto* task = mode11_task(*runtime);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->mode11_branch, ActionViewMode11Branch::Equal);
    EXPECT_EQ(task->mode11_counter, 5);
    const int setup_frame = task->mode11_setup_frame;

    for (auto& worker : runtime->workers) {
        worker.complete = true;
    }
    ASSERT_TRUE(runtime->active_action.has_value());
    runtime->active_action->phase = BattleFrameActionPhase::Complete;

    for (int frame_delta = 1; frame_delta < 6; ++frame_delta) {
        (void)run_first_turn_frame(*runtime, rng);
        task = mode11_task(*runtime);
        ASSERT_NE(task, nullptr);
        EXPECT_FALSE(task->mode11_gate_cleared) << frame_delta;
    }
    const auto clear_frame = run_first_turn_frame(*runtime, rng);
    task = mode11_task(*runtime);
    ASSERT_NE(task, nullptr);
    EXPECT_TRUE(task->mode11_gate_cleared);
    EXPECT_EQ(runtime->state.frame_index - setup_frame, 6);
    EXPECT_EQ(
        count_step(
            clear_frame.events,
            BattleFrameWorkerStepKind::VisualInstructionGate),
        1);
    EXPECT_GE(
        count_step(
            runtime->visual.history,
            BattleFrameWorkerStepKind::VisualInstructionGate),
        2);
    const auto* actor = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(actor, nullptr);
    EXPECT_EQ(actor->instruction_flags_0xf0 & 0x02000000u, 0u);
    EXPECT_EQ(runtime->visual.controller.selector_state_0x30, 1);

    const auto post_clear_frame = run_first_turn_frame(*runtime, rng);
    EXPECT_EQ(runtime->visual.controller.selector_state_0x30, 3);
    EXPECT_EQ(
        count_step(
            post_clear_frame.events,
            BattleFrameWorkerStepKind::VisualInstructionGate),
        0);
    EXPECT_EQ(
        std::count_if(
            runtime->visual.child_tasks.begin(),
            runtime->visual.child_tasks.end(),
            [](const BattleFrameVisualChildTask& child) {
                return child.payload_mode == 0x11;
            }),
        1);
}

TEST(SavorPredictActionViewMode11, ReplacementPositionControlsStateFaTiming) {
    std::uint32_t rng = 0x12345678u;

    auto same_frame = replacement_runtime(true);
    ASSERT_TRUE(same_frame.has_value());
    const auto first_same = run_first_turn_frame(*same_frame, rng);
    ASSERT_EQ(same_frame->visual.child_tasks.size(), 2u);
    EXPECT_TRUE(same_frame->visual.child_tasks[0].active_record_state_fa);
    EXPECT_EQ(
        count_step(
            first_same.events,
            BattleFrameWorkerStepKind::VisualReplacementState),
        1);
    (void)run_first_turn_frame(*same_frame, rng);
    EXPECT_TRUE(same_frame->visual.child_tasks[0].complete);

    auto next_frame = replacement_runtime(false);
    ASSERT_TRUE(next_frame.has_value());
    const auto first_next = run_first_turn_frame(*next_frame, rng);
    EXPECT_FALSE(next_frame->visual.child_tasks[0].active_record_state_fa);
    EXPECT_TRUE(
        next_frame->visual.child_tasks[0].active_record_replacement_pending);
    EXPECT_EQ(
        count_step(
            first_next.events,
            BattleFrameWorkerStepKind::VisualReplacementState),
        0);

    (void)run_first_turn_frame(*next_frame, rng);
    EXPECT_TRUE(next_frame->visual.child_tasks[0].active_record_state_fa);
    EXPECT_FALSE(next_frame->visual.child_tasks[0].complete);
    (void)run_first_turn_frame(*next_frame, rng);
    EXPECT_TRUE(next_frame->visual.child_tasks[0].complete);
}

TEST(SavorPredictActionViewMode11, OldCleanupRetainsGateForReplacementOwner) {
    auto runtime = replacement_runtime(true);
    ASSERT_TRUE(runtime.has_value());
    ASSERT_EQ(runtime->visual.child_tasks.size(), 2u);
    auto& replacement = runtime->visual.child_tasks[1];
    replacement.synthetic = true;
    replacement.payload_mode = 0x11;
    replacement.effective_mode = 0x11;
    replacement.mode11_gate_owned = true;
    replacement.maximum_visits = 256;
    auto* actor = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(actor, nullptr);
    actor->instruction_flags_0xf0 |= 0x02000000u;

    std::uint32_t rng = 0x12345678u;
    (void)run_first_turn_frame(*runtime, rng);
    EXPECT_TRUE(runtime->visual.child_tasks[0].active_record_state_fa);
    (void)run_first_turn_frame(*runtime, rng);

    EXPECT_TRUE(runtime->visual.child_tasks[0].complete);
    EXPECT_FALSE(runtime->visual.child_tasks[1].mode11_gate_cleared);
    EXPECT_NE(actor->instruction_flags_0xf0 & 0x02000000u, 0u);
}

TEST(SavorPredictActionViewMode11, ReplacementAndCleanupDeferWhileGateIsOwned) {
    auto runtime = replacement_runtime(true);
    ASSERT_TRUE(runtime.has_value());
    ASSERT_EQ(runtime->visual.child_tasks.size(), 2u);
    auto& old = runtime->visual.child_tasks[0];
    auto& replacement = runtime->visual.child_tasks[1];
    old.mode11_gate_cleared = false;
    old.mode11_counter = 5;
    auto* actor = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(actor, nullptr);
    actor->instruction_flags_0xf0 |= 0x02000000u;

    std::uint32_t rng = 0x12345678u;
    const auto replacement_frame = run_first_turn_frame(*runtime, rng);
    EXPECT_FALSE(replacement.active_record_installed);
    EXPECT_EQ(
        runtime->visual.active_record.task_sequence,
        std::optional<int>{old.sequence});
    EXPECT_FALSE(old.active_record_replacement_pending);
    EXPECT_GE(
        count_step(
            replacement_frame.events,
            BattleFrameWorkerStepKind::VisualActiveRecordReplace),
        1);

    old.active_record_state_fa = true;
    const auto cleanup_frame = run_first_turn_frame(*runtime, rng);
    EXPECT_FALSE(old.complete);
    EXPECT_FALSE(old.mode11_gate_cleared);
    EXPECT_NE(actor->instruction_flags_0xf0 & 0x02000000u, 0u);
    EXPECT_GE(
        count_step(
            cleanup_frame.events,
            BattleFrameWorkerStepKind::VisualChildCleanup),
        1);
}

} // namespace
