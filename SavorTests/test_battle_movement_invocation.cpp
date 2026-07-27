#include "../SavorPredict/BattleFrameSchedulerModel.h"
#include "../SavorPredict/BattleMovementInvocationModel.h"
#include "../SavorPredict/BattleSourceModel.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace savor::predict;

const BattleSourceSnapshot& first_battle_source_snapshot() {
    static const auto source = load_battle_source_bundle(
        "first-battle-soldiers-us-final");
    if (!source.ok) {
        throw std::runtime_error(
            source.errors.empty()
                ? "first-battle source bundle failed to load"
                : source.errors.front());
    }
    return source.snapshot;
}

std::vector<BattleMovementInvocationSlotState> invocation_slots() {
    return {
        {.slot = 0, .present = true, .alive = true, .is_player = true,
         .status_flags = 0, .movement_flags = 0x0fc7},
        {.slot = 1, .present = true, .alive = true, .is_player = true,
         .status_flags = 0, .movement_flags = 0x0ff7},
        {.slot = 4, .present = true, .alive = true, .is_player = false,
         .status_flags = 0, .movement_flags = 0x0fc7},
        {.slot = 5, .present = true, .alive = true, .is_player = false,
         .status_flags = 0, .movement_flags = 0x0fc7},
    };
}

template <typename Input>
void add_common_runtime_state(Input& input) {
    input.slots = invocation_slots();
    for (const int slot : {0, 1, 4, 5}) {
        input.packed_thread_order.push_back({.slot = slot, .active = true});
        input.prior_controller_states.push_back({
            .slot = slot,
            .state = BattleMovementControllerState::Idle,
        });
    }
}

BattleMovementPassiveDispatchInput passive_input(int actor, int target) {
    BattleMovementPassiveDispatchInput input;
    input.action_ordinal = 2;
    input.actor_slot = actor;
    input.final_target_slot = target;
    input.action_kind = BattleMovementActionKind::BasicAttack;
    input.relation_scope = BattleMovementRelationScope::SingleTarget;
    input.turn_type = BattleMovementTurnType::Normal;
    add_common_runtime_state(input);
    return input;
}

const BattleMovementInvocationDecision* decision_for_slot(
    const BattleMovementInvocationPlan& plan,
    int slot) {
    const auto found = std::find_if(
        plan.decisions.begin(), plan.decisions.end(),
        [slot](const BattleMovementInvocationDecision& decision) {
            return decision.slot == slot;
        });
    return found == plan.decisions.end() ? nullptr : &*found;
}

std::vector<MovementSlotState> frame_slots() {
    std::vector<MovementSlotState> slots;
    for (const int slot : {0, 1, 4, 5}) {
        MovementSlotState state;
        state.slot = slot;
        state.present = true;
        state.is_player = slot < 4;
        state.alive = true;
        state.movement_flags = slot == 1 ? 0x0ff7 : 0x0fc7;
        state.motion_base_speed = state.is_player ? 2.55f : 2.25f;
        state.motion_alt_speed = state.is_player ? 0.45f : 0.30f;
        state.motion_speeds_known = true;
        state.motion_turn_speed = 22.0f;
        state.motion_turn_speed_known = true;
        const auto placement = battle_source_placement_for_slot(
            first_battle_source_snapshot(), slot);
        if (placement.has_value()) {
            state.start_position = BattleStartPosition{
                .slot = placement->slot,
                .present = true,
                .is_player = placement->is_player,
                .combatant_id = placement->combatant_id,
                .combatant_name = placement->combatant_name,
                .grid_x = placement->grid_x,
                .grid_z = placement->grid_z,
            };
        }
        slots.push_back(state);
    }
    return slots;
}

std::optional<BattleFrameRuntime> initialize_frame_runtime(
    const std::vector<MovementSlotState>& slots) {
    auto runtime = initialize_first_battle_frame_runtime(
        0,
        slots,
        first_battle_source_snapshot().terrain_source_9x9);
    if (!runtime.has_value()) {
        return std::nullopt;
    }
    for (auto& combatant : runtime->state.combatants) {
        const auto created = create_battle_frame_thread(
            runtime->thread_list,
            BattleFrameThreadCreateRequest{
                .kind = BattleFrameThreadNodeKind::CombatantInstruction,
                .owner_slot = combatant.slot,
                .callback = BattleFrameThreadCallbackIdentity::CombatantInstruction,
                .active = true,
                .semantic_source_id = "test.std_resource.publication",
                .provenance = "invocation-runtime fixture",
            });
        if (created.status != BattleFrameThreadMutationStatus::Applied) {
            return std::nullopt;
        }
        combatant.selected_action_row_index = 0;
        combatant.selected_action_row_flags = 0x01000000u;
        combatant.selected_action_row_known = true;
        runtime->visual.persistent_instruction_callbacks[
            static_cast<std::size_t>(combatant.slot)].thread_state_0x19 = 1;
        runtime->visual.std_row_producers[
            static_cast<std::size_t>(combatant.slot)].thread_state_0x19 = 1;
    }
    return runtime;
}

bool install_low_level_invocation_callback(
    BattleFrameRuntime& runtime,
    int slot) {
    auto* combatant = find_frame_combatant(runtime.state, slot);
    if (combatant == nullptr) {
        return false;
    }
    combatant->selected_action_row_index = 0;
    combatant->selected_action_row_action_id = 1;
    combatant->selected_action_row_callback_index = 8;
    combatant->selected_action_row_callback_ordinal = 0;
    combatant->selected_action_row_duration_bits = 0x40a00000u;
    combatant->selected_action_row_duration_known = true;
    combatant->selected_action_row_known = true;
    auto& callback = runtime.visual.persistent_instruction_callbacks[
        static_cast<std::size_t>(slot)];
    callback.installed = true;
    callback.thread_state_0x19 = 1;
    callback.slot = slot;
    callback.publication_revision = 1;
    callback.instruction_state_revision = combatant->visual_instruction_revision;
    callback.callback_index = 8;
    callback.callback_family =
        ActionMotionPersistentCallbackFamily::ActionMotionBasic_8001B1B0;
    callback.callback_state = 15;
    callback.current_instruction_row = CombatantStdActionRow{
        .index = combatant->selected_action_row_index,
        .action_id = combatant->selected_action_row_action_id,
        .callback_index = combatant->selected_action_row_callback_index,
        .callback_ordinal = combatant->selected_action_row_callback_ordinal,
        .transition_gate_divisor_bits =
            combatant->selected_action_row_duration_bits,
    };
    return true;
}

BattleFrameActionScheduleResult schedule_fallback_action(
    BattleFrameRuntime& runtime,
    int actor,
    int target,
    int ordinal = -1) {
    return schedule_first_turn_actor_action(
        runtime,
        BattleFrameScheduleActionInput{
            .action_ordinal = ordinal,
            .actor_slot = actor,
            .target_slot = target,
            .enemy_owned = actor >= 4,
            .combatant_command_parameter = 1,
            .initial_instruction_parameter = 1,
            .final_instruction_parameter = 1,
            .execution_route = BasicAttackExecutionRoute::FallbackRanged,
            .selected_worker = actor >= 4
                ? MovementSelectedWorker::EnemyFallbackAttack_80087844
                : MovementSelectedWorker::PcFallbackAttack_80085ce0,
            .action_kind = BattleMovementActionKind::BasicAttack,
            .relation_scope = BattleMovementRelationScope::SingleTarget,
            .turn_type = BattleMovementTurnType::Normal,
        });
}

TEST(SavorPredictBattleMovementInvocationModel, ActiveInvocationIsSeparateFromPassiveDispatch) {
    BattleMovementActiveInvocationInput input;
    input.action_ordinal = 7;
    input.actor_slot = 0;
    input.final_target_slot = 4;
    input.selected_active_worker = MovementSelectedWorker::PcDirectAttack_80086308;
    add_common_runtime_state(input);

    const auto decision = model_active_movement_invocation(input);

    EXPECT_TRUE(decision.should_invoke);
    EXPECT_EQ(decision.slot, 0);
    EXPECT_EQ(decision.semantic_target_slot, 4);
    EXPECT_EQ(decision.controller_family, BattleMovementControllerFamily::ActivePcDirect);
    EXPECT_EQ(decision.thread_order_index, 0);
    EXPECT_EQ(decision.relation_route, BattleMovementRelationRoute::Unknown);
}

TEST(SavorPredictBattleMovementInvocationModel, MatchesObserved147896DispatchVectors) {
    auto action0 = passive_input(1, 4);
    const auto plan0 = model_passive_movement_dispatch(action0);
    ASSERT_EQ(plan0.decisions.size(), 3u);
    ASSERT_NE(decision_for_slot(plan0, 0), nullptr);
    ASSERT_NE(decision_for_slot(plan0, 4), nullptr);
    ASSERT_NE(decision_for_slot(plan0, 5), nullptr);
    EXPECT_EQ(decision_for_slot(plan0, 0)->controller_family,
              BattleMovementControllerFamily::AmbientPursuit);
    EXPECT_EQ(decision_for_slot(plan0, 4)->controller_family,
              BattleMovementControllerFamily::AffectedTargetReaction);
    EXPECT_EQ(decision_for_slot(plan0, 5)->controller_family,
              BattleMovementControllerFamily::AmbientPursuit);

    auto action1 = passive_input(0, 4);
    const auto plan1 = model_passive_movement_dispatch(action1);
    EXPECT_EQ(decision_for_slot(plan1, 1)->controller_family,
              BattleMovementControllerFamily::AmbientFormation);
    EXPECT_EQ(decision_for_slot(plan1, 4)->controller_family,
              BattleMovementControllerFamily::AffectedTargetReaction);
    EXPECT_EQ(decision_for_slot(plan1, 5)->controller_family,
              BattleMovementControllerFamily::AmbientPursuit);

    auto action3 = passive_input(5, 1);
    action3.slots[2].alive = false;
    const auto plan3 = model_passive_movement_dispatch(action3);
    ASSERT_EQ(plan3.decisions.size(), 2u);
    EXPECT_EQ(decision_for_slot(plan3, 0)->controller_family,
              BattleMovementControllerFamily::AmbientPursuit);
    EXPECT_EQ(decision_for_slot(plan3, 1)->controller_family,
              BattleMovementControllerFamily::AffectedTargetReaction);
}

TEST(SavorPredictBattleMovementInvocationModel, Route0TruthTableSelectsStaticFamilies) {
    struct Vector {
        std::uint32_t status;
        std::uint16_t movement;
        bool player;
        BattleMovementTurnType turn;
        BattleMovementControllerFamily expected;
    };
    const std::vector<Vector> vectors{
        {0x800, 0x0fc7, true, BattleMovementTurnType::Normal,
         BattleMovementControllerFamily::StatusReactionD960},
        {0x100, 0x0fc7, true, BattleMovementTurnType::Normal,
         BattleMovementControllerFamily::StatusReactionD610},
        {0, 0x0fc7, true, BattleMovementTurnType::BackAttack,
         BattleMovementControllerFamily::AmbientIdle},
        {0, 0x0fc7, false, BattleMovementTurnType::Advantage,
         BattleMovementControllerFamily::AmbientIdle},
        {0, 0x0000, true, BattleMovementTurnType::Normal,
         BattleMovementControllerFamily::AmbientIdle},
        {0, 0x0003, true, BattleMovementTurnType::Normal,
         BattleMovementControllerFamily::AmbientPursuit},
        {0, 0x0013, true, BattleMovementTurnType::Normal,
         BattleMovementControllerFamily::AmbientFormation},
        {0, 0x0013, false, BattleMovementTurnType::Normal,
         BattleMovementControllerFamily::AmbientIdle},
        {0, 0x0023, true, BattleMovementTurnType::Normal,
         BattleMovementControllerFamily::AmbientIdle},
    };

    for (const auto& vector : vectors) {
        auto input = passive_input(0, 4);
        auto& slot = input.slots[1];
        slot.status_flags = vector.status;
        slot.movement_flags = vector.movement;
        slot.is_player = vector.player;
        input.turn_type = vector.turn;
        const auto plan = model_passive_movement_dispatch(input);
        ASSERT_NE(decision_for_slot(plan, 1), nullptr);
        EXPECT_EQ(decision_for_slot(plan, 1)->controller_family, vector.expected);
    }
}

TEST(SavorPredictBattleMovementInvocationRuntime, AmbientIdleRelaysWithoutMovementWorker) {
    auto slots = frame_slots();
    auto idle_slot = std::find_if(
        slots.begin(),
        slots.end(),
        [](const MovementSlotState& slot) { return slot.slot == 5; });
    ASSERT_NE(idle_slot, slots.end());
    idle_slot->status_flags = 0x00000001u;

    auto runtime = initialize_frame_runtime(slots);
    ASSERT_TRUE(runtime.has_value());
    const auto* before = find_frame_combatant(runtime->state, 5);
    ASSERT_NE(before, nullptr);
    const auto grid_before = before->grid_position;
    const auto holder_before = before->pos_holder;
    const auto current_before = before->combatant_cur_pos_0x1c;
    ASSERT_TRUE(schedule_fallback_action(*runtime, 0, 4, 7).scheduled);

    std::uint32_t rng = 0x13572468u;
    std::vector<std::string> callbacks;
    for (int frame = 0; frame < 32; ++frame) {
        const auto step = run_first_turn_frame(*runtime, rng);
        ASSERT_TRUE(step.ok);
        for (const auto& event : step.events) {
            if (event.slot == 5
                && (event.callback.find("8008C98C") != std::string::npos
                    || event.callback.find("8008C6BC") != std::string::npos)) {
                callbacks.push_back(event.callback);
            }
        }
        if (runtime->passive_participants[5].phase
                == BattleFramePassiveParticipantPhase::CompletionDeferred
            && runtime->movement_controllers[5].actual_callback_pc
                == 0x8008C6BCu
            && runtime->movement_controllers[5].thread_state_0x19 == 2) {
            break;
        }
    }

    ASSERT_EQ(
        runtime->passive_participants[5].phase,
        BattleFramePassiveParticipantPhase::CompletionDeferred);
    EXPECT_EQ(runtime->passive_participants[5].worker_index, -1);
    EXPECT_EQ(runtime->movement_controllers[5].actual_callback_pc, 0x8008C6BCu);
    EXPECT_EQ(runtime->movement_controllers[5].deferred_callback_pc, 0u);
    EXPECT_EQ(runtime->movement_controllers[5].thread_state_0x19, 2);
    EXPECT_TRUE(std::none_of(
        runtime->workers.begin(),
        runtime->workers.end(),
        [](const BattleFrameWorker& worker) {
            return worker.slot == 5
                && worker.controller_family
                    == BattleMovementControllerFamily::AmbientIdle;
        }));
    EXPECT_NE(std::find(
        callbacks.begin(),
        callbacks.end(),
        "FUN_8008C98C_state0_publish_8008C6BC"), callbacks.end());
    EXPECT_NE(std::find(
        callbacks.begin(),
        callbacks.end(),
        "FUN_8008C98C_state1_to_8008C6BC"), callbacks.end());
    EXPECT_NE(std::find(
        callbacks.begin(),
        callbacks.end(),
        "FUN_8008C6BC_state0_to_state2"), callbacks.end());

    const auto* after = find_frame_combatant(runtime->state, 5);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->grid_position.grid_x, grid_before.grid_x);
    EXPECT_EQ(after->grid_position.grid_z, grid_before.grid_z);
    EXPECT_FLOAT_EQ(after->pos_holder.x, holder_before.x);
    EXPECT_FLOAT_EQ(after->pos_holder.y, holder_before.y);
    EXPECT_FLOAT_EQ(after->pos_holder.z, holder_before.z);
    EXPECT_FLOAT_EQ(after->combatant_cur_pos_0x1c.x, current_before.x);
    EXPECT_FLOAT_EQ(after->combatant_cur_pos_0x1c.y, current_before.y);
    EXPECT_FLOAT_EQ(after->combatant_cur_pos_0x1c.z, current_before.z);
    EXPECT_EQ(rng, 0x13572468u);
}

TEST(SavorPredictBattleMovementInvocationModel, UnsupportedScopeRemainsDrainable) {
    auto input = passive_input(0, 4);
    input.relation_scope = BattleMovementRelationScope::Unsupported;
    const auto plan = model_passive_movement_dispatch(input);
    ASSERT_FALSE(plan.decisions.empty());
    for (const auto& decision : plan.decisions) {
        EXPECT_TRUE(decision.should_invoke);
        EXPECT_EQ(decision.worker_kind,
                  BattleMovementInvocationWorkerKind::PassiveController);
        EXPECT_EQ(decision.controller_family,
                  BattleMovementControllerFamily::Unsupported);
        EXPECT_EQ(decision.status, BattleMovementInvocationStatus::Unsupported);
    }
}

TEST(SavorPredictBattleMovementInvocationRuntime, PreservesPersistentRelayAndPublishesLatestDeferredDispatch) {
    auto runtime = initialize_frame_runtime(frame_slots());
    ASSERT_TRUE(runtime.has_value());
    const auto scheduled = schedule_fallback_action(*runtime, 0, 4, 7);
    ASSERT_TRUE(scheduled.scheduled);
    ASSERT_TRUE(runtime->active_action.has_value());
    EXPECT_EQ(runtime->active_action->passive_completion_mask, 0);
    EXPECT_EQ(runtime->active_action->phase,
              BattleFrameActionPhase::Scheduled);
    EXPECT_EQ(runtime->active_action->active_controller_phase,
              BattleFrameActiveControllerPhase::SelectorPending);
    ASSERT_EQ(runtime->workers.size(), 1u);
    EXPECT_TRUE(runtime->workers.front().activation_pending);
    EXPECT_EQ(runtime->passive_participants[1].phase,
              BattleFramePassiveParticipantPhase::InitialRelayPending);
    EXPECT_EQ(runtime->movement_controllers[1].actual_callback_pc,
              0x800804B8u);
    EXPECT_EQ(runtime->movement_controllers[1].deferred_callback_pc,
              0x800804B8u);
    EXPECT_EQ(runtime->movement_controllers[1].thread_state_0x19, 0);
    EXPECT_EQ(runtime->movement_controllers[0].actual_callback_pc,
              0x80086C68u);
    EXPECT_FALSE(runtime->passive_participants[1].completion_bit_set);

    std::uint32_t rng = 0x12345678u;
    const auto relay0 = run_first_turn_frame(*runtime, rng);
    EXPECT_EQ(runtime->active_action->phase, BattleFrameActionPhase::Active);
    EXPECT_EQ(runtime->movement_controllers[0].actual_callback_pc, 0x80086C68u);
    EXPECT_EQ(runtime->movement_controllers[0].thread_state_0x19, 3);
    EXPECT_EQ(runtime->movement_controllers[1].actual_callback_pc, 0x800801A8u);
    EXPECT_EQ(runtime->movement_controllers[1].thread_state_0x19, 0);
    EXPECT_EQ(runtime->active_action->passive_completion_mask, 0);
    EXPECT_NE(std::find_if(relay0.events.begin(), relay0.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind == BattleFrameWorkerStepKind::PassiveRelayPublish
                && event.callback == "FUN_8008E2B0_setup_relay";
        }), relay0.events.end());

    const auto selected = run_first_turn_frame(*runtime, rng);
    EXPECT_EQ(runtime->movement_controllers[1].thread_state_0x19, 1);
    EXPECT_EQ(runtime->active_action->passive_completion_mask, 0);
    EXPECT_EQ(runtime->active_action->phase,
              BattleFrameActionPhase::HandoffPending);
    EXPECT_EQ(runtime->active_action->active_controller_phase,
              BattleFrameActiveControllerPhase::SelectedWorkerPending);
    EXPECT_EQ(runtime->active_action->passive_completion_mask, 0);
    ASSERT_EQ(runtime->workers.size(), 1u);
    EXPECT_TRUE(runtime->workers.front().activation_pending);
    EXPECT_NE(std::find_if(selected.events.begin(), selected.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind == BattleFrameWorkerStepKind::ActiveWorkerPublish;
        }), selected.events.end());

    const auto dispatched = run_first_turn_frame(*runtime, rng);
    EXPECT_EQ(rng, 0x12345678u);
    EXPECT_EQ(runtime->active_action->passive_completion_mask, 0x32);
    EXPECT_EQ(runtime->active_action->phase,
              BattleFrameActionPhase::PassiveDispatched);
    EXPECT_TRUE(runtime->passive_participants[1].completion_bit_set);
    EXPECT_EQ(runtime->movement_controllers[1].actual_callback_pc, 0x8008DEECu);
    EXPECT_EQ(runtime->movement_controllers[1].deferred_callback_pc, 0u);
    EXPECT_EQ(runtime->movement_controllers[1].thread_state_0x19, 0);
    EXPECT_EQ(runtime->passive_participants[1].phase,
              BattleFramePassiveParticipantPhase::Dispatching);
    EXPECT_NE(std::find_if(dispatched.events.begin(), dispatched.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                    == BattleFrameWorkerStepKind::PassiveDispatchPublish
                && event.callback == "FUN_8008E338_dispatch_relay"
                && event.passive_completion_mask_before == 0
                && event.passive_completion_mask_after == 0x32;
        }), dispatched.events.end());
}

TEST(SavorPredictBattleMovementInvocationRuntime, RelayPublishersPreservePersistentActualCallbackAndState) {
    auto runtime = initialize_frame_runtime(frame_slots());
    ASSERT_TRUE(runtime.has_value());
    auto& controller = runtime->movement_controllers[1];
    controller.actual_callback_pc = 0x8008C7B0u;
    controller.deferred_callback_pc = 0x8008D960u;
    controller.thread_state_0x19 = 1;
    const auto revision_before = controller.revision;

    const auto scheduled = schedule_fallback_action(*runtime, 0, 4, 7);
    ASSERT_TRUE(scheduled.scheduled);
    EXPECT_EQ(controller.actual_callback_pc, 0x8008C7B0u);
    EXPECT_EQ(controller.deferred_callback_pc, 0x800804B8u);
    EXPECT_EQ(controller.thread_state_0x19, 1);
    EXPECT_GT(controller.revision, revision_before);
    const auto setup_revision = controller.revision;

    std::uint32_t rng = 0x12345678u;
    for (int frame = 0;
         frame < 8
            && runtime->active_action->passive_completion_mask == 0;
         ++frame) {
        ASSERT_TRUE(run_first_turn_frame(*runtime, rng).ok);
    }

    EXPECT_NE(runtime->active_action->passive_completion_mask, 0);
    EXPECT_EQ(controller.actual_callback_pc, 0x8008C7B0u);
    EXPECT_EQ(controller.deferred_callback_pc, 0x8008DEECu);
    EXPECT_EQ(controller.thread_state_0x19, 1);
    EXPECT_GT(controller.revision, setup_revision);
    EXPECT_EQ(rng, 0x12345678u);
}

TEST(SavorPredictBattleMovementInvocationRuntime, PublishesObservedMasksDuringActionSetup) {
    struct Vector { int actor; int target; bool remove_slot4; std::uint16_t mask; };
    for (const auto& vector : std::vector<Vector>{{1, 4, false, 0x31},
                                                  {0, 4, false, 0x32},
                                                  {5, 1, true, 0x03}}) {
        auto slots = frame_slots();
        if (vector.remove_slot4) {
            slots[2].alive = false;
        }
        auto runtime = initialize_frame_runtime(slots);
        ASSERT_TRUE(runtime.has_value());
        ASSERT_TRUE(schedule_fallback_action(*runtime, vector.actor, vector.target).scheduled);
        ASSERT_TRUE(runtime->active_action.has_value());
        EXPECT_EQ(runtime->active_action->passive_completion_mask, 0);
        std::uint32_t rng = 0x10203040u;
        for (int frame = 0;
             frame < 8
                && runtime->active_action->passive_completion_mask == 0;
             ++frame) {
            ASSERT_TRUE(run_first_turn_frame(*runtime, rng).ok);
        }
        EXPECT_EQ(runtime->active_action->passive_completion_mask, vector.mask);
        EXPECT_EQ(runtime->active_action->completion_turn_phase, 4);
        EXPECT_EQ(runtime->active_action->phase,
                  BattleFrameActionPhase::PassiveDispatched);
    }
}

TEST(SavorPredictBattleMovementInvocationRuntime, PassiveActivationResetsPriorQueuedControllerState) {
    auto runtime = initialize_frame_runtime(frame_slots());
    ASSERT_TRUE(runtime.has_value());
    auto* passive = find_frame_combatant(runtime->state, 5);
    ASSERT_NE(passive, nullptr);
    passive->queued_controller_state = 4;
    ASSERT_TRUE(schedule_fallback_action(*runtime, 0, 4).scheduled);

    std::uint32_t rng = 0x10293847u;
    bool built_path = false;
    for (int frame = 0; frame < 96 && !built_path; ++frame) {
        const auto step = run_first_turn_frame(*runtime, rng);
        ASSERT_TRUE(step.ok);
        built_path = std::any_of(
            step.events.begin(),
            step.events.end(),
            [](const BattleFrameStepEvent& event) {
                return event.slot == 5
                    && event.step_kind == BattleFrameWorkerStepKind::PathBuild
                    && event.detail.find("queued_controller_state=0")
                        != std::string::npos;
            });
    }

    ASSERT_TRUE(built_path);
    EXPECT_EQ(rng, 0x10293847u);
}

TEST(SavorPredictBattleMovementInvocationRuntime, QueuedStdActionTransitionSeparatesWorksheetModeFromMotionMode) {
    auto runtime = initialize_frame_runtime(frame_slots());
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(install_low_level_invocation_callback(*runtime, 0));
    const auto scheduled = schedule_first_turn_actor_action(
        *runtime,
        BattleFrameScheduleActionInput{
            .actor_slot = 0,
            .target_slot = 4,
            .enemy_owned = false,
            .selected_worker = MovementSelectedWorker::PcDirectAttack_80086308,
            .action_kind = BattleMovementActionKind::BasicAttack,
            .relation_scope = BattleMovementRelationScope::SingleTarget,
            .turn_type = BattleMovementTurnType::Normal,
        });
    ASSERT_TRUE(scheduled.scheduled);

    std::uint32_t rng = 0x31415926u;
    bool saw_instruction_state = false;
    std::string instruction_state_detail;
    for (int frame = 0; frame < 64 && !saw_instruction_state; ++frame) {
        const auto step = run_first_turn_frame(*runtime, rng);
        ASSERT_TRUE(step.ok);
        const auto publication = std::find_if(
            step.events.begin(), step.events.end(),
            [](const BattleFrameStepEvent& event) {
                return event.step_kind
                        == BattleFrameWorkerStepKind::VisualInstructionDecision
                    && event.slot == 0
                    && event.detail.find("installs_epoch=0")
                        != std::string::npos;
            });
        saw_instruction_state = publication != step.events.end();
        if (saw_instruction_state) {
            instruction_state_detail = publication->detail;
        }
    }

    ASSERT_TRUE(saw_instruction_state);
    EXPECT_NE(instruction_state_detail.find("action_key=6"), std::string::npos);
    ASSERT_TRUE(runtime->active_action.has_value());
    EXPECT_EQ(runtime->active_action->phase,
              BattleFrameActionPhase::PassiveDispatched);
    const auto& active = runtime->workers[static_cast<std::size_t>(
        runtime->active_action->active_worker_index)];
    EXPECT_FALSE(active.complete);
    EXPECT_EQ(active.completed_motion_legs, 0);
    EXPECT_EQ(rng, 0x31415926u);
    const auto* combatant = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(combatant, nullptr);
    EXPECT_EQ(combatant->visual_instruction_mode_0x6, 6);
    EXPECT_EQ(runtime->combatant_instructions[0].action_mode,
              BattleFrameActionMode::ActiveApproach);

    for (int frame = 0;
         frame < 8 && runtime->visual.timelines[0].epoch == 0;
         ++frame) {
        ASSERT_TRUE(run_first_turn_frame(*runtime, rng).ok);
    }
    ASSERT_GT(runtime->visual.timelines[0].epoch, 0u);
    EXPECT_EQ(
        runtime->visual.timelines[0].instruction.runtime_instruction_mode,
        std::optional<std::int16_t>{6});
    const auto motion_epoch = runtime->visual.timelines[0].epoch;
    const auto instruction_revision = combatant->visual_instruction_revision;

    ASSERT_TRUE(notify_first_turn_action_resolution(
        *runtime,
        BattleFrameActionResolution{
            .action_ordinal = runtime->active_action->action_ordinal,
            .attack_result = 1,
            .attack_landed = true,
            .target_dead = false,
        }));
    EXPECT_EQ(rng, 0x31415926u);
    EXPECT_EQ(runtime->combatant_instructions[0].action_mode,
              BattleFrameActionMode::ActiveApproach);
    EXPECT_EQ(combatant->visual_instruction_mode_0x6, 6);
    EXPECT_EQ(combatant->visual_instruction_revision, instruction_revision);
    EXPECT_EQ(runtime->visual.timelines[0].epoch, motion_epoch);
    EXPECT_TRUE(runtime->active_action->queued_state_transition_pending);

    bool installed_queued_mode = false;
    for (int frame = 0; frame < 8 && !installed_queued_mode; ++frame) {
        const auto step = run_first_turn_frame(*runtime, rng);
        ASSERT_TRUE(step.ok);
        installed_queued_mode = std::any_of(
            step.events.begin(), step.events.end(),
            [](const BattleFrameStepEvent& event) {
                    return event.step_kind
                        == BattleFrameWorkerStepKind::VisualInstructionInstall
                    && event.slot == 0
                    && event.detail.find("action_key=4") != std::string::npos;
            });
    }
    EXPECT_TRUE(installed_queued_mode);
    EXPECT_GT(runtime->visual.timelines[0].epoch, motion_epoch);
    EXPECT_EQ(
        runtime->visual.timelines[0].instruction.runtime_instruction_mode,
        std::optional<std::int16_t>{4});
    EXPECT_EQ(combatant->selected_action_row_action_id, 4);
    EXPECT_TRUE(runtime->active_action->queued_state_transition_published);
    EXPECT_EQ(rng, 0x31415926u);

    EXPECT_EQ(runtime->active_action->passive_completion_mask, 0x32);
    EXPECT_EQ(runtime->active_action->completion_turn_phase, 4);
}

TEST(SavorPredictBattleMovementInvocationRuntime, NewInstructionRevisionRetiresPriorPlaybackBeforeCallbackVisit) {
    auto runtime = initialize_frame_runtime(frame_slots());
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(schedule_fallback_action(*runtime, 1, 4, 7).scheduled);
    auto* combatant = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(combatant, nullptr);
    combatant->visual_instruction_revision = 1;
    combatant->visual_instruction_mode_0x6 = 2;
    ASSERT_TRUE(install_low_level_invocation_callback(*runtime, 0));

    auto& callback =
        runtime->visual.persistent_instruction_callbacks[0];
    callback.action_ordinal = 7;
    callback.instruction_state_revision = 1;
    callback.current_motion_resource_present = true;
    callback.current_motion_id = 10;
    runtime->visual.resources[0] = CombatantVisualResource{
        .binding = {.slot = 0, .resource_stem = "test"},
        .action_rows = {
            CombatantStdActionRow{
                .index = 0,
                .action_id = 2,
                .row_type = 1,
                .callback_index = 8,
                .callback_ordinal = 10,
                .transition_gate_divisor_bits = 0x40A00000u,
            },
            CombatantStdActionRow{
                .index = 1,
                .action_id = -1,
                .row_type = 3,
            },
        },
        .includes_sentinel = true,
        .provenance = "revision-ownership fixture",
    };
    const auto installed = install_action_motion_playback({
        .action_ordinal = 7,
        .slot = 0,
        .instruction_state_revision = 1,
        .selected_action_row_index = 0,
        .selected_action_row_duration_bits = 0x40A00000u,
        .continuation =
            ActionMotionPlaybackContinuation::State7LoadLookedUpTo14,
        .provenance = "prior revision playback",
    });
    ASSERT_TRUE(installed.installed);
    runtime->visual.action_motion_playbacks[0] = installed.runtime;

    ASSERT_TRUE(stage_battle_frame_visual_instruction_state(
        *runtime,
        7,
        CombatantVisualInstructionSnapshot{
            .slot = 0,
            .runtime_instruction_mode = 2,
            .selected_std_action_key = 2,
            .subtype = 0,
            .target_slot = 4,
            .knowledge = CombatantVisualInstructionKnowledge::Known,
            .provenance = "new instruction revision",
        }));
    ASSERT_EQ(combatant->visual_instruction_revision, 2u);

    std::uint32_t rng = 0x89ABCDEFu;
    const auto frame = run_first_turn_frame(*runtime, rng);
    ASSERT_TRUE(frame.ok);

    EXPECT_EQ(
        runtime->visual.action_motion_playbacks[0].phase,
        ActionMotionPlaybackPhase::Inactive);
    EXPECT_EQ(callback.instruction_state_revision, 2u);
    EXPECT_EQ(callback.callback_state, 3);
    EXPECT_NE(std::find_if(
        frame.events.begin(),
        frame.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                    == BattleFrameWorkerStepKind::ActionMotionPublicationRelease
                && event.callback == "InstructionRevisionReplacement"
                && event.detail.find("playback_owner_revision=1")
                    != std::string::npos
                && event.detail.find("current_instruction_revision=2")
                    != std::string::npos;
        }), frame.events.end());
    EXPECT_EQ(rng, 0x89ABCDEFu);
}

TEST(SavorPredictBattleMovementInvocationRuntime, QueuedStdActionTransitionPublishesCriticalAndFallbackModes) {
    const auto run_case = [](
                              MovementSelectedWorker worker,
                              std::int16_t parameter,
                              BasicAttackExecutionRoute route,
                              int attack_result,
                              int expected_state,
                              int expected_mode) {
        auto runtime = initialize_frame_runtime(frame_slots());
        ASSERT_TRUE(runtime.has_value());
        ASSERT_TRUE(install_low_level_invocation_callback(*runtime, 0));
        const auto scheduled = schedule_first_turn_actor_action(
            *runtime,
            BattleFrameScheduleActionInput{
                .actor_slot = 0,
                .target_slot = 4,
                .enemy_owned = false,
                .combatant_command_parameter = parameter,
                .initial_instruction_parameter = parameter,
                .final_instruction_parameter = parameter,
                .execution_route = route,
                .selected_worker = worker,
                .action_kind = BattleMovementActionKind::BasicAttack,
                .relation_scope = BattleMovementRelationScope::SingleTarget,
                .turn_type = BattleMovementTurnType::Normal,
            });
        ASSERT_TRUE(scheduled.scheduled);

        std::uint32_t rng = 0x51425364u;
        if (route == BasicAttackExecutionRoute::DirectMelee) {
            for (int frame = 0;
                 frame < 72 && runtime->visual.timelines[0].epoch == 0;
                 ++frame) {
                ASSERT_TRUE(run_first_turn_frame(*runtime, rng).ok);
            }
            ASSERT_GT(runtime->visual.timelines[0].epoch, 0u);
        }
        const auto setup_epoch = runtime->visual.timelines[0].epoch;

        ASSERT_TRUE(notify_first_turn_action_resolution(
            *runtime,
            BattleFrameActionResolution{
                .action_ordinal = scheduled.action_ordinal,
                .attack_result = attack_result,
                .attack_landed = attack_result != 0,
                .target_dead = false,
            }));
        ASSERT_TRUE(runtime->active_action.has_value());
        ASSERT_TRUE(runtime->active_action->queued_state_transition.has_value());
        ASSERT_TRUE(runtime->active_action->queued_state_transition->queued_state.has_value());
        EXPECT_EQ(
            static_cast<int>(*runtime->active_action->queued_state_transition->queued_state),
            expected_state);

        bool installed = false;
        for (int frame = 0; frame < 8 && !installed; ++frame) {
            const auto step = run_first_turn_frame(*runtime, rng);
            ASSERT_TRUE(step.ok);
            installed = std::any_of(
                step.events.begin(), step.events.end(),
                [expected_mode](const BattleFrameStepEvent& event) {
                    return event.step_kind
                            == BattleFrameWorkerStepKind::VisualInstructionInstall
                        && event.slot == 0
                        && event.detail.find(
                            "action_key=" + std::to_string(expected_mode))
                            != std::string::npos;
                });
        }

        EXPECT_TRUE(installed);
        EXPECT_GT(runtime->visual.timelines[0].epoch, setup_epoch);
        EXPECT_TRUE(runtime->active_action->queued_state_transition_published);
        EXPECT_EQ(
            runtime->visual.timelines[0].instruction.runtime_instruction_mode,
            std::optional<std::int16_t>{static_cast<std::int16_t>(expected_mode)});
        EXPECT_EQ(rng, 0x51425364u);
    };

    run_case(
        MovementSelectedWorker::PcDirectAttack_80086308,
        0,
        BasicAttackExecutionRoute::DirectMelee,
        2,
        6,
        8);
    run_case(
        MovementSelectedWorker::PcFallbackAttack_80085ce0,
        1,
        BasicAttackExecutionRoute::FallbackRanged,
        1,
        7,
        5);
}

TEST(SavorPredictBattleMovementInvocationRuntime, RejectsSecondActionUntilMaskDrains) {
    auto runtime = initialize_frame_runtime(frame_slots());
    ASSERT_TRUE(runtime.has_value());
    const auto first = schedule_fallback_action(*runtime, 0, 4);
    ASSERT_TRUE(first.scheduled);
    const auto second = schedule_fallback_action(*runtime, 1, 4);
    EXPECT_FALSE(second.scheduled);
    EXPECT_EQ(second.action_ordinal, first.action_ordinal);
    EXPECT_EQ(second.status, BattleMovementInvocationStatus::Ambiguous);
    EXPECT_EQ(runtime->workers.size(), 1u);
}

TEST(SavorPredictBattleMovementInvocationRuntime, FallbackYieldsSlotToMechanicalAttackBoundary) {
    auto slots = frame_slots();
    for (auto& slot : slots) {
        if (slot.slot != 0 && slot.slot != 4) {
            slot.present = false;
        }
        slot.movement_flags = 0;
    }
    auto runtime = initialize_frame_runtime(slots);
    ASSERT_TRUE(runtime.has_value());
    const auto scheduled = schedule_fallback_action(*runtime, 0, 4);
    ASSERT_TRUE(scheduled.scheduled);

    std::uint32_t rng = 0x2468ace0u;
    ASSERT_TRUE(run_first_turn_until_idle(*runtime, rng, 64).ok);
    ASSERT_TRUE(runtime->active_action.has_value());
    const auto fallback_index = static_cast<std::size_t>(
        runtime->active_action->active_worker_index);
    const auto& fallback = runtime->workers[fallback_index];
    EXPECT_TRUE(fallback.waiting_for_action_resolution);
    EXPECT_EQ(fallback.worksheet_state_0x19, 0x0B);

    schedule_first_turn_mechanical_attack(*runtime, 0, 4);
    for (int frame = 0; frame < 8; ++frame) {
        ASSERT_TRUE(run_first_turn_frame(*runtime, rng).ok);
        const bool mechanical_pending = std::any_of(
            runtime->workers.begin(), runtime->workers.end(),
            [](const BattleFrameWorker& worker) {
                return worker.kind == BattleFrameWorkerKind::MechanicalAttack
                    && !worker.complete;
            });
        if (!mechanical_pending) {
            break;
        }
    }
    EXPECT_TRUE(runtime->workers[fallback_index].waiting_for_action_resolution);
    EXPECT_FALSE(runtime->workers[fallback_index].complete);

    ASSERT_TRUE(notify_first_turn_action_resolution(*runtime, {
        .action_ordinal = scheduled.action_ordinal,
        .attack_result = 1,
        .attack_landed = true,
        .target_dead = false,
    }));
    const auto resolved = run_first_turn_frame(*runtime, rng);
    ASSERT_TRUE(resolved.ok);
    EXPECT_FALSE(runtime->workers[fallback_index].waiting_for_action_resolution);
    EXPECT_TRUE(runtime->active_action->completion_override);
    EXPECT_EQ(runtime->workers[fallback_index].worksheet_state_0x19, 0x0F);
    EXPECT_EQ(rng, 0x2468ace0u);
}

TEST(SavorPredictBattleMovementInvocationRuntime, FormationUsesGeneratedGridWithoutActionTarget) {
    auto runtime = initialize_frame_runtime(frame_slots());
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(commit_movement_grid_8008178c(
        runtime->state, 0, {.grid_x = 2, .grid_z = 8}));
    ASSERT_TRUE(commit_movement_grid_8008178c(
        runtime->state, 1, {.grid_x = 5, .grid_z = 5}));
    ASSERT_TRUE(commit_movement_grid_8008178c(
        runtime->state, 4, {.grid_x = 5, .grid_z = 3}));
    ASSERT_TRUE(commit_movement_grid_8008178c(
        runtime->state, 5, {.grid_x = 8, .grid_z = 3}));
    ASSERT_EQ(runtime->state.active_grid[static_cast<std::size_t>(6 * 11 + 5)], 0);
    ASSERT_TRUE(schedule_fallback_action(*runtime, 0, 4).scheduled);
    std::uint32_t rng = 0x44556677u;
    const BattleFrameWorker* formation = nullptr;
    for (int frame = 0; frame < 64 && formation == nullptr; ++frame) {
        ASSERT_TRUE(run_first_turn_frame(*runtime, rng).ok);
        const auto found = std::find_if(
            runtime->workers.begin(), runtime->workers.end(),
            [](const BattleFrameWorker& worker) {
                return worker.controller_family
                    == BattleMovementControllerFamily::AmbientFormation;
            });
        if (found != runtime->workers.end()) {
            formation = &*found;
        }
    }
    ASSERT_NE(formation, nullptr);
    EXPECT_EQ(formation->slot, 1);
    EXPECT_EQ(formation->target_slot, -1);
    EXPECT_TRUE(formation->movement_path.available) << formation->detail;
    EXPECT_EQ(formation->destination_source,
              MovementCommitDestinationSource::GeneratedSingleSquare);
    EXPECT_EQ(formation->destination_grid.grid_x, 5);
    EXPECT_EQ(formation->destination_grid.grid_z, 6);
    EXPECT_EQ(rng, 0x44556677u);

    const BattleFrameStepEvent* setup = nullptr;
    BattleFrameRunResult setup_frame;
    for (int frame = 0; frame < 64 && setup == nullptr; ++frame) {
        setup_frame = run_first_turn_frame(*runtime, rng);
        ASSERT_TRUE(setup_frame.ok);
        const auto found = std::find_if(
            setup_frame.events.begin(), setup_frame.events.end(),
            [](const BattleFrameStepEvent& event) {
                return event.slot == 1
                    && event.controller_family
                        == BattleMovementControllerFamily::AmbientFormation
                    && event.step_kind
                        == BattleFrameWorkerStepKind::ActionMotionSetup_8001fabc;
            });
        if (found != setup_frame.events.end()) {
            setup = &*found;
        }
    }
    ASSERT_NE(setup, nullptr);
    EXPECT_EQ(setup->new_action_mode, BattleFrameActionMode::ActionMotionAltSpeed);
    EXPECT_FLOAT_EQ(setup->selected_motion_speed, 0.45f);
}

TEST(SavorPredictBattleMovementInvocationRuntime, PursuitRebuildObservesTargetMoveBeforeNextCallback) {
    auto slots = frame_slots();
    slots[0].movement_flags = 0;
    auto runtime = initialize_frame_runtime(slots);
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(commit_movement_grid_8008178c(
        runtime->state, 0, {.grid_x = 4, .grid_z = 5}));
    ASSERT_TRUE(commit_movement_grid_8008178c(
        runtime->state, 5, {.grid_x = 6, .grid_z = 3}));
    ASSERT_TRUE(schedule_fallback_action(*runtime, 1, 4).scheduled);

    std::uint32_t rng = 0x778899aau;
    int pursuit_index = -1;
    bool saw_first_commit = false;
    for (int frame = 0; frame < 96 && !saw_first_commit; ++frame) {
        const auto step = run_first_turn_frame(*runtime, rng);
        ASSERT_TRUE(step.ok);
        saw_first_commit = std::any_of(
            step.events.begin(), step.events.end(),
            [](const BattleFrameStepEvent& event) {
                return event.slot == 5
                    && event.controller_family
                        == BattleMovementControllerFamily::AmbientPursuit
                    && event.step_kind == BattleFrameWorkerStepKind::MovementCommit;
            });
        const auto found = std::find_if(
            runtime->workers.begin(), runtime->workers.end(),
            [](const BattleFrameWorker& worker) {
                return worker.slot == 5
                    && worker.controller_family
                        == BattleMovementControllerFamily::AmbientPursuit;
            });
        if (found != runtime->workers.end()) {
            pursuit_index = static_cast<int>(
                std::distance(runtime->workers.begin(), found));
        }
    }
    ASSERT_TRUE(saw_first_commit);
    ASSERT_GE(pursuit_index, 0);
    const auto& pursuit = runtime->workers[static_cast<std::size_t>(pursuit_index)];
    EXPECT_EQ(pursuit.target_slot, 0);
    EXPECT_TRUE(pursuit.movement_path.available);

    const auto pursuit_grid = find_frame_combatant(runtime->state, 5)->grid_position;
    ASSERT_TRUE(commit_movement_grid_8008178c(
        runtime->state,
        0,
        {.grid_x = pursuit_grid.grid_x - 1, .grid_z = pursuit_grid.grid_z}));
    const auto before = find_frame_combatant(runtime->state, 5)->grid_position;
    int later_commits = 0;
    bool saw_adjacent_rebuild = false;
    for (int frame = 0; frame < 96 && !saw_adjacent_rebuild; ++frame) {
        const auto step = run_first_turn_frame(*runtime, rng);
        ASSERT_TRUE(step.ok);
        for (const auto& event : step.events) {
            if (event.slot != 5
                || event.controller_family
                    != BattleMovementControllerFamily::AmbientPursuit) {
                continue;
            }
            if (event.step_kind == BattleFrameWorkerStepKind::MovementCommit) {
                ++later_commits;
            }
            saw_adjacent_rebuild = saw_adjacent_rebuild
                || event.detail.find("already adjacent") != std::string::npos;
        }
    }
    EXPECT_TRUE(saw_adjacent_rebuild);
    EXPECT_EQ(later_commits, 0);
    EXPECT_EQ(find_frame_combatant(runtime->state, 5)->grid_position.grid_x,
              before.grid_x);
    EXPECT_EQ(find_frame_combatant(runtime->state, 5)->grid_position.grid_z,
              before.grid_z);
}

TEST(SavorPredictBattleMovementInvocationRuntime, AdjacentPursuitEntersState17LifecycleInsteadOfCompleting) {
    auto runtime = initialize_frame_runtime(frame_slots());
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(commit_movement_grid_8008178c(
        runtime->state, 0, {.grid_x = 4, .grid_z = 5}));
    ASSERT_TRUE(commit_movement_grid_8008178c(
        runtime->state, 5, {.grid_x = 5, .grid_z = 5}));
    const auto scheduled = schedule_fallback_action(*runtime, 1, 4);
    ASSERT_TRUE(scheduled.scheduled);

    std::uint32_t rng = 0x778899aau;
    const BattleFrameStepEvent* publication = nullptr;
    BattleFrameRunResult observed;
    for (int frame = 0; frame < 96 && publication == nullptr; ++frame) {
        observed = run_first_turn_frame(*runtime, rng);
        ASSERT_TRUE(observed.ok);
        const auto found = std::find_if(
            observed.events.begin(),
            observed.events.end(),
            [](const BattleFrameStepEvent& event) {
                return event.slot == 0
                    && event.step_kind
                        == BattleFrameWorkerStepKind::PursuitCoordinationState2
                    && (event.pursuit_coordination_branch
                            == BattlePursuitCoordinationBranch::
                                PeerReadyPublication
                        || event.pursuit_coordination_branch
                            == BattlePursuitCoordinationBranch::
                                CoordinatedPublication);
            });
        if (found != observed.events.end()) {
            publication = &*found;
        }
    }

    ASSERT_NE(publication, nullptr);
    EXPECT_EQ(publication->pursuit_owner_state_0x50, 0x22);
    EXPECT_TRUE(
        publication->pursuit_peer_state_0x50 == 0x11
        || publication->pursuit_peer_state_0x50 == 0x12);
    EXPECT_EQ(publication->pursuit_owner_countdown_0x51, 8);
    EXPECT_EQ(runtime->pursuit_participants[0].queued_special_state, 0x11);
    EXPECT_EQ(runtime->pursuit_participants[0].queued_field9, 0);
    EXPECT_TRUE(runtime->pursuit_lifecycles[0].active);
    EXPECT_EQ(
        runtime->pursuit_lifecycles[0].phase,
        BattleFramePursuitLifecyclePhase::InstructionState3);
    EXPECT_EQ(rng, 0x778899aau);

    auto* owner = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(owner, nullptr);
    owner->visual_instruction_mode_0x6 = 4;
    owner->visual_instruction_knowledge =
        CombatantVisualInstructionKnowledge::Known;
    runtime->pursuit_participants[0].queued_field9 = 1;

    bool saw_terminal = false;
    for (int frame = 0; frame < 8 && !saw_terminal; ++frame) {
        const auto step = run_first_turn_frame(*runtime, rng);
        ASSERT_TRUE(step.ok);
        for (const auto& event : step.events) {
            if (event.slot != 0) {
                continue;
            }
            if (event.step_kind
                    == BattleFrameWorkerStepKind::PursuitInstructionState3Poll
                && event.pursuit_terminal_result == 1) {
                saw_terminal = true;
                EXPECT_NE(
                    event.detail.find("state2_publication=same_visit"),
                    std::string::npos);
            }
        }
    }
    EXPECT_TRUE(saw_terminal);
    EXPECT_EQ(runtime->pursuit_participants[0].queued_field9, 1);
    EXPECT_EQ(rng, 0x778899aau);
}

TEST(SavorPredictBattleMovementInvocationRuntime, PursuitLegsDoNotWaitForOpposingWorkerStop) {
    auto runtime = initialize_frame_runtime(frame_slots());
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(schedule_fallback_action(*runtime, 1, 4).scheduled);
    std::uint32_t rng = 0x778899aau;
    int slot0_commits = 0;
    for (int frame = 0; frame < 128 && slot0_commits < 2; ++frame) {
        const auto step = run_first_turn_frame(*runtime, rng);
        ASSERT_TRUE(step.ok);
        slot0_commits += static_cast<int>(std::count_if(
            step.events.begin(), step.events.end(),
            [](const BattleFrameStepEvent& event) {
                return event.slot == 0
                    && event.controller_family
                        == BattleMovementControllerFamily::AmbientPursuit
                    && event.step_kind == BattleFrameWorkerStepKind::MovementCommit;
            }));
    }
    EXPECT_GE(slot0_commits, 2);
    ASSERT_TRUE(runtime->active_action.has_value());
    EXPECT_FALSE(runtime->active_action->completion_gate_open);
    EXPECT_EQ(rng, 0x778899aau);
}

TEST(SavorPredictBattleMovementInvocationRuntime, DeferredCompletionClearsAfterGate) {
    auto slots = frame_slots();
    for (auto& slot : slots) {
        if (slot.slot != 0 && slot.slot != 4) {
            slot.present = false;
        }
        slot.movement_flags = 0;
    }
    auto runtime = initialize_frame_runtime(slots);
    ASSERT_TRUE(runtime.has_value());
    const auto scheduled = schedule_fallback_action(*runtime, 0, 4);
    ASSERT_TRUE(scheduled.scheduled);
    std::uint32_t rng = 0x55667788u;
    const auto before_resolution = run_first_turn_until_idle(*runtime, rng, 64);
    EXPECT_TRUE(before_resolution.ok);
    ASSERT_TRUE(runtime->active_action.has_value());
    EXPECT_EQ(runtime->active_action->passive_completion_mask, 0x10);
    EXPECT_NE(runtime->active_action->phase, BattleFrameActionPhase::Complete);

    EXPECT_TRUE(notify_first_turn_action_resolution(*runtime, {
        .action_ordinal = scheduled.action_ordinal,
        .attack_result = 1,
        .attack_landed = true,
        .target_dead = false,
    }));
    EXPECT_TRUE(open_first_turn_action_completion(*runtime, scheduled.action_ordinal));
    const auto drain = run_first_turn_action_until_complete(
        *runtime, rng, scheduled.action_ordinal, 64);
    EXPECT_TRUE(drain.ok);
    EXPECT_EQ(runtime->active_action->phase, BattleFrameActionPhase::Complete);
    EXPECT_EQ(runtime->active_action->passive_completion_mask, 0);
    EXPECT_NE(std::find_if(drain.events.begin(), drain.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind == BattleFrameWorkerStepKind::PassiveCompletionClear;
        }), drain.events.end());
}

TEST(SavorPredictBattleMovementInvocationRuntime, ResolutionOverrideClearsMaskBeforeActionGate) {
    auto slots = frame_slots();
    for (auto& slot : slots) {
        if (slot.slot != 0 && slot.slot != 4) {
            slot.present = false;
        }
        slot.movement_flags = 0;
    }
    auto runtime = initialize_frame_runtime(slots);
    ASSERT_TRUE(runtime.has_value());
    const auto scheduled = schedule_fallback_action(*runtime, 0, 4);
    ASSERT_TRUE(scheduled.scheduled);
    runtime->visual.action_view_role.valid = false;
    std::uint32_t rng = 0x66778899u;
    ASSERT_TRUE(run_first_turn_until_idle(*runtime, rng, 64).ok);
    ASSERT_TRUE(runtime->active_action.has_value());
    ASSERT_EQ(runtime->active_action->completion_turn_phase, 4);
    ASSERT_FALSE(runtime->active_action->completion_override);
    ASSERT_EQ(runtime->active_action->passive_completion_mask, 0x10);

    ASSERT_TRUE(notify_first_turn_action_resolution(*runtime, {
        .action_ordinal = scheduled.action_ordinal,
        .attack_result = 1,
        .attack_landed = true,
        .target_dead = false,
    }));
    ASSERT_FALSE(runtime->active_action->completion_override);
    ASSERT_TRUE(set_first_turn_action_completion_override(
        *runtime, scheduled.action_ordinal));
    ASSERT_TRUE(runtime->active_action->completion_override);
    const auto early_drain = run_first_turn_until_idle(*runtime, rng, 64);
    EXPECT_TRUE(early_drain.ok);
    EXPECT_EQ(runtime->active_action->passive_completion_mask, 0);
    EXPECT_FALSE(runtime->active_action->completion_gate_open);
    EXPECT_NE(runtime->active_action->phase, BattleFrameActionPhase::Complete);

    ASSERT_TRUE(open_first_turn_action_completion(*runtime, scheduled.action_ordinal));
    const auto final_drain = run_first_turn_action_until_complete(
        *runtime, rng, scheduled.action_ordinal, 8);
    EXPECT_TRUE(final_drain.ok);
    EXPECT_EQ(runtime->active_action->completion_turn_phase, 5);
    EXPECT_EQ(runtime->active_action->phase, BattleFrameActionPhase::Complete);
    EXPECT_EQ(rng, 0x66778899u);
}

TEST(SavorPredictBattleMovementInvocationRuntime, AutomaticOverrideDoesNotBypassMissingState17InstructionBoundary) {
    auto runtime = initialize_frame_runtime(frame_slots());
    ASSERT_TRUE(runtime.has_value());
    const auto scheduled = schedule_fallback_action(*runtime, 1, 4);
    ASSERT_TRUE(scheduled.scheduled);
    std::uint32_t rng = 0x8899aabbu;
    for (int frame = 0;
         frame < 32 && runtime->active_action->passive_completion_mask == 0;
         ++frame) {
        ASSERT_TRUE(run_first_turn_frame(*runtime, rng).ok);
    }
    ASSERT_NE(runtime->active_action->passive_completion_mask, 0);
    ASSERT_TRUE(notify_first_turn_action_resolution(*runtime, {
        .action_ordinal = scheduled.action_ordinal,
        .attack_result = 1,
        .attack_landed = true,
        .target_dead = false,
    }));
    ASSERT_TRUE(open_first_turn_action_completion(*runtime, scheduled.action_ordinal));

    const auto drain = run_first_turn_action_until_complete(
        *runtime, rng, scheduled.action_ordinal, 1024);
    EXPECT_FALSE(drain.ok);
    EXPECT_TRUE(drain.ambiguous);
    const auto slot0_commits = std::count_if(
        drain.events.begin(), drain.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.slot == 0
                && event.controller_family
                    == BattleMovementControllerFamily::AmbientPursuit
                && event.step_kind == BattleFrameWorkerStepKind::MovementCommit;
        });
    EXPECT_GE(slot0_commits, 1);
    const auto state17 = std::find_if(
        drain.events.begin(),
        drain.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                == BattleFrameWorkerStepKind::PursuitCoordinationState2;
        });
    ASSERT_NE(state17, drain.events.end());
    EXPECT_EQ(state17->draws_consumed, 0);
    EXPECT_NE(runtime->active_action->phase, BattleFrameActionPhase::Complete);
    EXPECT_TRUE(std::any_of(
        runtime->pursuit_lifecycles.begin(),
        runtime->pursuit_lifecycles.end(),
        [](const BattleFramePursuitLifecycleRuntime& lifecycle) {
            return lifecycle.active
                && lifecycle.phase
                    == BattleFramePursuitLifecyclePhase::InstructionState3
                && !lifecycle.instruction_transition_staged;
        }));
}

TEST(SavorPredictBattleMovementInvocationRuntime, DeadAffectedTargetClearsThroughRemoval) {
    auto slots = frame_slots();
    slots[1].present = false;
    slots[3].present = false;
    slots[0].movement_flags = 0;
    auto runtime = initialize_frame_runtime(slots);
    ASSERT_TRUE(runtime.has_value());
    const auto scheduled = schedule_fallback_action(*runtime, 0, 4);
    ASSERT_TRUE(scheduled.scheduled);
    std::uint32_t rng = 0xaabbccddu;
    ASSERT_TRUE(run_first_turn_until_idle(*runtime, rng, 64).ok);
    auto* target = find_frame_combatant(runtime->state, 4);
    ASSERT_NE(target, nullptr);
    target->alive = false;
    EXPECT_TRUE(notify_first_turn_action_resolution(*runtime, {
        .action_ordinal = scheduled.action_ordinal,
        .attack_result = 1,
        .attack_landed = true,
        .target_dead = true,
    }));
    EXPECT_TRUE(open_first_turn_action_completion(*runtime, scheduled.action_ordinal));
    const auto drain = run_first_turn_action_until_complete(
        *runtime, rng, scheduled.action_ordinal, 64);
    EXPECT_TRUE(drain.ok);
    EXPECT_NE(std::find_if(drain.events.begin(), drain.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind == BattleFrameWorkerStepKind::PassiveDeathClear;
        }), drain.events.end());
}

TEST(SavorPredictBattleMovementInvocationRuntime, EnemyDirectHandoffRunsSameVisitWithoutRng) {
    auto runtime = initialize_frame_runtime(frame_slots());
    ASSERT_TRUE(runtime.has_value());
    const auto scheduled = schedule_first_turn_actor_action(
        *runtime,
        BattleFrameScheduleActionInput{
            .actor_slot = 4,
            .target_slot = 0,
            .enemy_owned = true,
            .selected_worker = MovementSelectedWorker::EnemyDirectAttack_80087f6c,
            .action_kind = BattleMovementActionKind::BasicAttack,
            .relation_scope = BattleMovementRelationScope::SingleTarget,
            .turn_type = BattleMovementTurnType::Normal,
        });
    ASSERT_TRUE(scheduled.scheduled);
    std::uint32_t rng = 0x10203040u;
    std::optional<int> handoff_frame;
    std::optional<int> callback_frame;
    for (int frame_index = 0;
         frame_index < 8
            && (!handoff_frame.has_value() || !callback_frame.has_value());
         ++frame_index) {
        const auto frame = run_first_turn_frame(*runtime, rng);
        ASSERT_TRUE(frame.ok);
        const auto handoff = std::find_if(
            frame.events.begin(), frame.events.end(),
            [](const BattleFrameStepEvent& event) {
                return event.step_kind
                    == BattleFrameWorkerStepKind::MovementControllerHandoff;
            });
        if (handoff != frame.events.end()) {
            handoff_frame = handoff->frame_index;
        }
        const auto callback = std::find_if(
            frame.events.begin(), frame.events.end(),
            [](const BattleFrameStepEvent& event) {
                return event.step_kind == BattleFrameWorkerStepKind::CallbackEntry
                    && event.worker_kind
                        == BattleFrameWorkerKind::EnemyDirectAttack;
            });
        if (callback != frame.events.end()) {
            callback_frame = callback->frame_index;
        }
    }
    ASSERT_TRUE(handoff_frame.has_value());
    ASSERT_TRUE(callback_frame.has_value());
    EXPECT_EQ(*handoff_frame, *callback_frame);
    EXPECT_EQ(rng, 0x10203040u);
}

} // namespace
