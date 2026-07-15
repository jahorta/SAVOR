#include <gtest/gtest.h>

#include <BattleFrameStateModel.h>
#include <BattleInitialFacingModel.h>
#include <BattleInitialTurnTypeModel.h>
#include <RngCore.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using namespace savor::predict;

TEST(SavorPredictBattleInitialTurnType, EventDefinitionIsNormalWithoutRng) {
    const auto result = model_initial_battle_turn_type({
        .prerequisites = {
            .encounter_source = BattleEncounterSourceKind::EventDefinition,
            .encounter_id = 0,
        },
        .rng_seed_before = 0x12345678u,
    });

    EXPECT_EQ(result.status, BattleInitialTurnTypeStatus::Exact);
    ASSERT_TRUE(result.turn_type.has_value());
    EXPECT_EQ(*result.turn_type, soa::battle::TurnType::Normal);
    EXPECT_EQ(result.draws_consumed, 0);
    EXPECT_EQ(result.rng_seed_before, 0x12345678u);
    EXPECT_EQ(result.rng_seed_after, 0x12345678u);
    EXPECT_FALSE(result.advantage_draw.has_value());
    EXPECT_FALSE(result.back_attack_draw.has_value());
}

TEST(SavorPredictBattleInitialTurnType, RandomAdvantageConsumesOneDraw) {
    constexpr std::uint32_t seed = 0x10203040u;
    const auto expected = draw_rand15(seed);
    const auto result = model_initial_battle_turn_type({
        .prerequisites = {
            .encounter_source = BattleEncounterSourceKind::RandomTable,
            .advantage_chance_override = 101,
            .back_attack_chance_override = 0,
        },
        .rng_seed_before = seed,
    });

    EXPECT_EQ(result.status, BattleInitialTurnTypeStatus::Exact);
    ASSERT_TRUE(result.turn_type.has_value());
    EXPECT_EQ(*result.turn_type, soa::battle::TurnType::Advantage);
    EXPECT_EQ(result.draws_consumed, 1);
    EXPECT_EQ(result.rng_seed_after, expected.next_state);
    ASSERT_TRUE(result.advantage_draw.has_value());
    EXPECT_EQ(result.advantage_draw->rand_value, expected.value);
    EXPECT_FALSE(result.back_attack_draw.has_value());
}

TEST(SavorPredictBattleInitialTurnType, RandomBackAttackAndNormalConsumeTwoDraws) {
    constexpr std::uint32_t seed = 0x55667788u;
    const auto first = draw_rand15(seed);
    const auto second = draw_rand15(first.next_state);

    const auto back_attack = model_initial_battle_turn_type({
        .prerequisites = {
            .encounter_source = BattleEncounterSourceKind::RandomTable,
            .advantage_chance_override = 0,
            .back_attack_chance_override = 101,
        },
        .rng_seed_before = seed,
    });
    ASSERT_TRUE(back_attack.turn_type.has_value());
    EXPECT_EQ(*back_attack.turn_type, soa::battle::TurnType::BackAttack);
    EXPECT_EQ(back_attack.draws_consumed, 2);
    EXPECT_EQ(back_attack.rng_seed_after, second.next_state);

    const auto normal = model_initial_battle_turn_type({
        .prerequisites = {
            .encounter_source = BattleEncounterSourceKind::RandomTable,
            .advantage_chance_override = 0,
            .back_attack_chance_override = 0,
        },
        .rng_seed_before = seed,
    });
    ASSERT_TRUE(normal.turn_type.has_value());
    EXPECT_EQ(*normal.turn_type, soa::battle::TurnType::Normal);
    EXPECT_EQ(normal.draws_consumed, 2);
    EXPECT_EQ(normal.rng_seed_after, second.next_state);
}

TEST(SavorPredictBattleInitialTurnType, DerivesThresholdsBeforeDrawing) {
    const auto result = model_initial_battle_turn_type({
        .prerequisites = {
            .encounter_source = BattleEncounterSourceKind::RandomTable,
            .vyse_level = 5,
            .initiative = 25,
            .advantage_chance_override = -1,
            .back_attack_chance_override = -1,
        },
        .rng_seed_before = 0x01020304u,
    });

    EXPECT_EQ(result.status, BattleInitialTurnTypeStatus::Exact);
    ASSERT_TRUE(result.advantage_threshold.has_value());
    ASSERT_TRUE(result.back_attack_threshold.has_value());
    EXPECT_EQ(*result.advantage_threshold, 25);
    EXPECT_EQ(*result.back_attack_threshold, 5);
}

TEST(SavorPredictBattleInitialTurnType, MissingRandomInputsAreTransactional) {
    constexpr std::uint32_t seed = 0x89ABCDEFu;
    const auto result = model_initial_battle_turn_type({
        .prerequisites = {
            .encounter_source = BattleEncounterSourceKind::RandomTable,
            .advantage_chance_override = -1,
            .back_attack_chance_override = -1,
        },
        .rng_seed_before = seed,
    });

    EXPECT_EQ(result.status, BattleInitialTurnTypeStatus::MissingInput);
    EXPECT_FALSE(result.turn_type.has_value());
    EXPECT_EQ(result.draws_consumed, 0);
    EXPECT_EQ(result.rng_seed_after, seed);
    EXPECT_FALSE(result.advantage_draw.has_value());
    EXPECT_FALSE(result.back_attack_draw.has_value());
}

TEST(SavorPredictBattleInitialFacing, MapsAllTurnTypesBySide) {
    struct Vector {
        soa::battle::TurnType turn_type;
        bool is_player;
        std::uint32_t expected_angle;
    };
    constexpr std::array<Vector, 6> vectors{{
        {soa::battle::TurnType::BackAttack, true, 0x00000000u},
        {soa::battle::TurnType::BackAttack, false, 0x00000000u},
        {soa::battle::TurnType::Normal, true, 0x00008000u},
        {soa::battle::TurnType::Normal, false, 0x00000000u},
        {soa::battle::TurnType::Advantage, true, 0x00008000u},
        {soa::battle::TurnType::Advantage, false, 0x00008000u},
    }};

    for (const auto& vector : vectors) {
        const auto result = model_initial_battle_facing({
            .slot = vector.is_player ? 0 : 4,
            .present = true,
            .is_player = vector.is_player,
            .turn_type = vector.turn_type,
        });
        EXPECT_EQ(result.status, BattleInitialFacingStatus::Exact);
        ASSERT_TRUE(result.facing_angle_0x2c.has_value());
        EXPECT_EQ(*result.facing_angle_0x2c, vector.expected_angle);
    }
}

TEST(SavorPredictBattleInitialFacing, FrameStateSeedsWorksheetAndTurnDegrees) {
    const std::vector<MovementSlotState> slots{
        {
            .slot = 0,
            .present = true,
            .is_player = true,
            .alive = true,
            .start_position = BattleStartPosition{
                .present = true,
                .grid_x = 5,
                .grid_z = 7,
            },
        },
        {
            .slot = 4,
            .present = true,
            .is_player = false,
            .alive = true,
            .start_position = BattleStartPosition{
                .present = true,
                .grid_x = 5,
                .grid_z = 3,
            },
        },
    };
    const std::array<std::uint8_t, 81> terrain{};
    const auto state = initialize_first_battle_frame_state(
        0,
        slots,
        terrain,
        soa::battle::TurnType::Normal);

    ASSERT_TRUE(state.has_value());
    const auto* player = find_frame_combatant(*state, 0);
    const auto* enemy = find_frame_combatant(*state, 4);
    ASSERT_NE(player, nullptr);
    ASSERT_NE(enemy, nullptr);
    EXPECT_EQ(player->combatant_facing_angle_0x2c, 0x00008000u);
    EXPECT_EQ(player->last_written_facing_angle_0x2c, 0x00008000u);
    EXPECT_FLOAT_EQ(player->turn_current_degrees_0x11c, 180.0f);
    EXPECT_FLOAT_EQ(player->turn_target_degrees_0x120, 180.0f);
    EXPECT_EQ(enemy->combatant_facing_angle_0x2c, 0x00000000u);
    EXPECT_FLOAT_EQ(enemy->turn_current_degrees_0x11c, 0.0f);
}

} // namespace
