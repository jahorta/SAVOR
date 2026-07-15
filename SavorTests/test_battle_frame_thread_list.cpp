#include <BattleFrameSchedulerModel.h>
#include <BattleFrameThreadListModel.h>

#include <gtest/gtest.h>

#include <array>
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

} // namespace
