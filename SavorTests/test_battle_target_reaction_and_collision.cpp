#include "../SavorPredict/BattleCollisionBoxModel.h"
#include "../SavorPredict/BattleTargetReactionStateModel.h"
#include "../SavorPredict/DirectInstructionTransitionSelectorModel.h"
#include "../SavorPredict/RngCore.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>

namespace {

using namespace savor::predict;

BattleTargetReactionInput basic_reaction_input() {
    return {
        .action_kind = BattleTargetReactionActionKind::BasicAttack,
        .origin_slot = 0,
        .target_slot = 4,
        .hit_check = 1,
        .pending_damage = 20,
        .target_hp_before_flush = 100,
        .target_dead = false,
        .counter_accepted = false,
    };
}

DirectInstructionTransitionRequest selector_request() {
    return {
        .producer = DirectInstructionTransitionProducer::ActionService,
        .origin_slot = 0,
        .target_slot = 4,
        .origin_mode = 4,
        .origin_flags_0xec = 0,
        .target_flags_0xf0 = 0,
        .target_flags_0xf4 = 0,
        .available_target_action_ids = {9, 10, 11, 12, 13, 32},
        .rng_seed_before = 0x12345678U,
    };
}

TEST(SavorPredictTargetReaction, BasicAttackTruthTablePublishesSelectorFlags) {
    auto input = basic_reaction_input();
    const auto ordinary = model_battle_target_reaction(input);
    EXPECT_EQ(ordinary.status, BattleTargetReactionStatus::Matched);
    EXPECT_EQ(ordinary.queued_result, 1);
    EXPECT_EQ(ordinary.reaction_flags_0x50, 0U);

    input.hit_check = 0;
    const auto miss = model_battle_target_reaction(input);
    EXPECT_EQ(miss.reaction_flags_0x50, 0x80000000U);
    EXPECT_EQ(miss.selector_flags_0xf4, 0x01000000U);

    input.hit_check = 3;
    input.counter_accepted = true;
    const auto counter = model_battle_target_reaction(input);
    EXPECT_EQ(counter.queued_counter_byte, 1U);
    EXPECT_EQ(counter.reaction_flags_0x50, 0x20000000U);
    EXPECT_EQ(counter.selector_flags_0xf4, 0x04000000U);

    input.hit_check = 1;
    input.counter_accepted = false;
    input.pending_damage = 100;
    input.target_hp_before_flush = 100;
    input.target_dead = true;
    const auto lethal = model_battle_target_reaction(input);
    EXPECT_EQ(lethal.queued_result, 5);
    EXPECT_EQ(lethal.reaction_flags_0x50, 0x40000000U);
    EXPECT_EQ(lethal.selector_flags_0xf0, 0x04000000U);
}

TEST(SavorPredictTargetReaction, SelfTargetAndUnsupportedInputsDoNotGuess) {
    auto input = basic_reaction_input();
    input.target_slot = input.origin_slot;
    input.hit_check = 0;
    const auto self = model_battle_target_reaction(input);
    EXPECT_TRUE(self.self_target_suppressed);
    EXPECT_EQ(self.reaction_flags_0x50, 0U);

    input.action_kind = BattleTargetReactionActionKind::Unknown;
    const auto unsupported = model_battle_target_reaction(input);
    EXPECT_EQ(unsupported.status, BattleTargetReactionStatus::Unsupported);

    input = basic_reaction_input();
    input.counter_accepted.reset();
    const auto missing = model_battle_target_reaction(input);
    EXPECT_EQ(missing.status, BattleTargetReactionStatus::MissingInput);
}

TEST(SavorPredictDirectTransition, QueuedTransitionRemainsNegativeAndRngFree) {
    auto request = selector_request();
    request.producer = DirectInstructionTransitionProducer::QueuedStateTransition;
    request.origin_flags_0xec = 0x00100000U;
    const auto selected = select_direct_instruction_transition(request);
    EXPECT_EQ(selected.status, DirectInstructionTransitionStatus::Skipped);
    EXPECT_EQ(selected.branch,
              DirectInstructionTransitionBranch::IneligibleQueuedTransition);
    EXPECT_FALSE(selected.should_reset);
    EXPECT_EQ(selected.draws_consumed, 0);
}

TEST(SavorPredictDirectTransition, RandomizedBranchConsumesOneExactDraw) {
    auto request = selector_request();
    request.origin_flags_0xec = 0x00100000U;
    const auto expected = draw_rand15(*request.rng_seed_before);
    const auto selected = select_direct_instruction_transition(request);
    ASSERT_EQ(selected.status, DirectInstructionTransitionStatus::Matched);
    ASSERT_TRUE(selected.selected_mode.has_value());
    EXPECT_EQ(selected.branch,
              DirectInstructionTransitionBranch::RandomizedPassive);
    EXPECT_EQ(selected.draws_consumed, 1);
    EXPECT_EQ(selected.rand_value, expected.value);
    EXPECT_EQ(selected.rng_seed_after, expected.next_state);
    EXPECT_EQ(*selected.selected_mode, (expected.value % 2U) == 0U ? 13 : 12);
    EXPECT_TRUE(selected.clear_origin_random_gate);
}

TEST(SavorPredictDirectTransition, ReactionAndGenericBranchesSelectValidatedRows) {
    auto request = selector_request();
    request.target_flags_0xf4 = 0x01000000U;
    auto selected = select_direct_instruction_transition(request);
    EXPECT_EQ(selected.branch, DirectInstructionTransitionBranch::TargetMiss);
    EXPECT_EQ(selected.selected_mode, 13);
    EXPECT_EQ(selected.draws_consumed, 0);

    request.target_flags_0xf4 = 0x04000000U;
    selected = select_direct_instruction_transition(request);
    EXPECT_EQ(selected.branch, DirectInstructionTransitionBranch::TargetCounter);
    EXPECT_EQ(selected.selected_mode, 9);

    request.origin_mode = 5;
    selected = select_direct_instruction_transition(request);
    EXPECT_EQ(selected.selected_mode, 10);

    request.origin_mode = 8;
    request.target_flags_0xf4 = 0;
    selected = select_direct_instruction_transition(request);
    EXPECT_EQ(selected.branch, DirectInstructionTransitionBranch::GenericRow32);
    EXPECT_EQ(selected.selected_mode, 32);

    request.available_target_action_ids = {11};
    selected = select_direct_instruction_transition(request);
    EXPECT_EQ(selected.branch, DirectInstructionTransitionBranch::GenericRow11);
    EXPECT_EQ(selected.selected_mode, 11);
}

TEST(SavorPredictDirectTransition, MissingTargetRowDoesNotResetOrDraw) {
    auto request = selector_request();
    request.available_target_action_ids.clear();
    const auto selected = select_direct_instruction_transition(request);
    EXPECT_EQ(selected.status, DirectInstructionTransitionStatus::MissingInput);
    EXPECT_FALSE(selected.should_reset);
    EXPECT_EQ(selected.draws_consumed, 0);
}

TEST(SavorPredictCollision, CapturedSignedYRotationVectorIsBitExact) {
    struct CapturedVector {
        std::uint32_t local_y_bits;
        std::uint32_t local_z_bits;
        std::uint32_t origin_x_bits;
        std::uint32_t origin_z_bits;
        std::int32_t rotation_y;
        std::uint32_t world_x_bits;
        std::uint32_t world_y_bits;
        std::uint32_t world_z_bits;
    };
    constexpr CapturedVector vectors[] = {
        {0x412FFFFBU, 0x4187FFFAU, 0x41700000U, 0xC2340000U,
         -8192, 0x403EAB20U, 0x412FFFFBU, 0xC203EAB1U},
        {0x412FFFFBU, 0x4187FFFAU, 0xC1700000U, 0xC2340000U,
         0, 0xC1700000U, 0x412FFFFBU, 0xC1E00006U},
        {0x40F66658U, 0x415851B6U, 0xC1700000U, 0xC1700000U,
         24576, 0xC0AE140EU, 0x40F66658U, 0xC1C47AF8U},
        {0x40F66658U, 0x415851B6U, 0xC1700000U, 0x41700000U,
         27931, 0xC10F3DA6U, 0x40F66658U, 0x403A1C0CU},
        {0x40F66658U, 0x415851B6U, 0xC1700000U, 0x41700000U,
         32768, 0xC16FFFF7U, 0x40F66658U, 0x3FBD7250U},
    };
    for (const auto& vector : vectors) {
        const auto transformed = transform_battle_collision_point({
            .local = {
                .x = 0.0f,
                .y = std::bit_cast<float>(vector.local_y_bits),
                .z = std::bit_cast<float>(vector.local_z_bits),
            },
            .origin = {
                .x = std::bit_cast<float>(vector.origin_x_bits),
                .y = 0.0f,
                .z = std::bit_cast<float>(vector.origin_z_bits),
            },
            .rotation = {.x = 0, .y = vector.rotation_y, .z = 0},
        });
        SCOPED_TRACE(vector.rotation_y);
        EXPECT_EQ(transformed.status, BattleCollisionModelStatus::Matched);
        EXPECT_EQ(transformed.world_x_bits, vector.world_x_bits);
        EXPECT_EQ(transformed.world_y_bits, vector.world_y_bits);
        EXPECT_EQ(transformed.world_z_bits, vector.world_z_bits);
    }
}

TEST(SavorPredictCollision, VectorProgressionUsesSinglePrecisionOperations) {
    const BattleCollisionVec3 current{
        .x = 0.0f,
        .y = std::bit_cast<float>(0x40F66658U),
        .z = std::bit_cast<float>(0x415851B6U),
    };
    const BattleCollisionVec3 velocity{
        .x = 0.0f,
        .y = 0.0f,
        .z = std::bit_cast<float>(0x410FFFFDU),
    };
    const auto advanced = advance_battle_collision_vector(current, velocity);
    const auto reverse = reverse_half_step_battle_collision_vector(current, velocity);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(advanced.z), 0x41B428DAU);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(reverse.z), 0x411051B8U);
}

TEST(SavorPredictCollision, OccupancyRefreshClearsAndRewritesPerSlot) {
    auto occupancy = make_battle_collision_occupancy_runtime();
    auto refreshed = refresh_battle_collision_occupancy(occupancy, {
        .slot = 4,
        .present = true,
        .alive = true,
        .position = {.x = -45.0f, .y = 0.0f, .z = 30.0f},
    });
    EXPECT_EQ(refreshed.status, BattleCollisionModelStatus::Matched);
    EXPECT_EQ(refreshed.grid_x, 1);
    EXPECT_EQ(refreshed.grid_z, 6);
    EXPECT_EQ(refreshed.cells_written, 1);
    EXPECT_EQ(lookup_battle_collision_occupancy(occupancy, -45.0f, 30.0f), 4);

    refreshed = refresh_battle_collision_occupancy(occupancy, {
        .slot = 4,
        .present = true,
        .alive = true,
        .position = {.x = 0.0f, .y = 0.0f, .z = 0.0f},
        .instruction_flags_0xec = 0x00200000U,
    });
    EXPECT_EQ(refreshed.cells_written, 9);
    EXPECT_EQ(lookup_battle_collision_occupancy(occupancy, -45.0f, 30.0f), -1);
    EXPECT_EQ(lookup_battle_collision_occupancy(occupancy, 0.0f, 0.0f), 4);
}

TEST(SavorPredictCollision, MissingMldGateClearsWithoutGuessing) {
    auto occupancy = make_battle_collision_occupancy_runtime();
    const auto refreshed = refresh_battle_collision_occupancy(occupancy, {
        .slot = 5,
        .present = true,
        .alive = true,
        .position = {.x = 0.0f, .y = 0.0f, .z = 0.0f},
        .instruction_flags_0xec = 0x00000100U,
    });
    EXPECT_EQ(refreshed.status, BattleCollisionModelStatus::MissingInput);
    EXPECT_TRUE(refreshed.excluded);
    EXPECT_EQ(lookup_battle_collision_occupancy(occupancy, 0.0f, 0.0f), -1);
}

} // namespace
