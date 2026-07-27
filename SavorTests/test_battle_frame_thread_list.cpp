#include <BattleFrameSchedulerModel.h>
#include <BattleFrameThreadListModel.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using namespace savor::predict;

std::vector<MovementSlotState> thread_test_slots() {
    std::vector<MovementSlotState> result;
    for (const int slot : {5, 0, 4, 1}) {
        result.push_back(MovementSlotState{
            .slot = slot,
            .present = true,
            .is_player = slot < 4,
            .alive = true,
            .width = 1,
            .depth = 1,
            .start_position = BattleStartPosition{
                .present = true,
                .grid_x = slot < 4 ? 4 + slot * 2 : slot,
                .grid_z = slot < 4 ? 6 : 2,
            },
        });
    }
    return result;
}

CombatantVisualResource test_resource(int slot) {
    CombatantVisualResource resource;
    resource.binding.slot = slot;
    resource.binding.resource_stem = slot == 0
        ? "MA001"
        : slot == 1 ? "MA002" : "MB001";
    resource.action_rows.push_back(CombatantStdActionRow{
        .index = 0,
        .action_id = 6,
        .row_type = 0,
        .flags = 0x01000000u,
    });
    return resource;
}

std::vector<int> active_owner_order(
    const BattleFrameThreadListRuntime& runtime,
    BattleFrameThreadNodeKind kind) {
    std::vector<int> result;
    for (const auto* node : active_battle_frame_threads(runtime, kind)) {
        result.push_back(node->owner_slot);
    }
    return result;
}

TEST(SavorPredictBattleFrameThreadList, MovementSetupCreatesAscendingLiveSlots) {
    std::array<std::uint8_t, 81> terrain{};
    const auto runtime = initialize_first_battle_frame_runtime(
        0,
        thread_test_slots(),
        terrain);

    ASSERT_TRUE(runtime.has_value());
    EXPECT_EQ(
        active_owner_order(
            runtime->thread_list,
            BattleFrameThreadNodeKind::MovementController),
        (std::vector<int>{0, 1, 4, 5}));
    EXPECT_TRUE(active_owner_order(
        runtime->thread_list,
        BattleFrameThreadNodeKind::CombatantInstruction).empty());
}

TEST(SavorPredictBattleFrameThreadList, StdPublicationDeterminesInstructionOrder) {
    std::array<std::uint8_t, 81> terrain{};
    auto runtime = initialize_first_battle_frame_runtime(
        0,
        thread_test_slots(),
        terrain);
    ASSERT_TRUE(runtime.has_value());

    for (const int slot : {5, 0, 4, 1}) {
        ASSERT_TRUE(configure_battle_frame_visual_resource(
            *runtime,
            test_resource(slot)));
    }
    const auto publication = publish_configured_battle_frame_std_resources(
        *runtime,
        "test producer");
    ASSERT_EQ(publication.status, BattleFrameEventStatus::Provisional)
        << publication.provenance;
    EXPECT_EQ(publication.publication_order, (std::vector<int>{0, 1, 4, 5}));

    EXPECT_EQ(
        active_owner_order(
            runtime->thread_list,
            BattleFrameThreadNodeKind::MovementController),
        (std::vector<int>{0, 1, 4, 5}));
    EXPECT_EQ(
        active_owner_order(
            runtime->thread_list,
            BattleFrameThreadNodeKind::CombatantInstruction),
        (std::vector<int>{4, 5, 1, 0}));
}

TEST(SavorPredictBattleFrameThreadList, StdResourcePublicationTracksLiveInstructionThreadIdentity) {
    std::array<std::uint8_t, 81> terrain{};
    auto runtime = initialize_first_battle_frame_runtime(
        0,
        thread_test_slots(),
        terrain);
    ASSERT_TRUE(runtime.has_value());

    constexpr int slot = 0;
    const auto first_publication = publish_battle_frame_std_resource(
        *runtime,
        test_resource(slot),
        "first semantic resource publication");
    ASSERT_EQ(
        first_publication.status,
        BattleFrameThreadMutationStatus::Applied)
        << first_publication.detail;

    const auto* first_thread = find_battle_frame_thread(
        runtime->thread_list,
        first_publication.node_id);
    ASSERT_NE(first_thread, nullptr);
    ASSERT_TRUE(first_thread->active);
    ASSERT_TRUE(first_thread->semantic_instance_id.has_value());

    const auto& first_resource =
        runtime->visual.instruction_resources[static_cast<std::size_t>(slot)];
    EXPECT_EQ(
        first_resource.knowledge,
        BattleFrameInstructionResourceKnowledge::Present);
    EXPECT_EQ(first_resource.slot, slot);
    EXPECT_EQ(
        first_resource.instruction_thread_node_id,
        first_thread->node_id);
    EXPECT_EQ(
        first_resource.instruction_thread_creation_sequence,
        first_thread->creation_sequence);
    EXPECT_EQ(
        first_resource.instruction_thread_semantic_instance,
        *first_thread->semantic_instance_id);
    EXPECT_FALSE(first_resource.resource_semantic_id.empty());
    EXPECT_GT(first_resource.publication_revision, 0u);
    EXPECT_EQ(first_resource.publication_frame, runtime->state.frame_index);

    const auto first_revision = first_resource.publication_revision;
    const auto first_creation_sequence = first_thread->creation_sequence;
    const auto first_semantic_instance = *first_thread->semantic_instance_id;
    ASSERT_EQ(
        remove_battle_frame_thread(
            runtime->thread_list,
            first_thread->node_id,
            "test.std_resource.remove",
            "remove the owning instruction thread",
            runtime->state.frame_index).status,
        BattleFrameThreadMutationStatus::Applied);

    std::uint32_t rng = 0x12345678u;
    const auto refresh = run_first_turn_frame(*runtime, rng);
    ASSERT_TRUE(refresh.ok);
    EXPECT_EQ(
        runtime->visual.instruction_resources[
            static_cast<std::size_t>(slot)].knowledge,
        BattleFrameInstructionResourceKnowledge::Missing);

    const auto second_publication = publish_battle_frame_std_resource(
        *runtime,
        test_resource(slot),
        "replacement semantic resource publication");
    ASSERT_EQ(
        second_publication.status,
        BattleFrameThreadMutationStatus::Applied)
        << second_publication.detail;
    const auto* second_thread = find_battle_frame_thread(
        runtime->thread_list,
        second_publication.node_id);
    ASSERT_NE(second_thread, nullptr);
    ASSERT_TRUE(second_thread->active);
    ASSERT_TRUE(second_thread->semantic_instance_id.has_value());

    const auto& second_resource =
        runtime->visual.instruction_resources[static_cast<std::size_t>(slot)];
    EXPECT_EQ(
        second_resource.knowledge,
        BattleFrameInstructionResourceKnowledge::Present);
    EXPECT_EQ(
        second_resource.instruction_thread_node_id,
        second_thread->node_id);
    EXPECT_EQ(
        second_resource.instruction_thread_creation_sequence,
        second_thread->creation_sequence);
    EXPECT_EQ(
        second_resource.instruction_thread_semantic_instance,
        *second_thread->semantic_instance_id);
    EXPECT_NE(second_thread->creation_sequence, first_creation_sequence);
    EXPECT_NE(*second_thread->semantic_instance_id, first_semantic_instance);
    EXPECT_GT(second_resource.publication_revision, first_revision);
}

TEST(SavorPredictBattleFrameThreadList, CurrentCursorControlsMkChildInsertion) {
    BattleFrameThreadListRuntime runtime;
    const auto runner = create_battle_frame_thread(runtime, {
        .kind = BattleFrameThreadNodeKind::ResourceWorker,
        .owner_slot = -1,
        .callback = BattleFrameThreadCallbackIdentity::ResourceQueue,
        .insertion = BattleFrameThreadInsertionKind::AfterCurrentCursor,
        .semantic_source_id = "test.resource_runner",
        .provenance = "test runner",
    });
    ASSERT_EQ(runner.status, BattleFrameThreadMutationStatus::Applied);

    const auto first = create_battle_frame_thread(runtime, {
        .kind = BattleFrameThreadNodeKind::CombatantInstruction,
        .owner_slot = 0,
        .callback = BattleFrameThreadCallbackIdentity::CombatantInstruction,
        .insertion = BattleFrameThreadInsertionKind::AfterCurrentCursor,
        .semantic_source_id = "test.instruction",
        .provenance = "first group",
    });
    ASSERT_EQ(first.status, BattleFrameThreadMutationStatus::Applied);
    const auto cursor = set_battle_frame_thread_cursor(
        runtime,
        runner.node_id,
        "test.resource_runner.visit",
        "next group");
    ASSERT_EQ(cursor.status, BattleFrameThreadMutationStatus::Applied);
    const auto second = create_battle_frame_thread(runtime, {
        .kind = BattleFrameThreadNodeKind::CombatantInstruction,
        .owner_slot = 1,
        .callback = BattleFrameThreadCallbackIdentity::CombatantInstruction,
        .insertion = BattleFrameThreadInsertionKind::AfterCurrentCursor,
        .semantic_source_id = "test.instruction",
        .provenance = "second group",
    });
    ASSERT_EQ(second.status, BattleFrameThreadMutationStatus::Applied);

    ASSERT_EQ(runtime.nodes.size(), 3u);
    EXPECT_EQ(runtime.nodes[0].node_id, runner.node_id);
    EXPECT_EQ(runtime.nodes[1].owner_slot, 1);
    EXPECT_EQ(runtime.nodes[2].owner_slot, 0);
    EXPECT_EQ(runtime.current_node_id, second.node_id);
}

TEST(SavorPredictBattleFrameThreadList, ChildInsertionAndRemovalPreserveOwnership) {
    BattleFrameThreadListRuntime runtime;
    const auto parent = create_battle_frame_thread(runtime, {
        .kind = BattleFrameThreadNodeKind::ResourceWorker,
        .owner_slot = -1,
        .callback = BattleFrameThreadCallbackIdentity::ResourceQueue,
        .semantic_source_id = "battle.resource_queue.create",
        .provenance = "test parent",
    });
    ASSERT_EQ(parent.status, BattleFrameThreadMutationStatus::Applied);

    for (const int slot : {4, 5}) {
        const auto child = create_battle_frame_thread(runtime, {
            .kind = BattleFrameThreadNodeKind::CombatantInstruction,
            .owner_slot = slot,
            .callback = BattleFrameThreadCallbackIdentity::CombatantInstruction,
            .insertion = BattleFrameThreadInsertionKind::LastChild,
            .parent_node_id = parent.node_id,
            .semantic_source_id = "battle.std_resource.publication",
            .provenance = "test child",
        });
        ASSERT_EQ(child.status, BattleFrameThreadMutationStatus::Applied);
    }

    ASSERT_EQ(runtime.nodes.size(), 3u);
    EXPECT_EQ(runtime.nodes[0].node_id, parent.node_id);
    EXPECT_EQ(runtime.nodes[1].owner_slot, 4);
    EXPECT_EQ(runtime.nodes[2].owner_slot, 5);
    EXPECT_EQ(runtime.nodes[1].parent_node_id, parent.node_id);

    const auto removed = remove_battle_frame_thread(
        runtime,
        runtime.nodes[1].node_id,
        "battle.thread.remove",
        "test removal");
    EXPECT_EQ(removed.status, BattleFrameThreadMutationStatus::Applied);
    ASSERT_NE(find_battle_frame_thread(runtime, removed.node_id), nullptr);
    EXPECT_FALSE(find_battle_frame_thread(runtime, removed.node_id)->active);
    EXPECT_EQ(runtime.history.back().kind, BattleFrameThreadMutationKind::Remove);
}

TEST(SavorPredictBattleFrameThreadList, DuplicateAndUnknownProducerStayTyped) {
    BattleFrameThreadListRuntime runtime;
    const auto missing = create_battle_frame_thread(runtime, {
        .kind = BattleFrameThreadNodeKind::MovementController,
        .owner_slot = 0,
        .callback = BattleFrameThreadCallbackIdentity::MovementController,
    });
    EXPECT_EQ(missing.status, BattleFrameThreadMutationStatus::MissingInput);

    BattleFrameThreadCreateRequest valid{
        .kind = BattleFrameThreadNodeKind::MovementController,
        .owner_slot = 0,
        .callback = BattleFrameThreadCallbackIdentity::MovementController,
        .semantic_source_id = "battle.setup.movement_controller",
        .provenance = "test producer",
    };
    EXPECT_EQ(create_battle_frame_thread(runtime, valid).status,
              BattleFrameThreadMutationStatus::Applied);
    EXPECT_EQ(create_battle_frame_thread(runtime, valid).status,
              BattleFrameThreadMutationStatus::Duplicate);
}

TEST(SavorPredictBattleFrameThreadList, TraversalPositionPlansSameOrNextFrameDelivery) {
    BattleFrameThreadListRuntime runtime;
    std::vector<int> node_ids;
    for (const int slot : {0, 1, 2}) {
        const auto created = create_battle_frame_thread(runtime, {
            .kind = BattleFrameThreadNodeKind::MovementController,
            .owner_slot = slot,
            .callback = BattleFrameThreadCallbackIdentity::MovementController,
            .semantic_source_id = "test.thread.create",
            .provenance = "ordered traversal fixture",
        });
        ASSERT_EQ(created.status, BattleFrameThreadMutationStatus::Applied);
        node_ids.push_back(created.node_id);
    }

    begin_battle_frame_thread_traversal(runtime, 10);
    ASSERT_EQ(runtime.traversal_generation, 1u);
    ASSERT_EQ(set_battle_frame_thread_cursor(
        runtime,
        node_ids[1],
        "test.thread.visit",
        "middle node visit",
        10).status, BattleFrameThreadMutationStatus::Applied);

    const auto later = plan_battle_frame_thread_delivery(runtime, node_ids[2]);
    EXPECT_EQ(later.status, BattleFrameThreadDeliveryStatus::SameTraversal);
    EXPECT_EQ(later.current_node_index, 1);
    EXPECT_EQ(later.target_node_index, 2);
    EXPECT_EQ(later.eligible_traversal_generation, 1u);

    const auto earlier = plan_battle_frame_thread_delivery(runtime, node_ids[0]);
    EXPECT_EQ(earlier.status, BattleFrameThreadDeliveryStatus::NextTraversal);
    EXPECT_EQ(earlier.current_node_index, 1);
    EXPECT_EQ(earlier.target_node_index, 0);
    EXPECT_EQ(earlier.eligible_traversal_generation, 2u);

    const auto current = plan_battle_frame_thread_delivery(runtime, node_ids[1]);
    EXPECT_EQ(current.status, BattleFrameThreadDeliveryStatus::NextTraversal);
    EXPECT_EQ(current.eligible_traversal_generation, 2u);
    end_battle_frame_thread_traversal(runtime);

    const auto outside = plan_battle_frame_thread_delivery(runtime, node_ids[2]);
    EXPECT_EQ(outside.status, BattleFrameThreadDeliveryStatus::NextTraversal);
    EXPECT_EQ(outside.eligible_traversal_generation, 2u);
}

TEST(SavorPredictBattleFrameThreadList, TraversalTracksPreviousCurrentAndVisitedNodes) {
    BattleFrameThreadListRuntime runtime;
    std::vector<int> node_ids;
    for (const int slot : {0, 1, 2}) {
        const auto created = create_battle_frame_thread(runtime, {
            .kind = BattleFrameThreadNodeKind::MovementController,
            .owner_slot = slot,
            .callback = BattleFrameThreadCallbackIdentity::MovementController,
            .semantic_source_id = "test.thread.create",
            .provenance = "cursor fixture",
        });
        ASSERT_EQ(created.status, BattleFrameThreadMutationStatus::Applied);
        node_ids.push_back(created.node_id);
    }

    begin_battle_frame_thread_traversal(runtime, 22);
    for (const int node_id : node_ids) {
        ASSERT_EQ(set_battle_frame_thread_cursor(
            runtime,
            node_id,
            "test.thread.visit",
            "ordered node visit",
            22).status, BattleFrameThreadMutationStatus::Applied);
    }
    EXPECT_EQ(runtime.previous_node_id, node_ids[1]);
    EXPECT_EQ(runtime.current_node_id, node_ids[2]);
    ASSERT_TRUE(runtime.current_node_index.has_value());
    EXPECT_EQ(*runtime.current_node_index, 2u);
    EXPECT_EQ(runtime.visited_node_ids, node_ids);
    end_battle_frame_thread_traversal(runtime);
    EXPECT_FALSE(runtime.traversal_active);
}

TEST(SavorPredictBattleFrameThreadList, RepeatableAuxiliaryChildrenPreserveCursorOrder) {
    BattleFrameThreadListRuntime runtime;
    const auto owner = create_battle_frame_thread(runtime, {
        .kind = BattleFrameThreadNodeKind::CombatantInstruction,
        .owner_slot = 1,
        .callback = BattleFrameThreadCallbackIdentity::CombatantInstruction,
        .semantic_source_id = "test.instruction.owner",
        .provenance = "state-10 owner",
    });
    ASSERT_EQ(owner.status, BattleFrameThreadMutationStatus::Applied);

    begin_battle_frame_thread_traversal(runtime, 33);
    ASSERT_EQ(set_battle_frame_thread_cursor(
        runtime,
        owner.node_id,
        "test.instruction.visit",
        "state-10 publication visit",
        33).status, BattleFrameThreadMutationStatus::Applied);

    const auto first = create_battle_frame_thread(runtime, {
        .kind = BattleFrameThreadNodeKind::AuxiliaryVisualChild,
        .owner_slot = 1,
        .semantic_instance_id = 100u,
        .callback = BattleFrameThreadCallbackIdentity::VisualMoveModel,
        .insertion = BattleFrameThreadInsertionKind::AfterCurrentCursor,
        .parent_node_id = owner.node_id,
        .semantic_source_id = "test.visual.child",
        .provenance = "first repeated child",
    });
    const auto second = create_battle_frame_thread(runtime, {
        .kind = BattleFrameThreadNodeKind::AuxiliaryVisualChild,
        .owner_slot = 1,
        .semantic_instance_id = 101u,
        .callback = BattleFrameThreadCallbackIdentity::VisualMoveModel,
        .insertion = BattleFrameThreadInsertionKind::AfterCurrentCursor,
        .parent_node_id = owner.node_id,
        .semantic_source_id = "test.visual.child",
        .provenance = "second repeated child",
    });
    ASSERT_EQ(first.status, BattleFrameThreadMutationStatus::Applied);
    ASSERT_EQ(second.status, BattleFrameThreadMutationStatus::Applied);
    ASSERT_EQ(runtime.nodes.size(), 3u);
    EXPECT_EQ(runtime.nodes[0].node_id, owner.node_id);
    EXPECT_EQ(runtime.nodes[1].node_id, first.node_id);
    EXPECT_EQ(runtime.nodes[2].node_id, second.node_id);
    EXPECT_EQ(runtime.nodes[1].parent_node_id, owner.node_id);
    EXPECT_EQ(runtime.nodes[2].parent_node_id, owner.node_id);

    const auto duplicate = create_battle_frame_thread(runtime, {
        .kind = BattleFrameThreadNodeKind::AuxiliaryVisualChild,
        .owner_slot = 1,
        .semantic_instance_id = 101u,
        .callback = BattleFrameThreadCallbackIdentity::VisualMoveModel,
        .insertion = BattleFrameThreadInsertionKind::AfterCurrentCursor,
        .parent_node_id = owner.node_id,
        .semantic_source_id = "test.visual.child",
        .provenance = "duplicate instance",
    });
    EXPECT_EQ(duplicate.status, BattleFrameThreadMutationStatus::Duplicate);
    end_battle_frame_thread_traversal(runtime);
}

} // namespace
