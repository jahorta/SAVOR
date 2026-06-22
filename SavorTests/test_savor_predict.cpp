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

TEST(SavorPredictRngModel, EffectRngModelComputesFirstBattle007Burst) {
    const auto input = first_battle_landed_basic_attack_effect_burst_sequence();
    const auto model = model_combat_effect_burst_sequence_draws(input);

    ASSERT_EQ(model.bursts.size(), 2u);
    EXPECT_EQ(model.total_loop_count, 22);
    EXPECT_EQ(model.total_draws, 110);

    EXPECT_EQ(model.bursts[0].loop_count, 16);
    EXPECT_EQ(model.bursts[0].draws_per_iteration, 5);
    EXPECT_EQ(model.bursts[0].position_selector_draws, 16);
    EXPECT_EQ(model.bursts[0].scale_draws, 48);
    EXPECT_EQ(model.bursts[0].variant_index_draws, 16);
    EXPECT_EQ(model.bursts[0].axis_assignment_draws, 0);
    EXPECT_EQ(model.bursts[0].total_draws, 80);

    EXPECT_EQ(model.bursts[1].loop_count, 6);
    EXPECT_EQ(model.bursts[1].draws_per_iteration, 5);
    EXPECT_EQ(model.bursts[1].position_selector_draws, 6);
    EXPECT_EQ(model.bursts[1].scale_draws, 18);
    EXPECT_EQ(model.bursts[1].variant_index_draws, 6);
    EXPECT_EQ(model.bursts[1].axis_assignment_draws, 0);
    EXPECT_EQ(model.bursts[1].total_draws, 30);

    const auto emitter = model_effect_emitter_spawn_draws({3, true, false, 2});
    EXPECT_EQ(emitter.draws_per_outer, 6);
    EXPECT_EQ(emitter.total_draws, 18);
    EXPECT_EQ(emitter.child_tasks, 6);

    const auto particle = model_effect_particle_tick_draws({2, true});
    EXPECT_EQ(particle.total_draws, 8);
}

TEST(SavorPredictRngModel, EffectCheckpointModelSummarizes80042b10BurstShape) {
    std::ostringstream stream;
    int draw_index = 0;
    auto append_buffer = [&](std::string_view buffer, int loop_count, int source_key) {
        const std::string source_fields =
            " effect_source_key_0x28=" + std::to_string(source_key) +
            " effect_source_subtype_0x2a=2"
            " effect_source_secondary_0x2c=0"
            " effect_source_resource_id_0x30=0x9e";
        for (int i = 0; i < loop_count; ++i) {
            stream << "pc=80042fbc function=FUN_80042b10 checkpoint=position "
                   << "rng_draw_index_before=" << draw_index++
                   << " r29_effect_buffer=" << buffer
                   << source_fields
                   << " effect_loop_count_0x5c=0x" << std::hex << loop_count << std::dec
                   << " effect_flags_0x38=0 "
                   << "effect_variant_count_0x5e=1 effect_axis_mode_0x60=0\n";
            stream << "pc=80043020 function=FUN_80042b10 checkpoint=scale_x "
                   << "rng_draw_index_before=" << draw_index++
                   << " r29_effect_buffer=" << buffer
                   << source_fields
                   << " effect_loop_count_0x5c=0x" << std::hex << loop_count << std::dec
                   << " effect_flags_0x38=0 "
                   << "effect_variant_count_0x5e=1 effect_axis_mode_0x60=0\n";
            stream << "pc=80043048 function=FUN_80042b10 checkpoint=scale_y "
                   << "rng_draw_index_before=" << draw_index++
                   << " r29_effect_buffer=" << buffer
                   << source_fields
                   << " effect_loop_count_0x5c=0x" << std::hex << loop_count << std::dec
                   << " effect_flags_0x38=0 "
                   << "effect_variant_count_0x5e=1 effect_axis_mode_0x60=0\n";
            stream << "pc=80043070 function=FUN_80042b10 checkpoint=scale_z "
                   << "rng_draw_index_before=" << draw_index++
                   << " r29_effect_buffer=" << buffer
                   << source_fields
                   << " effect_loop_count_0x5c=0x" << std::hex << loop_count << std::dec
                   << " effect_flags_0x38=0 "
                   << "effect_variant_count_0x5e=1 effect_axis_mode_0x60=0\n";
            stream << "pc=800430fc function=FUN_80042b10 checkpoint=variant "
                   << "rng_draw_index_before=" << draw_index++
                   << " r29_effect_buffer=" << buffer
                   << source_fields
                   << " effect_loop_count_0x5c=0x" << std::hex << loop_count << std::dec
                   << " effect_flags_0x38=0 "
                   << "effect_variant_count_0x5e=1 effect_axis_mode_0x60=0\n";
        }
    };
    const std::vector<int> source_keys = {5, 4, 5};
    for (int attack = 0; attack < 3; ++attack) {
        append_buffer("0x100" + std::to_string(attack), 16, source_keys[attack]);
        append_buffer("0x200" + std::to_string(attack), 6, source_keys[attack]);
    }
    stream << "pc=80041f30 function=FUN_80041e64 checkpoint=effect_emitter_source_gate "
           << "rng_draw_index_before=" << draw_index << " owns_rng_draw=false "
           << "emitter_outer_count_0x142=3 emitter_child_count_0x1c=2 "
           << "emitter_variant_count_0x16=4 emitter_axis_mode_0x3c=8\n";
    stream << "pc=800425a0 function=FUN_800422d0 checkpoint=particle_tick "
           << "rng_draw_index_before=" << draw_index++
           << " particle_payload_source_ptr_0x20=0x12345678 "
           << "particle_payload_lifetime_0x28=12\n";

    std::istringstream input(stream.str());
    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.events.size(), 332u);
    EXPECT_EQ(parsed.events.front().known_rng_owner, "combat_effect_spawn_position_binary");

    const auto summary = summarize_effect_checkpoints(parsed.events);
    EXPECT_EQ(summary.status, EffectCheckpointStatus::MatchesBinaryVariantBurstShape);
    EXPECT_EQ(summary.observed_combat_effect_draws, 330);
    EXPECT_EQ(summary.observed_binary_position_draws, 66);
    EXPECT_EQ(summary.observed_scale_x_draws, 66);
    EXPECT_EQ(summary.observed_scale_y_draws, 66);
    EXPECT_EQ(summary.observed_scale_z_draws, 66);
    EXPECT_EQ(summary.observed_variant_index_draws, 66);
    EXPECT_EQ(summary.observed_axis_assignment_draws, 0);
    EXPECT_EQ(summary.combat_effect_draws_with_loop_count, 330);
    EXPECT_EQ(summary.combat_effect_draws_with_flags, 330);
    EXPECT_EQ(summary.combat_effect_draws_with_variant_count, 330);
    EXPECT_EQ(summary.combat_effect_draws_with_axis_mode, 330);
    EXPECT_EQ(summary.combat_effect_draws_with_source_key, 330);
    EXPECT_EQ(summary.combat_effect_draws_with_source_subtype, 330);
    EXPECT_EQ(summary.combat_effect_draws_with_source_secondary, 330);
    EXPECT_EQ(summary.combat_effect_draws_with_source_resource_id, 330);
    EXPECT_EQ(summary.combat_effect_draws_with_buffer_pointer, 330);
    EXPECT_EQ(summary.observed_combat_effect_buffers, 6);
    EXPECT_EQ(summary.complete_binary_variant_iterations, 66);
    EXPECT_EQ(summary.complete_binary_variant_buffers, 6);
    EXPECT_EQ(summary.complete_binary_variant_16_loop_buffers, 3);
    EXPECT_EQ(summary.complete_binary_variant_6_loop_buffers, 3);
    EXPECT_EQ(summary.complete_first_battle_landed_attack_effect_pairs, 3);
    EXPECT_EQ(summary.complete_first_battle_landed_attack_effect_pairs_with_matching_source_key, 3);
    EXPECT_EQ(summary.complete_first_battle_landed_attack_effect_pairs_without_matching_source_key, 0);
    ASSERT_EQ(summary.complete_first_battle_effect_pairs_by_source_key.size(), 2u);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[0].source_key, 4);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[0].pair_count, 1);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[1].source_key, 5);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[1].pair_count, 2);
    EXPECT_EQ(summary.unpaired_first_battle_effect_buffers, 0);
    EXPECT_EQ(summary.incomplete_binary_variant_iteration_remainder, 0);
    EXPECT_EQ(summary.observed_emitter_source_gate_events, 1);
    EXPECT_EQ(summary.emitter_source_gate_events_with_outer_count, 1);
    EXPECT_EQ(summary.emitter_source_gate_events_with_child_count, 1);
    EXPECT_EQ(summary.emitter_source_gate_events_with_variant_count, 1);
    EXPECT_EQ(summary.emitter_source_gate_events_with_axis_mode, 1);
    EXPECT_EQ(summary.observed_particle_tick_draws, 1);
    EXPECT_EQ(summary.particle_tick_draws_with_payload_source, 1);
    EXPECT_EQ(summary.particle_tick_draws_with_lifetime, 1);
    ASSERT_TRUE(summary.first_combat_effect_draw_index.has_value());
    EXPECT_EQ(*summary.first_combat_effect_draw_index, 0);
    ASSERT_TRUE(summary.last_combat_effect_draw_index.has_value());
    EXPECT_EQ(*summary.last_combat_effect_draw_index, 329);
}

TEST(SavorPredictRngModel, PreAiCheckpointModelSummarizesFakeCameraCursor) {
    {
        std::istringstream input(
            "pc=8001413c function=BattleCameraSetup checkpoint=battle_start_camera "
            "rng_draw_index_before=0 rng_seed_before=0x00000001 rng_seed_after=0x0000303a\n"
            "pc=800608dc function=TargetCamera checkpoint=targeting_camera "
            "rng_draw_index_before=1 rng_seed_before=0x0000303a rng_seed_after=0xd3dc167e "
            "active_slot=0 target_slot=4\n"
            "pc=800608dc function=TargetCamera checkpoint=targeting_camera "
            "rng_draw_index_before=2 rng_seed_before=0xd3dc167e rng_seed_after=0xa70427df "
            "active_slot=1 target_slot=4\n"
            "pc=8008b428 function=runAiRoutine checkpoint=soldier_ai_action "
            "rng_draw_index_before=3 rng_seed_before=0xa70427df rng_seed_after=0xd6651c2c "
            "active_slot=4\n");

        const auto parsed = parse_checkpoint_stream(input);
        ASSERT_TRUE(parsed.errors.empty());
        const auto baseline = summarize_pre_ai_checkpoints(parsed.events, 0);

        EXPECT_EQ(pre_ai_checkpoint_status_name(baseline.status), std::string("MatchesExpected"));
        ASSERT_TRUE(baseline.expectation.has_value());
        EXPECT_EQ(baseline.expectation->expected_targeting_camera_draws, 2);
        EXPECT_EQ(baseline.observed_battle_start_camera_draws, 1);
        EXPECT_EQ(baseline.observed_targeting_camera_draws, 2);
        EXPECT_EQ(baseline.observed_fake_attack_draws, 0);
        EXPECT_EQ(baseline.observed_pre_ai_draws, 3);
        ASSERT_TRUE(baseline.first_soldier_ai_draw_index.has_value());
        EXPECT_EQ(*baseline.first_soldier_ai_draw_index, 3);
    }

    {
        std::istringstream input(
            "pc=8001413c function=BattleCameraSetup checkpoint=battle_start_camera "
            "rng_draw_index_before=0\n"
            "pc=80000000 function=FakeAttack checkpoint=pre_ai_fake_attack "
            "rng_draw_index_before=1 owns_rng_draw=true fake_attack_index=0 camera_frame_gap=9\n"
            "pc=800608dc function=TargetCamera checkpoint=targeting_camera "
            "rng_draw_index_before=2 active_slot=1 target_slot=4\n"
            "pc=8008b428 function=runAiRoutine checkpoint=soldier_ai_action "
            "rng_draw_index_before=3 active_slot=4\n");

        const auto parsed = parse_checkpoint_stream(input);
        ASSERT_TRUE(parsed.errors.empty());
        const auto fake_one = summarize_pre_ai_checkpoints(parsed.events, 1);

        EXPECT_EQ(pre_ai_checkpoint_status_name(fake_one.status), std::string("MatchesExpected"));
        ASSERT_TRUE(fake_one.expectation.has_value());
        EXPECT_EQ(fake_one.expectation->expected_fake_attack_draws, 1);
        EXPECT_EQ(fake_one.expectation->expected_targeting_camera_draws, 1);
        EXPECT_EQ(fake_one.observed_fake_attack_attempts, 1);
        EXPECT_EQ(fake_one.observed_fake_attack_draws, 1);
        EXPECT_EQ(fake_one.observed_skipped_fake_attack_draws, 0);
        EXPECT_EQ(fake_one.fake_attack_draws_with_frame_gap, 1);
        ASSERT_TRUE(fake_one.min_fake_attack_draw_camera_frame_gap.has_value());
        ASSERT_TRUE(fake_one.max_fake_attack_draw_camera_frame_gap.has_value());
        EXPECT_EQ(*fake_one.min_fake_attack_draw_camera_frame_gap, 9);
        EXPECT_EQ(*fake_one.max_fake_attack_draw_camera_frame_gap, 9);
        EXPECT_EQ(fake_one.observed_targeting_camera_draws, 1);
        EXPECT_EQ(fake_one.observed_pre_ai_draws, 3);
    }
}

TEST(SavorPredictRngModel, PreAiCheckpointModelTracksObservedGapFakeAttackNoRand) {
    std::istringstream input(
        "pc=8001413c function=BattleCameraSetup checkpoint=battle_start_camera "
        "rng_draw_index_before=0\n"
        "pc=80000000 function=FakeAttack checkpoint=pre_ai_fake_attack "
        "rng_draw_index_before=1 owns_rng_draw=true fake_attack_index=0 camera_frame_gap=6\n"
        "pc=80000004 function=FakeAttack checkpoint=pre_ai_fake_attack_no_rand "
        "rng_draw_index_before=2 owns_rng_draw=false fake_attack_index=1 camera_frame_gap=5\n"
        "pc=80000008 function=FakeAttack checkpoint=pre_ai_fake_attack "
        "rng_draw_index_before=2 owns_rng_draw=true fake_attack_index=2 camera_frame_gap=12\n"
        "pc=800608dc function=TargetCamera checkpoint=targeting_camera "
        "rng_draw_index_before=3 active_slot=1 target_slot=4\n"
        "pc=8008b428 function=runAiRoutine checkpoint=soldier_ai_action "
        "rng_draw_index_before=4 active_slot=4\n");

    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());
    const auto summary = summarize_pre_ai_checkpoints(parsed.events, 3);

    EXPECT_EQ(
        pre_ai_checkpoint_status_name(summary.status),
        std::string("SkippedFakeAttackDrawsObserved"));
    EXPECT_EQ(summary.observed_fake_attack_attempts, 3);
    EXPECT_EQ(summary.observed_fake_attack_draws, 2);
    EXPECT_EQ(summary.observed_skipped_fake_attack_draws, 1);
    EXPECT_EQ(summary.fake_attack_draws_with_frame_gap, 2);
    EXPECT_EQ(summary.skipped_fake_attempts_with_frame_gap, 1);
    ASSERT_TRUE(summary.min_fake_attack_draw_camera_frame_gap.has_value());
    ASSERT_TRUE(summary.max_fake_attack_draw_camera_frame_gap.has_value());
    ASSERT_TRUE(summary.min_skipped_fake_attempt_camera_frame_gap.has_value());
    ASSERT_TRUE(summary.max_skipped_fake_attempt_camera_frame_gap.has_value());
    EXPECT_EQ(*summary.min_fake_attack_draw_camera_frame_gap, 6);
    EXPECT_EQ(*summary.max_fake_attack_draw_camera_frame_gap, 12);
    EXPECT_EQ(*summary.min_skipped_fake_attempt_camera_frame_gap, 5);
    EXPECT_EQ(*summary.max_skipped_fake_attempt_camera_frame_gap, 5);
    EXPECT_EQ(summary.fake_attack_attempt_transitions, 2);
    EXPECT_EQ(summary.draw_to_skip_fake_attack_transitions, 1);
    EXPECT_EQ(summary.skip_to_draw_fake_attack_transitions, 1);
    EXPECT_EQ(summary.draw_to_draw_fake_attack_transitions, 0);
    EXPECT_EQ(summary.skip_to_skip_fake_attack_transitions, 0);
    EXPECT_EQ(summary.draw_to_skip_transitions_with_previous_frame_gap, 1);
    EXPECT_EQ(summary.skip_to_draw_transitions_with_previous_frame_gap, 1);
    ASSERT_TRUE(summary.min_draw_to_skip_previous_camera_frame_gap.has_value());
    ASSERT_TRUE(summary.max_draw_to_skip_previous_camera_frame_gap.has_value());
    ASSERT_TRUE(summary.min_skip_to_draw_previous_camera_frame_gap.has_value());
    ASSERT_TRUE(summary.max_skip_to_draw_previous_camera_frame_gap.has_value());
    EXPECT_EQ(*summary.min_draw_to_skip_previous_camera_frame_gap, 6);
    EXPECT_EQ(*summary.max_draw_to_skip_previous_camera_frame_gap, 6);
    EXPECT_EQ(*summary.min_skip_to_draw_previous_camera_frame_gap, 5);
    EXPECT_EQ(*summary.max_skip_to_draw_previous_camera_frame_gap, 5);
    EXPECT_EQ(summary.observed_pre_ai_draws, 4);
    EXPECT_EQ(summary.pre_ai_draws_before_first_soldier_ai, 4);
    ASSERT_EQ(summary.draws.size(), 5u);
    EXPECT_TRUE(summary.draws[2].skipped_rng_draw);
    ASSERT_TRUE(summary.draws[2].camera_frame_gap.has_value());
    EXPECT_EQ(*summary.draws[2].camera_frame_gap, 5);
    EXPECT_FALSE(summary.draws[3].skipped_rng_draw);
    ASSERT_TRUE(summary.draws[3].camera_frame_gap.has_value());
    EXPECT_EQ(*summary.draws[3].camera_frame_gap, 12);
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
    EXPECT_EQ(expectation.expected_mode0e_camera_draws, 0);
    EXPECT_EQ(expectation.expected_mode0_rewrite_gate_draws, 3);
    EXPECT_EQ(expectation.expected_owner, std::string_view("mode0e_action_view_camera"));
    EXPECT_EQ(expectation.expected_pc, std::string_view("80052BF0"));
    EXPECT_EQ(expectation.rewrite_gate_owner, std::string_view("mode0_action_view_camera_fallback"));
    EXPECT_EQ(expectation.rewrite_gate_pc, std::string_view("800513D4"));
    EXPECT_EQ(expectation.rejected_fallback_owner, std::string_view("mode0_action_view_camera_fallback"));
    EXPECT_EQ(expectation.rejected_fallback_pc, std::string_view("800513D4"));
    EXPECT_NE(
        std::string_view(first_battle_action_view_camera_rule_detail()).find("mode-0 rewrite gate"),
        std::string_view::npos);
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
    EXPECT_EQ(expectation.pc, std::string_view("8002BAD8"));
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
    EXPECT_NE(
        std::string_view(first_battle_outcome_checkpoint_rule_detail()).find("victory reward"),
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

TEST(SavorPredictProgressParser, ParsesCounterAttacksAndInfersTarget) {
    const std::vector<std::string> messages = {
        "worker=16 progress=[4]Soldier attacks Vyse for 45 damage",
        "worker=16 progress=Vyse counter attacks...",
        "worker=16 progress=[4]Soldier died..."};

    const auto parsed = parse_progress_events(messages);
    ASSERT_EQ(parsed.attacks.size(), 1u);
    ASSERT_EQ(parsed.counters.size(), 1u);
    EXPECT_EQ(parsed.counters[0].actor, "Vyse");
    EXPECT_EQ(parsed.counters[0].target, "[4]Soldier");
    EXPECT_TRUE(parsed.counters[0].target_inferred);

    ASSERT_EQ(parsed.ordered_combat_events.size(), 3u);
    EXPECT_EQ(parsed.ordered_combat_events[0].kind, CombatEventKind::Attack);
    EXPECT_EQ(parsed.ordered_combat_events[1].kind, CombatEventKind::Counter);
    EXPECT_EQ(parsed.ordered_combat_events[1].counter.actor, "Vyse");
    EXPECT_EQ(parsed.ordered_combat_events[1].counter.target, "[4]Soldier");
    EXPECT_EQ(parsed.ordered_combat_events[2].kind, CombatEventKind::Death);
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
    events.ordered_combat_events.push_back(CombatEvent{CombatEventKind::Attack, aika_attack, {}, {}, {}});
    events.ordered_combat_events.push_back(CombatEvent{CombatEventKind::Attack, vyse_attack, {}, {}, {}});
    events.ordered_combat_events.push_back(CombatEvent{CombatEventKind::Death, {}, {}, "[4]Soldier", {}});
    events.ordered_combat_events.push_back(CombatEvent{CombatEventKind::Drop, {}, {}, "[4]Soldier", "[273]Electri Box x1"});
    events.ordered_combat_events.push_back(CombatEvent{CombatEventKind::Attack, soldier5_attack, {}, {}, {}});

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
    ASSERT_TRUE(expectation.expected_callback_pc.has_value());
    EXPECT_EQ(*expectation.expected_callback_pc, "800662BC");

    std::istringstream input(
        "pc=8006782c function=FUN_8006782c checkpoint=source_selection rng_draw_index_before=17 "
        "actor_slot=1 selected_source_slot=1 target_slot=4 action_sequence_id=7 action_id=4\n"
        "pc=8006721c function=FUN_8006721c checkpoint=action_source rng_draw_index_before=18 "
        "actor_slot=1 source_slot=1 target_slot=4 action_sequence_id=7 action_id=4 "
        "source_field6_0x6=14 actor_field6_0x6=14 handler_pc=0x800662bc callback_pc=0x800662bc\n"
        "pc=8006782c function=FUN_8006782c checkpoint=source_selection rng_draw_index_before=18 "
        "actor_slot=0 selected_source_slot=0 target_slot=4 action_sequence_id=8 action_id=8\n"
        "pc=8006721c function=FUN_8006721c checkpoint=action_source rng_draw_index_before=19 "
        "actor_slot=0 source_slot=0 target_slot=4 action_sequence_id=8 action_id=8 "
        "source_field6_0x6=14 actor_field6_0x6=14 selected_handler_pc=800662BC "
        "instruction_callback_pc=800662BC\n");

    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.events.size(), 4u);

    const auto matched = summarize_action_source_checkpoints(
        parsed.events,
        expectation.expected_handler_pc);
    EXPECT_EQ(matched.status, ActionSourceCheckpointStatus::MatchesExpected);
    EXPECT_EQ(matched.observed_source_selection_events, 2);
    EXPECT_EQ(matched.observed_action_source_events, 2);
    EXPECT_EQ(matched.source_selection_events_with_source_slot, 2);
    EXPECT_EQ(matched.events_with_actor_slot, 2);
    EXPECT_EQ(matched.events_with_source_slot, 2);
    EXPECT_EQ(matched.events_with_action_sequence_id, 4);
    EXPECT_EQ(matched.source_selection_events_with_action_sequence_id, 2);
    EXPECT_EQ(matched.action_source_events_with_action_sequence_id, 2);
    EXPECT_EQ(matched.events_with_action_id, 2);
    EXPECT_EQ(matched.events_with_handler_pc, 2);
    EXPECT_EQ(matched.events_with_callback_pc, 2);
    EXPECT_EQ(matched.events_with_source_field6, 2);
    EXPECT_EQ(matched.events_with_actor_field6, 2);
    EXPECT_EQ(matched.source_selection_bridge_pairs, 2);
    EXPECT_EQ(matched.source_selection_bridge_matches, 2);
    EXPECT_EQ(matched.source_selection_bridge_mismatches, 0);
    EXPECT_EQ(matched.source_selection_bridge_pairs_by_action_sequence_id, 2);
    EXPECT_EQ(matched.source_selection_bridge_missing_by_action_sequence_id, 0);
    EXPECT_EQ(matched.source_selection_bridge_order_matches, 2);
    EXPECT_EQ(matched.source_selection_bridge_order_mismatches, 0);
    EXPECT_EQ(matched.source_selection_bridge_pairing_strategy, std::string("action_sequence_id"));
    EXPECT_EQ(matched.field6_matches, 2);
    EXPECT_EQ(matched.handler_matches, 2);
    EXPECT_EQ(matched.callback_matches, 2);
    ASSERT_TRUE(matched.first_source_selection_draw_index.has_value());
    EXPECT_EQ(*matched.first_source_selection_draw_index, 17);
    ASSERT_TRUE(matched.first_action_source_draw_index.has_value());
    EXPECT_EQ(*matched.first_action_source_draw_index, 18);
    ASSERT_EQ(matched.events.size(), 4u);
    EXPECT_EQ(matched.events[0].kind, ActionSourceCheckpointKind::SourceSelection);
    EXPECT_EQ(matched.events[1].kind, ActionSourceCheckpointKind::Field6Bridge);
    ASSERT_TRUE(matched.events[1].handler_pc.has_value());
    EXPECT_EQ(*matched.events[1].handler_pc, "800662BC");
    ASSERT_TRUE(matched.events[1].callback_pc.has_value());
    EXPECT_EQ(*matched.events[1].callback_pc, "800662BC");
    ASSERT_TRUE(matched.events[1].matched_source_selection_draw_index.has_value());
    EXPECT_EQ(*matched.events[1].matched_source_selection_draw_index, 17);
    ASSERT_TRUE(matched.events[1].matched_source_selection_source_slot.has_value());
    EXPECT_EQ(*matched.events[1].matched_source_selection_source_slot, 1);
    ASSERT_TRUE(matched.events[1].source_selection_before_bridge.has_value());
    EXPECT_TRUE(*matched.events[1].source_selection_before_bridge);
    ASSERT_TRUE(matched.events[1].source_slot_matches_selection.has_value());
    EXPECT_TRUE(*matched.events[1].source_slot_matches_selection);

    std::istringstream out_of_order_input(
        "pc=8006782c function=FUN_8006782c checkpoint=source_selection rng_draw_index_before=17 "
        "actor_slot=0 selected_source_slot=0 action_sequence_id=7\n"
        "pc=8006782c function=FUN_8006782c checkpoint=source_selection rng_draw_index_before=18 "
        "actor_slot=1 selected_source_slot=1 action_sequence_id=8\n"
        "pc=8006721c function=FUN_8006721c checkpoint=action_source rng_draw_index_before=19 "
        "actor_slot=1 source_slot=1 action_sequence_id=8 source_field6_0x6=14 actor_field6_0x6=14 "
        "handler_pc=800662BC callback_pc=800662BC\n"
        "pc=8006721c function=FUN_8006721c checkpoint=action_source rng_draw_index_before=20 "
        "actor_slot=0 source_slot=0 action_sequence_id=7 source_field6_0x6=14 actor_field6_0x6=14 "
        "handler_pc=800662BC callback_pc=800662BC\n");
    const auto out_of_order_parsed = parse_checkpoint_stream(out_of_order_input);
    ASSERT_TRUE(out_of_order_parsed.errors.empty());
    const auto out_of_order = summarize_action_source_checkpoints(
        out_of_order_parsed.events,
        expectation.expected_handler_pc);
    EXPECT_EQ(out_of_order.status, ActionSourceCheckpointStatus::MatchesExpected);
    EXPECT_EQ(out_of_order.source_selection_bridge_pairing_strategy, std::string("action_sequence_id"));
    EXPECT_EQ(out_of_order.source_selection_bridge_pairs, 2);
    EXPECT_EQ(out_of_order.source_selection_bridge_matches, 2);
    EXPECT_EQ(out_of_order.source_selection_bridge_mismatches, 0);
    ASSERT_EQ(out_of_order.events.size(), 4u);
    ASSERT_TRUE(out_of_order.events[2].matched_source_selection_source_slot.has_value());
    EXPECT_EQ(*out_of_order.events[2].matched_source_selection_source_slot, 1);

    std::istringstream mismatch_input(
        "pc=8006782c function=FUN_8006782c checkpoint=source_selection rng_draw_index_before=19 "
        "actor_slot=1 selected_source_slot=1\n"
        "pc=8006721c function=FUN_8006721c checkpoint=action_source rng_draw_index_before=20 "
        "actor_slot=1 source_slot=1 source_field6_0x6=14 actor_field6_0x6=15 "
        "handler_pc=800662BC callback_pc=800662BC\n");
    const auto mismatch_parsed = parse_checkpoint_stream(mismatch_input);
    ASSERT_TRUE(mismatch_parsed.errors.empty());
    const auto field_mismatch = summarize_action_source_checkpoints(
        mismatch_parsed.events,
        expectation.expected_handler_pc);
    EXPECT_EQ(field_mismatch.status, ActionSourceCheckpointStatus::Field6Mismatch);

    std::istringstream missing_input(
        "pc=8006782c function=FUN_8006782c checkpoint=source_selection rng_draw_index_before=20 "
        "actor_slot=1 selected_source_slot=1\n"
        "pc=8006721c function=FUN_8006721c checkpoint=action_source rng_draw_index_before=21 "
        "actor_slot=1 source_slot=1 source_field6_0x6=14 handler_pc=800662BC callback_pc=800662BC\n");
    const auto missing_parsed = parse_checkpoint_stream(missing_input);
    ASSERT_TRUE(missing_parsed.errors.empty());
    const auto missing = summarize_action_source_checkpoints(
        missing_parsed.events,
        expectation.expected_handler_pc);
    EXPECT_EQ(missing.status, ActionSourceCheckpointStatus::MissingLiveSourceFields);
    EXPECT_EQ(
        action_source_checkpoint_status_name(missing.status),
        std::string("MissingLiveSourceFields"));

    std::istringstream missing_selection_input(
        "pc=8006721c function=FUN_8006721c checkpoint=action_source rng_draw_index_before=22 "
        "actor_slot=1 source_slot=1 source_field6_0x6=14 actor_field6_0x6=14 "
        "handler_pc=800662BC callback_pc=800662BC\n");
    const auto missing_selection_parsed = parse_checkpoint_stream(missing_selection_input);
    ASSERT_TRUE(missing_selection_parsed.errors.empty());
    const auto missing_selection = summarize_action_source_checkpoints(
        missing_selection_parsed.events,
        expectation.expected_handler_pc);
    EXPECT_EQ(
        missing_selection.status,
        ActionSourceCheckpointStatus::MissingSourceSelectionCheckpoint);

    std::istringstream selection_mismatch_input(
        "pc=8006782c function=FUN_8006782c checkpoint=source_selection rng_draw_index_before=23 "
        "actor_slot=1 selected_source_slot=0\n"
        "pc=8006721c function=FUN_8006721c checkpoint=action_source rng_draw_index_before=24 "
        "actor_slot=1 source_slot=1 source_field6_0x6=14 actor_field6_0x6=14 "
        "handler_pc=800662BC callback_pc=800662BC\n");
    const auto selection_mismatch_parsed = parse_checkpoint_stream(selection_mismatch_input);
    ASSERT_TRUE(selection_mismatch_parsed.errors.empty());
    const auto selection_mismatch = summarize_action_source_checkpoints(
        selection_mismatch_parsed.events,
        expectation.expected_handler_pc);
    EXPECT_EQ(selection_mismatch.status, ActionSourceCheckpointStatus::SourceSelectionMismatch);

    std::istringstream callback_mismatch_input(
        "pc=8006782c function=FUN_8006782c checkpoint=source_selection rng_draw_index_before=25 "
        "actor_slot=1 selected_source_slot=1\n"
        "pc=8006721c function=FUN_8006721c checkpoint=action_source rng_draw_index_before=26 "
        "actor_slot=1 source_slot=1 source_field6_0x6=14 actor_field6_0x6=14 "
        "handler_pc=800662BC callback_pc=80000000\n");
    const auto callback_mismatch_parsed = parse_checkpoint_stream(callback_mismatch_input);
    ASSERT_TRUE(callback_mismatch_parsed.errors.empty());
    const auto callback_mismatch = summarize_action_source_checkpoints(
        callback_mismatch_parsed.events,
        expectation.expected_handler_pc);
    EXPECT_EQ(callback_mismatch.status, ActionSourceCheckpointStatus::CallbackMismatch);
}

TEST(SavorPredictCheckpointTrace, SummarizesActionSetupCheckpoints) {
    std::istringstream input(
        "pc=800708c0 function=Battle::Run::setupAction checkpoint=setup_action "
        "rng_draw_index_before=20 actor_slot=0 handler_pc=80086c68 "
        "instruction=3 target_slot=4 instr_param_0x6=0\n"
        "pc=80086c68 function=Battle::HandlePCInst checkpoint=pc_handler "
        "rng_draw_index_before=21 active_slot=0 instruction=3 target_slot=4 instr_param_0x6=0\n"
        "pc=800708c0 function=Battle::Run::setupAction checkpoint=setup_action "
        "rng_draw_index_before=22 actor_slot=4 handler_pc=8008b9e0 "
        "instruction=3 target_slot=0 instr_param_0x6=1\n"
        "pc=8008b9e0 function=Battle::HandleECInst checkpoint=enemy_handler "
        "rng_draw_index_before=23 active_slot=4 instruction=3 target_slot=0 "
        "instr_param_0x6=1 movement_flags=0xc0\n"
        "pc=8008bc68 function=Battle::HandleECInst checkpoint=enemy_setup "
        "rng_draw_index_before=24 active_slot=4 instruction=3 target_slot=0 "
        "instr_param_0x6=1 movement_flags=0xc0 setup_rand=27 setup_rand_mod10=7 "
        "direct_close_candidate=1 final_instr_param_0x6=0 helper_8008a174_result=1 "
        "helper_80082340_result=0 target_distance=3 selected_worker_pc=80087f6c\n"
        "pc=80010bdc function=getAttackResult checkpoint=hit rng_draw_index_before=25\n");

    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());
    const auto matched = summarize_action_setup_checkpoints(parsed.events, 1);

    EXPECT_EQ(action_setup_checkpoint_status_name(matched.status), std::string("MatchesExpected"));
    EXPECT_EQ(matched.observed_setup_action_events, 2);
    EXPECT_EQ(matched.observed_pc_handler_entries, 1);
    EXPECT_EQ(matched.observed_enemy_handler_entries, 1);
    EXPECT_EQ(matched.observed_enemy_setup_draws, 1);
    EXPECT_EQ(matched.handler_matches, 4);
    EXPECT_EQ(matched.handler_mismatches, 0);
    EXPECT_EQ(matched.setup_handler_field_comparisons, 6);
    EXPECT_EQ(matched.setup_handler_field_matches, 6);
    EXPECT_EQ(matched.setup_handler_field_mismatches, 0);
    EXPECT_EQ(matched.pc_setup_handler_field_comparisons, 3);
    EXPECT_EQ(matched.pc_setup_handler_field_matches, 3);
    EXPECT_EQ(matched.pc_setup_handler_field_mismatches, 0);
    EXPECT_EQ(matched.enemy_setup_handler_field_comparisons, 3);
    EXPECT_EQ(matched.enemy_setup_handler_field_matches, 3);
    EXPECT_EQ(matched.enemy_setup_handler_field_mismatches, 0);
    EXPECT_EQ(matched.enemy_setup_draws_with_gate_inputs, 1);
    EXPECT_EQ(matched.enemy_setup_draws_with_rand_value, 1);
    EXPECT_EQ(matched.enemy_setup_draws_with_rand_mod10, 1);
    EXPECT_EQ(matched.enemy_setup_draws_with_direct_close_candidate, 1);
    EXPECT_EQ(matched.enemy_setup_draws_with_final_instr_param, 1);
    EXPECT_EQ(matched.enemy_setup_draws_with_helper_8008a174_result, 1);
    EXPECT_EQ(matched.enemy_setup_draws_with_helper_80082340_result, 1);
    EXPECT_EQ(matched.enemy_setup_draws_with_target_distance, 1);
    EXPECT_EQ(matched.enemy_setup_draws_with_selected_worker, 1);
    EXPECT_EQ(matched.enemy_setup_draws_with_required_helper_fields, 1);
    EXPECT_EQ(matched.worker_matches, 1);
    EXPECT_EQ(matched.worker_mismatches, 0);
    ASSERT_TRUE(matched.first_enemy_setup_draw_index.has_value());
    EXPECT_EQ(*matched.first_enemy_setup_draw_index, 24);
    ASSERT_TRUE(matched.first_attack_hit_draw_index.has_value());
    EXPECT_EQ(*matched.first_attack_hit_draw_index, 25);
    EXPECT_EQ(matched.enemy_setup_draws_before_first_attack_hit, 1);

    const auto missing_draw = summarize_action_setup_checkpoints(parsed.events, 2);
    EXPECT_EQ(
        action_setup_checkpoint_status_name(missing_draw.status),
        std::string("MissingEnemySetupDraws"));

    const auto extra_draw = summarize_action_setup_checkpoints(parsed.events, 0);
    EXPECT_EQ(
        action_setup_checkpoint_status_name(extra_draw.status),
        std::string("ExtraEnemySetupDraws"));

    std::istringstream mismatch_input(
        "pc=800708c0 function=Battle::Run::setupAction checkpoint=setup_action "
        "rng_draw_index_before=20 actor_slot=0 handler_pc=8008b9e0\n");
    const auto mismatch_parsed = parse_checkpoint_stream(mismatch_input);
    ASSERT_TRUE(mismatch_parsed.errors.empty());
    const auto mismatch = summarize_action_setup_checkpoints(mismatch_parsed.events, std::nullopt);
    EXPECT_EQ(
        action_setup_checkpoint_status_name(mismatch.status),
        std::string("HandlerMismatch"));

    std::istringstream setup_handler_mismatch_input(
        "pc=800708c0 function=Battle::Run::setupAction checkpoint=setup_action "
        "rng_draw_index_before=20 actor_slot=0 handler_pc=80086c68 "
        "instruction=3 target_slot=4 instr_param_0x6=0\n"
        "pc=80086c68 function=Battle::HandlePCInst checkpoint=pc_handler "
        "rng_draw_index_before=21 active_slot=0 instruction=3 target_slot=4 instr_param_0x6=1\n");
    const auto setup_handler_mismatch_parsed =
        parse_checkpoint_stream(setup_handler_mismatch_input);
    ASSERT_TRUE(setup_handler_mismatch_parsed.errors.empty());
    const auto setup_handler_mismatch =
        summarize_action_setup_checkpoints(setup_handler_mismatch_parsed.events, std::nullopt);
    EXPECT_EQ(
        action_setup_checkpoint_status_name(setup_handler_mismatch.status),
        std::string("SetupHandlerFieldMismatch"));
    EXPECT_EQ(setup_handler_mismatch.pc_setup_handler_field_comparisons, 3);
    EXPECT_EQ(setup_handler_mismatch.pc_setup_handler_field_matches, 2);
    EXPECT_EQ(setup_handler_mismatch.pc_setup_handler_field_mismatches, 1);
    ASSERT_EQ(setup_handler_mismatch.events.size(), 2u);
    ASSERT_TRUE(setup_handler_mismatch.events[1].matched_setup_draw_index.has_value());
    EXPECT_EQ(*setup_handler_mismatch.events[1].matched_setup_draw_index, 20);
    ASSERT_TRUE(setup_handler_mismatch.events[1].instr_param_matches_setup.has_value());
    EXPECT_FALSE(*setup_handler_mismatch.events[1].instr_param_matches_setup);

    std::istringstream missing_fields_input(
        "pc=80086c68 function=Battle::HandlePCInst checkpoint=pc_handler "
        "rng_draw_index_before=21 active_slot=0 instruction=3\n");
    const auto missing_fields_parsed = parse_checkpoint_stream(missing_fields_input);
    ASSERT_TRUE(missing_fields_parsed.errors.empty());
    const auto missing_fields =
        summarize_action_setup_checkpoints(missing_fields_parsed.events, std::nullopt);
    EXPECT_EQ(
        action_setup_checkpoint_status_name(missing_fields.status),
        std::string("MissingLiveSetupFields"));

    std::istringstream missing_helpers_input(
        "pc=8008bc68 function=Battle::HandleECInst checkpoint=enemy_setup "
        "rng_draw_index_before=24 active_slot=4 instruction=3 target_slot=0 "
        "instr_param_0x6=1 movement_flags=0xc0 setup_rand=27 setup_rand_mod10=7 "
        "direct_close_candidate=1\n");
    const auto missing_helpers_parsed = parse_checkpoint_stream(missing_helpers_input);
    ASSERT_TRUE(missing_helpers_parsed.errors.empty());
    const auto missing_helpers = summarize_action_setup_checkpoints(
        missing_helpers_parsed.events,
        1);
    EXPECT_EQ(
        action_setup_checkpoint_status_name(missing_helpers.status),
        std::string("MissingEnemySetupHelperFields"));

    std::istringstream worker_mismatch_input(
        "pc=8008bc68 function=Battle::HandleECInst checkpoint=enemy_setup "
        "rng_draw_index_before=24 active_slot=4 instruction=3 target_slot=0 "
        "instr_param_0x6=1 movement_flags=0xc0 setup_rand=27 setup_rand_mod10=7 "
        "direct_close_candidate=0 final_instr_param_0x6=0 helper_8008a280_reached=true "
        "target_adjacent=1 selected_worker_pc=80087844\n");
    const auto worker_mismatch_parsed = parse_checkpoint_stream(worker_mismatch_input);
    ASSERT_TRUE(worker_mismatch_parsed.errors.empty());
    const auto worker_mismatch = summarize_action_setup_checkpoints(
        worker_mismatch_parsed.events,
        1);
    EXPECT_EQ(
        action_setup_checkpoint_status_name(worker_mismatch.status),
        std::string("WorkerMismatch"));
    EXPECT_EQ(worker_mismatch.worker_mismatches, 1);
}

TEST(SavorPredictCheckpointTrace, SummarizesActionViewGateCheckpoints) {
    std::istringstream input(
        "pc=80012f58 function=FUN_80012f58 checkpoint=action_view_gate rng_draw_index_before=18 "
        "action_sequence_id=7 "
        "active_slot=1 source_slot=1 target_slot=4 source_field6_0x6=14 actor_field6_0x6=14 "
        "aux_list_root=0x80346bd8 query_arg0=4 query_arg1=-1 query_arg2=0x2a query_arg3=3 "
        "query_result=0x81234567 selected_record_mode=0 "
        "action_child_thread=0x81230000 child_payload=0x81231000 nested_payload=0x81232000 "
        "child_thread_state_byte=1 mode0_fallback_reached=0\n"
        "pc=80051424 function=UpdateActionViewRecord checkpoint=action_view_dispatch_state "
        "rng_draw_index_before=19 payload_primary_0x00=4 payload_secondary_0x02=0 "
        "payload_flags_0x10=0x00000000 payload_start_frame_0x18=0 payload_end_frame_0x1c=70 "
        "payload_hold_0x1e=7 payload_step_0x20=10 payload_mode_0x22=0 "
        "worksheet_saved_mode_0x110=0 worksheet_effective_mode_0x112=0xe "
        "worksheet_turn_timer_0x70=0 instruction_flags_0xf0=0x10000000 "
        "global_camera_override_80347394=0x00000000 global_camera_flags_803472F4=0x00000000\n"
        "pc=80052bf0 function=FUN_80052b24 checkpoint=mode0e_camera "
        "rng_draw_index_before=20 action_sequence_id=7\n"
        "pc=80010bdc function=getAttackResult checkpoint=hit "
        "rng_draw_index_before=21 action_sequence_id=7\n");

    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.events.size(), 4u);

    const auto matched = summarize_action_view_gate_checkpoints(parsed.events);
    EXPECT_EQ(matched.status, ActionViewGateCheckpointStatus::MatchesExpected);
    EXPECT_EQ(matched.observed_gate_events, 1);
    EXPECT_EQ(matched.observed_dispatch_events, 1);
    EXPECT_EQ(matched.dispatch_events_with_spicestd_payload_fields, 1);
    EXPECT_EQ(matched.dispatch_serialized_mode0_events, 1);
    EXPECT_EQ(matched.dispatch_effective_mode0e_events, 1);
    EXPECT_EQ(matched.dispatch_mode0_to_mode0e_rewrites, 1);
    EXPECT_EQ(matched.dispatch_mode0_stays_mode0_events, 0);
    EXPECT_EQ(matched.events_with_aux_list_root, 1);
    EXPECT_EQ(matched.events_with_query_args, 1);
    EXPECT_EQ(matched.events_with_query_result, 1);
    EXPECT_EQ(matched.events_with_selected_record_mode, 1);
    EXPECT_EQ(matched.query_args_match, 1);
    EXPECT_EQ(matched.selected_mode_matches, 1);
    EXPECT_EQ(matched.events_with_action_child_thread, 1);
    EXPECT_EQ(matched.events_with_child_payload, 1);
    EXPECT_EQ(matched.events_with_nested_payload, 1);
    EXPECT_EQ(matched.events_with_child_thread_state, 1);
    EXPECT_EQ(matched.events_with_scheduler_chain, 1);
    EXPECT_EQ(matched.events_with_mode0_fallback_flag, 1);
    EXPECT_EQ(matched.mode0_fallback_reached_events, 0);
    EXPECT_EQ(matched.observed_mode0e_camera_draws, 1);
    EXPECT_EQ(matched.observed_mode0_fallback_draws, 0);
    EXPECT_EQ(matched.observed_attack_hit_draws, 1);
    EXPECT_EQ(matched.gate_events_before_first_mode0e, 1);
    EXPECT_EQ(matched.gate_events_before_first_attack_hit, 1);
    EXPECT_EQ(matched.mode0e_draws_before_first_attack_hit, 1);
    EXPECT_EQ(matched.mode0_fallback_draws_before_first_mode0e, 0);
    EXPECT_EQ(matched.mode0_fallback_draws_before_first_attack_hit, 0);
    EXPECT_EQ(matched.events_with_action_sequence_id, 1);
    EXPECT_EQ(matched.action_sequence_order_comparisons, 1);
    EXPECT_EQ(matched.action_sequence_order_matches, 1);
    EXPECT_EQ(matched.action_sequence_order_mismatches, 0);
    EXPECT_EQ(matched.action_sequence_order_missing_camera_or_hit, 0);
    ASSERT_EQ(matched.events.size(), 1u);
    ASSERT_TRUE(matched.events[0].action_sequence_id.has_value());
    EXPECT_EQ(*matched.events[0].action_sequence_id, 7);
    ASSERT_TRUE(matched.events[0].query_arg2.has_value());
    EXPECT_EQ(*matched.events[0].query_arg2, 0x2a);
    ASSERT_TRUE(matched.events[0].selected_record_mode.has_value());
    EXPECT_EQ(*matched.events[0].selected_record_mode, 0);
    ASSERT_EQ(matched.dispatch_events.size(), 1u);
    ASSERT_TRUE(matched.dispatch_events[0].effective_mode.has_value());
    EXPECT_EQ(*matched.dispatch_events[0].effective_mode, 0x0e);
    ASSERT_TRUE(matched.events[0].action_child_thread.has_value());
    EXPECT_EQ(*matched.events[0].action_child_thread, "0x81230000");
    ASSERT_TRUE(matched.events[0].child_thread_state_byte.has_value());
    EXPECT_EQ(*matched.events[0].child_thread_state_byte, 1);
    ASSERT_TRUE(matched.events[0].mode0_fallback_reached.has_value());
    EXPECT_FALSE(*matched.events[0].mode0_fallback_reached);
    ASSERT_TRUE(matched.events[0].matched_mode0e_draw_index.has_value());
    EXPECT_EQ(*matched.events[0].matched_mode0e_draw_index, 20);
    ASSERT_TRUE(matched.events[0].matched_attack_hit_draw_index.has_value());
    EXPECT_EQ(*matched.events[0].matched_attack_hit_draw_index, 21);
    ASSERT_TRUE(matched.events[0].gate_before_mode0e_draw.has_value());
    EXPECT_TRUE(*matched.events[0].gate_before_mode0e_draw);
    ASSERT_TRUE(matched.events[0].gate_before_attack_hit_draw.has_value());
    EXPECT_TRUE(*matched.events[0].gate_before_attack_hit_draw);
    ASSERT_TRUE(matched.events[0].mode0e_draw_before_attack_hit_draw.has_value());
    EXPECT_TRUE(*matched.events[0].mode0e_draw_before_attack_hit_draw);

    std::istringstream query_mismatch_input(
        "pc=80012f58 function=FUN_80012f58 checkpoint=action_view_gate rng_draw_index_before=18 "
        "aux_list_root=0x80346bd8 query_arg0=4 query_arg1=-1 query_arg2=0x2b query_arg3=3 "
        "query_result=0x81234567 selected_record_mode=0\n");
    const auto query_mismatch_parsed = parse_checkpoint_stream(query_mismatch_input);
    ASSERT_TRUE(query_mismatch_parsed.errors.empty());
    const auto query_mismatch = summarize_action_view_gate_checkpoints(query_mismatch_parsed.events);
    EXPECT_EQ(query_mismatch.status, ActionViewGateCheckpointStatus::QueryArgsMismatch);

    std::istringstream mode_mismatch_input(
        "pc=80012f58 function=FUN_80012f58 checkpoint=action_view_gate rng_draw_index_before=18 "
        "aux_list_root=0x80346bd8 query_arg0=4 query_arg1=-1 query_arg2=0x2a query_arg3=3 "
        "query_result=0x81234567 selected_record_mode=0xe\n");
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

    std::istringstream missing_scheduler_input(
        "pc=80012f58 function=FUN_80012f58 checkpoint=action_view_gate rng_draw_index_before=18 "
        "aux_list_root=0x80346bd8 query_arg0=4 query_arg1=-1 query_arg2=0x2a query_arg3=3 "
        "query_result=0x81234567 selected_record_mode=0\n");
    const auto missing_scheduler_parsed = parse_checkpoint_stream(missing_scheduler_input);
    ASSERT_TRUE(missing_scheduler_parsed.errors.empty());
    const auto missing_scheduler =
        summarize_action_view_gate_checkpoints(missing_scheduler_parsed.events);
    EXPECT_EQ(
        action_view_gate_checkpoint_status_name(missing_scheduler.status),
        std::string("MissingSchedulerFields"));

    std::istringstream order_mismatch_input(
        "pc=80052bf0 function=FUN_80052b24 checkpoint=mode0e_camera "
        "rng_draw_index_before=17 action_sequence_id=7\n"
        "pc=80012f58 function=FUN_80012f58 checkpoint=action_view_gate rng_draw_index_before=18 "
        "action_sequence_id=7 aux_list_root=0x80346bd8 "
        "query_arg0=4 query_arg1=-1 query_arg2=0x2a query_arg3=3 "
        "query_result=0x81234567 selected_record_mode=0 "
        "action_child_thread=0x81230000 child_payload=0x81231000 nested_payload=0x81232000 "
        "child_thread_state_byte=1 mode0_fallback_reached=0\n"
        "pc=80010bdc function=getAttackResult checkpoint=hit "
        "rng_draw_index_before=20 action_sequence_id=7\n");
    const auto order_mismatch_parsed = parse_checkpoint_stream(order_mismatch_input);
    ASSERT_TRUE(order_mismatch_parsed.errors.empty());
    const auto order_mismatch = summarize_action_view_gate_checkpoints(order_mismatch_parsed.events);
    EXPECT_EQ(order_mismatch.status, ActionViewGateCheckpointStatus::ActionViewOrderMismatch);
    EXPECT_EQ(
        action_view_gate_checkpoint_status_name(order_mismatch.status),
        std::string("ActionViewOrderMismatch"));
    EXPECT_EQ(order_mismatch.action_sequence_order_comparisons, 1);
    EXPECT_EQ(order_mismatch.action_sequence_order_mismatches, 1);
    ASSERT_EQ(order_mismatch.events.size(), 1u);
    ASSERT_TRUE(order_mismatch.events[0].gate_before_mode0e_draw.has_value());
    EXPECT_FALSE(*order_mismatch.events[0].gate_before_mode0e_draw);

    std::istringstream fallback_input(
        "pc=80012f58 function=FUN_80012f58 checkpoint=action_view_gate rng_draw_index_before=18 "
        "aux_list_root=0x80346bd8 query_arg0=4 query_arg1=-1 query_arg2=0x2a query_arg3=3 "
        "query_result=0x81234567 selected_record_mode=0 "
        "action_child_thread=0x81230000 child_payload=0x81231000 nested_payload=0x81232000 "
        "child_thread_state_byte=1 mode0_fallback_reached=true\n"
        "pc=800513d4 function=UpdateActionViewRecord checkpoint=mode0_fallback rng_draw_index_before=19\n");
    const auto fallback_parsed = parse_checkpoint_stream(fallback_input);
    ASSERT_TRUE(fallback_parsed.errors.empty());
    const auto fallback = summarize_action_view_gate_checkpoints(fallback_parsed.events);
    EXPECT_EQ(
        action_view_gate_checkpoint_status_name(fallback.status),
        std::string("MatchesExpected"));
    EXPECT_EQ(fallback.mode0_fallback_reached_events, 1);
    EXPECT_EQ(fallback.observed_mode0_fallback_draws, 1);
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
    EXPECT_EQ(with_fallback.status, ActionViewCameraCheckpointStatus::MatchesExpected);
    EXPECT_EQ(with_fallback.observed_mode0_fallback_draws, 1);
    EXPECT_EQ(
        action_view_camera_checkpoint_status_name(with_fallback.status),
        std::string("MatchesExpected"));

    const auto missing = summarize_action_view_camera_checkpoints(no_fallback, 2);
    EXPECT_EQ(missing.status, ActionViewCameraCheckpointStatus::MissingMode0eDraws);
}

TEST(SavorPredictCheckpointTrace, SummarizesTurnOrderCheckpoints) {
    std::istringstream input(
        "pc=800711f8 function=setupTurn checkpoint=priority rng_draw_index_before=10 "
        "active_slot=0 quick=22 fixed_priority_result=0 assigned_priority=27 "
        "rand_value=5 jitter_modulus=10 sum_quick=64 queued_count=3\n"
        "pc=800711f8 function=setupTurn checkpoint=priority rng_draw_index_before=11 "
        "active_slot=1 quick=24 fixed_priority_result=0 assigned_priority=30 "
        "rand_value=6 jitter_modulus=10 sum_quick=64 queued_count=3\n"
        "pc=800711f8 function=setupTurn checkpoint=priority rng_draw_index_before=12 "
        "slot=4 quick=18 fixed_priority_result=0 assigned_priority=21 "
        "rand_value=3 jitter_modulus=10 sum_quick=64 queued_count=3\n");

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
    EXPECT_EQ(matched.events_with_expected_first_battle_quick, 3);
    EXPECT_EQ(matched.quick_matches, 3);
    EXPECT_EQ(matched.quick_mismatches, 0);
    EXPECT_EQ(matched.events_with_fixed_priority_result, 3);
    EXPECT_EQ(matched.fixed_priority_zero_results, 3);
    EXPECT_EQ(matched.fixed_priority_nonzero_results, 0);
    EXPECT_EQ(matched.priority_sources_with_fixed_priority_result, 3);
    EXPECT_EQ(matched.priority_sources_missing_fixed_priority_result, 0);
    EXPECT_EQ(matched.events_with_queue_metadata, 3);
    EXPECT_EQ(matched.queue_metadata_matches, 3);
    EXPECT_EQ(matched.queue_metadata_mismatches, 0);
    EXPECT_EQ(matched.priority_draws_with_expected_priority, 3);
    EXPECT_EQ(matched.priority_matches, 3);
    EXPECT_EQ(matched.priority_mismatches, 0);
    ASSERT_EQ(matched.draws.size(), 3u);
    ASSERT_TRUE(matched.draws[2].slot.has_value());
    EXPECT_EQ(*matched.draws[2].slot, 4);
    ASSERT_TRUE(matched.draws[2].expected_assigned_priority.has_value());
    EXPECT_EQ(*matched.draws[2].expected_assigned_priority, 21);

    const auto missing = summarize_turn_order_checkpoints(parsed.events, 4);
    EXPECT_EQ(missing.status, TurnOrderCheckpointStatus::MissingPriorityJitterDraws);

    const auto extra = summarize_turn_order_checkpoints(parsed.events, 2);
    EXPECT_EQ(extra.status, TurnOrderCheckpointStatus::ExtraPriorityJitterDraws);
    EXPECT_EQ(
        turn_order_checkpoint_status_name(extra.status),
        std::string("ExtraPriorityJitterDraws"));

    std::istringstream priority_mismatch_input(
        "pc=800711f8 function=setupTurn checkpoint=priority rng_draw_index_before=10 "
        "active_slot=0 quick=22 fixed_priority_result=0 assigned_priority=28 "
        "rand_value=5 jitter_modulus=10\n");
    const auto priority_mismatch_parsed = parse_checkpoint_stream(priority_mismatch_input);
    ASSERT_TRUE(priority_mismatch_parsed.errors.empty());
    const auto priority_mismatch =
        summarize_turn_order_checkpoints(priority_mismatch_parsed.events, 1);
    EXPECT_EQ(priority_mismatch.status, TurnOrderCheckpointStatus::PriorityMismatch);
    EXPECT_EQ(
        turn_order_checkpoint_status_name(priority_mismatch.status),
        std::string("PriorityMismatch"));

    std::istringstream quick_mismatch_input(
        "pc=800711f8 function=setupTurn checkpoint=priority rng_draw_index_before=10 "
        "active_slot=0 quick=23 fixed_priority_result=0 assigned_priority=28 "
        "rand_value=5 jitter_modulus=10\n");
    const auto quick_mismatch_parsed = parse_checkpoint_stream(quick_mismatch_input);
    ASSERT_TRUE(quick_mismatch_parsed.errors.empty());
    const auto quick_mismatch =
        summarize_turn_order_checkpoints(quick_mismatch_parsed.events, 1);
    EXPECT_EQ(quick_mismatch.status, TurnOrderCheckpointStatus::QuickMismatch);
    EXPECT_EQ(
        turn_order_checkpoint_status_name(quick_mismatch.status),
        std::string("QuickMismatch"));

    std::istringstream metadata_mismatch_input(
        "pc=800711f8 function=setupTurn checkpoint=priority rng_draw_index_before=10 "
        "active_slot=0 quick=22 fixed_priority_result=0 assigned_priority=27 "
        "rand_value=5 jitter_modulus=10 sum_quick=100 queued_count=4\n");
    const auto metadata_mismatch_parsed = parse_checkpoint_stream(metadata_mismatch_input);
    ASSERT_TRUE(metadata_mismatch_parsed.errors.empty());
    const auto metadata_mismatch =
        summarize_turn_order_checkpoints(metadata_mismatch_parsed.events, 1);
    EXPECT_EQ(metadata_mismatch.status, TurnOrderCheckpointStatus::QueueMetadataMismatch);
    EXPECT_EQ(
        turn_order_checkpoint_status_name(metadata_mismatch.status),
        std::string("QueueMetadataMismatch"));

    std::istringstream missing_fixed_input(
        "pc=800711f8 function=setupTurn checkpoint=priority rng_draw_index_before=10 "
        "active_slot=0 quick=22 assigned_priority=27 rand_value=5 jitter_modulus=10\n");
    const auto missing_fixed_parsed = parse_checkpoint_stream(missing_fixed_input);
    ASSERT_TRUE(missing_fixed_parsed.errors.empty());
    const auto missing_fixed =
        summarize_turn_order_checkpoints(missing_fixed_parsed.events, 1);
    EXPECT_EQ(missing_fixed.status, TurnOrderCheckpointStatus::MissingFixedPriorityResults);
    EXPECT_EQ(
        turn_order_checkpoint_status_name(missing_fixed.status),
        std::string("MissingFixedPriorityResults"));
}

TEST(SavorPredictCheckpointTrace, SummarizesTurnOrderExecutionOrderCheckpoints) {
    std::istringstream input(
        "pc=800711f8 function=setupTurn checkpoint=priority rng_draw_index_before=10 "
        "active_slot=0 quick=22 fixed_priority_result=0 assigned_priority=27 "
        "rand_value=5 jitter_modulus=10 sum_quick=64 queued_count=3\n"
        "pc=800711f8 function=setupTurn checkpoint=priority rng_draw_index_before=11 "
        "active_slot=1 quick=24 fixed_priority_result=0 assigned_priority=30 "
        "rand_value=6 jitter_modulus=10 sum_quick=64 queued_count=3\n"
        "pc=800711f8 function=setupTurn checkpoint=priority rng_draw_index_before=12 "
        "slot=4 quick=18 fixed_priority_result=0 assigned_priority=21 "
        "rand_value=3 jitter_modulus=10 sum_quick=64 queued_count=3\n"
        "pc=80070c18 function=setupTurn checkpoint=queued_entry rng_draw_index_before=20 "
        "queue_index=0 slot=0 quick=22 fixed_priority_result=0 assigned_priority=27 "
        "queued_instruction=3 jitter_modulus=10 sum_quick=64 queued_count=3\n"
        "pc=80070c18 function=setupTurn checkpoint=queued_entry rng_draw_index_before=20 "
        "queue_index=1 slot=1 quick=24 fixed_priority_result=0 assigned_priority=30 "
        "queued_instruction=3 jitter_modulus=10 sum_quick=64 queued_count=3\n"
        "pc=80070c18 function=setupTurn checkpoint=queued_entry rng_draw_index_before=20 "
        "queue_index=2 slot=4 quick=18 fixed_priority_result=0 assigned_priority=21 "
        "queued_instruction=3 jitter_modulus=10 sum_quick=64 queued_count=3\n"
        "pc=80071280 function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=0 slot=1\n"
        "pc=80071280 function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=1 slot=0\n"
        "pc=80071280 function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=2 slot=4\n");

    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());

    const auto matched = summarize_turn_order_checkpoints(parsed.events, std::nullopt);
    EXPECT_EQ(matched.status, TurnOrderCheckpointStatus::MatchesExpected);
    EXPECT_EQ(matched.observed_priority_jitter_draws, 3);
    EXPECT_EQ(matched.observed_queue_entries, 3);
    EXPECT_EQ(matched.observed_execution_order_entries, 3);
    EXPECT_TRUE(matched.execution_order_compared);
    EXPECT_TRUE(matched.execution_order_exact);
    EXPECT_FALSE(matched.priority_ties_observed);
    EXPECT_EQ(matched.execution_order_matches, 3);
    EXPECT_EQ(matched.execution_order_mismatches, 0);
    ASSERT_EQ(matched.expected_execution_slots.size(), 3u);
    EXPECT_EQ(matched.expected_execution_slots[0], 1);
    EXPECT_EQ(matched.expected_execution_slots[1], 0);
    EXPECT_EQ(matched.expected_execution_slots[2], 4);
    ASSERT_EQ(matched.observed_execution_slots.size(), 3u);
    EXPECT_EQ(matched.observed_execution_slots[0], 1);
    EXPECT_EQ(matched.observed_execution_slots[1], 0);
    EXPECT_EQ(matched.observed_execution_slots[2], 4);
    EXPECT_EQ(matched.priority_tie_groups, 0);
    EXPECT_EQ(matched.priority_tied_entries, 0);
    EXPECT_EQ(matched.tie_groups_with_observed_execution_order, 0);

    std::istringstream mismatch_input(
        "pc=80070c18 function=setupTurn checkpoint=queued_entry rng_draw_index_before=20 "
        "slot=0 quick=22 fixed_priority_result=0 assigned_priority=27\n"
        "pc=80070c18 function=setupTurn checkpoint=queued_entry rng_draw_index_before=20 "
        "slot=1 quick=24 fixed_priority_result=0 assigned_priority=30\n"
        "pc=80071280 function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=0 slot=0\n"
        "pc=80071280 function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=1 slot=1\n");
    const auto mismatch_parsed = parse_checkpoint_stream(mismatch_input);
    ASSERT_TRUE(mismatch_parsed.errors.empty());
    const auto mismatch = summarize_turn_order_checkpoints(mismatch_parsed.events, std::nullopt);
    EXPECT_EQ(mismatch.status, TurnOrderCheckpointStatus::ExecutionOrderMismatch);

    std::istringstream tied_input(
        "pc=80070c18 function=setupTurn checkpoint=queued_entry rng_draw_index_before=20 "
        "queue_index=0 slot=0 quick=22 fixed_priority_result=0 assigned_priority=27 "
        "rand_value=5 jitter_modulus=10 sum_quick=40 queued_count=2\n"
        "pc=80070c18 function=setupTurn checkpoint=queued_entry rng_draw_index_before=20 "
        "queue_index=1 slot=4 quick=18 fixed_priority_result=0 assigned_priority=27 "
        "rand_value=9 jitter_modulus=10 sum_quick=40 queued_count=2\n"
        "pc=80071280 function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=0 slot=4\n"
        "pc=80071280 function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=1 slot=0\n");
    const auto tied_parsed = parse_checkpoint_stream(tied_input);
    ASSERT_TRUE(tied_parsed.errors.empty());
    const auto tied = summarize_turn_order_checkpoints(tied_parsed.events, std::nullopt);
    EXPECT_EQ(tied.status, TurnOrderCheckpointStatus::MatchesExpected);
    EXPECT_TRUE(tied.priority_ties_observed);
    EXPECT_FALSE(tied.execution_order_exact);
    EXPECT_FALSE(tied.execution_order_compared);
    EXPECT_EQ(tied.priority_tie_groups, 1);
    EXPECT_EQ(tied.priority_tied_entries, 2);
    EXPECT_EQ(tied.tie_groups_with_observed_execution_order, 1);
    EXPECT_EQ(tied.tie_groups_matching_queue_ascending, 0);
    EXPECT_EQ(tied.tie_groups_matching_queue_descending, 1);
    ASSERT_EQ(tied.tie_groups.size(), 1u);
    const auto& group = tied.tie_groups[0];
    EXPECT_EQ(group.assigned_priority, 27);
    ASSERT_EQ(group.queue_indices.size(), 2u);
    EXPECT_EQ(group.queue_indices[0], 0);
    EXPECT_EQ(group.queue_indices[1], 1);
    ASSERT_EQ(group.slots_by_queue_order.size(), 2u);
    EXPECT_EQ(group.slots_by_queue_order[0], 0);
    EXPECT_EQ(group.slots_by_queue_order[1], 4);
    ASSERT_EQ(group.observed_execution_slots.size(), 2u);
    EXPECT_EQ(group.observed_execution_slots[0], 4);
    EXPECT_EQ(group.observed_execution_slots[1], 0);
    EXPECT_TRUE(group.observed_order_compared);
    EXPECT_FALSE(group.observed_order_matches_queue_ascending);
    EXPECT_TRUE(group.observed_order_matches_queue_descending);
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

TEST(SavorPredictCheckpointTrace, SummarizesAttackDamageValueCheckpoints) {
    const std::string live_hit_fields =
        "active_slot=0 target_slot=4 attacker_attack=10 attacker_hit=100 attacker_agile=50 "
        "attacker_element=0 target_defense=5 target_dodge=0 "
        "target_element_effectiveness_tenths=10 target_status_flags=0 instr_param_0x6=0 ";

    std::istringstream input(
        "pc=80010bdc function=getAttackResult checkpoint=hit rng_draw_index_before=20 "
        + live_hit_fields
        + "rand_value=10000 attack_result=2\n"
        "pc=80010c44 function=getAttackResult checkpoint=crit rng_draw_index_before=21 "
        "rand_value=10\n"
        "pc=80010958 function=rollDamage checkpoint=spread rng_draw_index_before=22 "
        "rand_value=0\n"
        "pc=80010984 function=rollDamage checkpoint=bonus rng_draw_index_before=23 "
        "rand_value=1 observed_damage=19\n"
        "pc=8002dd14 function=zzDealDamage checkpoint=damage_apply rng_draw_index_before=24 "
        "damage=19 hp_before=25 hp_after=6 lethal=0\n");

    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());
    const auto matched = summarize_attack_damage_value_checkpoints(parsed.events);
    EXPECT_EQ(matched.status, AttackDamageValueCheckpointStatus::MatchesFormula);
    EXPECT_EQ(matched.observed_attack_bursts, 1);
    EXPECT_EQ(matched.bursts_with_live_inputs, 1);
    EXPECT_EQ(matched.bursts_with_required_draws, 1);
    EXPECT_EQ(matched.simulated_bursts, 1);
    EXPECT_EQ(matched.attack_result_matches, 1);
    EXPECT_EQ(matched.damage_matches, 1);
    EXPECT_EQ(matched.bursts_with_damage_apply, 1);
    EXPECT_EQ(matched.bursts_with_damage_apply_fields, 1);
    EXPECT_EQ(matched.damage_apply_events_after_damage_draws, 1);
    EXPECT_EQ(matched.damage_apply_matches, 1);
    EXPECT_EQ(matched.hp_after_matches, 1);
    EXPECT_EQ(matched.lethal_matches, 1);
    ASSERT_EQ(matched.attacks.size(), 1u);
    EXPECT_EQ(matched.attacks[0].expected_attack_result, std::optional<int>(2));
    EXPECT_EQ(matched.attacks[0].expected_base_damage, std::optional<int>(20));
    EXPECT_EQ(matched.attacks[0].expected_damage, std::optional<int>(19));
    EXPECT_EQ(matched.attacks[0].expected_hp_after, std::optional<int>(6));
    EXPECT_EQ(matched.attacks[0].expected_lethal, std::optional<int>(0));

    std::istringstream missing_fields_input(
        "pc=80010bdc function=getAttackResult checkpoint=hit rng_draw_index_before=20 "
        "active_slot=0 target_slot=4 attacker_attack=10 attacker_hit=100 attacker_agile=50 "
        "attacker_element=0 target_dodge=0 target_element_effectiveness_tenths=10 "
        "target_status_flags=0 instr_param_0x6=0 rand_value=10000 attack_result=2\n");
    const auto missing_fields_parsed = parse_checkpoint_stream(missing_fields_input);
    ASSERT_TRUE(missing_fields_parsed.errors.empty());
    const auto missing_fields =
        summarize_attack_damage_value_checkpoints(missing_fields_parsed.events);
    EXPECT_EQ(
        attack_damage_value_checkpoint_status_name(missing_fields.status),
        std::string("MissingLiveDamageFields"));

    std::istringstream incomplete_input(
        "pc=80010bdc function=getAttackResult checkpoint=hit rng_draw_index_before=20 "
        + live_hit_fields
        + "rand_value=10000 attack_result=2\n"
        "pc=80010c44 function=getAttackResult checkpoint=crit rng_draw_index_before=21 "
        "rand_value=10\n"
        "pc=80010958 function=rollDamage checkpoint=spread rng_draw_index_before=22 "
        "rand_value=0\n");
    const auto incomplete_parsed = parse_checkpoint_stream(incomplete_input);
    ASSERT_TRUE(incomplete_parsed.errors.empty());
    const auto incomplete = summarize_attack_damage_value_checkpoints(incomplete_parsed.events);
    EXPECT_EQ(incomplete.status, AttackDamageValueCheckpointStatus::IncompleteDrawSequence);

    std::istringstream attack_result_mismatch_input(
        "pc=80010bdc function=getAttackResult checkpoint=hit rng_draw_index_before=20 "
        + live_hit_fields
        + "rand_value=10000 attack_result=1\n"
        "pc=80010c44 function=getAttackResult checkpoint=crit rng_draw_index_before=21 "
        "rand_value=10\n"
        "pc=80010958 function=rollDamage checkpoint=spread rng_draw_index_before=22 "
        "rand_value=0\n"
        "pc=80010984 function=rollDamage checkpoint=bonus rng_draw_index_before=23 "
        "rand_value=1 observed_damage=19\n"
        "pc=8002dd14 function=zzDealDamage checkpoint=damage_apply rng_draw_index_before=24 "
        "damage=19 hp_before=25 hp_after=6 lethal=0\n");
    const auto attack_result_mismatch_parsed =
        parse_checkpoint_stream(attack_result_mismatch_input);
    ASSERT_TRUE(attack_result_mismatch_parsed.errors.empty());
    const auto attack_result_mismatch =
        summarize_attack_damage_value_checkpoints(attack_result_mismatch_parsed.events);
    EXPECT_EQ(
        attack_damage_value_checkpoint_status_name(attack_result_mismatch.status),
        std::string("AttackResultMismatch"));

    std::istringstream damage_mismatch_input(
        "pc=80010bdc function=getAttackResult checkpoint=hit rng_draw_index_before=20 "
        + live_hit_fields
        + "rand_value=10000 attack_result=2\n"
        "pc=80010c44 function=getAttackResult checkpoint=crit rng_draw_index_before=21 "
        "rand_value=10\n"
        "pc=80010958 function=rollDamage checkpoint=spread rng_draw_index_before=22 "
        "rand_value=0\n"
        "pc=80010984 function=rollDamage checkpoint=bonus rng_draw_index_before=23 "
        "rand_value=1 observed_damage=18\n"
        "pc=8002dd14 function=zzDealDamage checkpoint=damage_apply rng_draw_index_before=24 "
        "damage=19 hp_before=25 hp_after=6 lethal=0\n");
    const auto damage_mismatch_parsed = parse_checkpoint_stream(damage_mismatch_input);
    ASSERT_TRUE(damage_mismatch_parsed.errors.empty());
    const auto damage_mismatch =
        summarize_attack_damage_value_checkpoints(damage_mismatch_parsed.events);
    EXPECT_EQ(damage_mismatch.status, AttackDamageValueCheckpointStatus::DamageMismatch);

    std::istringstream missing_apply_input(
        "pc=80010bdc function=getAttackResult checkpoint=hit rng_draw_index_before=20 "
        + live_hit_fields
        + "rand_value=10000 attack_result=2\n"
        "pc=80010c44 function=getAttackResult checkpoint=crit rng_draw_index_before=21 "
        "rand_value=10\n"
        "pc=80010958 function=rollDamage checkpoint=spread rng_draw_index_before=22 "
        "rand_value=0\n"
        "pc=80010984 function=rollDamage checkpoint=bonus rng_draw_index_before=23 "
        "rand_value=1 observed_damage=19\n");
    const auto missing_apply_parsed = parse_checkpoint_stream(missing_apply_input);
    ASSERT_TRUE(missing_apply_parsed.errors.empty());
    const auto missing_apply =
        summarize_attack_damage_value_checkpoints(missing_apply_parsed.events);
    EXPECT_EQ(
        attack_damage_value_checkpoint_status_name(missing_apply.status),
        std::string("MissingDamageApplyFields"));

    std::istringstream apply_mismatch_input(
        "pc=80010bdc function=getAttackResult checkpoint=hit rng_draw_index_before=20 "
        + live_hit_fields
        + "rand_value=10000 attack_result=2\n"
        "pc=80010c44 function=getAttackResult checkpoint=crit rng_draw_index_before=21 "
        "rand_value=10\n"
        "pc=80010958 function=rollDamage checkpoint=spread rng_draw_index_before=22 "
        "rand_value=0\n"
        "pc=80010984 function=rollDamage checkpoint=bonus rng_draw_index_before=23 "
        "rand_value=1 observed_damage=19\n"
        "pc=8002dd14 function=zzDealDamage checkpoint=damage_apply rng_draw_index_before=24 "
        "damage=19 hp_before=25 hp_after=7 lethal=0\n");
    const auto apply_mismatch_parsed = parse_checkpoint_stream(apply_mismatch_input);
    ASSERT_TRUE(apply_mismatch_parsed.errors.empty());
    const auto apply_mismatch =
        summarize_attack_damage_value_checkpoints(apply_mismatch_parsed.events);
    EXPECT_EQ(apply_mismatch.status, AttackDamageValueCheckpointStatus::DamageApplyMismatch);
    EXPECT_EQ(apply_mismatch.hp_after_mismatches, 1);

    std::istringstream apply_order_input(
        "pc=80010bdc function=getAttackResult checkpoint=hit rng_draw_index_before=20 "
        + live_hit_fields
        + "rand_value=10000 attack_result=2\n"
        "pc=80010c44 function=getAttackResult checkpoint=crit rng_draw_index_before=21 "
        "rand_value=10\n"
        "pc=80010958 function=rollDamage checkpoint=spread rng_draw_index_before=22 "
        "rand_value=0\n"
        "pc=80010984 function=rollDamage checkpoint=bonus rng_draw_index_before=23 "
        "rand_value=1 observed_damage=19\n"
        "pc=8002dd14 function=zzDealDamage checkpoint=damage_apply rng_draw_index_before=23 "
        "damage=19 hp_before=25 hp_after=6 lethal=0\n");
    const auto apply_order_parsed = parse_checkpoint_stream(apply_order_input);
    ASSERT_TRUE(apply_order_parsed.errors.empty());
    const auto apply_order =
        summarize_attack_damage_value_checkpoints(apply_order_parsed.events);
    EXPECT_EQ(apply_order.status, AttackDamageValueCheckpointStatus::DamageApplyOrderMismatch);
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
    EXPECT_EQ(within.status, CounterCheckpointStatus::MatchesLiveGate);
    EXPECT_EQ(within.observed_counter_rolls, 2);
    ASSERT_TRUE(within.first_counter_roll_draw_index.has_value());
    EXPECT_EQ(*within.first_counter_roll_draw_index, 30);
    ASSERT_TRUE(within.last_counter_roll_draw_index.has_value());
    EXPECT_EQ(*within.last_counter_roll_draw_index, 31);
    EXPECT_EQ(within.draws_with_actor_slots, 2);
    EXPECT_EQ(within.draws_with_gate_inputs, 2);
    EXPECT_EQ(within.draws_with_rand_value, 2);
    EXPECT_EQ(within.draws_with_counter_result, 2);
    EXPECT_EQ(within.draws_with_queue_result, 1);
    EXPECT_EQ(within.draws_with_counter_chance_update, 2);
    EXPECT_EQ(within.live_gate_simulated_draws, 2);
    EXPECT_EQ(within.counter_result_matches, 2);
    EXPECT_EQ(within.counter_chance_update_matches, 2);
    ASSERT_EQ(within.draws.size(), 2u);
    ASSERT_TRUE(within.draws[0].target_status_flags.has_value());
    EXPECT_EQ(*within.draws[0].target_status_flags, 0);
    ASSERT_TRUE(within.draws[0].target_movement_flags.has_value());
    EXPECT_EQ(*within.draws[0].target_movement_flags, 0xc0);
    ASSERT_TRUE(within.draws[0].expected_counter_result.has_value());
    EXPECT_EQ(*within.draws[0].expected_counter_result, 1);
    ASSERT_TRUE(within.draws[0].expected_reason.has_value());
    EXPECT_EQ(*within.draws[0].expected_reason, CounterResultReason::Counter);

    const auto observed_only = summarize_counter_checkpoints(parsed.events, std::nullopt);
    EXPECT_EQ(observed_only.status, CounterCheckpointStatus::MatchesLiveGate);

    const auto exceeds = summarize_counter_checkpoints(parsed.events, 1);
    EXPECT_EQ(exceeds.status, CounterCheckpointStatus::ExceedsExpectedCeiling);
    EXPECT_EQ(
        counter_checkpoint_status_name(exceeds.status),
        std::string("ExceedsExpectedCeiling"));

    std::istringstream count_only_input(
        "pc=80081a88 function=shouldCounter checkpoint=roll rng_draw_index_before=30\n");
    const auto count_only_parsed = parse_checkpoint_stream(count_only_input);
    ASSERT_TRUE(count_only_parsed.errors.empty());
    const auto count_only = summarize_counter_checkpoints(count_only_parsed.events, 1);
    EXPECT_EQ(count_only.status, CounterCheckpointStatus::WithinExpectedCeiling);

    std::istringstream missing_fields_input(
        "pc=80081a88 function=shouldCounter checkpoint=roll rng_draw_index_before=30 "
        "attacker_slot=1 target_slot=4 counter_rand=7\n");
    const auto missing_fields_parsed = parse_checkpoint_stream(missing_fields_input);
    ASSERT_TRUE(missing_fields_parsed.errors.empty());
    const auto missing_fields = summarize_counter_checkpoints(missing_fields_parsed.events, 1);
    EXPECT_EQ(
        counter_checkpoint_status_name(missing_fields.status),
        std::string("MissingLiveGateFields"));

    std::istringstream result_mismatch_input(
        "pc=80081a88 function=shouldCounter checkpoint=roll rng_draw_index_before=30 "
        "attacker_slot=1 target_slot=4 target_status_flags=0x0 target_movement_flags=0xc0 "
        "target_base_counter_chance=10 target_current_counter_chance=10 "
        "attacker_action_marker=0 attack_was_critical=0 counter_rand=7 "
        "counter_result=0 queued_field7_0xc=0 updated_current_counter_chance=0\n");
    const auto result_mismatch_parsed = parse_checkpoint_stream(result_mismatch_input);
    ASSERT_TRUE(result_mismatch_parsed.errors.empty());
    const auto result_mismatch = summarize_counter_checkpoints(result_mismatch_parsed.events, 1);
    EXPECT_EQ(result_mismatch.status, CounterCheckpointStatus::CounterResultMismatch);

    std::istringstream queue_mismatch_input(
        "pc=80081a88 function=shouldCounter checkpoint=roll rng_draw_index_before=30 "
        "attacker_slot=1 target_slot=4 target_status_flags=0x0 target_movement_flags=0xc0 "
        "target_base_counter_chance=10 target_current_counter_chance=10 "
        "attacker_action_marker=0 attack_was_critical=0 counter_rand=7 "
        "counter_result=1 queued_field7_0xc=1 updated_current_counter_chance=0\n");
    const auto queue_mismatch_parsed = parse_checkpoint_stream(queue_mismatch_input);
    ASSERT_TRUE(queue_mismatch_parsed.errors.empty());
    const auto queue_mismatch = summarize_counter_checkpoints(queue_mismatch_parsed.events, 1);
    EXPECT_EQ(queue_mismatch.status, CounterCheckpointStatus::CounterQueueMismatch);

    std::istringstream update_mismatch_input(
        "pc=80081a88 function=shouldCounter checkpoint=roll rng_draw_index_before=30 "
        "attacker_slot=1 target_slot=4 target_status_flags=0x0 target_movement_flags=0xc0 "
        "target_base_counter_chance=10 target_current_counter_chance=10 "
        "attacker_action_marker=0 attack_was_critical=0 counter_rand=7 "
        "counter_result=1 queued_field7_0xc=0 updated_current_counter_chance=10\n");
    const auto update_mismatch_parsed = parse_checkpoint_stream(update_mismatch_input);
    ASSERT_TRUE(update_mismatch_parsed.errors.empty());
    const auto update_mismatch = summarize_counter_checkpoints(update_mismatch_parsed.events, 1);
    EXPECT_EQ(update_mismatch.status, CounterCheckpointStatus::CounterChanceUpdateMismatch);
}

TEST(SavorPredictCheckpointTrace, SummarizesCounterGateAttemptsWithoutDraws) {
    std::istringstream no_draw_input(
        "pc=800819d0 function=shouldCounter_800819d0 checkpoint=gate rng_draw_index_before=44 "
        "attacker_slot=1 target_slot=4 target_status_flags=0x0 target_movement_flags=0xc0 "
        "target_base_counter_chance=10 target_current_counter_chance=10 "
        "attacker_action_marker=0 attack_was_critical=1 "
        "counter_result=0 updated_current_counter_chance=10\n");

    const auto no_draw_parsed = parse_checkpoint_stream(no_draw_input);
    ASSERT_TRUE(no_draw_parsed.errors.empty());

    const auto no_draw = summarize_counter_checkpoints(no_draw_parsed.events, std::nullopt);
    EXPECT_EQ(no_draw.status, CounterCheckpointStatus::MatchesLiveGate);
    EXPECT_EQ(no_draw.observed_counter_gate_attempts, 1);
    EXPECT_EQ(no_draw.gate_attempts_with_live_inputs, 1);
    EXPECT_EQ(no_draw.expected_counter_rolls_from_gate_inputs, 0);
    EXPECT_EQ(no_draw.expected_no_draw_gate_attempts, 1);
    EXPECT_EQ(no_draw.no_draw_gate_attempts_simulated, 1);
    ASSERT_EQ(no_draw.draws.size(), 1u);
    EXPECT_EQ(no_draw.draws[0].kind, CounterCheckpointKind::GateAttempt);
    ASSERT_TRUE(no_draw.draws[0].expected_counter_rolls.has_value());
    EXPECT_EQ(*no_draw.draws[0].expected_counter_rolls, 0);
    ASSERT_TRUE(no_draw.draws[0].expected_reason.has_value());
    EXPECT_EQ(*no_draw.draws[0].expected_reason, CounterResultReason::CriticalSuppressed);

    std::istringstream missing_roll_input(
        "pc=800819d0 function=shouldCounter_800819d0 checkpoint=gate rng_draw_index_before=44 "
        "attacker_slot=1 target_slot=4 target_status_flags=0x0 target_movement_flags=0xc0 "
        "target_base_counter_chance=10 target_current_counter_chance=10 "
        "attacker_action_marker=0 attack_was_critical=0\n");
    const auto missing_roll_parsed = parse_checkpoint_stream(missing_roll_input);
    ASSERT_TRUE(missing_roll_parsed.errors.empty());
    const auto missing_roll = summarize_counter_checkpoints(missing_roll_parsed.events, std::nullopt);
    EXPECT_EQ(missing_roll.status, CounterCheckpointStatus::MissingCounterRolls);
    EXPECT_EQ(
        counter_checkpoint_status_name(missing_roll.status),
        std::string("MissingCounterRolls"));
}

TEST(SavorPredictCheckpointTrace, SummarizesCounterFollowUpActions) {
    std::istringstream missing_follow_up_input(
        "pc=800819d0 function=shouldCounter_800819d0 checkpoint=gate rng_draw_index_before=50 "
        "attacker_slot=1 target_slot=4 target_status_flags=0x2 target_movement_flags=0xc0 "
        "target_base_counter_chance=10 target_current_counter_chance=10 "
        "attacker_action_marker=0 attack_was_critical=0 "
        "counter_result=1 queued_field7_0xc=0 updated_current_counter_chance=100\n");
    const auto missing_follow_up_parsed = parse_checkpoint_stream(missing_follow_up_input);
    ASSERT_TRUE(missing_follow_up_parsed.errors.empty());
    const auto missing_follow_up =
        summarize_counter_checkpoints(missing_follow_up_parsed.events, std::nullopt);
    EXPECT_EQ(missing_follow_up.status, CounterCheckpointStatus::MissingCounterFollowUp);
    EXPECT_EQ(missing_follow_up.expected_counter_follow_up_events, 1);
    EXPECT_EQ(missing_follow_up.observed_counter_follow_up_events, 0);

    std::istringstream matched_input(
        "pc=800819d0 function=shouldCounter_800819d0 checkpoint=gate rng_draw_index_before=50 "
        "attacker_slot=1 target_slot=4 target_status_flags=0x2 target_movement_flags=0xc0 "
        "target_base_counter_chance=10 target_current_counter_chance=10 "
        "attacker_action_marker=0 attack_was_critical=0 "
        "counter_result=1 queued_field7_0xc=0 updated_current_counter_chance=100\n"
        "pc=80082134 function=setupTurnAction checkpoint=counter_follow_up rng_draw_index_before=50 "
        "attacker_slot=1 target_slot=4 counter_follow_up=1\n");
    const auto matched_parsed = parse_checkpoint_stream(matched_input);
    ASSERT_TRUE(matched_parsed.errors.empty());
    const auto matched = summarize_counter_checkpoints(matched_parsed.events, std::nullopt);
    EXPECT_EQ(matched.status, CounterCheckpointStatus::MatchesLiveGate);
    EXPECT_EQ(matched.expected_counter_follow_up_events, 1);
    EXPECT_EQ(matched.observed_counter_follow_up_events, 1);
    ASSERT_EQ(matched.draws.size(), 2u);
    EXPECT_EQ(matched.draws[1].kind, CounterCheckpointKind::CounterFollowUp);
    EXPECT_EQ(
        counter_checkpoint_kind_name(matched.draws[1].kind),
        std::string("CounterFollowUp"));
}

TEST(SavorPredictCheckpointTrace, SummarizesDropCheckpoints) {
    std::istringstream input(
        "pc=80052bf0 function=FUN_80052b24 checkpoint=mode0e_camera rng_draw_index_before=20\n"
        "pc=80010628 function=status checkpoint=pc rng_draw_index_before=22\n"
        "pc=80010984 function=rollDamage checkpoint=bonus rng_draw_index_before=23\n"
        "pc=8002bad8 function=enemyDropItem checkpoint=row rng_draw_index_before=24 "
        "target_slot=4 enemy_entry_id=0 drop_row_index=1 drop_item_id=273 "
        "drop_amount=1 rand_value=44 rand_mod100=44 drop_success=0\n"
        "pc=80081a88 function=shouldCounter checkpoint=roll rng_draw_index_before=25\n"
        "pc=80010984 function=rollDamage checkpoint=bonus rng_draw_index_before=26\n"
        "pc=8002bad8 function=enemyDropItem checkpoint=row rng_draw_index_before=27 "
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
    EXPECT_EQ(matched.first_battle_drop_rows_validated, 2);
    EXPECT_EQ(matched.drop_rolls_with_live_outcome_fields, 2);
    EXPECT_EQ(matched.drop_rolls_missing_live_outcome_fields, 0);
    EXPECT_EQ(matched.drop_table_matches, 2);
    EXPECT_EQ(matched.drop_table_mismatches, 0);
    EXPECT_EQ(matched.drop_outcome_matches, 2);
    EXPECT_EQ(matched.drop_outcome_mismatches, 0);
    EXPECT_EQ(matched.disabled_first_battle_rows_observed, 0);
    EXPECT_EQ(matched.successful_drop_rolls, 1);
    EXPECT_EQ(matched.failed_drop_rolls, 1);
    EXPECT_EQ(matched.drop_rolls_after_success, 0);
    ASSERT_TRUE(matched.final_drop_row_index.has_value());
    EXPECT_EQ(*matched.final_drop_row_index, 2);
    ASSERT_TRUE(matched.final_drop_item_id.has_value());
    EXPECT_EQ(*matched.final_drop_item_id, 258);
    ASSERT_TRUE(matched.final_drop_amount.has_value());
    EXPECT_EQ(*matched.final_drop_amount, 1);
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
    ASSERT_TRUE(matched.draws[1].expected_drop_success.has_value());
    EXPECT_EQ(*matched.draws[1].expected_drop_success, 1);
    EXPECT_TRUE(matched.draws[1].drop_table_matches);
    EXPECT_TRUE(matched.draws[1].drop_outcome_matches);

    const auto missing = summarize_drop_checkpoints(parsed.events, 3);
    EXPECT_EQ(missing.status, DropCheckpointStatus::MissingDropRolls);

    const auto extra = summarize_drop_checkpoints(parsed.events, 1);
    EXPECT_EQ(extra.status, DropCheckpointStatus::ExtraDropRolls);
    EXPECT_EQ(
        drop_checkpoint_status_name(extra.status),
        std::string("ExtraDropRolls"));

    std::istringstream outcome_mismatch_input(
        "pc=8002bad8 function=enemyDropItem checkpoint=row rng_draw_index_before=24 "
        "target_slot=4 enemy_entry_id=0 drop_row_index=1 drop_item_id=273 "
        "drop_amount=1 rand_value=44 rand_mod100=44 drop_success=1\n");
    const auto outcome_mismatch_parsed = parse_checkpoint_stream(outcome_mismatch_input);
    ASSERT_TRUE(outcome_mismatch_parsed.errors.empty());
    const auto outcome_mismatch =
        summarize_drop_checkpoints(outcome_mismatch_parsed.events, std::nullopt);
    EXPECT_EQ(outcome_mismatch.status, DropCheckpointStatus::DropOutcomeMismatch);
    EXPECT_EQ(
        drop_checkpoint_status_name(outcome_mismatch.status),
        std::string("DropOutcomeMismatch"));
    EXPECT_EQ(outcome_mismatch.drop_outcome_mismatches, 1);

    std::istringstream table_mismatch_input(
        "pc=8002bad8 function=enemyDropItem checkpoint=row rng_draw_index_before=24 "
        "target_slot=4 enemy_entry_id=0 drop_row_index=2 drop_item_id=273 "
        "drop_amount=1 rand_value=44 rand_mod100=44 drop_success=0\n");
    const auto table_mismatch_parsed = parse_checkpoint_stream(table_mismatch_input);
    ASSERT_TRUE(table_mismatch_parsed.errors.empty());
    const auto table_mismatch =
        summarize_drop_checkpoints(table_mismatch_parsed.events, std::nullopt);
    EXPECT_EQ(table_mismatch.status, DropCheckpointStatus::DropTableMismatch);
    EXPECT_EQ(table_mismatch.drop_table_mismatches, 1);

    std::istringstream disabled_row_input(
        "pc=8002bad8 function=enemyDropItem checkpoint=row rng_draw_index_before=24 "
        "target_slot=4 enemy_entry_id=0 drop_row_index=3 drop_item_id=0 "
        "drop_amount=0 rand_value=44 rand_mod100=44 drop_success=0\n");
    const auto disabled_row_parsed = parse_checkpoint_stream(disabled_row_input);
    ASSERT_TRUE(disabled_row_parsed.errors.empty());
    const auto disabled_row =
        summarize_drop_checkpoints(disabled_row_parsed.events, std::nullopt);
    EXPECT_EQ(disabled_row.status, DropCheckpointStatus::DropTableMismatch);
    EXPECT_EQ(disabled_row.disabled_first_battle_rows_observed, 1);

    std::istringstream continuation_input(
        "pc=8002bad8 function=enemyDropItem checkpoint=row rng_draw_index_before=24 "
        "target_slot=4 enemy_entry_id=0 drop_row_index=1 drop_item_id=273 "
        "drop_amount=1 rand_value=0 rand_mod100=0 drop_success=1\n"
        "pc=8002bad8 function=enemyDropItem checkpoint=row rng_draw_index_before=25 "
        "target_slot=4 enemy_entry_id=0 drop_row_index=2 drop_item_id=258 "
        "drop_amount=1 rand_value=44 rand_mod100=44 drop_success=0\n");
    const auto continuation_parsed = parse_checkpoint_stream(continuation_input);
    ASSERT_TRUE(continuation_parsed.errors.empty());
    const auto continuation =
        summarize_drop_checkpoints(continuation_parsed.events, std::nullopt);
    EXPECT_EQ(continuation.status, DropCheckpointStatus::DropContinuationMismatch);
    EXPECT_EQ(continuation.drop_rolls_after_success, 1);
    ASSERT_EQ(continuation.draws.size(), 2u);
    EXPECT_TRUE(continuation.draws[1].roll_after_success);
}

TEST(SavorPredictCheckpointTrace, SummarizesDeathDropCheckpoints) {
    std::istringstream lethal_input(
        "pc=8002dd14 function=zzDealDamage checkpoint=damage_apply rng_draw_index_before=22 "
        "attacker_slot=0 target_slot=4 enemy_entry_id=0 damage=18 hp_before=18 hp_after=0\n"
        "pc=8002bd20 function=HandleCombatantDeath checkpoint=death_handler rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 cur_hp=0 entered_enemy_reward=1 called_enemy_drop=1\n"
        "pc=8002ba8c function=enemyDropItem checkpoint=enemy_drop_entry rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0\n"
        "pc=8002bad8 function=enemyDropItem checkpoint=row rng_draw_index_before=23 "
        "target_slot=4 enemy_entry_id=0 drop_row_index=1 rand_value=0 rand_mod100=0 "
        "drop_success=1\n");

    const auto lethal_parsed = parse_checkpoint_stream(lethal_input);
    ASSERT_TRUE(lethal_parsed.errors.empty());
    const auto lethal = summarize_death_drop_checkpoints(lethal_parsed.events);
    EXPECT_EQ(lethal.status, DeathDropCheckpointStatus::MatchesExpectedFlow);
    EXPECT_EQ(lethal.observed_damage_apply_events, 1);
    EXPECT_EQ(lethal.observed_death_handler_events, 1);
    EXPECT_EQ(lethal.observed_drop_entry_events, 1);
    EXPECT_EQ(lethal.observed_drop_rolls, 1);
    EXPECT_EQ(lethal.damage_events_with_live_death_fields, 1);
    EXPECT_EQ(lethal.lethal_damage_events, 1);
    EXPECT_EQ(lethal.damage_events_with_death_handler, 1);
    EXPECT_EQ(lethal.lethal_events_with_drop_entry, 1);
    EXPECT_EQ(lethal.lethal_events_with_drop_roll, 1);
    EXPECT_EQ(lethal.drop_rolls_after_drop_entry, 1);
    ASSERT_EQ(lethal.damage_flows.size(), 1u);
    EXPECT_TRUE(lethal.damage_flows[0].observed_death_handler);
    EXPECT_TRUE(lethal.damage_flows[0].observed_drop_entry);

    std::istringstream nonlethal_input(
        "pc=8002dd14 function=zzDealDamage checkpoint=damage_apply rng_draw_index_before=22 "
        "attacker_slot=0 target_slot=4 enemy_entry_id=0 damage=5 hp_before=18 hp_after=13\n"
        "pc=8002bd20 function=HandleCombatantDeath checkpoint=death_handler rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 cur_hp=13 entered_enemy_reward=0 called_enemy_drop=0\n");
    const auto nonlethal_parsed = parse_checkpoint_stream(nonlethal_input);
    ASSERT_TRUE(nonlethal_parsed.errors.empty());
    const auto nonlethal = summarize_death_drop_checkpoints(nonlethal_parsed.events);
    EXPECT_EQ(nonlethal.status, DeathDropCheckpointStatus::MatchesExpectedFlow);
    EXPECT_EQ(nonlethal.nonlethal_damage_events, 1);
    EXPECT_EQ(nonlethal.observed_drop_entry_events, 0);

    std::istringstream missing_fields_input(
        "pc=8002dd14 function=zzDealDamage checkpoint=damage_apply rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 damage=5\n"
        "pc=8002bd20 function=HandleCombatantDeath checkpoint=death_handler rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 cur_hp=13\n");
    const auto missing_fields_parsed = parse_checkpoint_stream(missing_fields_input);
    ASSERT_TRUE(missing_fields_parsed.errors.empty());
    const auto missing_fields = summarize_death_drop_checkpoints(missing_fields_parsed.events);
    EXPECT_EQ(
        death_drop_checkpoint_status_name(missing_fields.status),
        std::string("MissingLiveDeathFields"));

    std::istringstream missing_handler_input(
        "pc=8002dd14 function=zzDealDamage checkpoint=damage_apply rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 damage=18 hp_before=18 hp_after=0\n");
    const auto missing_handler_parsed = parse_checkpoint_stream(missing_handler_input);
    ASSERT_TRUE(missing_handler_parsed.errors.empty());
    const auto missing_handler = summarize_death_drop_checkpoints(missing_handler_parsed.events);
    EXPECT_EQ(missing_handler.status, DeathDropCheckpointStatus::MissingDeathHandler);

    std::istringstream unexpected_drop_input(
        "pc=8002dd14 function=zzDealDamage checkpoint=damage_apply rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 damage=5 hp_before=18 hp_after=13\n"
        "pc=8002bd20 function=HandleCombatantDeath checkpoint=death_handler rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 cur_hp=13 entered_enemy_reward=0 called_enemy_drop=1\n"
        "pc=8002ba8c function=enemyDropItem checkpoint=enemy_drop_entry rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0\n");
    const auto unexpected_drop_parsed = parse_checkpoint_stream(unexpected_drop_input);
    ASSERT_TRUE(unexpected_drop_parsed.errors.empty());
    const auto unexpected_drop = summarize_death_drop_checkpoints(unexpected_drop_parsed.events);
    EXPECT_EQ(unexpected_drop.status, DeathDropCheckpointStatus::UnexpectedDropForNonlethalDamage);

    std::istringstream missing_drop_entry_input(
        "pc=8002dd14 function=zzDealDamage checkpoint=damage_apply rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 damage=18 hp_before=18 hp_after=0\n"
        "pc=8002bd20 function=HandleCombatantDeath checkpoint=death_handler rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 cur_hp=0 entered_enemy_reward=1 called_enemy_drop=1\n");
    const auto missing_drop_entry_parsed = parse_checkpoint_stream(missing_drop_entry_input);
    ASSERT_TRUE(missing_drop_entry_parsed.errors.empty());
    const auto missing_drop_entry =
        summarize_death_drop_checkpoints(missing_drop_entry_parsed.events);
    EXPECT_EQ(missing_drop_entry.status, DeathDropCheckpointStatus::MissingDropEntry);

    std::istringstream missing_drop_roll_input(
        "pc=8002dd14 function=zzDealDamage checkpoint=damage_apply rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 damage=18 hp_before=18 hp_after=0\n"
        "pc=8002bd20 function=HandleCombatantDeath checkpoint=death_handler rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 cur_hp=0 entered_enemy_reward=1 called_enemy_drop=1\n"
        "pc=8002ba8c function=enemyDropItem checkpoint=enemy_drop_entry rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0\n");
    const auto missing_drop_roll_parsed = parse_checkpoint_stream(missing_drop_roll_input);
    ASSERT_TRUE(missing_drop_roll_parsed.errors.empty());
    const auto missing_drop_roll = summarize_death_drop_checkpoints(missing_drop_roll_parsed.events);
    EXPECT_EQ(missing_drop_roll.status, DeathDropCheckpointStatus::MissingDropRolls);
}

TEST(SavorPredictCheckpointTrace, SummarizesOutcomeCheckpoints) {
    std::istringstream input(
        "pc=8006ff38 function=endTurn checkpoint=status rng_draw_index_before=39 "
        "actor_slot=0 status_effect_id=0 rand_value=12\n"
        "pc=8006f020 function=runCase9 checkpoint=entry rng_draw_index_before=40 "
        "battle_outcome=0\n"
        "pc=8006f4b0 function=endBattleSuccess checkpoint=entry rng_draw_index_before=40 "
        "exp_awarded=6 gold_awarded=12\n"
        "pc=801f2a34 function=LevelUp checkpoint=entry rng_draw_index_before=41 "
        "actor_slot=0 level_before=1 level_after=2 exp_before=0 exp_after=6 "
        "next_level_exp=5 expected_stat_rolls=3\n"
        "pc=801f2b6c function=LevelUp checkpoint=stat1 rng_draw_index_before=41 "
        "actor_slot=0 stat_index=1 level=2 rand_value=3\n"
        "pc=801f2c00 function=LevelUp checkpoint=stat2 rng_draw_index_before=42 "
        "actor_slot=0 stat_index=2 level=2 rand_value=4\n"
        "pc=801f2d10 function=LevelUp checkpoint=stat3 rng_draw_index_before=43 "
        "actor_slot=0 stat_index=3 level=2 rand_value=5\n");

    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.events.size(), 7u);
    EXPECT_EQ(parsed.events[0].known_rng_owner, "end_turn_status_cleanup");
    EXPECT_EQ(parsed.events[4].known_rng_owner, "level_up_stat_roll_1");

    const auto matched = summarize_outcome_checkpoints(parsed.events, 1, 3);
    EXPECT_EQ(matched.status, OutcomeCheckpointStatus::MatchesExpectedFlow);
    EXPECT_EQ(matched.observed_end_turn_status_draws, 1);
    EXPECT_EQ(matched.observed_level_up_stat_rolls, 3);
    EXPECT_EQ(matched.observed_level_up_roll_1_draws, 1);
    EXPECT_EQ(matched.observed_level_up_roll_2_draws, 1);
    EXPECT_EQ(matched.observed_level_up_roll_3_draws, 1);
    EXPECT_EQ(matched.observed_run_case9_events, 1);
    EXPECT_EQ(matched.observed_battle_success_events, 1);
    EXPECT_EQ(matched.observed_level_up_entries, 1);
    EXPECT_EQ(matched.battle_success_events_with_reward_context, 1);
    EXPECT_EQ(matched.level_up_entries_with_exp_context, 1);
    EXPECT_EQ(matched.level_up_entries_with_expected_rolls, 1);
    EXPECT_EQ(matched.expected_level_up_stat_rolls_from_entries, 3);
    EXPECT_EQ(matched.stat_rolls_after_level_up_entry, 3);
    EXPECT_EQ(matched.stat_rolls_before_level_up_entry, 0);
    ASSERT_TRUE(matched.first_end_turn_status_draw_index.has_value());
    EXPECT_EQ(*matched.first_end_turn_status_draw_index, 39);
    ASSERT_TRUE(matched.first_run_case9_draw_index.has_value());
    EXPECT_EQ(*matched.first_run_case9_draw_index, 40);
    ASSERT_TRUE(matched.first_battle_success_draw_index.has_value());
    EXPECT_EQ(*matched.first_battle_success_draw_index, 40);
    ASSERT_TRUE(matched.first_level_up_entry_draw_index.has_value());
    EXPECT_EQ(*matched.first_level_up_entry_draw_index, 41);
    ASSERT_TRUE(matched.first_level_up_stat_roll_index.has_value());
    EXPECT_EQ(*matched.first_level_up_stat_roll_index, 41);
    EXPECT_EQ(matched.draws_with_actor_slot, 5);
    EXPECT_EQ(matched.draws_with_rand_value, 4);
    ASSERT_EQ(matched.draws.size(), 7u);
    EXPECT_EQ(matched.draws[1].kind, OutcomeCheckpointKind::RunCase9Entry);
    EXPECT_EQ(matched.draws[2].kind, OutcomeCheckpointKind::BattleSuccessEntry);
    EXPECT_EQ(matched.draws[3].kind, OutcomeCheckpointKind::LevelUpEntry);
    ASSERT_TRUE(matched.draws[5].stat_index.has_value());
    EXPECT_EQ(*matched.draws[5].stat_index, 2);

    const auto end_turn_mismatch = summarize_outcome_checkpoints(parsed.events, 0, 0);
    EXPECT_EQ(end_turn_mismatch.status, OutcomeCheckpointStatus::EndTurnStatusDrawMismatch);

    const auto level_up_mismatch = summarize_outcome_checkpoints(parsed.events, std::nullopt, 2);
    EXPECT_EQ(level_up_mismatch.status, OutcomeCheckpointStatus::LevelUpStatRollMismatch);
    EXPECT_EQ(
        outcome_checkpoint_status_name(level_up_mismatch.status),
        std::string("LevelUpStatRollMismatch"));
    EXPECT_EQ(
        outcome_checkpoint_kind_name(OutcomeCheckpointKind::BattleSuccessEntry),
        std::string("BattleSuccessEntry"));
}

TEST(SavorPredictCheckpointTrace, FlagsUnexpectedVictoryBranchForReachedNextTurnExpectation) {
    std::istringstream input(
        "pc=8006f020 function=runCase9 checkpoint=entry rng_draw_index_before=60 "
        "battle_outcome=0\n"
        "pc=8006f4b0 function=endBattleSuccess checkpoint=entry rng_draw_index_before=60 "
        "exp_awarded=6\n");

    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());

    const auto summary = summarize_outcome_checkpoints(parsed.events, 0, 0);
    EXPECT_EQ(summary.status, OutcomeCheckpointStatus::UnexpectedVictoryBranch);
    EXPECT_EQ(summary.observed_run_case9_events, 1);
    EXPECT_EQ(summary.observed_battle_success_events, 1);
    EXPECT_EQ(summary.observed_victory_branch_events, 2);
}

TEST(SavorPredictCheckpointTrace, RequiresLevelUpEntryContextForVictoryRolls) {
    std::istringstream missing_entry_input(
        "pc=801f2b6c function=LevelUp checkpoint=stat1 rng_draw_index_before=70 "
        "actor_slot=0 stat_index=1 level=2 rand_value=3\n");
    const auto missing_entry_parsed = parse_checkpoint_stream(missing_entry_input);
    ASSERT_TRUE(missing_entry_parsed.errors.empty());

    const auto missing_entry =
        summarize_outcome_checkpoints(missing_entry_parsed.events, std::nullopt, std::nullopt);
    EXPECT_EQ(missing_entry.status, OutcomeCheckpointStatus::MissingLevelUpEntry);

    std::istringstream missing_context_input(
        "pc=801f2a34 function=LevelUp checkpoint=entry rng_draw_index_before=70 "
        "actor_slot=0 expected_stat_rolls=1\n"
        "pc=801f2b6c function=LevelUp checkpoint=stat1 rng_draw_index_before=71 "
        "actor_slot=0 stat_index=1 level=2 rand_value=3\n");
    const auto missing_context_parsed = parse_checkpoint_stream(missing_context_input);
    ASSERT_TRUE(missing_context_parsed.errors.empty());

    const auto missing_context =
        summarize_outcome_checkpoints(missing_context_parsed.events, std::nullopt, std::nullopt);
    EXPECT_EQ(missing_context.status, OutcomeCheckpointStatus::MissingLevelUpContext);
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
