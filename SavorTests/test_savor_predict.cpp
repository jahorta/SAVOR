#include <gtest/gtest.h>

#include <CheckpointTrace.h>
#include <ProgressEventParser.h>
#include <RngModel.h>

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace savor::predict;

TEST(SavorPredictRngModel, DrawRand15AdvancesFromZero) {
    const auto draw = draw_rand15(0);
    EXPECT_EQ(draw.next_state, 0x00003039u);
    EXPECT_EQ(draw.value, 0u);
}

TEST(SavorPredictRngModel, BoundedDistanceFindsAdvancedTarget) {
    auto state = 0x12345678u;
    for (int i = 0; i < 17; ++i) {
        state = advance_once(state);
    }

    const auto distance = bounded_distance(0x12345678u, state, 100);
    ASSERT_TRUE(distance.has_value());
    EXPECT_EQ(*distance, 17);
}

TEST(SavorPredictRngModel, PreAiCameraModelKeepsBaselineAndSuppressionVisible) {
    const auto no_fake = model_pre_ai_camera_draws(0);
    EXPECT_EQ(no_fake.fake_attack_draws, 0);
    EXPECT_EQ(no_fake.baseline_camera_draws, 3);
    EXPECT_EQ(no_fake.expected_camera_draws, 3);
    EXPECT_EQ(no_fake.suppressed_attack_targeting_camera_draws, 0);
    EXPECT_EQ(no_fake.unsuppressed_total_draws, 3);
    EXPECT_EQ(no_fake.expected_total_draws, 3);
    EXPECT_FALSE(no_fake.suppresses_normal_attack_targeting_camera);
    EXPECT_EQ(pre_ai_camera_rule_name(no_fake), std::string("BaselineThreeCameraDraws"));

    const auto one_fake = model_pre_ai_camera_draws(1);
    EXPECT_EQ(one_fake.fake_attack_draws, 1);
    EXPECT_EQ(one_fake.baseline_camera_draws, 3);
    EXPECT_EQ(one_fake.expected_camera_draws, 2);
    EXPECT_EQ(one_fake.suppressed_attack_targeting_camera_draws, 1);
    EXPECT_EQ(one_fake.unsuppressed_total_draws, 4);
    EXPECT_EQ(one_fake.expected_total_draws, 3);
    EXPECT_TRUE(one_fake.suppresses_normal_attack_targeting_camera);
    EXPECT_EQ(
        pre_ai_camera_rule_name(one_fake),
        std::string("FakeAttackSuppressesOneTargetingCameraDraw"));

    const auto three_fake = model_pre_ai_camera_draws(3);
    EXPECT_EQ(three_fake.unsuppressed_total_draws, 6);
    EXPECT_EQ(three_fake.expected_total_draws, 5);
    EXPECT_EQ(pre_ai_draws_for_fake_attacks(3), three_fake.expected_total_draws);
}

TEST(SavorPredictRngModel, SoldierAiConsumesExpectedAttackDraws) {
    std::uint32_t state = 15u;
    for (int i = 0; i < pre_ai_draws_for_fake_attacks(3); ++i) {
        state = advance_once(state);
    }

    const auto slot4 = resolve_soldier_ai(4, state);
    EXPECT_EQ(slot4.slot, 4);
    EXPECT_TRUE(slot4.attacks);
    EXPECT_TRUE(slot4.target_rand.has_value());
    EXPECT_TRUE(slot4.attack_param_rand.has_value());
}

TEST(SavorPredictRngModel, SoldierAttackParamUsesMod10Threshold) {
    EXPECT_EQ(soldier_attack_param_from_rand(3), 1);
    EXPECT_EQ(soldier_attack_param_from_rand(4), 0);
    EXPECT_EQ(soldier_attack_param_from_rand(13), 1);
    EXPECT_EQ(soldier_attack_param_from_rand(14), 0);
}

TEST(SavorPredictRngModel, FirstBattleTurnOrderSpendsOneDrawPerQueuedBasicAction) {
    const auto entries = first_battle_basic_turn_order_entries(true, true);
    ASSERT_EQ(entries.size(), 4u);
    EXPECT_EQ(entries[0].slot, 0);
    EXPECT_EQ(entries[0].quick, 22);
    EXPECT_EQ(entries[1].slot, 1);
    EXPECT_EQ(entries[1].quick, 24);
    EXPECT_EQ(entries[2].slot, 4);
    EXPECT_EQ(entries[2].quick, 18);
    EXPECT_EQ(entries[3].slot, 5);
    EXPECT_EQ(entries[3].quick, 18);

    const auto result = simulate_turn_order(0, entries);
    EXPECT_EQ(result.queued_count, 4);
    EXPECT_EQ(result.sum_quick, 82);
    EXPECT_EQ(result.jitter_modulus, 10);
    EXPECT_EQ(result.draws_consumed, 4);
    EXPECT_TRUE(result.priorities_complete);
    ASSERT_EQ(result.entries.size(), entries.size());
    for (const auto& entry : result.entries) {
        EXPECT_EQ(entry.path, TurnOrderPriorityPath::RandomizedQuick);
        EXPECT_TRUE(entry.priority_rand.has_value());
        ASSERT_TRUE(entry.assigned_priority.has_value());
        EXPECT_GE(*entry.assigned_priority, entry.input.quick);
        EXPECT_LT(*entry.assigned_priority, entry.input.quick + result.jitter_modulus);
    }
    EXPECT_EQ(turn_order_priority_path_name(result.entries[0].path), std::string("RandomizedQuick"));
}

TEST(SavorPredictRngModel, FirstBattleTurnOrderCheckpointExpectationUsesSimulation) {
    const auto result = simulate_turn_order(
        0x12345678u,
        first_battle_basic_turn_order_entries(true, false));

    const auto expectation = turn_order_checkpoint_expectation(result);

    EXPECT_EQ(expectation.expected_priority_jitter_draws, 3);
    EXPECT_EQ(expectation.expected_queued_entries, 3);
    EXPECT_EQ(expectation.expected_jitter_modulus, 10);
    EXPECT_EQ(expectation.owner, std::string_view("turn_order_priority_jitter"));
    EXPECT_EQ(expectation.pc, std::string_view("800711F8"));
    EXPECT_NE(
        std::string_view(turn_order_checkpoint_rule_detail()).find("one 800711f8"),
        std::string_view::npos);
}

TEST(SavorPredictRngModel, TurnOrderUsesDescendingPriorityWhenPrioritiesAreKnown) {
    std::vector<TurnOrderEntryInput> entries;
    entries.push_back({.slot = 0, .quick = 10, .initial_priority = 30});
    entries.push_back({.slot = 1, .quick = 10, .initial_priority = 60});
    entries.push_back({.slot = 4, .quick = 10, .initial_priority = 40});

    const auto result = simulate_turn_order(0x12345678u, entries);

    EXPECT_EQ(result.draws_consumed, 0);
    EXPECT_TRUE(result.execution_order_exact);
    ASSERT_EQ(result.execution_slots.size(), 3u);
    EXPECT_EQ(result.execution_slots[0], 1);
    EXPECT_EQ(result.execution_slots[1], 4);
    EXPECT_EQ(result.execution_slots[2], 0);
}

TEST(SavorPredictRngModel, TurnOrderZeroJitterUsesQuickWithoutDraw) {
    std::vector<TurnOrderEntryInput> entries;
    entries.push_back({.slot = 0, .quick = 1});
    entries.push_back({.slot = 1, .quick = 1});

    const auto result = simulate_turn_order(0x12345678u, entries);

    EXPECT_EQ(result.jitter_modulus, 0);
    EXPECT_EQ(result.draws_consumed, 0);
    ASSERT_EQ(result.entries.size(), 2u);
    EXPECT_EQ(result.entries[0].path, TurnOrderPriorityPath::QuickOnlyNoJitter);
    EXPECT_EQ(result.entries[0].assigned_priority, 1);
    EXPECT_EQ(result.end_state, 0x12345678u);
}

TEST(SavorPredictRngModel, TurnOrderKeepsUnresolvedFixedPriorityVisible) {
    std::vector<TurnOrderEntryInput> entries;
    entries.push_back({.slot = 0, .quick = 22});
    entries.push_back({.slot = 1, .quick = 24, .fixed_priority_result = 2});

    const auto result = simulate_turn_order(0, entries);

    EXPECT_FALSE(result.priorities_complete);
    EXPECT_FALSE(result.execution_order_exact);
    EXPECT_TRUE(result.execution_slots.empty());
    ASSERT_EQ(result.entries.size(), 2u);
    EXPECT_EQ(result.entries[1].path, TurnOrderPriorityPath::FixedPriorityUnresolved);
    EXPECT_EQ(
        turn_order_priority_path_name(result.entries[1].path),
        std::string("FixedPriorityUnresolved"));
}

TEST(SavorPredictRngModel, FirstBattleDataProvidesCentralActorStats) {
    const auto vyse = first_battle_actor_by_slot(0);
    ASSERT_TRUE(vyse.has_value());
    EXPECT_EQ(vyse->kind, FirstBattleActorKind::Vyse);
    EXPECT_EQ(vyse->max_hp, 420);
    EXPECT_EQ(vyse->quick, 22);
    EXPECT_EQ(vyse->counter_chance, 15);
    EXPECT_EQ(vyse->attack, 43);
    EXPECT_TRUE(vyse->attack_known);
    EXPECT_FALSE(vyse->defense_known);

    const auto aika = first_battle_actor_by_progress_name("Aika");
    ASSERT_TRUE(aika.has_value());
    EXPECT_EQ(aika->slot, 1);
    EXPECT_EQ(aika->quick, 24);
    EXPECT_EQ(aika->attack, 36);
    EXPECT_EQ(aika->hit, 110);

    const auto soldier = first_battle_actor_by_progress_name("[5]Soldier");
    ASSERT_TRUE(soldier.has_value());
    EXPECT_EQ(soldier->kind, FirstBattleActorKind::Soldier);
    EXPECT_EQ(soldier->slot, 5);
    EXPECT_EQ(soldier->max_hp, 58);
    EXPECT_EQ(soldier->attack, 43);
    EXPECT_EQ(soldier->defense, 42);
    EXPECT_EQ(soldier->dodge, 15);
    EXPECT_TRUE(soldier->defense_known);
    EXPECT_EQ(first_battle_base_counter_chance_for_actor_name("[4]Soldier"), 10);
    EXPECT_EQ(first_battle_base_agile_for_actor_name("Vyse"), 11);
}

TEST(SavorPredictRngModel, FirstBattleStaticBasicAttackInputsAreAvailableForPcVsSoldier) {
    const auto vyse_vs_soldier = first_battle_static_basic_attack_inputs("Vyse", "[4]Soldier", 0);
    ASSERT_TRUE(vyse_vs_soldier.has_value());
    EXPECT_EQ(vyse_vs_soldier->attacker_attack, 43);
    EXPECT_EQ(vyse_vs_soldier->attacker_hit, 90);
    EXPECT_EQ(vyse_vs_soldier->attacker_agile, 11);
    EXPECT_EQ(vyse_vs_soldier->target_defense, 42);
    EXPECT_EQ(vyse_vs_soldier->target_dodge, 15);
    EXPECT_EQ(vyse_vs_soldier->target_element_effectiveness_tenths, 10);

    const auto aika_vs_soldier = first_battle_static_basic_attack_inputs("Aika", "[5]Soldier", 0);
    ASSERT_TRUE(aika_vs_soldier.has_value());
    EXPECT_EQ(aika_vs_soldier->attacker_attack, 36);
    EXPECT_EQ(aika_vs_soldier->attacker_hit, 110);
    EXPECT_EQ(aika_vs_soldier->attacker_agile, 22);

    EXPECT_FALSE(first_battle_static_basic_attack_inputs("[5]Soldier", "Vyse", 0).has_value());
}

TEST(SavorPredictRngModel, RollDamageUsesSpreadBonusElementAndGuard) {
    EXPECT_EQ(roll_damage_from_draws(100, 0, 10, 0, 1, false), 95);
    EXPECT_EQ(roll_damage_from_draws(100, 0, 10, 0, 0, false), 96);
    EXPECT_EQ(roll_damage_from_draws(100, 0, 5, 0, 0, false), 48);
    EXPECT_EQ(roll_damage_from_draws(100, 0, 10, 0, 0, true), 48);
    EXPECT_EQ(roll_damage_from_draws(-10, 0, 10, 0, 0, false), 1);
}

TEST(SavorPredictRngModel, BasicAttackBurstSpendsCritOnlyForParamZero) {
    BasicAttackInputs inputs;
    inputs.attacker_attack = 50;
    inputs.attacker_hit = 200;
    inputs.attacker_agile = 100;
    inputs.attacker_element = 0;
    inputs.target_defense = 20;
    inputs.target_dodge = 0;
    inputs.target_element_effectiveness_tenths = 10;
    inputs.instr_param_0x6 = 0;

    const auto with_crit_gate = simulate_basic_attack_burst(0, inputs);
    EXPECT_EQ(with_crit_gate.draws_consumed, 4);
    EXPECT_TRUE(with_crit_gate.crit_draw_spent);
    EXPECT_TRUE(with_crit_gate.crit_rand.has_value());
    EXPECT_TRUE(with_crit_gate.damage_draws_spent);
    EXPECT_EQ(with_crit_gate.attack_result, 2);
    EXPECT_EQ(with_crit_gate.base_damage, 100);
    EXPECT_GT(with_crit_gate.damage, 0);

    inputs.instr_param_0x6 = 1;
    const auto without_crit_gate = simulate_basic_attack_burst(0, inputs);
    EXPECT_EQ(without_crit_gate.draws_consumed, 3);
    EXPECT_FALSE(without_crit_gate.crit_draw_spent);
    EXPECT_FALSE(without_crit_gate.crit_rand.has_value());
    EXPECT_TRUE(without_crit_gate.damage_draws_spent);
    EXPECT_EQ(without_crit_gate.attack_result, 1);
    EXPECT_EQ(without_crit_gate.base_damage, 80);
    EXPECT_GT(without_crit_gate.damage, 0);
}

TEST(SavorPredictRngModel, BasicAttackBurstMissSkipsDamageDraws) {
    BasicAttackInputs inputs;
    inputs.attacker_attack = 50;
    inputs.attacker_hit = 0;
    inputs.attacker_agile = 100;
    inputs.target_defense = 20;
    inputs.target_dodge = 100;
    inputs.instr_param_0x6 = 0;

    const auto result = simulate_basic_attack_burst(0, inputs);
    EXPECT_EQ(result.draws_consumed, 1);
    EXPECT_FALSE(result.crit_draw_spent);
    EXPECT_FALSE(result.damage_draws_spent);
    EXPECT_EQ(result.attack_result, 0);
    EXPECT_EQ(result.hit_check, 0);
    EXPECT_EQ(result.damage, 0);
}

TEST(SavorPredictRngModel, FirstBattleActionViewCameraExpectationCountsObservedAttacks) {
    ParsedProgressEvents events;
    events.attacks.push_back(AttackEvent{"Aika", "[4]Soldier", 30});
    events.attacks.push_back(AttackEvent{"Vyse", "[4]Soldier", 43});
    events.attacks.push_back(AttackEvent{"[5]Soldier", "Vyse", 46});

    const auto expectation = first_battle_action_view_camera_expectation(events);

    EXPECT_EQ(expectation.observed_attack_events, 3);
    EXPECT_EQ(expectation.expected_mode0e_camera_draws, 3);
    EXPECT_EQ(expectation.expected_owner, std::string_view("mode0e_action_view_camera"));
    EXPECT_EQ(expectation.expected_pc, std::string_view("80052BF0"));
    EXPECT_EQ(expectation.rejected_fallback_owner, std::string_view("mode0_action_view_camera_fallback"));
    EXPECT_EQ(expectation.rejected_fallback_pc, std::string_view("800513D4"));
    EXPECT_NE(std::string_view(first_battle_action_view_camera_rule_detail()).find("mode 0xe"), std::string_view::npos);
}

TEST(SavorPredictRngModel, FirstBattleAttackResolutionCheckpointExpectationCountsDamageEvents) {
    ParsedProgressEvents events;
    events.attacks.push_back(AttackEvent{"Aika", "[4]Soldier", 30});
    events.attacks.push_back(AttackEvent{"Vyse", "[4]Soldier", 43});
    events.attacks.push_back(AttackEvent{"[5]Soldier", "Vyse", 46});

    const auto expectation = first_battle_attack_resolution_checkpoint_expectation(events, 2);

    EXPECT_EQ(expectation.observed_attack_events, 3);
    EXPECT_EQ(expectation.expected_hit_draws, 3);
    EXPECT_EQ(expectation.expected_damage_spread_draws, 3);
    EXPECT_EQ(expectation.expected_damage_bonus_draws, 3);
    ASSERT_TRUE(expectation.expected_crit_draws.has_value());
    EXPECT_EQ(*expectation.expected_crit_draws, 2);
    EXPECT_EQ(expectation.hit_owner, std::string_view("attack_hit_dodge"));
    EXPECT_EQ(expectation.crit_owner, std::string_view("attack_critical"));
    EXPECT_EQ(expectation.damage_spread_owner, std::string_view("damage_spread"));
    EXPECT_EQ(expectation.damage_bonus_owner, std::string_view("damage_low_bit_bonus"));
    EXPECT_NE(
        std::string_view(first_battle_attack_resolution_checkpoint_rule_detail()).find("damage event"),
        std::string_view::npos);
}

TEST(SavorPredictRngModel, FirstBattleSoldierDropSimulationMatchesEnabledRows) {
    std::optional<std::uint32_t> electri_seed;
    std::optional<std::uint32_t> moonberry_seed;
    std::optional<std::uint32_t> no_drop_seed;

    for (std::uint32_t seed = 0; seed < 2000000u; ++seed) {
        const auto result = simulate_first_battle_soldier_drop(seed);
        if (result.drop == FirstBattleSoldierDropKind::ElectriBox && !electri_seed.has_value()) {
            electri_seed = seed;
        } else if (result.drop == FirstBattleSoldierDropKind::Moonberry && !moonberry_seed.has_value()) {
            moonberry_seed = seed;
        } else if (result.drop == FirstBattleSoldierDropKind::None && !no_drop_seed.has_value()) {
            no_drop_seed = seed;
        }

        if (electri_seed.has_value() && moonberry_seed.has_value() && no_drop_seed.has_value()) {
            break;
        }
    }

    ASSERT_TRUE(electri_seed.has_value());
    ASSERT_TRUE(moonberry_seed.has_value());
    ASSERT_TRUE(no_drop_seed.has_value());

    const auto electri = simulate_first_battle_soldier_drop(*electri_seed);
    EXPECT_EQ(electri.draws_consumed, 1);
    EXPECT_EQ(electri.drop, FirstBattleSoldierDropKind::ElectriBox);
    EXPECT_EQ(electri.item_id, 273);
    EXPECT_EQ(electri.amount, 1);
    EXPECT_FALSE(electri.second_roll.has_value());
    EXPECT_EQ(first_battle_soldier_drop_name(electri.drop), std::string("Electri Box"));

    const auto moonberry = simulate_first_battle_soldier_drop(*moonberry_seed);
    EXPECT_EQ(moonberry.draws_consumed, 2);
    EXPECT_EQ(moonberry.drop, FirstBattleSoldierDropKind::Moonberry);
    EXPECT_EQ(moonberry.item_id, 258);
    EXPECT_EQ(moonberry.amount, 1);
    ASSERT_TRUE(moonberry.second_roll.has_value());
    EXPECT_NE(moonberry.first_roll % 100, 0);
    EXPECT_EQ(*moonberry.second_roll % 100, 0);
    EXPECT_EQ(first_battle_soldier_drop_name(moonberry.drop), std::string("Moonberry"));

    const auto no_drop = simulate_first_battle_soldier_drop(*no_drop_seed);
    EXPECT_EQ(no_drop.draws_consumed, 2);
    EXPECT_EQ(no_drop.drop, FirstBattleSoldierDropKind::None);
    EXPECT_EQ(no_drop.item_id, -1);
    EXPECT_EQ(no_drop.amount, 0);
    ASSERT_TRUE(no_drop.second_roll.has_value());
    EXPECT_NE(no_drop.first_roll % 100, 0);
    EXPECT_NE(*no_drop.second_roll % 100, 0);
}

TEST(SavorPredictRngModel, FirstBattleDropCheckpointExpectationUsesDropDrawCount) {
    const auto expectation = first_battle_drop_checkpoint_expectation(2);

    EXPECT_EQ(expectation.expected_drop_rolls, 2);
    EXPECT_EQ(expectation.owner, std::string_view("enemy_drop_roll"));
    EXPECT_EQ(expectation.pc, std::string_view("8002BAE8"));
    EXPECT_NE(
        std::string_view(first_battle_drop_checkpoint_rule_detail()).find("enemyDropItem_8002ba8c"),
        std::string_view::npos);
}

TEST(SavorPredictRngModel, FirstBattleOutcomeCheckpointExpectationSkipsReachedNextTurnCleanup) {
    const auto reached_next_turn = first_battle_outcome_checkpoint_expectation(6);

    ASSERT_TRUE(reached_next_turn.expected_end_turn_status_draws.has_value());
    EXPECT_EQ(*reached_next_turn.expected_end_turn_status_draws, 0);
    ASSERT_TRUE(reached_next_turn.expected_level_up_stat_rolls.has_value());
    EXPECT_EQ(*reached_next_turn.expected_level_up_stat_rolls, 0);
    EXPECT_EQ(reached_next_turn.end_turn_owner, std::string_view("end_turn_status_cleanup"));
    EXPECT_EQ(reached_next_turn.end_turn_pc, std::string_view("8006FF38"));

    const auto victory = first_battle_outcome_checkpoint_expectation(0);
    EXPECT_FALSE(victory.expected_end_turn_status_draws.has_value());
    EXPECT_FALSE(victory.expected_level_up_stat_rolls.has_value());
    EXPECT_NE(
        std::string_view(first_battle_outcome_checkpoint_rule_detail()).find("ReachedNextTurn"),
        std::string_view::npos);
}

TEST(SavorPredictRngModel, CounterCheckSuppressesWithoutDrawBeforeChanceRoll) {
    CounterInputs inputs;
    inputs.attacker_slot = 0;
    inputs.target_slot = 4;
    inputs.target_movement_flags = 0xC0;
    inputs.target_base_counter_chance = 10;
    inputs.target_current_counter_chance = 10;

    inputs.target_status_flags = 0x100;
    auto result = simulate_counter_check(0x12345678u, inputs);
    EXPECT_FALSE(result.counter);
    EXPECT_EQ(result.draws_consumed, 0);
    EXPECT_EQ(result.reason, CounterResultReason::StatusSuppressed);

    inputs.target_status_flags = 0;
    inputs.attacker_slot = 5;
    result = simulate_counter_check(0x12345678u, inputs);
    EXPECT_FALSE(result.counter);
    EXPECT_EQ(result.draws_consumed, 0);
    EXPECT_EQ(result.reason, CounterResultReason::SameSide);

    inputs.attacker_slot = 0;
    inputs.attack_was_critical = true;
    result = simulate_counter_check(0x12345678u, inputs);
    EXPECT_FALSE(result.counter);
    EXPECT_EQ(result.draws_consumed, 0);
    EXPECT_EQ(result.reason, CounterResultReason::CriticalSuppressed);
}

TEST(SavorPredictRngModel, CounterCheckConsumesOneDrawForNormalChanceGate) {
    std::optional<std::uint32_t> success_seed;
    std::optional<std::uint32_t> failure_seed;
    for (std::uint32_t seed = 0; seed < 2000000u; ++seed) {
        CounterInputs inputs;
        inputs.attacker_slot = 0;
        inputs.target_slot = 4;
        inputs.target_movement_flags = 0xC0;
        inputs.target_base_counter_chance = 10;
        inputs.target_current_counter_chance = 10;
        const auto result = simulate_counter_check(seed, inputs);
        if (result.counter && !success_seed.has_value()) {
            success_seed = seed;
        } else if (!result.counter && result.draws_consumed == 1 && !failure_seed.has_value()) {
            failure_seed = seed;
        }

        if (success_seed.has_value() && failure_seed.has_value()) {
            break;
        }
    }

    ASSERT_TRUE(success_seed.has_value());
    ASSERT_TRUE(failure_seed.has_value());

    CounterInputs inputs;
    inputs.attacker_slot = 0;
    inputs.target_slot = 4;
    inputs.target_movement_flags = 0xC0;
    inputs.target_base_counter_chance = 10;
    inputs.target_current_counter_chance = 10;

    const auto success = simulate_counter_check(*success_seed, inputs);
    EXPECT_TRUE(success.counter);
    EXPECT_EQ(success.draws_consumed, 1);
    ASSERT_TRUE(success.counter_rand.has_value());
    EXPECT_LT(*success.counter_rand % 100, 10);
    EXPECT_EQ(success.queued_field7_0xc, 0);
    EXPECT_EQ(success.updated_current_counter_chance, 0);
    EXPECT_EQ(counter_result_reason_name(success.reason), std::string("Counter"));

    const auto failure = simulate_counter_check(*failure_seed, inputs);
    EXPECT_FALSE(failure.counter);
    EXPECT_EQ(failure.draws_consumed, 1);
    ASSERT_TRUE(failure.counter_rand.has_value());
    EXPECT_GE(*failure.counter_rand % 100, 10);
    EXPECT_EQ(failure.reason, CounterResultReason::RandomFailed);
}

TEST(SavorPredictRngModel, CounterCheckHandlesForcedAndMovementSuppressedCases) {
    CounterInputs inputs;
    inputs.attacker_slot = 0;
    inputs.target_slot = 4;
    inputs.target_status_flags = 0x2;
    inputs.target_movement_flags = 0xC0;
    inputs.target_base_counter_chance = 0;
    inputs.target_current_counter_chance = 0;

    auto result = simulate_counter_check(0x12345678u, inputs);
    EXPECT_TRUE(result.counter);
    EXPECT_EQ(result.draws_consumed, 0);
    EXPECT_EQ(result.updated_current_counter_chance, 100);

    inputs.target_status_flags = 0;
    inputs.target_base_counter_chance = 10;
    inputs.target_current_counter_chance = 100;
    inputs.attacker_action_marker = 7;
    inputs.target_movement_flags = 0x40;

    result = simulate_counter_check(0x12345678u, inputs);
    EXPECT_FALSE(result.counter);
    EXPECT_EQ(result.draws_consumed, 1);
    EXPECT_EQ(result.reason, CounterResultReason::MovementFlagSuppressed);
    EXPECT_FALSE(result.queued_field7_0xc.has_value());
}

TEST(SavorPredictRngModel, FirstBattleCounterCheckpointExpectationUsesCandidateCeiling) {
    const auto expectation = first_battle_counter_checkpoint_expectation(2);

    EXPECT_EQ(expectation.expected_counter_roll_ceiling, 2);
    EXPECT_EQ(expectation.owner, std::string_view("counter_roll"));
    EXPECT_EQ(expectation.pc, std::string_view("80081A88"));
    EXPECT_NE(
        std::string_view(first_battle_counter_checkpoint_rule_detail()).find("bounded by nonlethal"),
        std::string_view::npos);
}

TEST(SavorPredictRngModel, EnemyAttackSetupGateSpendsDrawForFirstBattleSoldierFlags) {
    EnemyAttackSetupInputs inputs;
    inputs.queued_instruction = 3;
    inputs.movement_flags = 0x0FC7;

    const auto result = simulate_enemy_attack_setup_gate(0, inputs);

    EXPECT_EQ(result.draws_consumed, 1);
    ASSERT_TRUE(result.setup_rand.has_value());
    ASSERT_TRUE(result.setup_rand_mod10.has_value());
    EXPECT_EQ(*result.setup_rand_mod10, *result.setup_rand % 10);
    EXPECT_EQ(result.path, EnemyAttackSetupPath::TargetAdjacencySetup);
    EXPECT_FALSE(result.direct_close_branch_candidate);
    EXPECT_EQ(enemy_attack_setup_path_name(result.path), std::string("TargetAdjacencySetup"));
}

TEST(SavorPredictRngModel, EnemyAttackSetupGateReportsDirectCloseCandidate) {
    std::optional<std::uint32_t> direct_seed;
    for (std::uint32_t seed = 0; seed < 2000000u; ++seed) {
        EnemyAttackSetupInputs inputs;
        inputs.queued_instruction = 3;
        inputs.movement_flags = 0x0FC7;
        const auto result = simulate_enemy_attack_setup_gate(seed, inputs);
        if (result.direct_close_branch_candidate) {
            direct_seed = seed;
            break;
        }
    }

    ASSERT_TRUE(direct_seed.has_value());
    EnemyAttackSetupInputs inputs;
    inputs.queued_instruction = 3;
    inputs.movement_flags = 0x0FC7;
    const auto result = simulate_enemy_attack_setup_gate(*direct_seed, inputs);

    EXPECT_EQ(result.draws_consumed, 1);
    ASSERT_TRUE(result.setup_rand_mod10.has_value());
    EXPECT_GT(*result.setup_rand_mod10, 3);
    EXPECT_EQ(result.path, EnemyAttackSetupPath::DirectCloseSetupCandidate);
    EXPECT_TRUE(result.direct_close_branch_candidate);
}

TEST(SavorPredictRngModel, EnemyAttackSetupGateSkipsForNonAttackOrMissingMovementRandGate) {
    EnemyAttackSetupInputs inputs;
    inputs.queued_instruction = 4;
    inputs.movement_flags = 0x0FC7;
    auto result = simulate_enemy_attack_setup_gate(0x12345678u, inputs);
    EXPECT_EQ(result.draws_consumed, 0);
    EXPECT_EQ(result.path, EnemyAttackSetupPath::NotAttack);

    inputs.queued_instruction = 3;
    inputs.movement_flags = 0x0040;
    result = simulate_enemy_attack_setup_gate(0x12345678u, inputs);
    EXPECT_EQ(result.draws_consumed, 0);
    EXPECT_EQ(result.path, EnemyAttackSetupPath::NoRandomSetupDraw);

    inputs.movement_flags = 0x00E0;
    result = simulate_enemy_attack_setup_gate(0x12345678u, inputs);
    EXPECT_EQ(result.draws_consumed, 0);
    EXPECT_EQ(result.path, EnemyAttackSetupPath::TargetAdjacencySetup);
}

TEST(SavorPredictProgressParser, ParsesPlannedActionsPredicateAndCombatEvents) {
    const std::vector<std::string> messages = {
        "worker=24 progress=Vyse: Attack->[4]Soldier\nAika: Attack->[4]Soldier\n[4]Soldier: Defend\n[5]Soldier: Attack->Vyse",
        "worker=24 progress=Pred(Passed): PCs act before ECs - 1 < 2",
        "worker=24 progress=Aika attacks [4]Soldier for 15 damage",
        "worker=24 progress=[4]Soldier died",
        "worker=24 progress=[4]Soldier dropped Sacri Crystal"};

    const auto parsed = parse_progress_events(messages);
    ASSERT_TRUE(parsed.planned_actions.has_value());
    EXPECT_EQ(parsed.planned_actions->soldier4.kind, PlannedActionKind::Defend);
    EXPECT_EQ(parsed.planned_actions->soldier5.kind, PlannedActionKind::Attack);
    EXPECT_EQ(parsed.planned_actions->soldier5.target, "Vyse");

    ASSERT_TRUE(parsed.pc_before_ec_predicate.has_value());
    EXPECT_EQ(parsed.pc_before_ec_predicate->lhs, 1);
    EXPECT_EQ(parsed.pc_before_ec_predicate->rhs, 2);

    ASSERT_EQ(parsed.attacks.size(), 1u);
    EXPECT_EQ(parsed.attacks[0].actor, "Aika");
    EXPECT_EQ(parsed.attacks[0].target, "[4]Soldier");
    EXPECT_EQ(parsed.attacks[0].damage, 15);

    ASSERT_EQ(parsed.deaths.size(), 1u);
    EXPECT_EQ(parsed.deaths[0], "[4]Soldier");

    ASSERT_EQ(parsed.drops.size(), 1u);
    EXPECT_EQ(parsed.drops[0].target, "[4]Soldier");
    EXPECT_EQ(parsed.drops[0].drop, "Sacri Crystal");

    ASSERT_EQ(parsed.ordered_combat_events.size(), 3u);
    EXPECT_EQ(parsed.ordered_combat_events[0].kind, CombatEventKind::Attack);
    EXPECT_EQ(parsed.ordered_combat_events[0].attack.actor, "Aika");
    EXPECT_EQ(parsed.ordered_combat_events[1].kind, CombatEventKind::Death);
    EXPECT_EQ(parsed.ordered_combat_events[1].target, "[4]Soldier");
    EXPECT_EQ(parsed.ordered_combat_events[2].kind, CombatEventKind::Drop);
    EXPECT_EQ(parsed.ordered_combat_events[2].target, "[4]Soldier");
    EXPECT_EQ(parsed.ordered_combat_events[2].drop, "Sacri Crystal");
}

TEST(SavorPredictProgressParser, SoldierActionExecutionInfersDeathPreventedPlannedAttack) {
    ParsedProgressEvents events;
    PlannedTurnActions actions;
    actions.vyse = parse_planned_action("Attack->[4]Soldier");
    actions.aika = parse_planned_action("Attack->[4]Soldier");
    actions.soldier4 = parse_planned_action("Attack->Vyse");
    actions.soldier5 = parse_planned_action("Attack->Vyse");
    events.planned_actions = actions;

    const AttackEvent aika_attack{"Aika", "[4]Soldier", 30};
    const AttackEvent vyse_attack{"Vyse", "[4]Soldier", 43};
    const AttackEvent soldier5_attack{"[5]Soldier", "Vyse", 46};
    events.ordered_combat_events.push_back(CombatEvent{CombatEventKind::Attack, aika_attack, {}, {}});
    events.ordered_combat_events.push_back(CombatEvent{CombatEventKind::Attack, vyse_attack, {}, {}});
    events.ordered_combat_events.push_back(CombatEvent{CombatEventKind::Death, {}, "[4]Soldier", {}});
    events.ordered_combat_events.push_back(CombatEvent{CombatEventKind::Drop, {}, "[4]Soldier", "[273]Electri Box x1"});
    events.ordered_combat_events.push_back(CombatEvent{CombatEventKind::Attack, soldier5_attack, {}, {}});

    TurnOrderSimulation turn_order;
    turn_order.execution_order_exact = true;
    turn_order.execution_slots = {1, 0, 4, 5};

    const auto summary = analyze_first_battle_soldier_action_execution(events, turn_order);

    EXPECT_EQ(summary.planned_attack_count, 2);
    EXPECT_EQ(summary.reached_execution_count, 1);
    EXPECT_EQ(summary.death_prevented_count, 1);
    EXPECT_EQ(summary.unresolved_planned_attack_count, 0);
    ASSERT_EQ(summary.soldiers.size(), 2u);

    EXPECT_EQ(summary.soldiers[0].slot, 4);
    EXPECT_EQ(summary.soldiers[0].status, SoldierActionExecutionStatus::PreventedByDeathBeforeAction);
    ASSERT_TRUE(summary.soldiers[0].turn_order_rank.has_value());
    EXPECT_EQ(*summary.soldiers[0].turn_order_rank, 2);
    ASSERT_TRUE(summary.soldiers[0].death_event_order.has_value());
    EXPECT_EQ(*summary.soldiers[0].death_event_order, 2);
    EXPECT_FALSE(summary.soldiers[0].attack_event_order.has_value());

    EXPECT_EQ(summary.soldiers[1].slot, 5);
    EXPECT_EQ(summary.soldiers[1].status, SoldierActionExecutionStatus::ReachedExecution);
    ASSERT_TRUE(summary.soldiers[1].turn_order_rank.has_value());
    EXPECT_EQ(*summary.soldiers[1].turn_order_rank, 3);
    ASSERT_TRUE(summary.soldiers[1].attack_event_order.has_value());
    EXPECT_EQ(*summary.soldiers[1].attack_event_order, 4);
    EXPECT_EQ(
        soldier_action_execution_status_name(summary.soldiers[1].status),
        std::string("ReachedExecution"));
}

TEST(SavorPredictCheckpointTrace, ParsesKnownRngOwnersAndSeeds) {
    std::istringstream input(
        "pc=80010bdc function=getAttackResult checkpoint=hit rng_draw_index_before=12 "
        "rng_seed_before=0x12345678 rng_seed_after=0x456789AB active_slot=1 target_slot=4\n"
        "pc=80010958 function=rollDamage checkpoint=spread rng_draw_index_before=13 "
        "rng_seed_before=0x456789AB rng_seed_after=0xAABBCCDD owns_rng_draw=true\n");

    const auto parsed = parse_checkpoint_stream(input);

    EXPECT_TRUE(parsed.errors.empty());
    EXPECT_TRUE(parsed.warnings.empty());
    ASSERT_EQ(parsed.events.size(), 2u);

    EXPECT_EQ(parsed.events[0].pc, "80010BDC");
    EXPECT_TRUE(parsed.events[0].owns_rng_draw);
    EXPECT_EQ(parsed.events[0].known_rng_owner, "attack_hit_dodge");
    ASSERT_TRUE(parsed.events[0].rng_draw_index_before.has_value());
    EXPECT_EQ(*parsed.events[0].rng_draw_index_before, 12);
    ASSERT_TRUE(parsed.events[0].rng_seed_before.has_value());
    EXPECT_EQ(*parsed.events[0].rng_seed_before, 0x12345678u);
    ASSERT_TRUE(parsed.events[0].target_slot.has_value());
    EXPECT_EQ(*parsed.events[0].target_slot, 4);

    EXPECT_EQ(parsed.events[1].pc, "80010958");
    EXPECT_EQ(parsed.events[1].known_rng_owner, "damage_spread");
    ASSERT_TRUE(parsed.events[1].rng_seed_after.has_value());
    EXPECT_EQ(*parsed.events[1].rng_seed_after, 0xAABBCCDDu);
}

TEST(SavorPredictCheckpointTrace, SummarizesActionSourceCheckpoints) {
    const auto expectation = first_battle_action_source_checkpoint_expectation();
    ASSERT_TRUE(expectation.expected_handler_pc.has_value());
    EXPECT_EQ(*expectation.expected_handler_pc, "800662BC");

    std::istringstream input(
        "pc=8006721c function=FUN_8006721c checkpoint=action_source rng_draw_index_before=18 "
        "actor_slot=1 source_slot=1 target_slot=4 action_id=4 "
        "source_field6_0x6=14 actor_field6_0x6=14 handler_pc=0x800662bc\n"
        "pc=8006721c function=FUN_8006721c checkpoint=action_source rng_draw_index_before=19 "
        "actor_slot=0 source_slot=0 target_slot=4 action_id=8 "
        "source_field6_0x6=14 actor_field6_0x6=14 selected_handler_pc=800662BC\n");

    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.events.size(), 2u);

    const auto matched = summarize_action_source_checkpoints(
        parsed.events,
        expectation.expected_handler_pc);
    EXPECT_EQ(matched.status, ActionSourceCheckpointStatus::MatchesExpected);
    EXPECT_EQ(matched.observed_action_source_events, 2);
    EXPECT_EQ(matched.events_with_actor_slot, 2);
    EXPECT_EQ(matched.events_with_source_slot, 2);
    EXPECT_EQ(matched.events_with_action_id, 2);
    EXPECT_EQ(matched.events_with_handler_pc, 2);
    EXPECT_EQ(matched.events_with_source_field6, 2);
    EXPECT_EQ(matched.events_with_actor_field6, 2);
    EXPECT_EQ(matched.field6_matches, 2);
    EXPECT_EQ(matched.handler_matches, 2);
    ASSERT_TRUE(matched.first_action_source_draw_index.has_value());
    EXPECT_EQ(*matched.first_action_source_draw_index, 18);
    ASSERT_EQ(matched.events.size(), 2u);
    ASSERT_TRUE(matched.events[0].handler_pc.has_value());
    EXPECT_EQ(*matched.events[0].handler_pc, "800662BC");

    std::istringstream mismatch_input(
        "pc=8006721c function=FUN_8006721c checkpoint=action_source rng_draw_index_before=20 "
        "actor_slot=1 source_slot=1 source_field6_0x6=14 actor_field6_0x6=15 handler_pc=800662BC\n");
    const auto mismatch_parsed = parse_checkpoint_stream(mismatch_input);
    ASSERT_TRUE(mismatch_parsed.errors.empty());
    const auto field_mismatch = summarize_action_source_checkpoints(
        mismatch_parsed.events,
        expectation.expected_handler_pc);
    EXPECT_EQ(field_mismatch.status, ActionSourceCheckpointStatus::Field6Mismatch);

    std::istringstream missing_input(
        "pc=8006721c function=FUN_8006721c checkpoint=action_source rng_draw_index_before=21 "
        "actor_slot=1 source_slot=1 source_field6_0x6=14 handler_pc=800662BC\n");
    const auto missing_parsed = parse_checkpoint_stream(missing_input);
    ASSERT_TRUE(missing_parsed.errors.empty());
    const auto missing = summarize_action_source_checkpoints(
        missing_parsed.events,
        expectation.expected_handler_pc);
    EXPECT_EQ(missing.status, ActionSourceCheckpointStatus::MissingLiveSourceFields);
    EXPECT_EQ(
        action_source_checkpoint_status_name(missing.status),
        std::string("MissingLiveSourceFields"));
}

TEST(SavorPredictCheckpointTrace, SummarizesActionViewGateCheckpoints) {
    std::istringstream input(
        "pc=80012f58 function=FUN_80012f58 checkpoint=action_view_gate rng_draw_index_before=18 "
        "active_slot=1 source_slot=1 target_slot=4 source_field6_0x6=14 actor_field6_0x6=14 "
        "aux_list_root=0x80346bd8 query_arg0=4 query_arg1=-1 query_arg2=0x2a query_arg3=3 "
        "query_result=0x81234567 selected_record_mode=0xe\n"
        "pc=80052bf0 function=FUN_80052b24 checkpoint=mode0e_camera rng_draw_index_before=19\n"
        "pc=80010bdc function=getAttackResult checkpoint=hit rng_draw_index_before=20\n");

    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.events.size(), 3u);

    const auto matched = summarize_action_view_gate_checkpoints(parsed.events);
    EXPECT_EQ(matched.status, ActionViewGateCheckpointStatus::MatchesExpected);
    EXPECT_EQ(matched.observed_gate_events, 1);
    EXPECT_EQ(matched.events_with_aux_list_root, 1);
    EXPECT_EQ(matched.events_with_query_args, 1);
    EXPECT_EQ(matched.events_with_query_result, 1);
    EXPECT_EQ(matched.events_with_selected_record_mode, 1);
    EXPECT_EQ(matched.query_args_match, 1);
    EXPECT_EQ(matched.selected_mode_matches, 1);
    EXPECT_EQ(matched.observed_mode0e_camera_draws, 1);
    EXPECT_EQ(matched.observed_attack_hit_draws, 1);
    EXPECT_EQ(matched.gate_events_before_first_mode0e, 1);
    EXPECT_EQ(matched.gate_events_before_first_attack_hit, 1);
    EXPECT_EQ(matched.mode0e_draws_before_first_attack_hit, 1);
    ASSERT_EQ(matched.events.size(), 1u);
    ASSERT_TRUE(matched.events[0].query_arg2.has_value());
    EXPECT_EQ(*matched.events[0].query_arg2, 0x2a);
    ASSERT_TRUE(matched.events[0].selected_record_mode.has_value());
    EXPECT_EQ(*matched.events[0].selected_record_mode, 0x0e);

    std::istringstream query_mismatch_input(
        "pc=80012f58 function=FUN_80012f58 checkpoint=action_view_gate rng_draw_index_before=18 "
        "aux_list_root=0x80346bd8 query_arg0=4 query_arg1=-1 query_arg2=0x2b query_arg3=3 "
        "query_result=0x81234567 selected_record_mode=0xe\n");
    const auto query_mismatch_parsed = parse_checkpoint_stream(query_mismatch_input);
    ASSERT_TRUE(query_mismatch_parsed.errors.empty());
    const auto query_mismatch = summarize_action_view_gate_checkpoints(query_mismatch_parsed.events);
    EXPECT_EQ(query_mismatch.status, ActionViewGateCheckpointStatus::QueryArgsMismatch);

    std::istringstream mode_mismatch_input(
        "pc=80012f58 function=FUN_80012f58 checkpoint=action_view_gate rng_draw_index_before=18 "
        "aux_list_root=0x80346bd8 query_arg0=4 query_arg1=-1 query_arg2=0x2a query_arg3=3 "
        "query_result=0x81234567 selected_record_mode=0\n");
    const auto mode_mismatch_parsed = parse_checkpoint_stream(mode_mismatch_input);
    ASSERT_TRUE(mode_mismatch_parsed.errors.empty());
    const auto mode_mismatch = summarize_action_view_gate_checkpoints(mode_mismatch_parsed.events);
    EXPECT_EQ(mode_mismatch.status, ActionViewGateCheckpointStatus::SelectedModeMismatch);

    std::istringstream missing_input(
        "pc=80012f58 function=FUN_80012f58 checkpoint=action_view_gate rng_draw_index_before=18 "
        "aux_list_root=0x80346bd8 query_arg0=4 query_arg1=-1 query_arg2=0x2a query_arg3=3\n");
    const auto missing_parsed = parse_checkpoint_stream(missing_input);
    ASSERT_TRUE(missing_parsed.errors.empty());
    const auto missing = summarize_action_view_gate_checkpoints(missing_parsed.events);
    EXPECT_EQ(missing.status, ActionViewGateCheckpointStatus::MissingLiveGateFields);
    EXPECT_EQ(
        action_view_gate_checkpoint_status_name(missing.status),
        std::string("MissingLiveGateFields"));
}

TEST(SavorPredictCheckpointTrace, SummarizesActionViewCameraCheckpoints) {
    std::istringstream input(
        "pc=80052bf0 function=FUN_80052b24 checkpoint=mode0e_camera rng_draw_index_before=20 "
        "rng_seed_before=0x12345678 rng_seed_after=0x456789AB\n"
        "pc=80010bdc function=getAttackResult checkpoint=hit rng_draw_index_before=21 "
        "rng_seed_before=0x456789AB rng_seed_after=0xAABBCCDD\n"
        "pc=800513d4 function=UpdateActionViewRecord checkpoint=mode0_fallback rng_draw_index_before=22 "
        "rng_seed_before=0xAABBCCDD rng_seed_after=0x01020304\n");

    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.events.size(), 3u);
    EXPECT_EQ(parsed.events[0].known_rng_owner, "mode0e_action_view_camera");
    EXPECT_EQ(parsed.events[2].known_rng_owner, "mode0_action_view_camera_fallback");

    const std::vector<CheckpointEvent> no_fallback(parsed.events.begin(), parsed.events.begin() + 2);
    const auto matched = summarize_action_view_camera_checkpoints(no_fallback, 1);
    EXPECT_EQ(matched.status, ActionViewCameraCheckpointStatus::MatchesExpected);
    EXPECT_EQ(matched.observed_mode0e_camera_draws, 1);
    EXPECT_EQ(matched.observed_mode0_fallback_draws, 0);
    EXPECT_EQ(matched.observed_attack_hit_draws, 1);
    ASSERT_TRUE(matched.first_mode0e_draw_index.has_value());
    EXPECT_EQ(*matched.first_mode0e_draw_index, 20);
    ASSERT_TRUE(matched.first_attack_hit_draw_index.has_value());
    EXPECT_EQ(*matched.first_attack_hit_draw_index, 21);
    EXPECT_EQ(matched.mode0e_draws_before_first_attack_hit, 1);
    EXPECT_EQ(matched.mode0e_draws_after_first_attack_hit, 0);

    const auto with_fallback = summarize_action_view_camera_checkpoints(parsed.events, 1);
    EXPECT_EQ(with_fallback.status, ActionViewCameraCheckpointStatus::UnexpectedMode0FallbackObserved);
    EXPECT_EQ(with_fallback.observed_mode0_fallback_draws, 1);
    EXPECT_EQ(
        action_view_camera_checkpoint_status_name(with_fallback.status),
        std::string("UnexpectedMode0FallbackObserved"));

    const auto missing = summarize_action_view_camera_checkpoints(no_fallback, 2);
    EXPECT_EQ(missing.status, ActionViewCameraCheckpointStatus::MissingMode0eDraws);
}

TEST(SavorPredictCheckpointTrace, SummarizesTurnOrderCheckpoints) {
    std::istringstream input(
        "pc=800711f8 function=setupTurn checkpoint=priority rng_draw_index_before=10 "
        "active_slot=0 quick=22 assigned_priority=27 rand_value=5\n"
        "pc=800711f8 function=setupTurn checkpoint=priority rng_draw_index_before=11 "
        "active_slot=1 quick=24 assigned_priority=30 rand_value=6\n"
        "pc=800711f8 function=setupTurn checkpoint=priority rng_draw_index_before=12 "
        "slot=4 quick=18 assigned_priority=21 rand_value=3\n");

    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.events.size(), 3u);
    EXPECT_EQ(parsed.events[0].known_rng_owner, "turn_order_priority_jitter");

    const auto matched = summarize_turn_order_checkpoints(parsed.events, 3);
    EXPECT_EQ(matched.status, TurnOrderCheckpointStatus::MatchesExpected);
    EXPECT_EQ(matched.observed_priority_jitter_draws, 3);
    ASSERT_TRUE(matched.first_priority_jitter_draw_index.has_value());
    EXPECT_EQ(*matched.first_priority_jitter_draw_index, 10);
    ASSERT_TRUE(matched.last_priority_jitter_draw_index.has_value());
    EXPECT_EQ(*matched.last_priority_jitter_draw_index, 12);
    EXPECT_EQ(matched.draws_with_slot, 3);
    EXPECT_EQ(matched.draws_with_quick, 3);
    EXPECT_EQ(matched.draws_with_assigned_priority, 3);
    EXPECT_EQ(matched.draws_with_rand_value, 3);
    ASSERT_EQ(matched.draws.size(), 3u);
    ASSERT_TRUE(matched.draws[2].slot.has_value());
    EXPECT_EQ(*matched.draws[2].slot, 4);

    const auto missing = summarize_turn_order_checkpoints(parsed.events, 4);
    EXPECT_EQ(missing.status, TurnOrderCheckpointStatus::MissingPriorityJitterDraws);

    const auto extra = summarize_turn_order_checkpoints(parsed.events, 2);
    EXPECT_EQ(extra.status, TurnOrderCheckpointStatus::ExtraPriorityJitterDraws);
    EXPECT_EQ(
        turn_order_checkpoint_status_name(extra.status),
        std::string("ExtraPriorityJitterDraws"));
}

TEST(SavorPredictCheckpointTrace, SummarizesAttackResolutionCheckpoints) {
    std::istringstream input(
        "pc=80010bdc function=getAttackResult checkpoint=hit rng_draw_index_before=20\n"
        "pc=80010c44 function=getAttackResult checkpoint=crit rng_draw_index_before=21\n"
        "pc=80010958 function=rollDamage checkpoint=spread rng_draw_index_before=22\n"
        "pc=80010984 function=rollDamage checkpoint=bonus rng_draw_index_before=23\n"
        "pc=80010bdc function=getAttackResult checkpoint=hit rng_draw_index_before=24\n"
        "pc=80010958 function=rollDamage checkpoint=spread rng_draw_index_before=25\n"
        "pc=80010984 function=rollDamage checkpoint=bonus rng_draw_index_before=26\n");

    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.events.size(), 7u);
    EXPECT_EQ(parsed.events[0].known_rng_owner, "attack_hit_dodge");
    EXPECT_EQ(parsed.events[1].known_rng_owner, "attack_critical");
    EXPECT_EQ(parsed.events[2].known_rng_owner, "damage_spread");
    EXPECT_EQ(parsed.events[3].known_rng_owner, "damage_low_bit_bonus");

    const auto matched = summarize_attack_resolution_checkpoints(parsed.events, 2, 1);
    EXPECT_EQ(matched.status, AttackResolutionCheckpointStatus::MatchesExpected);
    EXPECT_EQ(matched.observed_hit_draws, 2);
    EXPECT_EQ(matched.observed_crit_draws, 1);
    EXPECT_EQ(matched.observed_damage_spread_draws, 2);
    EXPECT_EQ(matched.observed_damage_bonus_draws, 2);
    ASSERT_TRUE(matched.first_hit_draw_index.has_value());
    EXPECT_EQ(*matched.first_hit_draw_index, 20);
    ASSERT_TRUE(matched.first_damage_spread_draw_index.has_value());
    EXPECT_EQ(*matched.first_damage_spread_draw_index, 22);
    EXPECT_EQ(matched.damage_pairs_in_order, 2);
    EXPECT_EQ(matched.damage_pairs_out_of_order, 0);

    const auto missing = summarize_attack_resolution_checkpoints(parsed.events, 3, 1);
    EXPECT_EQ(missing.status, AttackResolutionCheckpointStatus::MissingHitDraws);

    const auto crit_mismatch = summarize_attack_resolution_checkpoints(parsed.events, 2, 2);
    EXPECT_EQ(crit_mismatch.status, AttackResolutionCheckpointStatus::CritDrawCountMismatch);
    EXPECT_EQ(
        attack_resolution_checkpoint_status_name(crit_mismatch.status),
        std::string("CritDrawCountMismatch"));

    std::istringstream out_of_order_input(
        "pc=80010bdc function=getAttackResult checkpoint=hit rng_draw_index_before=20\n"
        "pc=80010984 function=rollDamage checkpoint=bonus rng_draw_index_before=21\n"
        "pc=80010958 function=rollDamage checkpoint=spread rng_draw_index_before=22\n");
    const auto out_of_order = parse_checkpoint_stream(out_of_order_input);
    ASSERT_TRUE(out_of_order.errors.empty());
    const auto order_summary = summarize_attack_resolution_checkpoints(out_of_order.events, 1, 0);
    EXPECT_EQ(order_summary.status, AttackResolutionCheckpointStatus::DamagePairOrderMismatch);
    EXPECT_EQ(order_summary.damage_pairs_out_of_order, 1);
}

TEST(SavorPredictCheckpointTrace, SummarizesCritGateCheckpoints) {
    std::istringstream input(
        "pc=80010bdc function=getAttackResult checkpoint=hit rng_draw_index_before=20 "
        "active_slot=1 target_slot=4 instr_param_0x6=0 hit_success=1 rand_value=12\n"
        "pc=80010c44 function=getAttackResult checkpoint=crit rng_draw_index_before=21\n"
        "pc=80010bdc function=getAttackResult checkpoint=hit rng_draw_index_before=22 "
        "active_slot=5 target_slot=0 instr_param_0x6=1 hit_success=1 rand_value=30\n"
        "pc=80010bdc function=getAttackResult checkpoint=hit rng_draw_index_before=23 "
        "active_slot=0 target_slot=4 instr_param_0x6=0 hit_success=0 rand_value=99\n");

    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.events.size(), 4u);
    EXPECT_EQ(parsed.events[0].known_rng_owner, "attack_hit_dodge");
    EXPECT_EQ(parsed.events[1].known_rng_owner, "attack_critical");

    const auto matched = summarize_crit_gate_checkpoints(parsed.events);
    EXPECT_EQ(matched.status, CritGateCheckpointStatus::MatchesLiveGate);
    EXPECT_EQ(matched.observed_hit_draws, 3);
    EXPECT_EQ(matched.observed_crit_draws, 1);
    EXPECT_EQ(matched.hit_draws_with_instr_param, 3);
    EXPECT_EQ(matched.hit_draws_with_hit_success, 3);
    ASSERT_TRUE(matched.expected_crit_draws_from_live_gate.has_value());
    EXPECT_EQ(*matched.expected_crit_draws_from_live_gate, 1);
    ASSERT_TRUE(matched.first_hit_draw_index.has_value());
    EXPECT_EQ(*matched.first_hit_draw_index, 20);
    ASSERT_TRUE(matched.first_crit_draw_index.has_value());
    EXPECT_EQ(*matched.first_crit_draw_index, 21);
    ASSERT_EQ(matched.hit_draws.size(), 3u);
    ASSERT_TRUE(matched.hit_draws[2].hit_success.has_value());
    EXPECT_EQ(*matched.hit_draws[2].hit_success, 0);

    std::istringstream missing_crit_input(
        "pc=80010bdc function=getAttackResult checkpoint=hit rng_draw_index_before=20 "
        "instr_param_0x6=0 hit_success=1\n");
    const auto missing_crit = parse_checkpoint_stream(missing_crit_input);
    ASSERT_TRUE(missing_crit.errors.empty());
    const auto mismatch = summarize_crit_gate_checkpoints(missing_crit.events);
    EXPECT_EQ(mismatch.status, CritGateCheckpointStatus::CritDrawCountMismatch);

    std::istringstream missing_fields_input(
        "pc=80010bdc function=getAttackResult checkpoint=hit rng_draw_index_before=20 "
        "instr_param_0x6=0\n");
    const auto missing_fields = parse_checkpoint_stream(missing_fields_input);
    ASSERT_TRUE(missing_fields.errors.empty());
    const auto incomplete = summarize_crit_gate_checkpoints(missing_fields.events);
    EXPECT_EQ(incomplete.status, CritGateCheckpointStatus::MissingLiveGateFields);
    EXPECT_EQ(
        crit_gate_checkpoint_status_name(incomplete.status),
        std::string("MissingLiveGateFields"));
}

TEST(SavorPredictCheckpointTrace, SummarizesCounterCheckpoints) {
    std::istringstream input(
        "pc=80081a88 function=shouldCounter checkpoint=roll rng_draw_index_before=30 "
        "attacker_slot=1 target_slot=4 target_status_flags=0x0 target_movement_flags=0xc0 "
        "target_base_counter_chance=10 target_current_counter_chance=10 "
        "attacker_action_marker=0 attack_was_critical=0 counter_rand=7 "
        "counter_result=1 queued_field7_0xc=0 updated_current_counter_chance=0\n"
        "pc=80081a88 function=shouldCounter checkpoint=roll rng_draw_index_before=31 "
        "attacker_slot=5 target_slot=0 target_status_flags=0x0 target_movement_flags=0xc0 "
        "target_base_counter_chance=15 target_current_counter_chance=15 "
        "attacker_action_marker=0 attack_was_critical=0 counter_rand=54 "
        "counter_result=0 updated_current_counter_chance=15\n");

    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.events.size(), 2u);
    EXPECT_EQ(parsed.events[0].known_rng_owner, "counter_roll");

    const auto within = summarize_counter_checkpoints(parsed.events, 2);
    EXPECT_EQ(within.status, CounterCheckpointStatus::WithinExpectedCeiling);
    EXPECT_EQ(within.observed_counter_rolls, 2);
    ASSERT_TRUE(within.first_counter_roll_draw_index.has_value());
    EXPECT_EQ(*within.first_counter_roll_draw_index, 30);
    ASSERT_TRUE(within.last_counter_roll_draw_index.has_value());
    EXPECT_EQ(*within.last_counter_roll_draw_index, 31);
    EXPECT_EQ(within.draws_with_actor_slots, 2);
    EXPECT_EQ(within.draws_with_gate_inputs, 2);
    EXPECT_EQ(within.draws_with_counter_result, 2);
    ASSERT_EQ(within.draws.size(), 2u);
    ASSERT_TRUE(within.draws[0].target_status_flags.has_value());
    EXPECT_EQ(*within.draws[0].target_status_flags, 0);
    ASSERT_TRUE(within.draws[0].target_movement_flags.has_value());
    EXPECT_EQ(*within.draws[0].target_movement_flags, 0xc0);

    const auto observed_only = summarize_counter_checkpoints(parsed.events, std::nullopt);
    EXPECT_EQ(observed_only.status, CounterCheckpointStatus::ObservedOnly);

    const auto exceeds = summarize_counter_checkpoints(parsed.events, 1);
    EXPECT_EQ(exceeds.status, CounterCheckpointStatus::ExceedsExpectedCeiling);
    EXPECT_EQ(
        counter_checkpoint_status_name(exceeds.status),
        std::string("ExceedsExpectedCeiling"));
}

TEST(SavorPredictCheckpointTrace, SummarizesDropCheckpoints) {
    std::istringstream input(
        "pc=80052bf0 function=FUN_80052b24 checkpoint=mode0e_camera rng_draw_index_before=20\n"
        "pc=80010628 function=status checkpoint=pc rng_draw_index_before=22\n"
        "pc=80010984 function=rollDamage checkpoint=bonus rng_draw_index_before=23\n"
        "pc=8002bae8 function=enemyDropItem checkpoint=row rng_draw_index_before=24 "
        "target_slot=4 enemy_entry_id=0 drop_row_index=1 drop_item_id=273 "
        "drop_amount=1 rand_value=44 rand_mod100=44 drop_success=0\n"
        "pc=80081a88 function=shouldCounter checkpoint=roll rng_draw_index_before=25\n"
        "pc=80010984 function=rollDamage checkpoint=bonus rng_draw_index_before=26\n"
        "pc=8002bae8 function=enemyDropItem checkpoint=row rng_draw_index_before=27 "
        "target_slot=4 enemy_entry_id=0 drop_row_index=2 drop_item_id=258 "
        "drop_amount=1 rand_value=0 rand_mod100=0 drop_success=1\n");

    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.events.size(), 7u);
    EXPECT_EQ(parsed.events[3].known_rng_owner, "enemy_drop_roll");

    const auto matched = summarize_drop_checkpoints(parsed.events, 2);
    EXPECT_EQ(matched.status, DropCheckpointStatus::MatchesExpected);
    EXPECT_EQ(matched.observed_drop_rolls, 2);
    ASSERT_TRUE(matched.first_drop_roll_draw_index.has_value());
    EXPECT_EQ(*matched.first_drop_roll_draw_index, 24);
    ASSERT_TRUE(matched.last_drop_roll_draw_index.has_value());
    EXPECT_EQ(*matched.last_drop_roll_draw_index, 27);
    EXPECT_EQ(matched.draws_with_target_slot, 2);
    EXPECT_EQ(matched.draws_with_drop_row, 2);
    EXPECT_EQ(matched.draws_with_rand_value, 2);
    EXPECT_EQ(matched.observed_damage_bonus_draws, 2);
    EXPECT_EQ(matched.damage_bonus_draws_before_first_drop, 1);
    EXPECT_EQ(matched.damage_bonus_draws_after_first_drop, 1);
    EXPECT_EQ(matched.observed_counter_rolls, 1);
    EXPECT_EQ(matched.counter_rolls_before_first_drop, 0);
    EXPECT_EQ(matched.observed_action_view_camera_draws, 1);
    EXPECT_EQ(matched.action_view_camera_draws_before_first_drop, 1);
    EXPECT_EQ(matched.observed_status_attempt_draws, 1);
    EXPECT_EQ(matched.status_attempt_draws_before_first_drop, 1);
    ASSERT_EQ(matched.draws.size(), 2u);
    ASSERT_TRUE(matched.draws[1].drop_success.has_value());
    EXPECT_EQ(*matched.draws[1].drop_success, 1);
    ASSERT_TRUE(matched.draws[1].drop_item_id.has_value());
    EXPECT_EQ(*matched.draws[1].drop_item_id, 258);

    const auto missing = summarize_drop_checkpoints(parsed.events, 3);
    EXPECT_EQ(missing.status, DropCheckpointStatus::MissingDropRolls);

    const auto extra = summarize_drop_checkpoints(parsed.events, 1);
    EXPECT_EQ(extra.status, DropCheckpointStatus::ExtraDropRolls);
    EXPECT_EQ(
        drop_checkpoint_status_name(extra.status),
        std::string("ExtraDropRolls"));
}

TEST(SavorPredictCheckpointTrace, SummarizesOutcomeCheckpoints) {
    std::istringstream input(
        "pc=8006ff38 function=endTurn checkpoint=status rng_draw_index_before=40 "
        "actor_slot=0 status_effect_id=0 rand_value=12\n"
        "pc=801f2b6c function=LevelUp checkpoint=stat1 rng_draw_index_before=41 "
        "actor_slot=0 stat_index=1 level=2 rand_value=3\n"
        "pc=801f2c00 function=LevelUp checkpoint=stat2 rng_draw_index_before=42 "
        "actor_slot=0 stat_index=2 level=2 rand_value=4\n"
        "pc=801f2d10 function=LevelUp checkpoint=stat3 rng_draw_index_before=43 "
        "actor_slot=0 stat_index=3 level=2 rand_value=5\n");

    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.events.size(), 4u);
    EXPECT_EQ(parsed.events[0].known_rng_owner, "end_turn_status_cleanup");
    EXPECT_EQ(parsed.events[1].known_rng_owner, "level_up_stat_roll_1");

    const auto matched = summarize_outcome_checkpoints(parsed.events, 1, 3);
    EXPECT_EQ(matched.status, OutcomeCheckpointStatus::MatchesExpected);
    EXPECT_EQ(matched.observed_end_turn_status_draws, 1);
    EXPECT_EQ(matched.observed_level_up_stat_rolls, 3);
    EXPECT_EQ(matched.observed_level_up_roll_1_draws, 1);
    EXPECT_EQ(matched.observed_level_up_roll_2_draws, 1);
    EXPECT_EQ(matched.observed_level_up_roll_3_draws, 1);
    ASSERT_TRUE(matched.first_end_turn_status_draw_index.has_value());
    EXPECT_EQ(*matched.first_end_turn_status_draw_index, 40);
    ASSERT_TRUE(matched.first_level_up_stat_roll_index.has_value());
    EXPECT_EQ(*matched.first_level_up_stat_roll_index, 41);
    EXPECT_EQ(matched.draws_with_actor_slot, 4);
    EXPECT_EQ(matched.draws_with_rand_value, 4);
    ASSERT_EQ(matched.draws.size(), 4u);
    ASSERT_TRUE(matched.draws[2].stat_index.has_value());
    EXPECT_EQ(*matched.draws[2].stat_index, 2);

    const auto end_turn_mismatch = summarize_outcome_checkpoints(parsed.events, 0, 0);
    EXPECT_EQ(end_turn_mismatch.status, OutcomeCheckpointStatus::EndTurnStatusDrawMismatch);

    const auto level_up_mismatch = summarize_outcome_checkpoints(parsed.events, std::nullopt, 2);
    EXPECT_EQ(level_up_mismatch.status, OutcomeCheckpointStatus::LevelUpStatRollMismatch);
    EXPECT_EQ(
        outcome_checkpoint_status_name(level_up_mismatch.status),
        std::string("LevelUpStatRollMismatch"));
}

TEST(SavorPredictCheckpointTrace, RejectsRegressingDrawIndexes) {
    std::istringstream input(
        "pc=80010BDC function=getAttackResult checkpoint=hit rng_draw_index_before=20\n"
        "pc=80010958 function=rollDamage checkpoint=spread rng_draw_index_before=19\n");

    const auto parsed = parse_checkpoint_stream(input);

    ASSERT_EQ(parsed.errors.size(), 1u);
    EXPECT_NE(parsed.errors[0].find("regressed"), std::string::npos);
}

} // namespace
