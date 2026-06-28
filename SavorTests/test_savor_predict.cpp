#include <gtest/gtest.h>

#include <ActionViewResourceCheckpointModel.h>
#include <ActionViewPathingTailModel.h>
#include <ActionViewSelectorModel.h>
#include <ActionViewStdJsonCache.h>
#include <ActionViewStdJsonLoader.h>
#include <ActionViewStdResourceResolver.h>
#include <CheckpointTrace.h>
#include <BattlePredictionDbInput.h>
#include <BattleFrameSchedulerModel.h>
#include <BattleFrameStateModel.h>
#include <BattlePredictorCli.h>
#include <EnemyEventDataModel.h>
#include <MovementModel.h>
#include <ProgressEventParser.h>
#include <RngModel.h>
#include <SoaQSortModel.h>
#include <SstActionCommandCheckpointModel.h>

#include <Core/Input/SoaBattle/BattleCommandCodec.h>
#include <Core/Memory/Soa/Battle/BattleContextCodec.h>

#include "common/SqliteDbFixture.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace savor::predict;

const ActionViewSelectorHelperCall* find_selector_helper_call(
    const ActionViewSelectorResult& result,
    std::uint32_t call_site_pc,
    std::string_view role) {
    const auto it = std::find_if(
        result.helper_calls.begin(),
        result.helper_calls.end(),
        [&](const ActionViewSelectorHelperCall& call) {
            return call.call_site_pc == call_site_pc && call.role == role;
        });
    return it == result.helper_calls.end() ? nullptr : &*it;
}

void append_effect_buffer_trace(
    std::ostringstream& stream,
    int& draw_index,
    std::string_view buffer,
    int loop_count,
    int source_key) {
    const std::string source_fields =
        " effect_source_key_0x28=" + std::to_string(source_key) +
        " effect_source_subtype_0x2a=2"
        " effect_source_secondary_0x2c=0"
        " effect_source_resource_id_0x30=0x9e";
    stream << "pc=8003bb24 function=FUN_8003ba08 checkpoint=effect_record_copy_complete "
           << "rng_draw_index_before=" << draw_index
           << " r6_effect_buffer=" << buffer
           << " r30_parent_action_thread=0x81230000"
           << " r31_source_record=0x80f40000"
           << " effect_parent_action_thread_0x04=0x81230000"
           << source_fields
           << " effect_loop_count_0x5c=0x" << std::hex << loop_count << std::dec
           << " source_record_key_0x00=" << source_key
           << " source_record_loop_count_0x34=0x" << std::hex << loop_count << std::dec
           << "\n";
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
}

void set_all_element_effectiveness(soa::ElementalEffectiveness& effectiveness, std::uint16_t value) {
    effectiveness.green = value;
    effectiveness.red = value;
    effectiveness.purple = value;
    effectiveness.blue = value;
    effectiveness.Yellow = value;
    effectiveness.Silver = value;
}

void fill_predictor_slot(
    soa::battle::ctx::BattleContext& context,
    int slot,
    bool player,
    int hp,
    int attack,
    int defense,
    int hit,
    int dodge,
    int agile,
    int element,
    int counter_chance,
    std::uint16_t movement_flags) {
    auto& battle_slot = context.slots_[slot];
    battle_slot.present = 1;
    battle_slot.is_player = player ? 1 : 0;
    battle_slot.is_alive = 1;
    battle_slot.id = static_cast<std::uint16_t>(slot);
    battle_slot.instance.Current_HP = static_cast<std::uint32_t>(hp);
    battle_slot.instance.Max_HP = static_cast<std::uint32_t>(hp);
    battle_slot.instance.current_derived_stats.Attack = static_cast<std::uint16_t>(attack);
    battle_slot.instance.current_derived_stats.Defense = static_cast<std::uint16_t>(defense);
    battle_slot.instance.current_derived_stats.HitChance = static_cast<std::uint16_t>(hit);
    battle_slot.instance.current_derived_stats.DodgeChance = static_cast<std::uint16_t>(dodge);
    battle_slot.instance.current_base_stats.Agility = static_cast<std::int16_t>(agile);
    battle_slot.instance.current_weapon_element = static_cast<std::uint8_t>(element);
    battle_slot.instance.counter_chance = static_cast<std::uint16_t>(counter_chance);
    battle_slot.instance.base_counter_chance = static_cast<std::uint16_t>(counter_chance);
    battle_slot.instance.current_counter_chance = static_cast<std::uint16_t>(counter_chance);
    battle_slot.instance.movement_flags = movement_flags;
    set_all_element_effectiveness(battle_slot.instance.current_elemental_eff, 10);
}

soa::battle::ctx::BattleContext make_predictor_first_battle_context(int soldier_hp = 58) {
    soa::battle::ctx::BattleContext context{};
    fill_predictor_slot(context, 0, true, 420, 43, 0, 90, 0, 11, 0, 15, 0x0FC7);
    fill_predictor_slot(context, 1, true, 360, 36, 0, 110, 0, 22, 1, 6, 0x0FF7);
    fill_predictor_slot(context, 4, false, soldier_hp, 43, 42, 95, 15, 10, 4, 10, 0x0FC7);
    fill_predictor_slot(context, 5, false, soldier_hp, 43, 42, 95, 15, 10, 4, 10, 0x0FC7);
    for (int slot : {4, 5}) {
        auto& battle_slot = context.slots_[slot];
        battle_slot.has_enemy_def = 1;
        battle_slot.enemy_def.items[1].chance = 1;
        battle_slot.enemy_def.items[1].amount = 1;
        battle_slot.enemy_def.items[1].itemId = 273;
        battle_slot.enemy_def.items[2].chance = 1;
        battle_slot.enemy_def.items[2].amount = 1;
        battle_slot.enemy_def.items[2].itemId = 258;
    }
    return context;
}

soa::battle::actions::TurnPlan make_two_pc_attack_turn_plan(std::uint32_t fake_attacks = 2) {
    using soa::battle::actions::ActionParameters;
    using soa::battle::actions::BattleAction;
    using soa::battle::actions::BattleCommand;
    soa::battle::actions::TurnPlan plan;
    plan.fake_attack_count = fake_attacks;
    plan.commands.push_back(BattleCommand{
        .actor_slot = 0,
        .macro = BattleAction::Attack,
        .params = ActionParameters{.target_slot = 4},
    });
    plan.commands.push_back(BattleCommand{
        .actor_slot = 1,
        .macro = BattleAction::Attack,
        .params = ActionParameters{.target_slot = 4},
    });
    return plan;
}

std::vector<MovementSlotState> make_first_battle_movement_slots(bool soldier4_alive = true) {
    return {
        MovementSlotState{
            .slot = 0,
            .present = true,
            .is_player = true,
            .alive = true,
            .movement_flags = 0x0FC7,
        },
        MovementSlotState{
            .slot = 1,
            .present = true,
            .is_player = true,
            .alive = true,
            .movement_flags = 0x0FF7,
        },
        MovementSlotState{
            .slot = 4,
            .present = true,
            .is_player = false,
            .alive = soldier4_alive,
            .movement_flags = 0x0FC7,
        },
        MovementSlotState{
            .slot = 5,
            .present = true,
            .is_player = false,
            .alive = true,
            .movement_flags = 0x0FC7,
        },
    };
}

std::vector<MovementSlotState> make_first_battle_movement_slots_with_event0_positions(
    bool soldier4_alive = true) {
    auto slots = make_first_battle_movement_slots(soldier4_alive);
    for (auto& slot : slots) {
        slot.start_position = enemy_event_start_position_for_slot(0, slot.slot);
    }
    return slots;
}

struct PredictionDbFixtureRows {
    std::int64_t battle_set_id = 0;
    std::int64_t seed_candidate_id = 0;
    std::int64_t source_input_frame_id = 0;
    std::int64_t unique_seed_id = 0;
    std::int64_t wave_id = 0;
    std::int64_t context_probe_id = 0;
    std::int64_t turn_job_id = 0;
    std::int64_t turn_exec_job_id = 88001;
    std::uint32_t candidate_seed = 0x11111111u;
    std::uint32_t unique_seed = 0x44444444u;
    std::optional<std::uint32_t> live_seed = 0x22222222u;
};

class SavorPredictDbInputFixture : public SqliteDbFixture {
protected:
    PredictionDbFixtureRows SeedPredictionRows(
        std::optional<std::uint32_t> live_seed = 0x22222222u,
        bool create_unique_seed = true,
        bool direct_unique_link = true) {
        using namespace savor::db;

        auto* analysis_db = db_service_->AnalysisDb();
        EXPECT_NE(analysis_db, nullptr);

        PredictionDbFixtureRows rows;
        rows.live_seed = live_seed;
        const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));
        std::string err;

        EXPECT_TRUE(analysis_db->CreateBattleSet(
            {
                .name = "prediction-input-fixture",
                .entry_savestate_id = 501,
                .battle_run_spec_id = 601,
                .explorer_settings_id = 701,
                .status = BattleSetStatus::Active,
                .created_at_utc = now,
                .correlation_id = "predict-corr",
                .causation_id = "predict-cause-1",
            },
            &rows.battle_set_id,
            &err))
            << err;

        if (create_unique_seed) {
            std::int64_t probe_set_id = 0;
            EXPECT_TRUE(analysis_db->CreateSeedProbeSet(
                {
                    .name = "prediction-input-seedprobe",
                    .probe_flavor = "BATTLE_PRE",
                    .breakpoint_policy_name = "fixture",
                    .segment_source_kind = "fixture",
                    .created_at_utc = now,
                    .correlation_id = "predict-corr",
                    .causation_id = "predict-cause-seedprobe-set",
                },
                &probe_set_id,
                &err))
                << err;

            std::int64_t probe_run_id = 0;
            EXPECT_TRUE(analysis_db->RequestSeedProbeRun(
                {
                    .probe_set_id = probe_set_id,
                    .entry_savestate_id = 501,
                    .seed_probe_spec_id = 1,
                    .codec_version = 1,
                    .status = "requested",
                    .requested_at_utc = now,
                    .correlation_id = "predict-corr",
                    .causation_id = "predict-cause-seedprobe-run",
                },
                &probe_run_id,
                &err))
                << err;

            EXPECT_TRUE(analysis_db->EnsureSeedProbeInputFrame(
                0x8080,
                0x8080,
                0x0000,
                &rows.source_input_frame_id,
                &err))
                << err;
            EXPECT_TRUE(analysis_db->SetSeedProbeRunNeutralSeed(probe_run_id, 0, &err)) << err;
            const auto probe_result_id = analysis_db->LookupSeedProbeResultId(probe_run_id);
            EXPECT_TRUE(probe_result_id.has_value());

            bool inserted = false;
            EXPECT_TRUE(analysis_db->EnsureSeedProbeUniqueSeedDelta(
                {
                    .probe_result_id = probe_result_id.value_or(0),
                    .input_frame_id = rows.source_input_frame_id,
                    .seed_value = rows.unique_seed,
                    .seed_delta = 0,
                    .recorded_at_utc = now,
                    .correlation_id = "predict-corr",
                    .causation_id = "predict-cause-unique-seed",
                },
                &inserted,
                &rows.unique_seed_id,
                &err))
                << err;
        }

        EXPECT_TRUE(analysis_db->AddBattleSeedCandidate(
            {
                .battle_set_id = rows.battle_set_id,
                .source_unique_seed_id = create_unique_seed && direct_unique_link
                    ? std::optional<std::int64_t>(rows.unique_seed_id)
                    : std::nullopt,
                .source_input_frame_id = create_unique_seed
                    ? std::optional<std::int64_t>(rows.source_input_frame_id)
                    : std::nullopt,
                .seed_value = rows.candidate_seed,
                .source_kind = create_unique_seed && direct_unique_link
                    ? BattleSeedCandidateSourceKind::SeedProbeUnique
                    : BattleSeedCandidateSourceKind::Synthetic,
                .candidate_status = BattleSeedCandidateStatus::Pending,
                .created_at_utc = now,
                .correlation_id = "predict-corr",
                .causation_id = "predict-cause-2",
            },
            &rows.seed_candidate_id,
            &err))
            << err;

        EXPECT_TRUE(analysis_db->CreateBattleTurnWave(
            {
                .battle_set_id = rows.battle_set_id,
                .turn_index = 1,
                .seed_candidate_id = rows.seed_candidate_id,
                .status = BattleTurnWaveStatus::Ready,
                .created_at_utc = now,
                .correlation_id = "predict-corr",
                .causation_id = "predict-cause-3",
            },
            &rows.wave_id,
            &err))
            << err;

        std::string context_blob;
        EXPECT_TRUE(soa::battle::ctx::codec::encode(make_predictor_first_battle_context(), context_blob));
        EXPECT_TRUE(analysis_db->CreateBattleContextProbe(
            {
                .wave_id = rows.wave_id,
                .source_savestate_id = 501,
                .probe_status = BattleContextProbeStatus::Queued,
                .created_at_utc = now,
                .correlation_id = "predict-corr",
                .causation_id = "predict-cause-4",
            },
            &rows.context_probe_id,
            &err))
            << err;
        EXPECT_TRUE(analysis_db->SetBattleContextProbeExecJobId(rows.context_probe_id, 87001, &err)) << err;
        EXPECT_TRUE(analysis_db->CompleteBattleContextProbe(
            {
                .exec_job_id = 87001,
                .probe_status = BattleContextProbeStatus::Succeeded,
                .context_blob = context_blob,
                .context_version = soa::battle::ctx::codec::ver,
                .recorded_at_utc = now,
            },
            &err))
            << err;

        const auto turn_plan = make_two_pc_attack_turn_plan(2);
        const auto command_blob = soa::battle::actions::encode_battle_turn_commands_hex(turn_plan.commands);
        EXPECT_TRUE(analysis_db->RecordBattleTurnJob(
            {
                .wave_id = rows.wave_id,
                .exec_job_id = rows.turn_exec_job_id,
                .plan_id = 9001,
                .seed_candidate_id = rows.seed_candidate_id,
                .resolved_turn_commands_blob = command_blob,
                .resolved_turn_variant_key = "fixture-variant",
                .fake_attacks_this_turn = 2,
                .fake_attacks_used_before = 0,
                .job_state = BattleTurnJobState::Completed,
                .started_at_utc = now,
                .ended_at_utc = now,
                .has_results = live_seed.has_value(),
                .rng_seed = live_seed.has_value()
                    ? std::optional<std::int64_t>(static_cast<std::int64_t>(*live_seed))
                    : std::nullopt,
                .battle_outcome = savor::battle::Outcome::ReachedNextTurn,
                .recorded_at_utc = now,
                .correlation_id = "predict-corr",
                .causation_id = "predict-cause-5",
            },
            &rows.turn_job_id,
            &err))
            << err;

        return rows;
    }
};

const BattlePredictionEvent* find_prediction_event(
    const BattlePredictionResult& result,
    std::string_view phase,
    std::string_view label) {
    for (const auto& event : result.events) {
        if (event.phase == phase && event.label == label) {
            return &event;
        }
    }
    return nullptr;
}

const BattlePredictionValidationItem* find_prediction_validation(
    const BattlePredictionResult& result,
    std::string_view scope) {
    for (const auto& item : result.validation) {
        if (item.scope == scope) {
            return &item;
        }
    }
    return nullptr;
}

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

TEST(SavorPredictRngModel, PreAiCameraModelUsesFixedFakePlusCameraContract) {
    const auto no_fake = model_pre_ai_camera_draws(0);
    EXPECT_EQ(no_fake.fake_attack_draws, 0);
    EXPECT_EQ(no_fake.pc_count, 2);
    EXPECT_EQ(no_fake.baseline_camera_draws, 3);
    EXPECT_EQ(no_fake.expected_camera_draws, 3);
    EXPECT_EQ(no_fake.suppressed_attack_targeting_camera_draws, 0);
    EXPECT_EQ(no_fake.unsuppressed_total_draws, 3);
    EXPECT_EQ(no_fake.expected_total_draws, 3);
    EXPECT_FALSE(no_fake.suppresses_normal_attack_targeting_camera);
    EXPECT_EQ(pre_ai_camera_rule_name(no_fake), std::string("FixedCameraDraws"));

    const auto one_fake = model_pre_ai_camera_draws(1);
    EXPECT_EQ(one_fake.fake_attack_draws, 1);
    EXPECT_EQ(one_fake.baseline_camera_draws, 3);
    EXPECT_EQ(one_fake.expected_camera_draws, 3);
    EXPECT_EQ(one_fake.suppressed_attack_targeting_camera_draws, 0);
    EXPECT_EQ(one_fake.unsuppressed_total_draws, 4);
    EXPECT_EQ(one_fake.expected_total_draws, 4);
    EXPECT_FALSE(one_fake.suppresses_normal_attack_targeting_camera);
    EXPECT_EQ(
        pre_ai_camera_rule_name(one_fake),
        std::string("FakeAttacksPlusFixedCameraDraws"));

    const auto three_fake = model_pre_ai_camera_draws(3);
    EXPECT_EQ(three_fake.unsuppressed_total_draws, 6);
    EXPECT_EQ(three_fake.expected_total_draws, 6);
    EXPECT_EQ(pre_ai_draws_for_fake_attacks(3), three_fake.expected_total_draws);

    const auto one_pc = model_pre_ai_camera_draws(2, 1);
    EXPECT_EQ(one_pc.baseline_camera_draws, 2);
    EXPECT_EQ(one_pc.expected_total_draws, 4);
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

TEST(SavorPredictRngModel, EffectRngModelComputesFirstBattleSourceKey8CritBurst) {
    const auto input = first_battle_effect_burst_sequence_for_source_key(8);
    const auto model = model_combat_effect_burst_sequence_draws(input);

    ASSERT_EQ(model.bursts.size(), 2u);
    EXPECT_EQ(model.total_loop_count, 20);
    EXPECT_EQ(model.total_draws, 100);

    EXPECT_EQ(model.bursts[0].loop_count, 16);
    EXPECT_EQ(model.bursts[0].total_draws, 80);
    EXPECT_EQ(model.bursts[1].loop_count, 4);
    EXPECT_EQ(model.bursts[1].total_draws, 20);

    EXPECT_TRUE(first_battle_effect_burst_sequence_for_source_key(99).empty());
}

TEST(SavorPredictRngModel, ForcedCounterFollowUpConsumesDamageDrawsOnly) {
    const BasicAttackInputs inputs{
        .attacker_attack = 43,
        .attacker_hit = 95,
        .attacker_agile = 10,
        .attacker_element = 4,
        .target_defense = 42,
        .target_dodge = 0,
        .target_element_effectiveness_tenths = 10,
    };

    const auto forced = simulate_forced_basic_attack_damage_burst(0x12345678u, inputs);

    EXPECT_EQ(forced.attack_result, 1);
    EXPECT_EQ(forced.hit_check, 1);
    EXPECT_EQ(forced.draws_consumed, 2);
    EXPECT_FALSE(forced.hit_draw_spent);
    EXPECT_FALSE(forced.crit_draw_spent);
    EXPECT_TRUE(forced.damage_draws_spent);
    EXPECT_TRUE(forced.damage_spread_rand.has_value());
    EXPECT_TRUE(forced.damage_bonus_rand.has_value());
    EXPECT_GT(forced.damage, 0);
}

TEST(SavorPredictRngModel, CounterChanceIncrementBelongsToDamageApplication) {
    const auto hit = simulate_counter_chance_increment_after_damage({
        .hit_check = 1,
        .current_counter_chance = 10,
        .counter_chance_increment = 10,
    });
    EXPECT_TRUE(hit.incremented);
    EXPECT_EQ(hit.updated_current_counter_chance, 20);

    const auto no_cap = simulate_counter_chance_increment_after_damage({
        .hit_check = 1,
        .current_counter_chance = 95,
        .counter_chance_increment = 10,
    });
    EXPECT_TRUE(no_cap.incremented);
    EXPECT_EQ(no_cap.updated_current_counter_chance, 105);

    const auto miss = simulate_counter_chance_increment_after_damage({
        .hit_check = 0,
        .current_counter_chance = 10,
        .counter_chance_increment = 10,
    });
    EXPECT_FALSE(miss.incremented);
    EXPECT_EQ(miss.updated_current_counter_chance, 10);

    const auto suppressed = simulate_counter_chance_increment_after_damage({
        .hit_check = 4,
        .current_counter_chance = 10,
        .counter_chance_increment = 10,
    });
    EXPECT_FALSE(suppressed.incremented);
    EXPECT_EQ(suppressed.updated_current_counter_chance, 10);
}

TEST(SavorPredictRngModel, FirstBattleVisualRngModelComposesCameraAndEffectDraws) {
    const auto pre_hit = model_first_battle_basic_attack_visual_rng({
        .actor_slot = 0,
        .target_slot = 4,
        .attack_landed = false,
    });
    ASSERT_EQ(pre_hit.steps.size(), 1u);
    EXPECT_EQ(pre_hit.total_draws, 1);
    EXPECT_EQ(pre_hit.steps[0].label, "mode0_action_view_camera_rewrite_gate");
    EXPECT_EQ(pre_hit.steps[0].status, BattleVisualRngStepStatus::Exact);
    EXPECT_EQ(
        battle_visual_rng_step_status_name(pre_hit.steps[0].status),
        std::string("Exact"));

    const auto vyse_hit = model_first_battle_basic_attack_visual_rng({
        .actor_slot = 0,
        .target_slot = 4,
        .attack_landed = true,
        .include_action_view_camera = false,
    });
    ASSERT_EQ(vyse_hit.steps.size(), 1u);
    EXPECT_EQ(vyse_hit.total_draws, 110);
    ASSERT_TRUE(vyse_hit.steps[0].effect_source_key.has_value());
    EXPECT_EQ(*vyse_hit.steps[0].effect_source_key, 4);

    const auto critical = model_first_battle_basic_attack_visual_rng({
        .actor_slot = 0,
        .target_slot = 4,
        .attack_landed = true,
        .attack_was_critical = true,
        .include_action_view_camera = false,
    });
    ASSERT_EQ(critical.steps.size(), 1u);
    EXPECT_EQ(critical.total_draws, 100);
    ASSERT_TRUE(critical.steps[0].effect_source_key.has_value());
    EXPECT_EQ(*critical.steps[0].effect_source_key, 8);

    const auto unknown_actor = model_first_battle_basic_attack_visual_rng({
        .actor_slot = 2,
        .target_slot = 4,
        .attack_landed = true,
        .include_action_view_camera = false,
    });
    ASSERT_EQ(unknown_actor.steps.size(), 1u);
    EXPECT_TRUE(unknown_actor.has_unsupported_steps);
    EXPECT_EQ(unknown_actor.steps[0].label, "unsupported_effect_source_actor_slot");
    EXPECT_EQ(unknown_actor.steps[0].status, BattleVisualRngStepStatus::Unsupported);
}

TEST(SavorPredictRngModel, ActionViewSelectorDrivesCameraOwner) {
    Std0Table empty_table;
    empty_table.includes_sentinel = true;
    empty_table.entries = { { .location_code = -1 } };
    const auto mode0e_selector = select_action_view_mode({
        .instruction_field6_0x6 = 4,
        .previous_effective_mode_0x2f = 2,
        .previous_selector_state_0x30 = 2,
        .previous_actor_slot_0x2 = 4,
        .current_actor_slot = 4,
        .selected_aux_table = empty_table,
    });
    const auto mode0e_camera = model_first_battle_basic_attack_visual_rng({
        .actor_slot = 4,
        .target_slot = 0,
        .include_action_view_camera = true,
        .include_effect_bursts = false,
        .action_view_selector = mode0e_selector,
    });
    ASSERT_EQ(mode0e_camera.steps.size(), 1u);
    EXPECT_EQ(mode0e_camera.total_draws, 1);
    EXPECT_EQ(mode0e_camera.steps[0].label, "mode0e_action_view_camera");
    EXPECT_EQ(mode0e_camera.steps[0].status, BattleVisualRngStepStatus::Exact);
    EXPECT_NE(mode0e_camera.steps[0].detail.find("mode0e_count=0"), std::string::npos);

    Std0Table suppressing_table;
    suppressing_table.includes_sentinel = true;
    suppressing_table.entries = {
        {
            .location_code = 0x2a,
            .opcode = 3,
            .payload = { .primary_action_key = 4 },
            .has_payload = true,
        },
        { .location_code = -1 },
    };
    const auto fallback_selector = select_action_view_mode({
        .instruction_field6_0x6 = 4,
        .previous_effective_mode_0x2f = 2,
        .previous_selector_state_0x30 = 2,
        .previous_actor_slot_0x2 = 0,
        .current_actor_slot = 0,
        .selected_aux_table = suppressing_table,
    });
    const auto fallback_camera = model_first_battle_basic_attack_visual_rng({
        .actor_slot = 0,
        .target_slot = 4,
        .include_action_view_camera = true,
        .include_effect_bursts = false,
        .action_view_selector = fallback_selector,
    });
    ASSERT_EQ(fallback_camera.steps.size(), 1u);
    EXPECT_EQ(fallback_camera.total_draws, 1);
    EXPECT_EQ(fallback_camera.steps[0].label, "mode0_action_view_camera_fallback");
    EXPECT_EQ(fallback_camera.steps[0].status, BattleVisualRngStepStatus::Exact);
    EXPECT_NE(fallback_camera.steps[0].detail.find("mode0e_count=1"), std::string::npos);

    const auto missing_selector = select_action_view_mode({
        .instruction_field6_0x6 = 4,
        .previous_effective_mode_0x2f = 2,
        .previous_selector_state_0x30 = 2,
        .previous_actor_slot_0x2 = 1,
        .current_actor_slot = 1,
    });
    const auto missing_camera = model_first_battle_basic_attack_visual_rng({
        .actor_slot = 1,
        .target_slot = 4,
        .include_action_view_camera = true,
        .include_effect_bursts = false,
        .action_view_selector = missing_selector,
    });
    ASSERT_EQ(missing_camera.steps.size(), 1u);
    EXPECT_EQ(missing_camera.steps[0].label, "missing_input_action_view_camera_aux_table");
    EXPECT_EQ(missing_camera.steps[0].status, BattleVisualRngStepStatus::MissingInput);
    EXPECT_TRUE(missing_camera.has_missing_input_steps);

    ActionViewSelectorResult ambiguous_selector;
    ambiguous_selector.requested_mode = 1;
    ambiguous_selector.dispatch_effective_mode_0x2f = 1;
    ambiguous_selector.selector_state_0x30 = 1;
    const auto ambiguous_camera = model_first_battle_basic_attack_visual_rng({
        .actor_slot = 1,
        .target_slot = 4,
        .include_action_view_camera = true,
        .include_effect_bursts = false,
        .action_view_selector = ambiguous_selector,
    });
    ASSERT_EQ(ambiguous_camera.steps.size(), 1u);
    EXPECT_EQ(ambiguous_camera.total_draws, 0);
    EXPECT_EQ(ambiguous_camera.steps[0].label, "ambiguous_action_view_camera_selector_path");
    EXPECT_EQ(ambiguous_camera.steps[0].status, BattleVisualRngStepStatus::Ambiguous);
    EXPECT_EQ(ambiguous_camera.steps[0].draws_consumed, 0);
    EXPECT_TRUE(ambiguous_camera.has_ambiguous_steps);

    ActionViewSelectorResult missing_count_selector;
    missing_count_selector.requested_mode = 0xe;
    missing_count_selector.dispatch_effective_mode_0x2f = 0xe;
    missing_count_selector.selector_state_0x30 = 4;
    missing_count_selector.mode0e_query_reached = true;
    const auto missing_count_camera = model_first_battle_basic_attack_visual_rng({
        .actor_slot = 1,
        .target_slot = 4,
        .include_action_view_camera = true,
        .include_effect_bursts = false,
        .action_view_selector = missing_count_selector,
    });
    ASSERT_EQ(missing_count_camera.steps.size(), 1u);
    EXPECT_EQ(missing_count_camera.total_draws, 0);
    EXPECT_EQ(missing_count_camera.steps[0].label, "missing_input_action_view_camera_mode0e_count");
    EXPECT_EQ(missing_count_camera.steps[0].status, BattleVisualRngStepStatus::MissingInput);
    EXPECT_EQ(missing_count_camera.steps[0].draws_consumed, 0);
    EXPECT_TRUE(missing_count_camera.has_missing_input_steps);
}

TEST(SavorPredictRngModel, ActionViewStdMatcherUsesDirectSecondaryOnlyForSpecialKeys) {
    EXPECT_TRUE(match_std_payload_action_key(4, 0x1234, 4, -1));
    EXPECT_FALSE(match_std_payload_action_key(4, 0x1234, 5, -1));

    EXPECT_TRUE(match_std_payload_action_key(0x18, 0x002a, 0x18, 0x002a));
    EXPECT_FALSE(match_std_payload_action_key(0x18, 0x002b, 0x18, 0x002a));
    EXPECT_TRUE(match_std_payload_action_key(0x1d, -1, 0x1d, -1));
    EXPECT_TRUE(match_std_payload_action_key(0x1e, 7, 0x1e, 7));

    EXPECT_TRUE(match_std_payload_action_key(0x1f, 0, 0x1f, 99));
}

TEST(SavorPredictRngModel, CountMatchingStd0EntriesMatchesDisassemblyPredicate) {
    Std0Table table;
    table.includes_sentinel = true;
    table.entries = {
        {
            .location_code = 0x2a,
            .opcode = 3,
            .payload = { .primary_action_key = 4, .generic_secondary_key = 77, .direct_gate_secondary_key = 88 },
            .has_payload = true,
        },
        {
            .location_code = 0x41,
            .opcode = 3,
            .payload = { .primary_action_key = 4 },
            .has_payload = true,
        },
        {
            .location_code = 0x2e,
            .opcode = 3,
            .payload = { .primary_action_key = 4 },
            .has_payload = true,
        },
        {
            .location_code = 0x2a,
            .opcode = 3,
            .payload = { .primary_action_key = 5 },
            .has_payload = true,
        },
        { .location_code = -1 },
    };

    const auto mode0e = count_matching_std0_entries(&table, mode0e_action_view_count_query());
    EXPECT_EQ(mode0e.count, 1);
    EXPECT_EQ(mode0e.scanned_entries, 4);
    EXPECT_TRUE(mode0e.reached_sentinel);
    EXPECT_TRUE(mode0e.used_combined_id_filter);

    const auto no_table = count_matching_std0_entries(nullptr, mode0e_action_view_count_query());
    EXPECT_EQ(no_table.count, 0);
    EXPECT_TRUE(no_table.reached_sentinel);
}

TEST(SavorPredictRngModel, LoadsSpiceStd0JsonEntryTableGateFields) {
    const std::string json = R"json({
  "schema": "spice_std_ir_v1",
  "layoutKind": "entry_table",
  "parseOk": true,
  "entryTable": {
    "records": [
      {
        "index": 0,
        "isSentinel": false,
        "locationCode": 42,
        "opcode": 3,
        "payloadInBounds": true,
        "payloadBytesHex": "0004004d0058"
      },
      {
        "index": 1,
        "isSentinel": false,
        "locationCode": 65,
        "opcode": 3,
        "payloadInBounds": true,
        "payloadBytesHex": "000400000000"
      },
      {
        "index": 2,
        "isSentinel": false,
        "locationCode": 42,
        "opcode": 3,
        "payloadInBounds": false,
        "payloadBytesHex": ""
      },
      {
        "index": 3,
        "isSentinel": true,
        "locationCode": -1,
        "opcode": 0,
        "payloadInBounds": false,
        "payloadBytesHex": ""
      }
    ]
  }
})json";

    const auto loaded = load_spice_std0_table_from_json_text(json);
    ASSERT_TRUE(loaded.ok);
    EXPECT_TRUE(loaded.errors.empty());
    EXPECT_EQ(loaded.records_seen, 4);
    EXPECT_EQ(loaded.records_imported, 4);
    EXPECT_TRUE(loaded.table.includes_sentinel);
    ASSERT_EQ(loaded.table.entries.size(), 4u);
    EXPECT_EQ(loaded.table.entries[0].location_code, 0x2a);
    EXPECT_EQ(loaded.table.entries[0].opcode, 3);
    EXPECT_TRUE(loaded.table.entries[0].has_payload);
    EXPECT_EQ(loaded.table.entries[0].payload.primary_action_key, 4);
    EXPECT_EQ(loaded.table.entries[0].payload.generic_secondary_key, 0x4d);
    EXPECT_EQ(loaded.table.entries[0].payload.direct_gate_secondary_key, 0x58);
    EXPECT_FALSE(loaded.table.entries[2].has_payload);

    const auto mode0e = count_matching_std0_entries(
        &loaded.table,
        mode0e_action_view_count_query());
    EXPECT_EQ(mode0e.count, 1);
    EXPECT_EQ(mode0e.scanned_entries, 3);
    EXPECT_TRUE(mode0e.reached_sentinel);
}

TEST(SavorPredictRngModel, LoadsSpiceActionRowsRuntimePrefixRecord) {
    const std::string json = R"json({
  "schema": "spice_std_ir_v1",
  "layoutKind": "action_rows",
  "parseOk": true,
  "actionRows": {
    "rows": [
      {
        "index": 0,
        "actionId": 2,
        "rowType": 1,
        "callbackIndex": 8
      },
      {
        "index": 1,
        "actionId": 7,
        "rowType": 1,
        "callbackIndex": 8
      }
    ]
  }
})json";

    const auto loaded = load_spice_std_action_row_prefix_from_json_text(json);
    ASSERT_TRUE(loaded.ok);
    EXPECT_TRUE(loaded.errors.empty());
    EXPECT_EQ(loaded.rows_seen, 2);
    EXPECT_EQ(loaded.rows_imported, 1);
    ASSERT_EQ(loaded.table.entries.size(), 1u);
    EXPECT_EQ(loaded.table.entries[0].location_code, 0);
    EXPECT_EQ(loaded.table.entries[0].opcode, 1);
    EXPECT_TRUE(loaded.table.entries[0].has_payload);
    EXPECT_EQ(loaded.table.entries[0].payload.primary_action_key, 2);
    EXPECT_EQ(loaded.table.entries[0].payload.generic_secondary_key, 1);
    EXPECT_EQ(loaded.table.entries[0].payload.direct_gate_secondary_key, 8);
}

TEST(SavorPredictRngModel, SpiceStd0JsonCanDriveMode0eSelectorGate) {
    const std::string suppressing_json = R"json({
  "schema": "spice_std_ir_v1",
  "layoutKind": "entry_table",
  "parseOk": true,
  "entryTable": {
    "records": [
      {
        "index": 0,
        "isSentinel": false,
        "locationCode": 42,
        "opcode": 3,
        "payloadInBounds": true,
        "payloadBytesHex": "000400000000"
      },
      {
        "index": 1,
        "isSentinel": true,
        "locationCode": -1,
        "opcode": 0,
        "payloadInBounds": false,
        "payloadBytesHex": ""
      }
    ]
  }
})json";
    const auto suppressing_table = load_spice_std0_table_from_json_text(suppressing_json);
    ASSERT_TRUE(suppressing_table.ok);

    const auto suppressed = select_action_view_mode({
        .instruction_field6_0x6 = 4,
        .previous_effective_mode_0x2f = 2,
        .previous_selector_state_0x30 = 2,
        .previous_actor_slot_0x2 = 1,
        .current_actor_slot = 1,
        .selected_aux_table = suppressing_table.table,
    });
    ASSERT_TRUE(suppressed.mode0e_count.has_value());
    EXPECT_EQ(suppressed.mode0e_count->count, 1);
    EXPECT_FALSE(suppressed.mode0e_synthetic_call_selected);

    const std::string empty_json = R"json({
  "schema": "spice_std_ir_v1",
  "layoutKind": "entry_table",
  "parseOk": true,
  "entryTable": {
    "records": [
      {
        "index": 0,
        "isSentinel": true,
        "locationCode": -1,
        "opcode": 0,
        "payloadInBounds": false,
        "payloadBytesHex": ""
      }
    ]
  }
})json";
    const auto empty_table = load_spice_std0_table_from_json_text(empty_json);
    ASSERT_TRUE(empty_table.ok);

    const auto selected = select_action_view_mode({
        .instruction_field6_0x6 = 4,
        .previous_effective_mode_0x2f = 2,
        .previous_selector_state_0x30 = 2,
        .previous_actor_slot_0x2 = 1,
        .current_actor_slot = 1,
        .selected_aux_table = empty_table.table,
    });
    ASSERT_TRUE(selected.mode0e_count.has_value());
    EXPECT_EQ(selected.mode0e_count->count, 0);
    EXPECT_TRUE(selected.mode0e_synthetic_call_selected);
}

TEST(SavorPredictRngModel, FirstBattleActionViewStdResolverMapsSlotsAndCompanions) {
    ASSERT_TRUE(first_battle_action_view_resource_stem_for_slot(0).has_value());
    ASSERT_TRUE(first_battle_action_view_resource_stem_for_slot(1).has_value());
    ASSERT_TRUE(first_battle_action_view_resource_stem_for_slot(4).has_value());
    ASSERT_TRUE(first_battle_action_view_resource_stem_for_slot(5).has_value());
    EXPECT_EQ(*first_battle_action_view_resource_stem_for_slot(0), "ma000");
    EXPECT_EQ(*first_battle_action_view_resource_stem_for_slot(1), "MA001");
    EXPECT_EQ(*first_battle_action_view_resource_stem_for_slot(4), "MB000");
    EXPECT_EQ(*first_battle_action_view_resource_stem_for_slot(5), "MB000");
    EXPECT_FALSE(first_battle_action_view_resource_stem_for_slot(2).has_value());
    ASSERT_TRUE(first_battle_action_view_std0_cache_key_for_slot(0).has_value());
    ASSERT_TRUE(first_battle_action_view_std0_cache_key_for_slot(1).has_value());
    ASSERT_TRUE(first_battle_action_view_std0_cache_key_for_slot(4).has_value());
    ASSERT_TRUE(first_battle_action_view_std0_cache_key_for_slot(5).has_value());
    EXPECT_EQ(*first_battle_action_view_std0_cache_key_for_slot(0), 0x00989680u);
    EXPECT_EQ(*first_battle_action_view_std0_cache_key_for_slot(1), 0x00989681u);
    EXPECT_EQ(*first_battle_action_view_std0_cache_key_for_slot(4), 0x00989A68u);
    EXPECT_EQ(*first_battle_action_view_std0_cache_key_for_slot(5), 0x00989A68u);
    EXPECT_FALSE(first_battle_action_view_std0_cache_key_for_slot(2).has_value());
    ASSERT_TRUE(first_battle_action_view_std0_cache_slot_for_slot(0).has_value());
    ASSERT_TRUE(first_battle_action_view_std0_cache_slot_for_slot(1).has_value());
    ASSERT_TRUE(first_battle_action_view_std0_cache_slot_for_slot(4).has_value());
    ASSERT_TRUE(first_battle_action_view_std0_cache_slot_for_slot(5).has_value());
    EXPECT_EQ(*first_battle_action_view_std0_cache_slot_for_slot(0), 0);
    EXPECT_EQ(*first_battle_action_view_std0_cache_slot_for_slot(1), 1);
    EXPECT_EQ(*first_battle_action_view_std0_cache_slot_for_slot(4), 2);
    EXPECT_EQ(*first_battle_action_view_std0_cache_slot_for_slot(5), 2);
    EXPECT_FALSE(first_battle_action_view_std0_cache_slot_for_slot(2).has_value());

    EXPECT_EQ(
        action_view_std0_companion_filename_for_std_resource("ma000.std"),
        "ma0000.std");
    EXPECT_EQ(
        action_view_std0_companion_filename_for_std_resource("MA001.std"),
        "ma0010.std");
    EXPECT_EQ(
        action_view_std0_companion_filename_for_std_resource("MB000"),
        "mb0000.std");
}

TEST(SavorPredictRngModel, FirstBattleActionViewStdResolverLoadsSelectedStd0Json) {
    const auto temp_dir =
        std::filesystem::temp_directory_path() / "savor_predict_first_battle_std_resolver_test";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto json_path = temp_dir / "ma0000.std.json";
    const auto primary_json_path = temp_dir / "ma000.std.json";
    {
        std::ofstream file(primary_json_path);
        ASSERT_TRUE(file.good());
        file << R"json({
  "schema": "spice_std_ir_v1",
  "layoutKind": "action_rows",
  "parseOk": true,
  "actionRows": {
    "rows": [
      {
        "index": 0,
        "actionId": 2,
        "rowType": 1,
        "callbackIndex": 8
      }
    ]
  }
})json";
    }
    {
        std::ofstream file(json_path);
        ASSERT_TRUE(file.good());
        file << R"json({
  "schema": "spice_std_ir_v1",
  "layoutKind": "entry_table",
  "parseOk": true,
  "entryTable": {
    "records": [
      {
        "index": 0,
        "isSentinel": false,
        "locationCode": 42,
        "opcode": 3,
        "payloadInBounds": true,
        "payloadBytesHex": "000400000000"
      },
      {
        "index": 1,
        "isSentinel": true,
        "locationCode": -1,
        "opcode": 0,
        "payloadInBounds": false,
        "payloadBytesHex": ""
      }
    ]
  }
})json";
    }

    const auto resolved = resolve_first_battle_action_view_std0_table_for_slot(0, temp_dir);
    EXPECT_TRUE(resolved.ok);
    EXPECT_TRUE(resolved.errors.empty());
    EXPECT_EQ(resolved.actor_slot, 0);
    EXPECT_EQ(resolved.resource_stem, "ma000");
    EXPECT_EQ(resolved.std_filename, "ma000.std");
    EXPECT_EQ(resolved.std0_filename, "ma0000.std");
    EXPECT_EQ(resolved.std_json_path.filename().string(), "ma000.std.json");
    EXPECT_EQ(resolved.std0_json_path.filename().string(), "ma0000.std.json");
    EXPECT_EQ(
        resolved.materialization_source,
        ActionViewStdMaterializationSource::Cache);
    ASSERT_TRUE(resolved.first_battle_cache_key.has_value());
    EXPECT_EQ(*resolved.first_battle_cache_key, 0x00989680u);
    ASSERT_TRUE(resolved.first_battle_cache_slot.has_value());
    EXPECT_EQ(*resolved.first_battle_cache_slot, 0);
    EXPECT_TRUE(resolved.runtime_loaded_resource_plus_0x30_is_aux_root);
    EXPECT_TRUE(resolved.table_contents_source_data_equivalent);
    EXPECT_TRUE(resolved.runtime_aux_table_has_action_row_prefix);
    EXPECT_EQ(resolved.runtime_aux_table_prefix_rows, 1);
    ASSERT_EQ(resolved.companion_table.entries.size(), 2u);
    ASSERT_EQ(resolved.table.entries.size(), 3u);
    EXPECT_EQ(resolved.table.entries[0].location_code, 0);
    EXPECT_EQ(resolved.table.entries[0].opcode, 1);
    EXPECT_TRUE(resolved.table.entries[0].has_payload);
    EXPECT_EQ(resolved.table.entries[0].payload.primary_action_key, 2);
    EXPECT_EQ(resolved.table.entries[1].location_code, 42);
    ASSERT_TRUE(resolved.mode0e_count.has_value());
    EXPECT_EQ(resolved.mode0e_count->count, 1);
    EXPECT_EQ(resolved.mode0e_count->scanned_entries, 2);

    const auto unsupported = resolve_first_battle_action_view_std0_table_for_slot(2, temp_dir);
    EXPECT_FALSE(unsupported.ok);
    EXPECT_FALSE(unsupported.errors.empty());
    EXPECT_FALSE(unsupported.first_battle_cache_key.has_value());
    EXPECT_FALSE(unsupported.first_battle_cache_slot.has_value());
    EXPECT_FALSE(unsupported.runtime_loaded_resource_plus_0x30_is_aux_root);

    std::filesystem::remove_all(temp_dir);
}

TEST(SavorPredictRngModel, FirstBattleActionViewStdResolverMatchesKnownCountProfiles) {
    const auto temp_dir =
        std::filesystem::temp_directory_path() / "savor_predict_first_battle_std_count_profiles_test";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto write_primary = [&](std::string_view filename) {
        std::ofstream file(temp_dir / std::string(filename));
        ASSERT_TRUE(file.good());
        file << R"json({
  "schema": "spice_std_ir_v1",
  "layoutKind": "action_rows",
  "parseOk": true,
  "actionRows": {
    "rows": [
      {
        "index": 0,
        "actionId": 2,
        "rowType": 1,
        "callbackIndex": 8
      }
    ]
  }
})json";
    };

    const auto write_entry_table = [&](
        std::string_view filename,
        bool include_mode0e_row,
        bool include_mode3_row) {
        std::ofstream file(temp_dir / std::string(filename));
        ASSERT_TRUE(file.good());
        file << R"json({
  "schema": "spice_std_ir_v1",
  "layoutKind": "entry_table",
  "parseOk": true,
  "entryTable": {
    "records": [
)json";
        int index = 0;
        const auto write_row = [&](int action_key) {
            if (index != 0) {
                file << ",\n";
            }
            file << R"json(      {
        "index": )json" << index << R"json(,
        "isSentinel": false,
        "locationCode": 42,
        "opcode": 3,
        "payloadInBounds": true,
        "payloadBytesHex": ")json"
                 << std::hex << std::setw(4) << std::setfill('0') << action_key
                 << R"json(00000000"
      })json";
            ++index;
        };
        if (include_mode0e_row) {
            write_row(4);
        }
        if (include_mode3_row) {
            write_row(5);
        }
        if (index != 0) {
            file << ",\n";
        }
        file << R"json(      {
        "index": )json" << index << R"json(,
        "isSentinel": true,
        "locationCode": -1,
        "opcode": 0,
        "payloadInBounds": false,
        "payloadBytesHex": ""
      }
    ]
  }
})json";
    };

    write_primary("ma000.std.json");
    write_primary("MA001.std.json");
    write_primary("MB000.std.json");
    write_entry_table("ma0000.std.json", true, true);
    write_entry_table("ma0010.std.json", true, true);
    write_entry_table("mb0000.std.json", false, true);

    struct Expected {
        int slot = -1;
        const char* std0_filename = "";
        std::uint32_t cache_key = 0;
        int cache_slot = -1;
        int mode0e_count = 0;
        int mode3_count = 0;
    };

    for (const auto& expected : {
             Expected{0, "ma0000.std", 0x00989680u, 0, 1, 1},
             Expected{1, "ma0010.std", 0x00989681u, 1, 1, 1},
             Expected{4, "mb0000.std", 0x00989A68u, 2, 0, 1},
             Expected{5, "mb0000.std", 0x00989A68u, 2, 0, 1},
         }) {
        const auto resolved =
            resolve_first_battle_action_view_std0_table_for_slot(expected.slot, temp_dir);
        ASSERT_TRUE(resolved.ok) << expected.slot;
        EXPECT_EQ(resolved.std0_filename, expected.std0_filename);
        ASSERT_TRUE(resolved.first_battle_cache_key.has_value());
        EXPECT_EQ(*resolved.first_battle_cache_key, expected.cache_key);
        ASSERT_TRUE(resolved.first_battle_cache_slot.has_value());
        EXPECT_EQ(*resolved.first_battle_cache_slot, expected.cache_slot);
        EXPECT_TRUE(resolved.runtime_aux_table_has_action_row_prefix);
        ASSERT_TRUE(resolved.mode0e_count.has_value());
        EXPECT_EQ(resolved.mode0e_count->count, expected.mode0e_count);

        const auto mode3_count =
            count_matching_std0_entries(&resolved.table, mode3_action_view_count_query());
        EXPECT_EQ(mode3_count.count, expected.mode3_count);

        const auto mode5_count = count_matching_std0_entries(
            &resolved.table,
            mode5_action_view_count_query(0x1d, 0x34));
        EXPECT_EQ(mode5_count.count, 0);
    }

    std::filesystem::remove_all(temp_dir);
}

TEST(SavorPredictRngModel, ActionViewRequestedModeUsesField6JumpTableMapping) {
    EXPECT_EQ(action_view_requested_mode_from_field6(6, 0, false), 0);
    EXPECT_EQ(action_view_requested_mode_from_field6(4, 0, false), 2);
    EXPECT_EQ(action_view_requested_mode_from_field6(8, 0, false), 2);
    EXPECT_EQ(action_view_requested_mode_from_field6(5, 0, false), 3);
    EXPECT_EQ(action_view_requested_mode_from_field6(2, 0, false), 4);
    EXPECT_EQ(action_view_requested_mode_from_field6(12, 0, false), 4);
    EXPECT_EQ(action_view_requested_mode_from_field6(29, 0x24, true), 1);
    EXPECT_EQ(action_view_requested_mode_from_field6(29, 0x24, false), 5);
    EXPECT_EQ(action_view_requested_mode_from_field6(14, 0, false), 1);
}

TEST(SavorPredictRngModel, ActionViewSelectorKeepsOnlyDisassemblySpecialField6States) {
    const auto field6_0b_keep = select_action_view_mode({
        .instruction_field6_0x6 = 0x0b,
        .previous_effective_mode_0x2f = 2,
        .previous_selector_state_0x30 = 3,
        .previous_actor_slot_0x2 = 1,
        .current_actor_slot = 1,
    });
    EXPECT_EQ(field6_0b_keep.requested_mode, 1);
    EXPECT_EQ(field6_0b_keep.dispatch_effective_mode_0x2f, 2);
    EXPECT_EQ(field6_0b_keep.selector_state_0x30, 3);
    EXPECT_TRUE(field6_0b_keep.helper_family_selected);

    const auto field6_0c_reset = select_action_view_mode({
        .instruction_field6_0x6 = 0x0c,
        .previous_effective_mode_0x2f = 2,
        .previous_selector_state_0x30 = 3,
        .previous_actor_slot_0x2 = 1,
        .current_actor_slot = 1,
        .instruction_flags_bit6_set = false,
    });
    EXPECT_EQ(field6_0c_reset.requested_mode, 4);
    EXPECT_EQ(field6_0c_reset.dispatch_effective_mode_0x2f, 4);
    EXPECT_EQ(field6_0c_reset.selector_state_0x30, 3);
    EXPECT_TRUE(field6_0c_reset.synthetic_call_80053f38_selected);
    const auto* mode4_spawn = find_selector_helper_call(
        field6_0c_reset,
        0x8001321cu,
        "mode4_state2_spawn_mode0");
    ASSERT_NE(mode4_spawn, nullptr);
    EXPECT_NE(
        std::find(
            field6_0c_reset.branch_path.begin(),
            field6_0c_reset.branch_path.end(),
            "state0_transition_result=2"),
        field6_0c_reset.branch_path.end());
}

TEST(SavorPredictRngModel, ActionViewSelectorModelsMode0eAuxListGate) {
    Std0Table empty_table;
    empty_table.includes_sentinel = true;
    empty_table.entries = { { .location_code = -1 } };

    const auto selected = select_action_view_mode({
        .instruction_field6_0x6 = 4,
        .previous_effective_mode_0x2f = 2,
        .previous_selector_state_0x30 = 2,
        .previous_actor_slot_0x2 = 1,
        .current_actor_slot = 1,
        .selected_aux_table = empty_table,
    });
    EXPECT_EQ(selected.requested_mode, 2);
    EXPECT_TRUE(selected.mode0e_query_reached);
    ASSERT_TRUE(selected.mode0e_count.has_value());
    EXPECT_EQ(selected.mode0e_count->count, 0);
    EXPECT_TRUE(selected.mode0e_synthetic_call_selected);
    ASSERT_TRUE(selected.spawned_action_view_record_mode_if_known.has_value());
    EXPECT_EQ(*selected.spawned_action_view_record_mode_if_known, 0xe);
    EXPECT_EQ(selected.selector_state_0x30, 3);
    const auto* selected_spawn = find_selector_helper_call(
        selected,
        0x80013334u,
        "mode2_count_zero_spawn_mode0");
    ASSERT_NE(selected_spawn, nullptr);
    EXPECT_EQ(selected_spawn->callee, "FUN_80053f38");
    ASSERT_TRUE(selected_spawn->mode_arg.has_value());
    EXPECT_EQ(*selected_spawn->mode_arg, 0);
    const auto* selected_tail = find_selector_helper_call(
        selected,
        0x8001338cu,
        "mode2_tail_call_mode1");
    ASSERT_NE(selected_tail, nullptr);
    EXPECT_EQ(selected_tail->callee, "FUN_80032bbc");
    ASSERT_TRUE(selected_tail->mode_arg.has_value());
    EXPECT_EQ(*selected_tail->mode_arg, 1);

    Std0Table suppressing_table;
    suppressing_table.includes_sentinel = true;
    suppressing_table.entries = {
        {
            .location_code = 0x2a,
            .opcode = 3,
            .payload = { .primary_action_key = 4 },
            .has_payload = true,
        },
        { .location_code = -1 },
    };

    const auto suppressed = select_action_view_mode({
        .instruction_field6_0x6 = 4,
        .previous_effective_mode_0x2f = 2,
        .previous_selector_state_0x30 = 2,
        .previous_actor_slot_0x2 = 1,
        .current_actor_slot = 1,
        .selected_aux_table = suppressing_table,
    });
    ASSERT_TRUE(suppressed.mode0e_count.has_value());
    EXPECT_EQ(suppressed.mode0e_count->count, 1);
    EXPECT_FALSE(suppressed.mode0e_synthetic_call_selected);
    EXPECT_FALSE(suppressed.spawned_action_view_record_mode_if_known.has_value());
    EXPECT_EQ(
        find_selector_helper_call(
            suppressed,
            0x80013334u,
            "mode2_count_zero_spawn_mode0"),
        nullptr);
    EXPECT_NE(
        find_selector_helper_call(
            suppressed,
            0x8001338cu,
            "mode2_tail_call_mode1"),
        nullptr);
}

TEST(SavorPredictRngModel, ActionViewSelectorReportsMissingAuxTableOnlyWhenGateIsReached) {
    const auto skipped = select_action_view_mode({
        .instruction_field6_0x6 = 14,
        .previous_effective_mode_0x2f = 1,
        .previous_selector_state_0x30 = 2,
    });
    EXPECT_FALSE(skipped.mode0e_query_reached);
    EXPECT_FALSE(skipped.unsupported_without_aux_table);

    const auto missing = select_action_view_mode({
        .instruction_field6_0x6 = 4,
        .previous_effective_mode_0x2f = 2,
        .previous_selector_state_0x30 = 2,
    });
    EXPECT_TRUE(missing.mode0e_query_reached);
    EXPECT_TRUE(missing.unsupported_without_aux_table);
    EXPECT_FALSE(missing.mode0e_synthetic_call_selected);
}

TEST(SavorPredictRngModel, ActionViewSelectorRunsState0TransitionAfterModeChange) {
    Std0Table empty_table;
    empty_table.includes_sentinel = true;
    empty_table.entries = { { .location_code = -1 } };

    const auto changed_mode = select_action_view_mode({
        .instruction_field6_0x6 = 4,
        .previous_effective_mode_0x2f = 1,
        .previous_selector_state_0x30 = 3,
        .previous_actor_slot_0x2 = 0,
        .current_actor_slot = 0,
        .instruction_flags_bit6_set = false,
        .selected_aux_table = empty_table,
    });

    EXPECT_EQ(changed_mode.requested_mode, 2);
    EXPECT_EQ(changed_mode.dispatch_effective_mode_0x2f, 2);
    EXPECT_TRUE(changed_mode.mode0e_query_reached);
    EXPECT_TRUE(changed_mode.mode0e_synthetic_call_selected);
    ASSERT_TRUE(changed_mode.spawned_action_view_record_mode_if_known.has_value());
    EXPECT_EQ(*changed_mode.spawned_action_view_record_mode_if_known, 0xe);
    EXPECT_NE(
        std::find(
            changed_mode.branch_path.begin(),
            changed_mode.branch_path.end(),
            "state0_transition_result=2"),
        changed_mode.branch_path.end());

    const auto actor_changed = select_action_view_mode({
        .instruction_field6_0x6 = 4,
        .previous_effective_mode_0x2f = 1,
        .previous_selector_state_0x30 = 3,
        .previous_actor_slot_0x2 = 1,
        .current_actor_slot = 0,
        .instruction_flags_bit6_set = true,
        .selected_aux_table = empty_table,
    });
    const auto* state0_spawn = find_selector_helper_call(
        actor_changed,
        0x8001318cu,
        "state0_actor_changed_spawn_mode1");
    ASSERT_NE(state0_spawn, nullptr);
    EXPECT_EQ(state0_spawn->callee, "FUN_80053f38");
    EXPECT_EQ(state0_spawn->actor_slot, 0);
    ASSERT_TRUE(state0_spawn->mode_arg.has_value());
    EXPECT_EQ(*state0_spawn->mode_arg, 1);
    EXPECT_FALSE(actor_changed.mode0e_query_reached);

    const auto waiting_on_flags = select_action_view_mode({
        .instruction_field6_0x6 = 4,
        .previous_effective_mode_0x2f = 1,
        .previous_selector_state_0x30 = 3,
        .previous_actor_slot_0x2 = 0,
        .current_actor_slot = 0,
        .instruction_flags_bit6_set = true,
        .selected_aux_table = empty_table,
    });

    EXPECT_EQ(waiting_on_flags.dispatch_effective_mode_0x2f, 2);
    EXPECT_EQ(waiting_on_flags.selector_state_0x30, 1);
    EXPECT_FALSE(waiting_on_flags.mode0e_query_reached);
}

TEST(SavorPredictRngModel, ActionViewSelectorModelsEntryActorChangeLookupStateWrite) {
    Std0Table empty_table;
    empty_table.includes_sentinel = true;
    empty_table.entries = { { .location_code = -1 } };

    const auto lookup_zero = select_action_view_mode({
        .instruction_field6_0x6 = 4,
        .previous_effective_mode_0x2f = 2,
        .previous_selector_state_0x30 = 3,
        .previous_actor_slot_0x2 = 1,
        .current_actor_slot = 5,
        .current_secondary_slot = 0,
        .instruction_flags_bit6_set = false,
        .actor_lookup_8001d41c_nonzero = false,
        .selected_aux_table = empty_table,
    });

    EXPECT_EQ(lookup_zero.dispatch_effective_mode_0x2f, 2);
    EXPECT_TRUE(lookup_zero.mode0e_query_reached);
    EXPECT_TRUE(lookup_zero.mode0e_synthetic_call_selected);
    EXPECT_EQ(lookup_zero.selector_actor_slot_0x2_written, 5);
    EXPECT_EQ(lookup_zero.selector_secondary_slot_0x4_written, 0);
    EXPECT_NE(
        std::find(
            lookup_zero.branch_path.begin(),
            lookup_zero.branch_path.end(),
            "entry_actor_changed_lookup_zero_state0"),
        lookup_zero.branch_path.end());
    EXPECT_NE(
        std::find(
            lookup_zero.branch_path.begin(),
            lookup_zero.branch_path.end(),
            "state0_transition_result=2"),
        lookup_zero.branch_path.end());

    const auto lookup_nonzero = select_action_view_mode({
        .instruction_field6_0x6 = 4,
        .previous_effective_mode_0x2f = 2,
        .previous_selector_state_0x30 = 3,
        .previous_actor_slot_0x2 = 1,
        .current_actor_slot = 5,
        .current_secondary_slot = 0,
        .instruction_flags_bit6_set = true,
        .actor_lookup_8001d41c_nonzero = true,
        .selected_aux_table = empty_table,
    });

    EXPECT_EQ(lookup_nonzero.dispatch_effective_mode_0x2f, 2);
    EXPECT_TRUE(lookup_nonzero.mode0e_query_reached);
    EXPECT_NE(
        std::find(
            lookup_nonzero.branch_path.begin(),
            lookup_nonzero.branch_path.end(),
            "entry_actor_changed_lookup_nonzero_state2"),
        lookup_nonzero.branch_path.end());
}

TEST(SavorPredictRngModel, ActionViewSelectorModelsSiblingAuxQueryPaths) {
    Std0Table table;
    table.includes_sentinel = true;
    table.entries = {
        {
            .location_code = 0x2a,
            .opcode = 3,
            .payload = { .primary_action_key = 5 },
            .has_payload = true,
        },
        {
            .location_code = 0x2e,
            .opcode = 3,
            .payload = { .primary_action_key = 29, .direct_gate_secondary_key = 0x34 },
            .has_payload = true,
        },
        { .location_code = -1 },
    };

    const auto mode3 = select_action_view_mode({
        .instruction_field6_0x6 = 5,
        .previous_effective_mode_0x2f = 3,
        .previous_selector_state_0x30 = 2,
        .selected_aux_table = table,
    });
    EXPECT_EQ(mode3.dispatch_effective_mode_0x2f, 3);
    EXPECT_TRUE(mode3.mode3_query_reached);
    ASSERT_TRUE(mode3.mode3_count.has_value());
    EXPECT_EQ(mode3.mode3_count->count, 1);
    EXPECT_FALSE(mode3.mode3_synthetic_call_selected);
    EXPECT_EQ(mode3.selector_state_0x30, 3);
    EXPECT_FALSE(mode3.synthetic_call_80053f38_selected);

    const auto mode3_zero = select_action_view_mode({
        .instruction_field6_0x6 = 5,
        .previous_effective_mode_0x2f = 3,
        .previous_selector_state_0x30 = 2,
        .selected_aux_table = Std0Table{ .entries = { { .location_code = -1 } }, .includes_sentinel = true },
    });
    ASSERT_TRUE(mode3_zero.mode3_count.has_value());
    EXPECT_EQ(mode3_zero.mode3_count->count, 0);
    EXPECT_TRUE(mode3_zero.mode3_synthetic_call_selected);
    EXPECT_TRUE(mode3_zero.synthetic_call_80053f38_selected);
    EXPECT_FALSE(mode3_zero.spawned_action_view_record_mode_if_known.has_value());
    const auto* mode3_spawn = find_selector_helper_call(
        mode3_zero,
        0x800133e4u,
        "mode3_count_zero_spawn_mode0");
    ASSERT_NE(mode3_spawn, nullptr);
    ASSERT_TRUE(mode3_spawn->mode_arg.has_value());
    EXPECT_EQ(*mode3_spawn->mode_arg, 0);

    const auto mode5 = select_action_view_mode({
        .instruction_field6_0x6 = 29,
        .instruction_field8_0x8 = 0x34,
        .previous_effective_mode_0x2f = 5,
        .previous_selector_state_0x30 = 2,
        .helper_800153e0_result = false,
        .selected_aux_table = table,
    });
    EXPECT_EQ(mode5.dispatch_effective_mode_0x2f, 5);
    EXPECT_TRUE(mode5.mode5_query_reached);
    ASSERT_TRUE(mode5.mode5_count.has_value());
    EXPECT_EQ(mode5.mode5_count->count, 1);
    EXPECT_TRUE(mode5.mode5_count_selected_state4);
    EXPECT_EQ(mode5.selector_state_0x30, 4);
    EXPECT_FALSE(mode5.call_80032bbc_selected);

    const auto mode5_zero = select_action_view_mode({
        .instruction_field6_0x6 = 29,
        .instruction_field8_0x8 = 0x77,
        .previous_effective_mode_0x2f = 5,
        .previous_selector_state_0x30 = 2,
        .helper_800153e0_result = false,
        .selected_aux_table = table,
    });
    ASSERT_TRUE(mode5_zero.mode5_count.has_value());
    EXPECT_EQ(mode5_zero.mode5_count->count, 0);
    EXPECT_FALSE(mode5_zero.mode5_count_selected_state4);
    EXPECT_EQ(mode5_zero.selector_state_0x30, 3);
    EXPECT_TRUE(mode5_zero.call_80032bbc_selected);
    EXPECT_TRUE(mode5_zero.helper_family_selected);
    const auto* mode5_call = find_selector_helper_call(
        mode5_zero,
        0x800134a4u,
        "mode5_count_zero_call_mode0");
    ASSERT_NE(mode5_call, nullptr);
    EXPECT_EQ(mode5_call->callee, "FUN_80032bbc");
    ASSERT_TRUE(mode5_call->mode_arg.has_value());
    EXPECT_EQ(*mode5_call->mode_arg, 0);
}

TEST(SavorPredictRngModel, ActionViewSelectorModelsMode1State2HelperPath) {
    const auto mode1 = select_action_view_mode({
        .instruction_field6_0x6 = 13,
        .previous_effective_mode_0x2f = 1,
        .previous_selector_state_0x30 = 2,
        .current_actor_slot = 1,
        .current_secondary_slot = 4,
    });

    EXPECT_EQ(mode1.requested_mode, 1);
    EXPECT_EQ(mode1.dispatch_effective_mode_0x2f, 1);
    EXPECT_EQ(mode1.selector_state_0x30, 3);
    EXPECT_FALSE(mode1.mode0e_query_reached);
    EXPECT_FALSE(mode1.mode3_query_reached);
    EXPECT_FALSE(mode1.mode5_query_reached);
    EXPECT_TRUE(mode1.call_80032bbc_selected);
    EXPECT_FALSE(mode1.synthetic_call_80053f38_selected);
    const auto* mode1_call = find_selector_helper_call(
        mode1,
        0x8001329cu,
        "mode1_state2_call_mode0");
    ASSERT_NE(mode1_call, nullptr);
    EXPECT_EQ(mode1_call->callee, "FUN_80032bbc");
    EXPECT_EQ(mode1_call->actor_slot, 1);
    ASSERT_TRUE(mode1_call->mode_arg.has_value());
    EXPECT_EQ(*mode1_call->mode_arg, 0);
}

TEST(SavorPredictRngModel, ActionViewEffectiveMode4FallsThroughToMode0Dispatch) {
    const auto mode4 = select_action_view_mode({
        .instruction_field6_0x6 = 2,
        .previous_effective_mode_0x2f = 4,
        .previous_selector_state_0x30 = 2,
    });
    EXPECT_EQ(mode4.dispatch_effective_mode_0x2f, 4);
    EXPECT_FALSE(mode4.mode5_query_reached);
    EXPECT_TRUE(mode4.synthetic_call_80053f38_selected);
    EXPECT_EQ(mode4.selector_state_0x30, 3);
    EXPECT_FALSE(mode4.spawned_action_view_record_mode_if_known.has_value());
    const auto* mode4_spawn = find_selector_helper_call(
        mode4,
        0x8001321cu,
        "mode4_state2_spawn_mode0");
    ASSERT_NE(mode4_spawn, nullptr);
    ASSERT_TRUE(mode4_spawn->mode_arg.has_value());
    EXPECT_EQ(*mode4_spawn->mode_arg, 0);
    ASSERT_FALSE(mode4.branch_path.empty());
    EXPECT_NE(
        std::find(
            mode4.branch_path.begin(),
            mode4.branch_path.end(),
            "effective_mode_4_falls_through_to_mode0_path"),
        mode4.branch_path.end());
}

TEST(SavorPredictRngModel, FirstBattleCounterFollowUpVisualsUseCounterSourceKeys) {
    const auto enemy_counter = model_first_battle_basic_attack_visual_rng({
        .actor_slot = 4,
        .target_slot = 1,
        .attack_landed = true,
        .attack_was_critical = true,
        .counter_follow_up = true,
        .include_action_view_camera = false,
    });
    ASSERT_EQ(enemy_counter.steps.size(), 1u);
    ASSERT_TRUE(enemy_counter.steps[0].effect_source_key.has_value());
    EXPECT_EQ(*enemy_counter.steps[0].effect_source_key, 4);

    const auto pc_counter = model_first_battle_basic_attack_visual_rng({
        .actor_slot = 1,
        .target_slot = 4,
        .attack_landed = true,
        .counter_follow_up = true,
        .include_action_view_camera = false,
    });
    ASSERT_EQ(pc_counter.steps.size(), 1u);
    ASSERT_TRUE(pc_counter.steps[0].effect_source_key.has_value());
    EXPECT_EQ(*pc_counter.steps[0].effect_source_key, 5);
}

TEST(SavorPredictRngModel, EffectCheckpointModelSummarizes80042b10BurstShape) {
    std::ostringstream stream;
    int draw_index = 0;
    const std::vector<int> source_keys = {5, 4, 5};
    for (int attack = 0; attack < 3; ++attack) {
        append_effect_buffer_trace(stream, draw_index, "0x100" + std::to_string(attack), 16, source_keys[attack]);
        append_effect_buffer_trace(stream, draw_index, "0x200" + std::to_string(attack), 6, source_keys[attack]);
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
    ASSERT_EQ(parsed.events.size(), 338u);
    EXPECT_EQ(parsed.events.front().checkpoint, "effect_record_copy_complete");

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
    EXPECT_EQ(summary.observed_effect_record_copy_events, 6);
    EXPECT_EQ(summary.effect_record_copy_events_with_effect_buffer, 6);
    EXPECT_EQ(summary.effect_record_copy_events_with_parent_action_thread, 6);
    EXPECT_EQ(summary.effect_record_copy_events_with_source_key, 6);
    EXPECT_EQ(summary.effect_record_copy_events_with_loop_count, 6);
    EXPECT_EQ(summary.effect_record_copy_events_matching_source_record_fields, 6);
    EXPECT_EQ(summary.effect_record_copy_events_matching_combat_effect_buffer, 6);
    ASSERT_EQ(summary.record_copy_events.size(), 6u);
    ASSERT_TRUE(summary.record_copy_events[0].matched_combat_effect_first_draw_index.has_value());
    EXPECT_EQ(*summary.record_copy_events[0].matched_combat_effect_first_draw_index, 0);
    EXPECT_EQ(summary.observed_combat_effect_buffers, 6);
    EXPECT_EQ(summary.complete_binary_variant_iterations, 66);
    EXPECT_EQ(summary.complete_binary_variant_buffers, 6);
    EXPECT_EQ(summary.complete_binary_variant_16_loop_buffers, 3);
    EXPECT_EQ(summary.complete_binary_variant_6_loop_buffers, 3);
    EXPECT_EQ(summary.complete_binary_variant_4_loop_buffers, 0);
    EXPECT_EQ(summary.complete_first_battle_landed_attack_effect_pairs, 3);
    EXPECT_EQ(summary.complete_first_battle_16_6_effect_pairs, 3);
    EXPECT_EQ(summary.complete_first_battle_16_4_effect_pairs, 0);
    EXPECT_EQ(summary.complete_first_battle_landed_attack_effect_pair_iterations, 66);
    EXPECT_EQ(summary.complete_first_battle_landed_attack_effect_pair_draws, 330);
    EXPECT_EQ(summary.complete_first_battle_landed_attack_effect_pairs_with_matching_source_key, 3);
    EXPECT_EQ(summary.complete_first_battle_landed_attack_effect_pairs_without_matching_source_key, 0);
    ASSERT_EQ(summary.complete_first_battle_effect_pairs_by_source_key.size(), 2u);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[0].source_key, 4);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[0].first_loop_count, 16);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[0].second_loop_count, 6);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[0].loop_count_sum, 22);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[0].draw_count, 110);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[0].pair_count, 1);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[1].source_key, 5);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[1].first_loop_count, 16);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[1].second_loop_count, 6);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[1].loop_count_sum, 22);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[1].draw_count, 110);
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

TEST(SavorPredictRngModel, EffectCheckpointModelSummarizesSourceKey8CritBurstShape) {
    std::ostringstream stream;
    int draw_index = 0;

    append_effect_buffer_trace(stream, draw_index, "0x5000", 16, 5);
    append_effect_buffer_trace(stream, draw_index, "0x5001", 6, 5);
    append_effect_buffer_trace(stream, draw_index, "0x8000", 16, 8);
    append_effect_buffer_trace(stream, draw_index, "0x8001", 4, 8);

    std::istringstream input(stream.str());
    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());

    const auto summary = summarize_effect_checkpoints(parsed.events);
    EXPECT_EQ(summary.status, EffectCheckpointStatus::MatchesBinaryVariantBurstShape);
    EXPECT_EQ(summary.observed_combat_effect_draws, 210);
    EXPECT_EQ(summary.complete_binary_variant_iterations, 42);
    EXPECT_EQ(summary.complete_binary_variant_buffers, 4);
    EXPECT_EQ(summary.complete_binary_variant_16_loop_buffers, 2);
    EXPECT_EQ(summary.complete_binary_variant_6_loop_buffers, 1);
    EXPECT_EQ(summary.complete_binary_variant_4_loop_buffers, 1);
    EXPECT_EQ(summary.complete_first_battle_landed_attack_effect_pairs, 2);
    EXPECT_EQ(summary.complete_first_battle_16_6_effect_pairs, 1);
    EXPECT_EQ(summary.complete_first_battle_16_4_effect_pairs, 1);
    EXPECT_EQ(summary.complete_first_battle_landed_attack_effect_pair_iterations, 42);
    EXPECT_EQ(summary.complete_first_battle_landed_attack_effect_pair_draws, 210);
    EXPECT_EQ(summary.complete_first_battle_landed_attack_effect_pairs_with_matching_source_key, 2);
    EXPECT_EQ(summary.complete_first_battle_landed_attack_effect_pairs_without_matching_source_key, 0);
    ASSERT_EQ(summary.complete_first_battle_effect_pairs_by_source_key.size(), 2u);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[0].source_key, 5);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[0].first_loop_count, 16);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[0].second_loop_count, 6);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[0].draw_count, 110);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[0].pair_count, 1);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[1].source_key, 8);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[1].first_loop_count, 16);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[1].second_loop_count, 4);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[1].draw_count, 100);
    EXPECT_EQ(summary.complete_first_battle_effect_pairs_by_source_key[1].pair_count, 1);
    EXPECT_EQ(summary.unpaired_first_battle_effect_buffers, 0);
    EXPECT_EQ(summary.incomplete_binary_variant_iteration_remainder, 0);
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
            "pc=800608dc function=TargetCamera checkpoint=targeting_camera "
            "rng_draw_index_before=3 active_slot=0 target_slot=4\n"
            "pc=8008b428 function=runAiRoutine checkpoint=soldier_ai_action "
            "rng_draw_index_before=4 active_slot=4\n");

        const auto parsed = parse_checkpoint_stream(input);
        ASSERT_TRUE(parsed.errors.empty());
        const auto fake_one = summarize_pre_ai_checkpoints(parsed.events, 1);

        EXPECT_EQ(pre_ai_checkpoint_status_name(fake_one.status), std::string("MatchesExpected"));
        ASSERT_TRUE(fake_one.expectation.has_value());
        EXPECT_EQ(fake_one.expectation->expected_fake_attack_draws, 1);
        EXPECT_EQ(fake_one.expectation->expected_targeting_camera_draws, 2);
        EXPECT_EQ(fake_one.observed_fake_attack_attempts, 1);
        EXPECT_EQ(fake_one.observed_fake_attack_draws, 1);
        EXPECT_EQ(fake_one.observed_skipped_fake_attack_draws, 0);
        EXPECT_EQ(fake_one.fake_attack_draws_with_frame_gap, 1);
        ASSERT_TRUE(fake_one.min_fake_attack_draw_camera_frame_gap.has_value());
        ASSERT_TRUE(fake_one.max_fake_attack_draw_camera_frame_gap.has_value());
        EXPECT_EQ(*fake_one.min_fake_attack_draw_camera_frame_gap, 9);
        EXPECT_EQ(*fake_one.max_fake_attack_draw_camera_frame_gap, 9);
        EXPECT_EQ(fake_one.observed_targeting_camera_draws, 2);
        EXPECT_EQ(fake_one.observed_pre_ai_draws, 4);
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

TEST(SavorPredictBattlePredictor, ResolvesFirstBattleProfile) {
    const auto profile = battle_prediction_profile_by_name("first-battle");
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(profile->name, "first-battle");
    EXPECT_FALSE(battle_prediction_profile_by_name("FirstBattlePredictor").has_value());
}

TEST(SavorPredictEnemyEventDataModel, ProvidesScriptedBattleStartPositions) {
    const auto first = enemy_event_start_positions(0);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->enemy_event_id, 0);

    const auto vyse = enemy_event_start_position_for_slot(0, 0);
    ASSERT_TRUE(vyse.has_value());
    EXPECT_TRUE(vyse->present);
    EXPECT_TRUE(vyse->is_player);
    EXPECT_EQ(vyse->combatant_id, 0);
    EXPECT_EQ(std::string(vyse->combatant_name), "Vyse");
    EXPECT_EQ(vyse->grid_x, 4);
    EXPECT_EQ(vyse->grid_z, 6);

    const auto soldier5 = enemy_event_start_position_for_slot(0, 5);
    ASSERT_TRUE(soldier5.has_value());
    EXPECT_FALSE(soldier5->is_player);
    EXPECT_EQ(std::string(soldier5->combatant_name), "Soldier");
    EXPECT_EQ(soldier5->grid_x, 6);
    EXPECT_EQ(soldier5->grid_z, 2);

    const auto guard7 = enemy_event_start_position_for_slot(1, 7);
    ASSERT_TRUE(guard7.has_value());
    EXPECT_EQ(std::string(guard7->combatant_name), "Guard");
    EXPECT_EQ(guard7->grid_x, 8);
    EXPECT_EQ(guard7->grid_z, 3);

    EXPECT_FALSE(enemy_event_start_positions(999).has_value());
}

TEST(SavorPredictMovementModel, PcAttackSelectsDirectWorkerWhenWorksheetProvesReachability) {
    MovementModelInputs inputs;
    inputs.rng_state = 0x12345678u;
    inputs.actor_slot = 0;
    inputs.target_slot = 4;
    inputs.queued_instruction = 3;
    inputs.instr_param_0x6 = 0;
    inputs.enemy_owned = false;
    inputs.slots = make_first_battle_movement_slots();
    inputs.actor_worksheet.available = true;
    inputs.actor_worksheet.target_adjacent = true;
    inputs.actor_worksheet.reachability_result = 1;
    inputs.actor_worksheet.path_shape_forces_fallback = false;
    inputs.actor_worksheet.dist_to_target = 1;

    const auto result = simulate_first_battle_movement_setup(inputs);

    EXPECT_EQ(result.status, MovementSimulationStatus::Exact);
    EXPECT_EQ(result.draws_consumed, 0);
    EXPECT_EQ(result.final_target_slot, 4);
    EXPECT_EQ(result.final_instr_param_0x6, 0);
    EXPECT_EQ(result.reachability, MovementReachabilityStatus::Adjacent1);
    EXPECT_EQ(result.selected_worker, MovementSelectedWorker::PcDirectAttack_80086308);
    EXPECT_EQ(movement_selected_worker_name(result.selected_worker), std::string("PcDirectAttack_80086308"));
}

TEST(SavorPredictMovementModel, Event0WorksheetProjectionKeepsAlxGridSeparateFromDerivedRawStageUnits) {
    MovementModelInputs inputs;
    inputs.actor_slot = 1;
    inputs.target_slot = 4;
    inputs.queued_instruction = 3;
    inputs.instr_param_0x6 = 0;
    inputs.enemy_owned = false;
    inputs.slots = make_first_battle_movement_slots_with_event0_positions();

    const auto worksheet = project_enemy_event0_movement_worksheet_snapshot(inputs);

    ASSERT_TRUE(worksheet.available);
    ASSERT_TRUE(worksheet.actor_grid_position.has_value());
    EXPECT_EQ(worksheet.actor_grid_position->grid_x, 6);
    EXPECT_EQ(worksheet.actor_grid_position->grid_z, 6);
    ASSERT_TRUE(worksheet.target_grid_position.has_value());
    EXPECT_EQ(worksheet.target_grid_position->grid_x, 4);
    EXPECT_EQ(worksheet.target_grid_position->grid_z, 2);
    ASSERT_TRUE(worksheet.actor_raw_stage_position.has_value());
    EXPECT_EQ(worksheet.actor_raw_stage_position->raw_x, 15);
    EXPECT_EQ(worksheet.actor_raw_stage_position->raw_z, 15);
    ASSERT_TRUE(worksheet.target_raw_stage_position.has_value());
    EXPECT_EQ(worksheet.target_raw_stage_position->raw_x, -15);
    EXPECT_EQ(worksheet.target_raw_stage_position->raw_z, -45);
    ASSERT_TRUE(worksheet.path_shape_forces_fallback.has_value());
    EXPECT_TRUE(*worksheet.path_shape_forces_fallback);
}

TEST(SavorPredictMovementModel, Event0SameColumnPcAttackSelectsDirectWorker) {
    MovementModelInputs inputs;
    inputs.rng_state = 0x12345678u;
    inputs.actor_slot = 0;
    inputs.target_slot = 4;
    inputs.queued_instruction = 3;
    inputs.instr_param_0x6 = 0;
    inputs.enemy_owned = false;
    inputs.slots = make_first_battle_movement_slots_with_event0_positions();
    inputs.actor_worksheet = project_enemy_event0_movement_worksheet_snapshot(inputs);

    const auto result = simulate_first_battle_movement_setup(inputs);

    EXPECT_EQ(result.status, MovementSimulationStatus::Exact);
    EXPECT_EQ(result.final_instr_param_0x6, 0);
    EXPECT_EQ(result.reachability, MovementReachabilityStatus::Path4);
    EXPECT_EQ(result.selected_worker, MovementSelectedWorker::PcDirectAttack_80086308);
}

TEST(SavorPredictMovementModel, Event0DiagonalPcAttackSelectsFallbackWorker) {
    MovementModelInputs inputs;
    inputs.rng_state = 0x12345678u;
    inputs.actor_slot = 1;
    inputs.target_slot = 4;
    inputs.queued_instruction = 3;
    inputs.instr_param_0x6 = 0;
    inputs.enemy_owned = false;
    inputs.slots = make_first_battle_movement_slots_with_event0_positions();
    inputs.actor_worksheet = project_enemy_event0_movement_worksheet_snapshot(inputs);

    const auto result = simulate_first_battle_movement_setup(inputs);

    EXPECT_EQ(result.status, MovementSimulationStatus::Exact);
    EXPECT_EQ(result.final_instr_param_0x6, 1);
    EXPECT_EQ(result.reachability, MovementReachabilityStatus::Failed0);
    EXPECT_EQ(result.selected_worker, MovementSelectedWorker::PcFallbackAttack_80085ce0);
}

TEST(SavorPredictMovementModel, PcAttackWithoutWorksheetIsMissingInputInsteadOfDefaultDirect) {
    MovementModelInputs inputs;
    inputs.rng_state = 0x12345678u;
    inputs.actor_slot = 1;
    inputs.target_slot = 4;
    inputs.queued_instruction = 3;
    inputs.instr_param_0x6 = 0;
    inputs.enemy_owned = false;
    inputs.slots = make_first_battle_movement_slots();

    const auto result = simulate_first_battle_movement_setup(inputs);

    EXPECT_EQ(result.status, MovementSimulationStatus::MissingInput);
    EXPECT_EQ(result.final_instr_param_0x6, 0);
    EXPECT_EQ(result.selected_worker, MovementSelectedWorker::PcDirectAttack_80086308);
    EXPECT_NE(result.detail.find("FUN_80083728"), std::string::npos);
}

TEST(SavorPredictMovementModel, DeadFirstBattleSoldierRetargetsToOnlyLivingSoldier) {
    MovementModelInputs inputs;
    inputs.rng_state = 0x12345678u;
    inputs.actor_slot = 0;
    inputs.target_slot = 4;
    inputs.queued_instruction = 3;
    inputs.instr_param_0x6 = 0;
    inputs.enemy_owned = false;
    inputs.slots = make_first_battle_movement_slots(false);

    const auto result = simulate_first_battle_movement_setup(inputs);

    EXPECT_TRUE(result.target_repaired);
    EXPECT_EQ(result.target_repair_status, MovementSimulationStatus::Exact);
    EXPECT_EQ(result.original_target_slot, 4);
    EXPECT_EQ(result.final_target_slot, 5);
}

TEST(SavorPredictMovementModel, SoldierAttackSetupConsumesMovementDraw) {
    MovementModelInputs inputs;
    inputs.rng_state = 0;
    inputs.actor_slot = 5;
    inputs.target_slot = 0;
    inputs.queued_instruction = 3;
    inputs.instr_param_0x6 = 0;
    inputs.enemy_owned = true;
    inputs.slots = make_first_battle_movement_slots();

    const auto result = simulate_first_battle_movement_setup(inputs);

    EXPECT_EQ(result.draws_consumed, 1);
    ASSERT_TRUE(result.setup_rand.has_value());
    ASSERT_TRUE(result.setup_rand_mod10.has_value());
    EXPECT_EQ(*result.setup_rand_mod10, *result.setup_rand % 10);
    EXPECT_EQ(result.status, MovementSimulationStatus::MissingInput);
    EXPECT_EQ(result.enemy_setup_path, EnemyAttackSetupPath::TargetAdjacencySetup);
}

TEST(SavorPredictMovementModel, Event0EnemyDirectCloseCandidateCanSelectDirectWorker) {
    std::optional<std::uint32_t> direct_seed;
    for (std::uint32_t seed = 0; seed < 2000000u; ++seed) {
        EnemyAttackSetupInputs setup_inputs;
        setup_inputs.queued_instruction = 3;
        setup_inputs.movement_flags = 0x0FC7;
        const auto setup = simulate_enemy_attack_setup_gate(seed, setup_inputs);
        if (setup.direct_close_branch_candidate) {
            direct_seed = seed;
            break;
        }
    }
    ASSERT_TRUE(direct_seed.has_value());

    MovementModelInputs inputs;
    inputs.rng_state = *direct_seed;
    inputs.actor_slot = 4;
    inputs.target_slot = 0;
    inputs.queued_instruction = 3;
    inputs.instr_param_0x6 = 1;
    inputs.enemy_owned = true;
    inputs.slots = make_first_battle_movement_slots_with_event0_positions();
    inputs.actor_worksheet = project_enemy_event0_movement_worksheet_snapshot(inputs);

    const auto result = simulate_first_battle_movement_setup(inputs);

    EXPECT_EQ(result.draws_consumed, 1);
    EXPECT_EQ(result.status, MovementSimulationStatus::Exact);
    EXPECT_EQ(result.enemy_setup_path, EnemyAttackSetupPath::DirectCloseSetupCandidate);
    EXPECT_EQ(result.final_instr_param_0x6, 0);
    EXPECT_EQ(result.reachability, MovementReachabilityStatus::Path4);
    EXPECT_EQ(result.selected_worker, MovementSelectedWorker::EnemyDirectAttack_80087f6c);
}

TEST(SavorPredictMovementModel, PassiveRoutesIncludeTargetParticipant) {
    MovementModelInputs inputs;
    inputs.rng_state = 0x12345678u;
    inputs.actor_slot = 0;
    inputs.target_slot = 4;
    inputs.queued_instruction = 3;
    inputs.instr_param_0x6 = 0;
    inputs.enemy_owned = false;
    inputs.slots = make_first_battle_movement_slots();
    inputs.actor_worksheet.available = true;
    inputs.actor_worksheet.target_adjacent = true;
    inputs.actor_worksheet.reachability_result = 1;
    inputs.actor_worksheet.path_shape_forces_fallback = false;
    inputs.actor_worksheet.dist_to_target = 1;

    const auto result = simulate_first_battle_movement_setup(inputs);

    const auto target_route = std::find_if(
        result.passive_routes.begin(),
        result.passive_routes.end(),
        [](const PassiveMovementRoute& route) {
            return route.slot == 4;
        });
    ASSERT_NE(target_route, result.passive_routes.end());
    EXPECT_EQ(target_route->route, PassiveMovementRouteKind::TargetParticipant);
    EXPECT_EQ(passive_movement_route_kind_name(target_route->route), std::string("TargetParticipant"));
}

TEST(SavorPredictBattleFrameSchedulerModel, InitializesRuntimeWithPackedThreadOrder) {
    const auto runtime = initialize_first_battle_frame_runtime(
        0,
        make_first_battle_movement_slots_with_event0_positions());

    ASSERT_TRUE(runtime.has_value());
    EXPECT_TRUE(runtime->initialized);
    ASSERT_EQ(runtime->state.packed_thread_order.size(), 4u);
    EXPECT_EQ(runtime->state.packed_thread_order[0].slot, 0);
    EXPECT_EQ(runtime->state.packed_thread_order[1].slot, 1);
    EXPECT_EQ(runtime->state.packed_thread_order[2].slot, 4);
    EXPECT_EQ(runtime->state.packed_thread_order[3].slot, 5);

    const auto* aika = find_frame_combatant(runtime->state, 1);
    ASSERT_NE(aika, nullptr);
    EXPECT_EQ(aika->grid_position.grid_x, 6);
    EXPECT_EQ(aika->grid_position.grid_z, 6);
    EXPECT_EQ(aika->combatant_position.x, 15.0f);
    EXPECT_EQ(aika->combatant_position.z, 15.0f);
}

TEST(SavorPredictBattleFrameSchedulerModel, CommitAndBridgeAreSeparateFrameSteps) {
    auto runtime = initialize_first_battle_frame_runtime(
        0,
        make_first_battle_movement_slots_with_event0_positions());
    ASSERT_TRUE(runtime.has_value());

    auto* vyse = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(vyse, nullptr);
    const auto old_combatant_position = vyse->combatant_position;

    ASSERT_TRUE(commit_movement_grid_8008178c(
        runtime->state,
        0,
        MovementGridPosition{.grid_x = 5, .grid_z = 6}));
    vyse = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(vyse, nullptr);
    EXPECT_EQ(vyse->grid_position.grid_x, 5);
    EXPECT_EQ(vyse->pos_holder.x, 0.0f);
    EXPECT_EQ(vyse->combatant_position.x, old_combatant_position.x);

    ASSERT_TRUE(bridge_pos_holder_to_combatant_8001ab60(runtime->state, 0));
    vyse = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(vyse, nullptr);
    EXPECT_EQ(vyse->combatant_position.x, vyse->pos_holder.x);
    EXPECT_EQ(vyse->instruction_snapshot_position.x, vyse->pos_holder.x);
}

TEST(SavorPredictBattleFrameSchedulerModel, RunsWorkersForActiveAndPassiveCombatants) {
    auto runtime = initialize_first_battle_frame_runtime(
        0,
        make_first_battle_movement_slots_with_event0_positions());
    ASSERT_TRUE(runtime.has_value());

    schedule_first_battle_action_workers(
        *runtime,
        BattleFrameScheduleActionInput{
            .actor_slot = 0,
            .target_slot = 4,
            .enemy_owned = false,
            .combatant_command_parameter = 0,
            .selected_worker = MovementSelectedWorker::PcDirectAttack_80086308,
            .passive_routes = {
                PassiveMovementRoute{
                    .slot = 4,
                    .route = PassiveMovementRouteKind::TargetParticipant,
                    .selected_worker = MovementSelectedWorker::None,
                    .status = MovementSimulationStatus::Provisional,
                },
                PassiveMovementRoute{
                    .slot = 1,
                    .route = PassiveMovementRouteKind::SameSideParticipant,
                    .selected_worker = MovementSelectedWorker::None,
                    .status = MovementSimulationStatus::Provisional,
                },
            },
        });

    const auto result = run_scheduled_frame_workers(*runtime, 16);

    EXPECT_TRUE(result.ok);
    EXPECT_GE(result.frames_executed, 4);
    EXPECT_NE(
        std::find_if(
            result.events.begin(),
            result.events.end(),
            [](const BattleFrameStepEvent& event) {
                return event.slot == 0
                    && event.worker_kind == BattleFrameWorkerKind::ActiveDirectAttack;
            }),
        result.events.end());
    EXPECT_NE(
        std::find_if(
            result.events.begin(),
            result.events.end(),
            [](const BattleFrameStepEvent& event) {
                return event.slot == 4
                    && event.worker_kind == BattleFrameWorkerKind::PassiveTarget;
            }),
        result.events.end());
    EXPECT_NE(
        std::find_if(
            result.events.begin(),
            result.events.end(),
            [](const BattleFrameStepEvent& event) {
                return event.slot == 1
                    && event.worker_kind == BattleFrameWorkerKind::PassiveSameSide;
            }),
        result.events.end());
}

TEST(SavorPredictBattleFrameSchedulerModel, TicksPersistentWorkersOneFrameAtATime) {
    auto runtime = initialize_first_battle_frame_runtime(
        0,
        make_first_battle_movement_slots_with_event0_positions());
    ASSERT_TRUE(runtime.has_value());

    schedule_first_turn_actor_action(
        *runtime,
        BattleFrameScheduleActionInput{
            .actor_slot = 0,
            .target_slot = 4,
            .enemy_owned = false,
            .combatant_command_parameter = 0,
            .selected_worker = MovementSelectedWorker::PcDirectAttack_80086308,
            .passive_routes = {
                PassiveMovementRoute{
                    .slot = 4,
                    .route = PassiveMovementRouteKind::TargetParticipant,
                    .selected_worker = MovementSelectedWorker::None,
                    .status = MovementSimulationStatus::Provisional,
                },
            },
        });

    std::uint32_t rng_state = 0x12345678u;
    const auto first_frame = run_first_turn_frame(*runtime, rng_state);

    EXPECT_TRUE(first_frame.ok);
    EXPECT_EQ(first_frame.frames_executed, 1);
    EXPECT_EQ(runtime->state.frame_index, 1);
    EXPECT_NE(
        std::find_if(
            runtime->workers.begin(),
            runtime->workers.end(),
            [](const BattleFrameWorker& worker) {
                return worker.slot == 0 && !worker.complete;
            }),
        runtime->workers.end());
}

TEST(SavorPredictBattleFrameSchedulerModel, SchedulesEffectChunksPreservingBurstDrawTotal) {
    auto runtime = initialize_first_battle_frame_runtime(
        0,
        make_first_battle_movement_slots_with_event0_positions());
    ASSERT_TRUE(runtime.has_value());

    ASSERT_TRUE(schedule_effect_chunks_for_source_key(*runtime, 1, 4, 5));
    std::uint32_t rng_state = 0x12345678u;
    const auto seed_before = rng_state;
    const auto result = run_first_turn_until_idle(*runtime, rng_state, 16);

    const auto expected = model_combat_effect_burst_sequence_draws(
        first_battle_effect_burst_sequence_for_source_key(5));
    int chunk_draws = 0;
    int chunk_rng_events = 0;
    for (const auto& event : result.events) {
        if (event.worker_kind != BattleFrameWorkerKind::EffectChunk || !event.rng_event) {
            continue;
        }
        chunk_draws += event.draws_consumed;
        ++chunk_rng_events;
        EXPECT_EQ(event.rng_label, "combat_effect_chunk");
        EXPECT_EQ(event.status, BattleFrameEventStatus::Provisional);
        ASSERT_TRUE(event.effect_source_key.has_value());
        EXPECT_EQ(*event.effect_source_key, 5);
    }

    EXPECT_TRUE(result.ok);
    EXPECT_EQ(chunk_rng_events, 2);
    EXPECT_EQ(chunk_draws, expected.total_draws);
    EXPECT_NE(rng_state, seed_before);
}

TEST(SavorPredictBattleFrameSchedulerModel, PassiveClashConsumesOneDrawAndSelectsObservedMode) {
    auto runtime = initialize_first_battle_frame_runtime(
        0,
        make_first_battle_movement_slots_with_event0_positions());
    ASSERT_TRUE(runtime.has_value());

    schedule_first_turn_actor_action(
        *runtime,
        BattleFrameScheduleActionInput{
            .actor_slot = 0,
            .target_slot = 4,
            .enemy_owned = false,
            .combatant_command_parameter = 0,
            .selected_worker = MovementSelectedWorker::PcDirectAttack_80086308,
            .passive_routes = {
                PassiveMovementRoute{
                    .slot = 4,
                    .route = PassiveMovementRouteKind::TargetParticipant,
                    .selected_worker = MovementSelectedWorker::None,
                    .status = MovementSimulationStatus::Provisional,
                },
            },
        });

    std::uint32_t rng_state = 0x12345678u;
    const auto expected_draw = draw_rand15(rng_state);
    const auto result = run_first_turn_until_idle(*runtime, rng_state, 16);

    const auto clash = std::find_if(
        result.events.begin(),
        result.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.worker_kind == BattleFrameWorkerKind::PassiveClashReaction;
        });
    ASSERT_NE(clash, result.events.end());
    EXPECT_EQ(clash->rng_label, "fun_8002eb4c_passive_clash");
    EXPECT_EQ(clash->draws_consumed, 1);
    ASSERT_TRUE(clash->rand_value.has_value());
    EXPECT_EQ(*clash->rand_value, expected_draw.value);
    EXPECT_EQ(clash->old_action_mode, BattleFrameActionMode::Standing);
    EXPECT_TRUE(
        clash->new_action_mode == BattleFrameActionMode::PassiveBlock
        || clash->new_action_mode == BattleFrameActionMode::PassiveDodge);
    ASSERT_TRUE(clash->passive_clash_selected_index.has_value());
    EXPECT_EQ(*clash->passive_clash_selected_index, expected_draw.value % 2);
}

TEST(SavorPredictActionViewPathingTailModel, AngleShortConversionDoesNotImplyCircularDelta) {
    const float near_359 = angle_short_to_degrees_8006116c(0xff49u);
    const float near_1 = angle_short_to_degrees_8006116c(0x00b6u);
    EXPECT_GT(near_359, 358.0f);
    EXPECT_LT(near_1, 2.0f);
    EXPECT_GT(near_359 - near_1, 356.0f);
}

TEST(SavorPredictActionViewPathingTailModel, GeometryScorerRejectsOutsideRawAngleCone) {
    const auto scored = score_geometry_800117ec({
        .input_reference = BattleFrameVec3{.x = 10.0f, .y = 0.0f, .z = 0.0f},
        .candidate_position = BattleFrameVec3{.x = -10.0f, .y = 0.0f, .z = 0.0f},
        .path_base = BattleFrameVec3{},
    });
    EXPECT_FALSE(scored.accepted);
    EXPECT_GE(scored.raw_angle_diff_degrees, 45.0f);
}

TEST(SavorPredictActionViewPathingTailModel, FirstBattleAikaTailDerivesSevenFallbackDraws) {
    const auto tail = model_first_battle_action_view_pathing_tail({
        .profile_name = "first-battle",
        .actor_slot = 1,
        .target_slot = 4,
        .combatant_action_mode = 5,
        .combatant_command_parameter = 1,
        .attack_landed = true,
        .enemy_event_id = 0,
        .slots = make_first_battle_movement_slots_with_event0_positions(),
    });

    ASSERT_EQ(tail.steps.size(), 2u);
    EXPECT_EQ(tail.total_draws, 8);
    EXPECT_EQ(tail.steps[0].label, "mode1_pathing_record_draw");
    EXPECT_EQ(tail.steps[0].status, ActionViewPathingTailStatus::Provisional);
    EXPECT_EQ(tail.steps[0].draws_consumed, 1);
    EXPECT_EQ(tail.steps[1].label, "fun_80011694_target_side_fallback");
    EXPECT_EQ(tail.steps[1].status, ActionViewPathingTailStatus::Provisional);
    EXPECT_EQ(tail.steps[1].draws_consumed, 7);
    ASSERT_TRUE(tail.steps[1].accepted_candidates.has_value());
    EXPECT_EQ(*tail.steps[1].accepted_candidates, 0);
}

TEST(SavorPredictBattlePredictor, FailFastsAfterTurnOrderWhenMovementInputsAreMissing) {
    BattlePredictionInput input;
    input.profile = first_battle_prediction_profile();
    input.starting_rng_seed = 15u;
    input.context = make_predictor_first_battle_context();
    input.turn_plan = make_two_pc_attack_turn_plan(2);

    const auto result = predict_battle(input);

    const auto* fake = find_prediction_event(result, "pre_ai", "fake_attack_draws");
    ASSERT_NE(fake, nullptr);
    EXPECT_EQ(fake->draws_consumed, 2);
    EXPECT_EQ(fake->status, BattlePredictionEventStatus::Exact);

    const auto* camera = find_prediction_event(result, "pre_ai", "camera_draws");
    ASSERT_NE(camera, nullptr);
    EXPECT_EQ(camera->draws_consumed, 3);
    EXPECT_EQ(camera->status, BattlePredictionEventStatus::Exact);

    const auto enemy_ai_count = std::count_if(
        result.events.begin(),
        result.events.end(),
        [](const BattlePredictionEvent& event) {
            return event.phase == "enemy_ai" && event.label == "soldier_ai";
        });
    EXPECT_EQ(enemy_ai_count, 2);

    const auto* turn_order = find_prediction_event(result, "turn_order", "resolve_turn_order");
    ASSERT_NE(turn_order, nullptr);
    EXPECT_EQ(turn_order->status, BattlePredictionEventStatus::Exact);
    EXPECT_TRUE(result.exact_through_turn_order);
    EXPECT_GT(result.exact_draws_through_turn_order, 5);

    const auto turn_order_entry_count = std::count_if(
        result.events.begin(),
        result.events.end(),
        [](const BattlePredictionEvent& event) {
            return event.phase == "turn_order" && event.label == "entry";
        });
    EXPECT_EQ(turn_order_entry_count, 4);

    const auto turn_order_entry = std::find_if(
        result.events.begin(),
        result.events.end(),
        [](const BattlePredictionEvent& event) {
            return event.phase == "turn_order"
                && event.label == "entry"
                && event.actor_slot == 0;
        });
    ASSERT_NE(turn_order_entry, result.events.end());
    EXPECT_EQ(turn_order_entry->status, BattlePredictionEventStatus::Exact);
    EXPECT_TRUE(turn_order_entry->queue_index.has_value());
    ASSERT_TRUE(turn_order_entry->quick.has_value());
    EXPECT_EQ(*turn_order_entry->quick, 22);
    EXPECT_TRUE(turn_order_entry->jitter_modulus.has_value());
    EXPECT_TRUE(turn_order_entry->assigned_priority.has_value());
    EXPECT_TRUE(turn_order_entry->qsort_index.has_value());
    EXPECT_TRUE(turn_order_entry->execution_index.has_value());

    const auto* unresolved_visual = find_prediction_event(
        result,
        "action_visual_rng",
        "unresolved_action_view_effect_rng");
    EXPECT_EQ(unresolved_visual, nullptr);

    const auto* action_view = find_prediction_event(
        result,
        "action_visual_rng",
        "mode0_action_view_camera_rewrite_gate");
    EXPECT_EQ(action_view, nullptr);

    const auto* worker_select = find_prediction_event(result, "movement_setup", "worker_select");
    ASSERT_NE(worker_select, nullptr);
    EXPECT_EQ(worker_select->status, BattlePredictionEventStatus::MissingInput);

    const auto* pre_ai_validation = find_prediction_validation(result, "pre_ai");
    ASSERT_NE(pre_ai_validation, nullptr);
    EXPECT_EQ(pre_ai_validation->status, BattlePredictionValidationStatus::Provisional);
    EXPECT_NE(pre_ai_validation->detail.find("fake_attacks + 1 + pc_count"), std::string::npos);

    const auto* soldier_ai_validation = find_prediction_validation(result, "soldier_ai");
    ASSERT_NE(soldier_ai_validation, nullptr);
    EXPECT_EQ(soldier_ai_validation->status, BattlePredictionValidationStatus::Validated);

    const auto* turn_order_validation = find_prediction_validation(result, "turn_order");
    ASSERT_NE(turn_order_validation, nullptr);
    EXPECT_EQ(turn_order_validation->status, BattlePredictionValidationStatus::Validated);
    EXPECT_EQ(turn_order_validation->draws_exact_through, result.exact_draws_through_turn_order);

    const auto* movement_validation = find_prediction_validation(result, "movement_setup");
    ASSERT_NE(movement_validation, nullptr);
    EXPECT_EQ(movement_validation->status, BattlePredictionValidationStatus::MissingInput);

    const auto* movement_param_validation = find_prediction_validation(result, "movement_instr_param");
    ASSERT_NE(movement_param_validation, nullptr);
    EXPECT_EQ(movement_param_validation->status, BattlePredictionValidationStatus::MissingInput);

    const auto* action_source_validation = find_prediction_validation(result, "action_source_selection");
    ASSERT_NE(action_source_validation, nullptr);
    EXPECT_EQ(action_source_validation->status, BattlePredictionValidationStatus::NotExercised);

    const auto* total_draws_validation = find_prediction_validation(result, "total_draws");
    ASSERT_NE(total_draws_validation, nullptr);
    EXPECT_EQ(total_draws_validation->status, BattlePredictionValidationStatus::MissingInput);
    EXPECT_EQ(total_draws_validation->draws_exact_through, result.exact_draws_through_turn_order);
}

TEST(SavorPredictBattlePredictor, MarksQSortPriorityTiesProvisionalInPrediction) {
    std::optional<BattlePredictionResult> matched;
    for (std::uint32_t seed = 0; seed < 4096 && !matched.has_value(); ++seed) {
        BattlePredictionInput input;
        input.profile = first_battle_prediction_profile();
        input.starting_rng_seed = seed;
        input.enemy_event_id = 0;
        input.context = make_predictor_first_battle_context(1000);
        input.turn_plan = make_two_pc_attack_turn_plan(0);

        auto result = predict_battle(input);
        const auto* turn_order = find_prediction_event(result, "turn_order", "resolve_turn_order");
        if (turn_order != nullptr && turn_order->status == BattlePredictionEventStatus::Provisional) {
            matched = std::move(result);
        }
    }

    ASSERT_TRUE(matched.has_value());
    const auto& result = *matched;
    const auto* turn_order = find_prediction_event(result, "turn_order", "resolve_turn_order");
    ASSERT_NE(turn_order, nullptr);
    EXPECT_EQ(turn_order->status, BattlePredictionEventStatus::Provisional);
    EXPECT_FALSE(result.exact_through_turn_order);
    EXPECT_TRUE(result.has_provisional_events);
    EXPECT_EQ(result.outcome, BattlePredictionOutcome::Provisional);

    const auto* turn_order_validation = find_prediction_validation(result, "turn_order");
    ASSERT_NE(turn_order_validation, nullptr);
    EXPECT_EQ(turn_order_validation->status, BattlePredictionValidationStatus::Provisional);
    EXPECT_NE(turn_order_validation->detail.find("priority-tie"), std::string::npos);
}

TEST(SavorPredictBattlePredictor, OrdersLethalDropBeforeCombatEffectBurst) {
    std::optional<BattlePredictionResult> matched;
    for (std::uint32_t seed = 0; seed < 512 && !matched.has_value(); ++seed) {
        BattlePredictionInput input;
        input.profile = first_battle_prediction_profile();
        input.starting_rng_seed = seed;
        input.enemy_event_id = 0;
        input.context = make_predictor_first_battle_context(1);
        input.turn_plan = make_two_pc_attack_turn_plan(0);

        auto result = predict_battle(input);
        const auto lethal = std::find_if(
            result.events.begin(),
            result.events.end(),
            [](const BattlePredictionEvent& event) {
                return event.phase == "damage_application"
                    && event.label == "damage_applied_lethal";
            });
        if (lethal == result.events.end()) {
            continue;
        }

        const auto drop = std::find_if(
            result.events.begin(),
            result.events.end(),
            [&](const BattlePredictionEvent& event) {
                return event.phase == "death_drop"
                    && (event.label == "enemy_drop" || event.label == "enemy_no_drop")
                    && event.actor_slot == lethal->actor_slot
                    && event.target_slot == lethal->target_slot;
            });
        const auto burst = std::find_if(
            result.events.begin(),
            result.events.end(),
            [&](const BattlePredictionEvent& event) {
                return event.phase == "action_visual_rng"
                    && event.label == "combat_effect_burst"
                    && event.actor_slot == lethal->actor_slot
                    && event.target_slot == lethal->target_slot;
            });
        if (drop != result.events.end() && burst != result.events.end()) {
            matched = std::move(result);
        }
    }

    ASSERT_TRUE(matched.has_value());
    const auto& result = *matched;
    const auto lethal = std::find_if(
        result.events.begin(),
        result.events.end(),
        [](const BattlePredictionEvent& event) {
            return event.phase == "damage_application"
                && event.label == "damage_applied_lethal";
        });
    ASSERT_NE(lethal, result.events.end());

    const auto drop = std::find_if(
        result.events.begin(),
        result.events.end(),
        [&](const BattlePredictionEvent& event) {
            return event.phase == "death_drop"
                && (event.label == "enemy_drop" || event.label == "enemy_no_drop")
                && event.actor_slot == lethal->actor_slot
                && event.target_slot == lethal->target_slot;
        });
    ASSERT_NE(drop, result.events.end());

    const auto burst = std::find_if(
        result.events.begin(),
        result.events.end(),
        [&](const BattlePredictionEvent& event) {
            return event.phase == "action_visual_rng"
                && event.label == "combat_effect_burst"
                && event.actor_slot == lethal->actor_slot
                && event.target_slot == lethal->target_slot;
        });
    ASSERT_NE(burst, result.events.end());

    EXPECT_LT(lethal->sequence, drop->sequence);
    EXPECT_LT(drop->sequence, burst->sequence);

    const auto* drop_validation = find_prediction_validation(result, "drop");
    ASSERT_NE(drop_validation, nullptr);
    EXPECT_NE(drop_validation->detail.find("before the following combat-effect RNG burst"), std::string::npos);
}

TEST(SavorPredictBattlePredictor, DrainsLandedEffectBurstBeforeNextActorSetup) {
    std::optional<BattlePredictionResult> matched;
    for (std::uint32_t seed = 0; seed < 4096 && !matched.has_value(); ++seed) {
        BattlePredictionInput input;
        input.profile = first_battle_prediction_profile();
        input.starting_rng_seed = seed;
        input.enemy_event_id = 0;
        input.context = make_predictor_first_battle_context(1000);
        input.turn_plan = make_two_pc_attack_turn_plan(0);

        auto result = predict_battle(input);

        const auto aika_burst = std::find_if(
            result.events.begin(),
            result.events.end(),
            [](const BattlePredictionEvent& event) {
                return event.phase == "action_visual_rng"
                    && event.label == "combat_effect_burst"
                    && event.actor_slot == 1
                    && event.target_slot == 4;
            });
        if (aika_burst == result.events.end()) {
            continue;
        }

        const auto vyse_setup = std::find_if(
            result.events.begin(),
            result.events.end(),
            [](const BattlePredictionEvent& event) {
                return event.phase == "movement_setup"
                    && event.label == "worker_select"
                    && event.actor_slot == 0
                    && event.target_slot == 4;
            });
        if (vyse_setup == result.events.end()) {
            continue;
        }

        if (aika_burst->sequence < vyse_setup->sequence) {
            matched = std::move(result);
        }
    }

    ASSERT_TRUE(matched.has_value());
    const auto& result = *matched;

    const auto aika_attack = std::find_if(
        result.events.begin(),
        result.events.end(),
        [](const BattlePredictionEvent& event) {
            return event.phase == "attack_resolution"
                && (event.label == "attack_hit" || event.label == "attack_crit")
                && event.actor_slot == 1
                && event.target_slot == 4;
        });
    ASSERT_NE(aika_attack, result.events.end());

    const auto aika_burst = std::find_if(
        result.events.begin(),
        result.events.end(),
        [](const BattlePredictionEvent& event) {
            return event.phase == "action_visual_rng"
                && event.label == "combat_effect_burst"
                && event.actor_slot == 1
                && event.target_slot == 4;
        });
    ASSERT_NE(aika_burst, result.events.end());
    EXPECT_EQ(aika_burst->draws_consumed, 110);
    ASSERT_TRUE(aika_burst->effect_source_key.has_value());
    EXPECT_EQ(*aika_burst->effect_source_key, 5);
    EXPECT_TRUE(aika_burst->rng_seed_before.has_value());
    EXPECT_TRUE(aika_burst->rng_seed_after.has_value());

    const auto aika_tail = std::find_if(
        result.events.begin(),
        result.events.end(),
        [](const BattlePredictionEvent& event) {
            return event.phase == "action_view_pathing_tail"
                && event.label == "fun_80011694_target_side_fallback"
                && event.actor_slot == 1
                && event.target_slot == 4;
        });
    ASSERT_NE(aika_tail, result.events.end());
    EXPECT_EQ(aika_tail->draws_consumed, 7);
    EXPECT_EQ(aika_tail->status, BattlePredictionEventStatus::Provisional);
    EXPECT_LT(aika_attack->sequence, aika_tail->sequence);
    EXPECT_LT(aika_tail->sequence, aika_burst->sequence);

    const auto vyse_setup = std::find_if(
        result.events.begin(),
        result.events.end(),
        [](const BattlePredictionEvent& event) {
            return event.phase == "movement_setup"
                && event.label == "worker_select"
                && event.actor_slot == 0
                && event.target_slot == 4;
        });
    ASSERT_NE(vyse_setup, result.events.end());

    EXPECT_LT(aika_attack->sequence, aika_burst->sequence);
    EXPECT_LT(aika_burst->sequence, vyse_setup->sequence);
    EXPECT_TRUE(result.has_provisional_events);
    EXPECT_EQ(result.outcome, BattlePredictionOutcome::Provisional);
}

TEST(SavorPredictBattlePredictor, UsesActionViewStdJsonDirForSelectorBackedCameraPrediction) {
    const auto temp_dir =
        std::filesystem::temp_directory_path() / "savor_predict_battle_predictor_std_json_test";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto write_table = [&](std::string_view filename, int action_key) {
        std::ofstream file(temp_dir / std::string(filename));
        ASSERT_TRUE(file.good());
        file << R"json({
  "schema": "spice_std_ir_v1",
  "layoutKind": "entry_table",
  "parseOk": true,
  "entryTable": {
    "records": [
      {
        "index": 0,
        "isSentinel": false,
        "locationCode": 42,
        "opcode": 3,
        "payloadInBounds": true,
        "payloadBytesHex": ")json"
             << std::hex << std::setw(4) << std::setfill('0') << action_key
             << R"json(00000000"
      },
      {
        "index": 1,
        "isSentinel": true,
        "locationCode": -1,
        "opcode": 0,
        "payloadInBounds": false,
        "payloadBytesHex": ""
      }
    ]
  }
})json";
    };
    write_table("ma0000.std.json", 4);
    write_table("ma0010.std.json", 5);
    write_table("mb0000.std.json", 4);

    BattlePredictionInput input;
    input.profile = first_battle_prediction_profile();
    input.starting_rng_seed = 15u;
    input.enemy_event_id = 0;
    input.context = make_predictor_first_battle_context();
    input.turn_plan.fake_attack_count = 0;
    input.turn_plan.commands.push_back(soa::battle::actions::BattleCommand{
        .actor_slot = 0,
        .macro = soa::battle::actions::BattleAction::Attack,
        .params = soa::battle::actions::ActionParameters{.target_slot = 4},
    });
    input.options.action_view_std_json_dir = temp_dir;

    const auto result = predict_battle(input);

    const auto action_view = std::find_if(
        result.events.begin(),
        result.events.end(),
        [](const BattlePredictionEvent& event) {
            return event.phase == "action_visual_rng"
                && event.label == "mode0_action_view_camera_fallback"
                && event.actor_slot == 0;
        });
    ASSERT_NE(action_view, result.events.end());
    EXPECT_EQ(action_view->status, BattlePredictionEventStatus::Exact);
    EXPECT_NE(action_view->detail.find("mode0e_count=1"), std::string::npos);
    EXPECT_NE(action_view->detail.find("std0_companion=ma0000.std"), std::string::npos);
    EXPECT_NE(action_view->detail.find("std0_materialization_source=cache"), std::string::npos);
    EXPECT_NE(action_view->detail.find("std0_cache_key=0x00989680"), std::string::npos);
    EXPECT_NE(action_view->detail.find("std0_cache_slot=0"), std::string::npos);
    EXPECT_NE(
        action_view->detail.find("runtime_loaded_resource_plus_0x30_is_aux_root=1"),
        std::string::npos);

    const auto* selector_validation = find_prediction_validation(result, "action_view_selector");
    ASSERT_NE(selector_validation, nullptr);
    EXPECT_EQ(selector_validation->status, BattlePredictionValidationStatus::Provisional);

    std::filesystem::remove_all(temp_dir);
}

TEST(SavorPredictBattlePredictor, UsesEnemyEventStartPositionsWhenSpecified) {
    BattlePredictionInput input;
    input.profile = first_battle_prediction_profile();
    input.starting_rng_seed = 15u;
    input.enemy_event_id = 0;
    input.context = make_predictor_first_battle_context();
    input.turn_plan = make_two_pc_attack_turn_plan(0);
    input.options.movement_backend = BattlePredictionMovementBackend::FrameStateMachine;

    const auto result = predict_battle(input);

    ASSERT_TRUE(result.enemy_event_id.has_value());
    EXPECT_EQ(*result.enemy_event_id, 0);
    const auto* position = find_prediction_event(
        result,
        "encounter_setup",
        "enemy_event_start_position");
    ASSERT_NE(position, nullptr);
    EXPECT_EQ(position->status, BattlePredictionEventStatus::Exact);
    EXPECT_EQ(position->actor_slot, 0);
    EXPECT_NE(position->detail.find("enemy_event_id=0"), std::string::npos);
    EXPECT_NE(position->detail.find("grid_x=4 grid_z=6"), std::string::npos);

    ASSERT_GT(result.final_slots.size(), 5u);
    ASSERT_TRUE(result.final_slots[0].start_position.has_value());
    EXPECT_EQ(result.final_slots[0].start_position->grid_x, 4);
    EXPECT_EQ(result.final_slots[0].start_position->grid_z, 6);
    ASSERT_TRUE(result.final_slots[5].start_position.has_value());
    EXPECT_EQ(result.final_slots[5].start_position->grid_x, 6);
    EXPECT_EQ(result.final_slots[5].start_position->grid_z, 2);

    const auto* worker = find_prediction_event(result, "movement_setup", "worker_select");
    ASSERT_NE(worker, nullptr);
    EXPECT_NE(worker->detail.find("actor_start=("), std::string::npos);
    EXPECT_NE(worker->detail.find("worksheet_source=enemy_event_0_frame_state_projection"), std::string::npos);
    EXPECT_NE(worker->detail.find("raw_stage_position=known"), std::string::npos);

    const auto* validation = find_prediction_validation(result, "start_positions");
    ASSERT_NE(validation, nullptr);
    EXPECT_EQ(validation->status, BattlePredictionValidationStatus::Exact);
}

TEST(SavorPredictBattlePredictor, CompareMovementBackendEmitsFrameAndComparisonEvents) {
    BattlePredictionInput input;
    input.profile = first_battle_prediction_profile();
    input.starting_rng_seed = 15u;
    input.enemy_event_id = 0;
    input.context = make_predictor_first_battle_context();
    input.turn_plan = make_two_pc_attack_turn_plan(0);
    input.options.movement_backend = BattlePredictionMovementBackend::Compare;

    const auto result = predict_battle(input);

    const auto* init = find_prediction_event(result, "frame_scheduler", "runtime_initialized");
    ASSERT_NE(init, nullptr);
    EXPECT_EQ(init->movement_backend, "frame");

    const auto* frame_event = find_prediction_event(result, "frame_scheduler", "worker_frame");
    ASSERT_NE(frame_event, nullptr);
    EXPECT_EQ(frame_event->movement_backend, "frame");
    EXPECT_TRUE(frame_event->frame_index.has_value());

    const auto* compare = find_prediction_event(result, "movement_compare", "handler_frame_comparison");
    ASSERT_NE(compare, nullptr);
    EXPECT_EQ(compare->movement_backend, "compare");

    const auto* movement_validation = find_prediction_validation(result, "movement_setup");
    ASSERT_NE(movement_validation, nullptr);
    EXPECT_NE(movement_validation->detail.find("persistent frame scheduler"), std::string::npos);
}

TEST(SavorPredictBattlePredictor, FrameBackendEmitsPassiveClashAndChunkedEffects) {
    std::optional<BattlePredictionResult> matched;
    for (std::uint32_t seed = 0; seed < 4096 && !matched.has_value(); ++seed) {
        BattlePredictionInput input;
        input.profile = first_battle_prediction_profile();
        input.starting_rng_seed = seed;
        input.enemy_event_id = 0;
        input.context = make_predictor_first_battle_context(1000);
        input.turn_plan = make_two_pc_attack_turn_plan(0);
        input.options.movement_backend = BattlePredictionMovementBackend::FrameStateMachine;

        auto result = predict_battle(input);
        const auto effect_chunk = find_prediction_event(
            result,
            "frame_scheduler",
            "combat_effect_chunk");
        const auto passive_clash = find_prediction_event(
            result,
            "frame_scheduler",
            "fun_8002eb4c_passive_clash");
        if (effect_chunk != nullptr && passive_clash != nullptr) {
            matched = std::move(result);
        }
    }

    ASSERT_TRUE(matched.has_value());
    const auto& result = *matched;

    const auto aggregate_burst = std::find_if(
        result.events.begin(),
        result.events.end(),
        [](const BattlePredictionEvent& event) {
            return event.phase == "action_visual_rng"
                && event.label == "combat_effect_burst";
        });
    EXPECT_EQ(aggregate_burst, result.events.end());

    int effect_chunk_events = 0;
    int effect_chunk_draws = 0;
    for (const auto& event : result.events) {
        if (event.phase != "frame_scheduler" || event.label != "combat_effect_chunk") {
            continue;
        }
        ++effect_chunk_events;
        effect_chunk_draws += event.draws_consumed;
        EXPECT_EQ(event.status, BattlePredictionEventStatus::Provisional);
        EXPECT_TRUE(event.frame_index.has_value());
        EXPECT_TRUE(event.effect_source_key.has_value());
    }

    EXPECT_GE(effect_chunk_events, 2);
    EXPECT_TRUE(effect_chunk_draws == 100 || effect_chunk_draws == 110 || effect_chunk_draws > 110);

    const auto* passive_clash = find_prediction_event(
        result,
        "frame_scheduler",
        "fun_8002eb4c_passive_clash");
    ASSERT_NE(passive_clash, nullptr);
    EXPECT_EQ(passive_clash->draws_consumed, 1);
    EXPECT_TRUE(passive_clash->rand_value.has_value());
    EXPECT_NE(passive_clash->detail.find("candidate0=0x0000000D"), std::string::npos);
    EXPECT_NE(passive_clash->detail.find("candidate1=0x0000000C"), std::string::npos);
}

TEST(SavorPredictBattlePredictor, UsesBattleInstanceCounterChanceAsDamageIncrement) {
    BattlePredictionInput input;
    input.profile = first_battle_prediction_profile();
    input.starting_rng_seed = 15u;
    input.context = make_predictor_first_battle_context();
    input.context.slots_[4].instance.base_counter_chance = 31;
    input.context.slots_[4].instance.counter_chance = 7;
    input.context.slots_[4].instance.current_counter_chance = 3;
    input.turn_plan.fake_attack_count = 0;

    const auto result = predict_battle(input);

    const auto slot = std::find_if(
        result.final_slots.begin(),
        result.final_slots.end(),
        [](const BattlePredictionSlotState& state) {
            return state.slot == 4;
        });
    ASSERT_NE(slot, result.final_slots.end());
    EXPECT_EQ(slot->base_counter_chance, 31);
    EXPECT_EQ(slot->counter_chance_increment, 7);
    EXPECT_EQ(
        std::find_if(
            result.warnings.begin(),
            result.warnings.end(),
            [](const std::string& warning) {
                return warning.find("counter chance increment") != std::string::npos;
            }),
        result.warnings.end());
}

TEST(SavorPredictBattlePredictor, ReportsUnsupportedPlayerActionsExplicitly) {
    BattlePredictionInput input;
    input.profile = first_battle_prediction_profile();
    input.starting_rng_seed = 15u;
    input.enemy_event_id = 0;
    input.context = make_predictor_first_battle_context();
    input.turn_plan.fake_attack_count = 0;
    input.turn_plan.commands.push_back(soa::battle::actions::BattleCommand{
        .actor_slot = 0,
        .macro = soa::battle::actions::BattleAction::Focus,
    });

    const auto result = predict_battle(input);

    EXPECT_TRUE(result.has_unsupported_events);
    EXPECT_EQ(result.outcome, BattlePredictionOutcome::Unsupported);
    const auto* unsupported = find_prediction_event(
        result,
        "player_command",
        "unsupported_player_action");
    ASSERT_NE(unsupported, nullptr);
    EXPECT_EQ(unsupported->status, BattlePredictionEventStatus::Unsupported);
}

TEST(SavorPredictBattlePredictor, RetargetsDeadFirstBattleSoldierToOnlyLivingSoldier) {
    BattlePredictionInput input;
    input.profile = first_battle_prediction_profile();
    input.starting_rng_seed = 15u;
    input.enemy_event_id = 0;
    input.context = make_predictor_first_battle_context();
    input.context.slots_[4].is_alive = 0;
    input.context.slots_[4].instance.Current_HP = 0;
    input.options.include_visual_rng_gap_events = false;

    input.turn_plan.fake_attack_count = 0;
    input.turn_plan.commands.push_back(soa::battle::actions::BattleCommand{
        .actor_slot = 0,
        .macro = soa::battle::actions::BattleAction::Attack,
        .params = soa::battle::actions::ActionParameters{.target_slot = 4},
    });

    const auto result = predict_battle(input);

    const auto* retarget = find_prediction_event(
        result,
        "movement_setup",
        "pc_attack_retarget");
    ASSERT_NE(retarget, nullptr);
    EXPECT_EQ(retarget->status, BattlePredictionEventStatus::Exact);
    EXPECT_EQ(retarget->actor_slot, 0);
    EXPECT_EQ(retarget->target_slot, 5);
    EXPECT_NE(retarget->detail.find("original_target=4"), std::string::npos);
    EXPECT_NE(retarget->detail.find("final_target=5"), std::string::npos);
    EXPECT_EQ(find_prediction_event(
        result,
        "attack_resolution",
        "skipped_dead_or_missing_actor"), nullptr);

    const auto* retarget_validation = find_prediction_validation(result, "retargeting");
    ASSERT_NE(retarget_validation, nullptr);
    EXPECT_EQ(retarget_validation->status, BattlePredictionValidationStatus::Validated);
    EXPECT_NE(retarget_validation->detail.find("RNG-neutral"), std::string::npos);
}

TEST(SavorPredictBattlePredictorCli, RejectsMutableDebugDbRoot) {
    const auto parsed = parse_predict_battle_tokens({
        "--turn-job-id", "123",
        "--db-root", "D:/SoaSimDBDebug",
    });

    EXPECT_FALSE(parsed.errors.empty());
    EXPECT_NE(
        std::find(
            parsed.errors.begin(),
            parsed.errors.end(),
            "Refusing to use D:/SoaSimDBDebug for prediction; use D:/SavorPredictDB."),
        parsed.errors.end());
}

TEST(SavorPredictBattlePredictorCli, ParsesSeedCandidateFallbackFlag) {
    const auto parsed = parse_predict_battle_tokens({
        "--exec-job-id",
        "10",
        "--allow-seed-candidate-fallback",
    });

    EXPECT_TRUE(parsed.errors.empty());
    EXPECT_TRUE(parsed.options.allow_seed_candidate_fallback);
}

TEST(SavorPredictBattlePredictorCli, ParsesStartSeedListForDbSelector) {
    const auto parsed = parse_predict_battle_tokens({
        "--exec-job-id",
        "147884",
        "--start-seed-list",
        "seeds.txt",
    });

    EXPECT_TRUE(parsed.errors.empty());
    EXPECT_EQ(parsed.options.exec_job_id, 147884);
    EXPECT_EQ(parsed.options.start_seed_list, std::filesystem::path("seeds.txt"));
}

TEST(SavorPredictBattlePredictorCli, ParsesEnemyEventId) {
    const auto parsed = parse_predict_battle_tokens({
        "--exec-job-id",
        "147884",
        "--enemy-event-id",
        "0",
    });

    EXPECT_TRUE(parsed.errors.empty());
    ASSERT_TRUE(parsed.options.enemy_event_id.has_value());
    EXPECT_EQ(*parsed.options.enemy_event_id, 0);
}

TEST(SavorPredictBattlePredictorCli, ParsesMovementBackend) {
    const auto parsed = parse_predict_battle_tokens({
        "--exec-job-id",
        "147884",
        "--movement-backend",
        "compare",
    });

    EXPECT_TRUE(parsed.errors.empty()) << (parsed.errors.empty() ? "" : parsed.errors.front());
    EXPECT_EQ(parsed.options.movement_backend, BattlePredictionMovementBackend::Compare);
}

TEST(SavorPredictBattlePredictorCli, RejectsInvalidMovementBackend) {
    const auto parsed = parse_predict_battle_tokens({
        "--exec-job-id",
        "147884",
        "--movement-backend",
        "old",
    });

    EXPECT_FALSE(parsed.errors.empty());
    EXPECT_NE(
        std::find(
            parsed.errors.begin(),
            parsed.errors.end(),
            "--movement-backend must be handler, frame, or compare."),
        parsed.errors.end());
}

TEST(SavorPredictBattlePredictorCli, ParsesActionViewStdJsonDir) {
    const auto temp_dir =
        std::filesystem::temp_directory_path() / "savor_predict_cli_std_json_dir_test";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto parsed = parse_predict_battle_tokens({
        "--exec-job-id",
        "147884",
        "--action-view-std-json-dir",
        temp_dir.string(),
    });

    EXPECT_TRUE(parsed.errors.empty());
    EXPECT_EQ(parsed.options.action_view_std_json_dir, temp_dir);

    std::filesystem::remove_all(temp_dir);
}

TEST(SavorPredictBattlePredictorCli, ParsesStdJsonCacheOptions) {
    const auto parsed = parse_predict_battle_tokens({
        "--exec-job-id",
        "147884",
        "--std-disc-dump-root",
        "D:/disc",
        "--spice-file-parsing-exe",
        "D:/tools/SpiceFileParsing.exe",
    });

    EXPECT_TRUE(parsed.errors.empty()) << (parsed.errors.empty() ? "" : parsed.errors.front());
    EXPECT_EQ(parsed.options.std_disc_dump_root, std::filesystem::path("D:/disc"));
    EXPECT_EQ(parsed.options.spice_file_parsing_exe, std::filesystem::path("D:/tools/SpiceFileParsing.exe"));
}

TEST(SavorPredictBattlePredictorCli, RejectsMissingActionViewStdJsonDir) {
    const auto missing_dir =
        std::filesystem::temp_directory_path() / "savor_predict_cli_missing_std_json_dir_test";
    std::filesystem::remove_all(missing_dir);

    const auto parsed = parse_predict_battle_tokens({
        "--exec-job-id",
        "147884",
        "--action-view-std-json-dir",
        missing_dir.string(),
    });

    EXPECT_FALSE(parsed.errors.empty());
    EXPECT_NE(
        std::find_if(
            parsed.errors.begin(),
            parsed.errors.end(),
            [](const std::string& error) {
                return error.find("--action-view-std-json-dir must name an existing directory")
                    != std::string::npos;
            }),
        parsed.errors.end());
}

TEST(SavorPredictStdJsonCache, ReportsDefaultPaths) {
    EXPECT_EQ(
        default_action_view_std_json_cache_dir("D:/SavorPredictDB"),
        std::filesystem::path("D:/SavorPredictDB/.std_json"));
    EXPECT_EQ(
        default_action_view_std_disc_dump_root(),
        std::filesystem::path("D:/SoAGC/2002-12-19-gc-us-final_Skies_of_Arcadia_Legends"));
}

TEST(SavorPredictStdJsonCache, CompleteCacheReturnsWithoutInvokingSpice) {
    const auto root = std::filesystem::temp_directory_path()
        / ("savor_std_cache_hit_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto cache = root / ".std_json";
    std::filesystem::create_directories(cache);
    for (const auto& name : required_first_battle_action_view_std_json_files()) {
        std::ofstream(cache / name, std::ios::binary | std::ios::trunc) << "{}";
    }

    bool invoked = false;
    const auto resolved = resolve_action_view_std_json_cache(
        {
            .db_root = root,
            .std_disc_dump_root = root / "missing_disc",
            .spice_file_parsing_exe = root / "missing.exe",
        },
        [&](const SpiceStdJsonExportRequest&) {
            invoked = true;
            return SpiceStdJsonExportResult{};
        });

    EXPECT_TRUE(resolved.available);
    EXPECT_TRUE(resolved.cache_complete_before);
    EXPECT_FALSE(resolved.generation_attempted);
    EXPECT_FALSE(invoked);
    EXPECT_EQ(resolved.resolved_std_json_dir, cache);

    std::filesystem::remove_all(root);
}

TEST(SavorPredictStdJsonCache, GeneratesMissingCacheFromDiscDump) {
    const auto root = std::filesystem::temp_directory_path()
        / ("savor_std_cache_generate_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto disc = root / "disc";
    const auto bchara = disc / "bchara";
    const auto fake_exe = root / "SpiceFileParsing.exe";
    std::filesystem::create_directories(bchara);
    std::ofstream(fake_exe, std::ios::binary | std::ios::trunc) << "fake";

    bool invoked = false;
    const auto resolved = resolve_action_view_std_json_cache(
        {
            .db_root = root / "db",
            .std_disc_dump_root = disc,
            .spice_file_parsing_exe = fake_exe,
        },
        [&](const SpiceStdJsonExportRequest& request) {
            invoked = true;
            EXPECT_EQ(request.bchara_dir, bchara);
            EXPECT_EQ(request.output_dir, root / "db" / ".std_json");
            std::filesystem::create_directories(request.output_dir);
            for (const auto& name : required_first_battle_action_view_std_json_files()) {
                std::ofstream(request.output_dir / name, std::ios::binary | std::ios::trunc) << "{}";
            }
            return SpiceStdJsonExportResult{ .exit_code = 0, .output = "ok" };
        });

    EXPECT_TRUE(invoked);
    EXPECT_TRUE(resolved.available);
    EXPECT_TRUE(resolved.generation_attempted);
    EXPECT_TRUE(resolved.generation_succeeded);
    EXPECT_FALSE(resolved.fatal_error);
    EXPECT_EQ(resolved.resolved_std_json_dir, root / "db" / ".std_json");

    std::filesystem::remove_all(root);
}

TEST(SavorPredictStdJsonCache, ExplicitDirOverridesCacheGeneration) {
    const auto root = std::filesystem::temp_directory_path()
        / ("savor_std_cache_explicit_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto explicit_dir = root / "explicit";
    std::filesystem::create_directories(explicit_dir);

    bool invoked = false;
    const auto resolved = resolve_action_view_std_json_cache(
        {
            .db_root = root / "db",
            .explicit_std_json_dir = explicit_dir,
            .std_disc_dump_root = root / "disc",
            .spice_file_parsing_exe = root / "SpiceFileParsing.exe",
        },
        [&](const SpiceStdJsonExportRequest&) {
            invoked = true;
            return SpiceStdJsonExportResult{};
        });

    EXPECT_TRUE(resolved.available);
    EXPECT_TRUE(resolved.used_explicit_dir);
    EXPECT_FALSE(resolved.generation_attempted);
    EXPECT_FALSE(invoked);
    EXPECT_EQ(resolved.resolved_std_json_dir, explicit_dir);

    std::filesystem::remove_all(root);
}

TEST(SavorPredictStdJsonCache, MissingFilesAfterExportIsFatal) {
    const auto root = std::filesystem::temp_directory_path()
        / ("savor_std_cache_missing_after_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto disc = root / "disc";
    const auto fake_exe = root / "SpiceFileParsing.exe";
    std::filesystem::create_directories(disc / "bchara");
    std::ofstream(fake_exe, std::ios::binary | std::ios::trunc) << "fake";

    const auto resolved = resolve_action_view_std_json_cache(
        {
            .db_root = root / "db",
            .std_disc_dump_root = disc,
            .spice_file_parsing_exe = fake_exe,
        },
        [](const SpiceStdJsonExportRequest&) {
            return SpiceStdJsonExportResult{ .exit_code = 0, .output = "ok" };
        });

    EXPECT_FALSE(resolved.available);
    EXPECT_TRUE(resolved.generation_attempted);
    EXPECT_FALSE(resolved.generation_succeeded);
    EXPECT_TRUE(resolved.fatal_error);
    EXPECT_FALSE(resolved.missing_files_after.empty());

    std::filesystem::remove_all(root);
}

TEST(SavorPredictBattlePredictorCli, RejectsUnsupportedEnemyEventId) {
    const auto parsed = parse_predict_battle_tokens({
        "--exec-job-id",
        "147884",
        "--enemy-event-id",
        "999",
    });

    EXPECT_FALSE(parsed.errors.empty());
    EXPECT_NE(
        std::find(
            parsed.errors.begin(),
            parsed.errors.end(),
            "Unsupported --enemy-event-id: 999"),
        parsed.errors.end());
}

TEST(SavorPredictBattlePredictorCli, RejectsStartSeedAndStartSeedListTogether) {
    const auto parsed = parse_predict_battle_tokens({
        "--exec-job-id",
        "147884",
        "--start-seed",
        "0x8D5AD625",
        "--start-seed-list",
        "seeds.txt",
    });

    EXPECT_FALSE(parsed.errors.empty());
    EXPECT_NE(
        std::find(
            parsed.errors.begin(),
            parsed.errors.end(),
            "Specify only one of --start-seed or --start-seed-list."),
        parsed.errors.end());
}

TEST(SavorPredictBattlePredictorCli, RejectsStartSeedListWithContextFile) {
    const auto parsed = parse_predict_battle_tokens({
        "--context-file",
        "context.bin",
        "--turn-plan-hex",
        "00",
        "--fake-attacks",
        "0",
        "--start-seed-list",
        "seeds.txt",
    });

    EXPECT_FALSE(parsed.errors.empty());
    EXPECT_NE(
        std::find(
            parsed.errors.begin(),
            parsed.errors.end(),
            "--start-seed-list is only supported with a DB job selector."),
        parsed.errors.end());
}

TEST(SavorPredictBattlePredictionDbInput, RejectsMutableDebugDbRootInResolver) {
    BattlePredictionDbInputOptions options;
    options.db_root = "D:/SoaSimDBDebug";
    options.selector.exec_job_id = 10;
    std::ostringstream err;

    const auto resolved = build_battle_prediction_input_from_db_root(options, err);

    EXPECT_FALSE(resolved.has_value());
    EXPECT_NE(err.str().find("Refusing to use D:/SoaSimDBDebug"), std::string::npos);
}

TEST_F(SavorPredictDbInputFixture, UsesSeedProbeUniqueSeedForExecJob) {
    const auto rows = SeedPredictionRows(0x22222222u);

    BattlePredictionDbInputOptions options;
    options.selector.exec_job_id = rows.turn_exec_job_id;
    options.enemy_event_id = 0;
    options.action_view_std_json_dir = "C:/savor/std-json-fixture";
    std::ostringstream err;

    const auto resolved = build_battle_prediction_input_from_analysis_db(
        *db_service_->AnalysisDb(),
        options,
        err);

    ASSERT_TRUE(resolved.has_value()) << err.str();
    EXPECT_EQ(resolved->input.starting_rng_seed, rows.unique_seed);
    EXPECT_EQ(resolved->metadata.seed_source, BattlePredictionSeedSource::SeedProbeUniqueSeed);
    EXPECT_EQ(resolved->metadata.turn_job_id, rows.turn_job_id);
    EXPECT_EQ(resolved->metadata.exec_job_id.value_or(0), rows.turn_exec_job_id);
    EXPECT_EQ(resolved->metadata.context_source, BattlePredictionContextSource::LatestWaveContextProbe);
    EXPECT_EQ(resolved->metadata.context_probe_id.value_or(0), rows.context_probe_id);
    EXPECT_EQ(resolved->metadata.fake_attack_source, BattlePredictionFakeAttackSource::TurnJob);
    EXPECT_EQ(resolved->input.turn_plan.fake_attack_count, 2u);
    EXPECT_EQ(resolved->input.enemy_event_id.value_or(-1), 0);
    EXPECT_EQ(
        resolved->input.options.action_view_std_json_dir,
        std::filesystem::path("C:/savor/std-json-fixture"));
    EXPECT_EQ(resolved->metadata.enemy_event_id.value_or(-1), 0);
    ASSERT_EQ(resolved->input.turn_plan.commands.size(), 2u);
    EXPECT_EQ(resolved->input.turn_plan.commands[0].actor_slot, 0);
    EXPECT_EQ(resolved->metadata.resolved_turn_variant_key.value_or(""), "fixture-variant");
}

TEST_F(SavorPredictDbInputFixture, UsesSeedProbeUniqueSeedFromSourceInputFrameForLegacyCandidate) {
    const auto rows = SeedPredictionRows(0x22222222u, true, false);

    BattlePredictionDbInputOptions options;
    options.selector.exec_job_id = rows.turn_exec_job_id;
    options.enemy_event_id = 0;
    std::ostringstream err;

    const auto resolved = build_battle_prediction_input_from_analysis_db(
        *db_service_->AnalysisDb(),
        options,
        err);

    ASSERT_TRUE(resolved.has_value()) << err.str();
    EXPECT_EQ(resolved->input.starting_rng_seed, rows.unique_seed);
    EXPECT_EQ(resolved->metadata.seed_source, BattlePredictionSeedSource::SeedProbeUniqueSeed);
}

TEST_F(SavorPredictDbInputFixture, OverridesSeedAndFakeAttackCountExplicitly) {
    const auto rows = SeedPredictionRows(0x22222222u);

    BattlePredictionDbInputOptions options;
    options.selector.turn_job_id = rows.turn_job_id;
    options.start_seed_override = 0x33333333u;
    options.fake_attacks_override = 4;
    std::ostringstream err;

    const auto resolved = build_battle_prediction_input_from_analysis_db(
        *db_service_->AnalysisDb(),
        options,
        err);

    ASSERT_TRUE(resolved.has_value()) << err.str();
    EXPECT_EQ(resolved->input.starting_rng_seed, 0x33333333u);
    EXPECT_EQ(resolved->metadata.seed_source, BattlePredictionSeedSource::Override);
    EXPECT_FALSE(resolved->metadata.warnings.empty());
    EXPECT_EQ(resolved->input.turn_plan.fake_attack_count, 4u);
    EXPECT_EQ(resolved->metadata.fake_attack_source, BattlePredictionFakeAttackSource::Override);
}

TEST_F(SavorPredictDbInputFixture, UsesCandidateSeedWhenNoUniqueSeedSourceExists) {
    const auto rows = SeedPredictionRows(std::nullopt, false);

    BattlePredictionDbInputOptions options;
    options.selector.exec_job_id = rows.turn_exec_job_id;
    std::ostringstream err;

    const auto resolved = build_battle_prediction_input_from_analysis_db(
        *db_service_->AnalysisDb(),
        options,
        err);

    ASSERT_TRUE(resolved.has_value()) << err.str();
    EXPECT_EQ(resolved->input.starting_rng_seed, rows.candidate_seed);
    EXPECT_EQ(resolved->metadata.seed_source, BattlePredictionSeedSource::SeedCandidate);
}

TEST(SavorPredictRngModel, SoaQSortModelMatchesObservedEqualKeyPermutation) {
    const auto two = soa_qsort_indices_by_key_ascending({10, 10});
    ASSERT_EQ(two.size(), 2u);
    EXPECT_EQ(two[0], 1);
    EXPECT_EQ(two[1], 0);

    const auto four = soa_qsort_indices_by_key_ascending({10, 10, 10, 10});
    ASSERT_EQ(four.size(), 4u);
    EXPECT_EQ(four[0], 1);
    EXPECT_EQ(four[1], 2);
    EXPECT_EQ(four[2], 3);
    EXPECT_EQ(four[3], 0);

    const auto mixed = soa_qsort_indices_by_key_ascending({22, 24, 18, 18});
    ASSERT_EQ(mixed.size(), 4u);
    EXPECT_EQ(mixed[0], 3);
    EXPECT_EQ(mixed[1], 2);
    EXPECT_EQ(mixed[2], 0);
    EXPECT_EQ(mixed[3], 1);
}

TEST(SavorPredictRngModel, SoaQSortModelMatchesDisassembledHeapSortCases) {
    EXPECT_TRUE(soa_qsort_indices_by_key_ascending({}).empty());

    const auto one = soa_qsort_indices_by_key_ascending({1});
    ASSERT_EQ(one.size(), 1u);
    EXPECT_EQ(one[0], 0);

    const auto two = soa_qsort_indices_by_key_ascending({3, 1});
    ASSERT_EQ(two.size(), 2u);
    EXPECT_EQ(two[0], 1);
    EXPECT_EQ(two[1], 0);

    const auto mixed_five = soa_qsort_indices_by_key_ascending({5, 1, 3, 1, 4});
    ASSERT_EQ(mixed_five.size(), 5u);
    EXPECT_EQ(mixed_five[0], 1);
    EXPECT_EQ(mixed_five[1], 3);
    EXPECT_EQ(mixed_five[2], 2);
    EXPECT_EQ(mixed_five[3], 4);
    EXPECT_EQ(mixed_five[4], 0);

    const auto descending_input = soa_qsort_indices_by_key_ascending({5, 4, 3, 2, 1});
    ASSERT_EQ(descending_input.size(), 5u);
    EXPECT_EQ(descending_input[0], 4);
    EXPECT_EQ(descending_input[1], 3);
    EXPECT_EQ(descending_input[2], 2);
    EXPECT_EQ(descending_input[3], 1);
    EXPECT_EQ(descending_input[4], 0);

    const auto ascending_input = soa_qsort_indices_by_key_ascending({1, 2, 3, 4, 5});
    ASSERT_EQ(ascending_input.size(), 5u);
    EXPECT_EQ(ascending_input[0], 0);
    EXPECT_EQ(ascending_input[1], 1);
    EXPECT_EQ(ascending_input[2], 2);
    EXPECT_EQ(ascending_input[3], 3);
    EXPECT_EQ(ascending_input[4], 4);
}

TEST(SavorPredictRngModel, SoaQSortGenericEngineUsesInjectedComparator) {
    std::vector<std::uint8_t> records;
    for (const auto value : {1u, 3u, 2u}) {
        records.push_back(static_cast<std::uint8_t>(value));
    }

    const auto descending = simulate_soa_qsort_records(
        records,
        1,
        3,
        [](std::span<const std::uint8_t> lhs, std::span<const std::uint8_t> rhs) {
            return static_cast<int>(rhs[0]) - static_cast<int>(lhs[0]);
        });

    ASSERT_EQ(descending.size(), 3u);
    EXPECT_EQ(descending[0], 3);
    EXPECT_EQ(descending[1], 2);
    EXPECT_EQ(descending[2], 1);
}

TEST(SavorPredictRngModel, BattleActionQueueQSortUsesCallCountPrefixAndPreservesTail) {
    std::vector<std::uint8_t> records;
    const auto append_record = [&records](int slot, int priority) {
        const auto record = make_battle_turn_order_action_queue_record(
            static_cast<std::uint32_t>(slot & 0xff) << 24,
            static_cast<std::uint32_t>(priority),
            0);
        records.insert(records.end(), record.begin(), record.end());
    };
    append_record(0, 26);
    append_record(1, 26);
    append_record(4, 28);
    append_record(255, 0x7fffffff);

    const auto sorted = simulate_battle_turn_order_action_queue_qsort(records, 3);

    const auto slot_at = [&sorted](std::size_t index) {
        return static_cast<int>(battle_turn_order_action_queue_slot(
            std::span<const std::uint8_t>(sorted.data() + index * 12u, 12u)));
    };
    EXPECT_EQ(slot_at(0), 1);
    EXPECT_EQ(slot_at(1), 0);
    EXPECT_EQ(slot_at(2), 4);
    EXPECT_EQ(slot_at(3), 255);
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
    ASSERT_EQ(result.qsort_sorted_indices.size(), entries.size());
    ASSERT_EQ(result.qsort_sorted_slots.size(), entries.size());
    EXPECT_EQ(result.execution_slots, std::vector<int>(result.qsort_sorted_slots.rbegin(), result.qsort_sorted_slots.rend()));
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
    ASSERT_EQ(result.qsort_sorted_slots.size(), 3u);
    EXPECT_EQ(result.qsort_sorted_slots[0], 0);
    EXPECT_EQ(result.qsort_sorted_slots[1], 4);
    EXPECT_EQ(result.qsort_sorted_slots[2], 1);
    ASSERT_EQ(result.execution_slots.size(), 3u);
    EXPECT_EQ(result.execution_slots[0], 1);
    EXPECT_EQ(result.execution_slots[1], 4);
    EXPECT_EQ(result.execution_slots[2], 0);
}

TEST(SavorPredictRngModel, TurnOrderPriorityTiesUseExactSoaQSortPermutation) {
    std::vector<TurnOrderEntryInput> entries;
    entries.push_back({.slot = 0, .quick = 10, .initial_priority = 22});
    entries.push_back({.slot = 1, .quick = 10, .initial_priority = 22});
    entries.push_back({.slot = 4, .quick = 10, .initial_priority = 22});
    entries.push_back({.slot = 5, .quick = 10, .initial_priority = 22});

    const auto result = simulate_turn_order(0x12345678u, entries);

    EXPECT_EQ(result.draws_consumed, 0);
    EXPECT_TRUE(result.priority_ties_ambiguous);
    EXPECT_TRUE(result.execution_order_exact);
    ASSERT_EQ(result.qsort_sorted_indices.size(), 4u);
    EXPECT_EQ(result.qsort_sorted_indices[0], 1);
    EXPECT_EQ(result.qsort_sorted_indices[1], 2);
    EXPECT_EQ(result.qsort_sorted_indices[2], 3);
    EXPECT_EQ(result.qsort_sorted_indices[3], 0);
    ASSERT_EQ(result.qsort_sorted_slots.size(), 4u);
    EXPECT_EQ(result.qsort_sorted_slots[0], 1);
    EXPECT_EQ(result.qsort_sorted_slots[1], 4);
    EXPECT_EQ(result.qsort_sorted_slots[2], 5);
    EXPECT_EQ(result.qsort_sorted_slots[3], 0);
    ASSERT_EQ(result.execution_slots.size(), 4u);
    EXPECT_EQ(result.execution_slots[0], 0);
    EXPECT_EQ(result.execution_slots[1], 5);
    EXPECT_EQ(result.execution_slots[2], 4);
    EXPECT_EQ(result.execution_slots[3], 1);
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
        "pc=80067b50 function=FUN_8006782c checkpoint=source_selection rng_draw_index_before=17 "
        "actor_slot=1 selected_source_slot_global=1 target_slot=4 action_sequence_id=7 action_id=4\n"
        "pc=8006721c function=FUN_8006721c checkpoint=action_source rng_draw_index_before=18 "
        "actor_slot=1 source_slot=1 target_slot=4 action_sequence_id=7 action_id=4 "
        "source_field6_0x6=14 actor_field6_0x6=14 handler_pc=0x800662bc callback_pc=0x800662bc\n"
        "pc=80067a9c function=FUN_8006782c checkpoint=source_selection rng_draw_index_before=18 "
        "actor_slot=0 r5_selected_source_slot_candidate=0 target_slot=4 action_sequence_id=8 action_id=8\n"
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
        "pc=80067ad0 function=FUN_8006782c checkpoint=source_selection rng_draw_index_before=17 "
        "actor_slot=0 r0_selected_source_slot_fallback=0 action_sequence_id=7\n"
        "pc=80067b50 function=FUN_8006782c checkpoint=source_selection rng_draw_index_before=18 "
        "actor_slot=1 selected_source_slot_global=1 action_sequence_id=8\n"
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
        "pc=80067b50 function=FUN_8006782c checkpoint=source_selection rng_draw_index_before=19 "
        "actor_slot=1 selected_source_slot_global=1\n"
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
        "pc=80067b50 function=FUN_8006782c checkpoint=source_selection rng_draw_index_before=20 "
        "actor_slot=1 selected_source_slot_global=1\n"
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
        "pc=80067b50 function=FUN_8006782c checkpoint=source_selection rng_draw_index_before=23 "
        "actor_slot=1 selected_source_slot_global=0\n"
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
        "pc=80067b50 function=FUN_8006782c checkpoint=source_selection rng_draw_index_before=25 "
        "actor_slot=1 selected_source_slot_global=1\n"
        "pc=8006721c function=FUN_8006721c checkpoint=action_source rng_draw_index_before=26 "
        "actor_slot=1 source_slot=1 source_field6_0x6=14 actor_field6_0x6=14 "
        "handler_pc=800662BC callback_pc=80000000\n");
    const auto callback_mismatch_parsed = parse_checkpoint_stream(callback_mismatch_input);
    ASSERT_TRUE(callback_mismatch_parsed.errors.empty());
    const auto callback_mismatch = summarize_action_source_checkpoints(
        callback_mismatch_parsed.events,
        expectation.expected_handler_pc);
    EXPECT_EQ(callback_mismatch.status, ActionSourceCheckpointStatus::CallbackMismatch);

    std::istringstream non_selection_input(
        "pc=8006782c function=FUN_8006782c checkpoint=action_source_selection_entry "
        "rng_draw_index_before=27 actor_slot=1\n"
        "pc=80067bd0 function=FUN_8006782c checkpoint=action_source_state_downstream_read "
        "rng_draw_index_before=28 actor_slot=1 selected_source_slot=1\n"
        "pc=8006721c function=FUN_8006721c checkpoint=action_source rng_draw_index_before=29 "
        "actor_slot=1 source_slot=1 source_field6_0x6=14 actor_field6_0x6=14 "
        "handler_pc=800662BC callback_pc=800662BC\n");
    const auto non_selection_parsed = parse_checkpoint_stream(non_selection_input);
    ASSERT_TRUE(non_selection_parsed.errors.empty());
    const auto non_selection = summarize_action_source_checkpoints(
        non_selection_parsed.events,
        expectation.expected_handler_pc);
    EXPECT_EQ(non_selection.observed_source_selection_events, 0);
    EXPECT_EQ(non_selection.status, ActionSourceCheckpointStatus::MissingSourceSelectionCheckpoint);
}

TEST(SavorPredictCheckpointTrace, SummarizesSstActionCommandField6Stores) {
    std::istringstream input(
        "pc=8000c4c8 function=SST::Command::Dispatch_8000c19c "
        "checkpoint=sst_action_field6_case2_store_complete rng_draw_index_before=10 "
        "source_field6_0x06=4 dest_field6_after_0x06=4 r0_written_field6=4 "
        "action_sequence_id=3\n"
        "pc=8000c6e8 function=SST::Command::Dispatch_8000c19c "
        "checkpoint=sst_action_field6_case8_store_complete rng_draw_index_before=11 "
        "source_field6_0x0a=8 dest_field6_after_0x06=8 r0_written_field6=8 "
        "action_sequence_id=4\n");

    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());
    const auto matched = summarize_sst_action_command_checkpoints(parsed.events);
    EXPECT_EQ(matched.status, SstActionCommandCheckpointStatus::MatchesExpected);
    EXPECT_EQ(matched.observed_store_events, 2);
    EXPECT_EQ(matched.observed_case2_store_events, 1);
    EXPECT_EQ(matched.observed_case8_store_events, 1);
    EXPECT_EQ(matched.events_with_source_field6, 2);
    EXPECT_EQ(matched.events_with_destination_field6, 2);
    EXPECT_EQ(matched.events_with_written_field6_register, 2);
    EXPECT_EQ(matched.events_with_action_sequence_id, 2);
    EXPECT_EQ(matched.source_destination_matches, 2);
    EXPECT_EQ(matched.source_destination_mismatches, 0);
    EXPECT_EQ(matched.key8_store_events, 1);
    ASSERT_TRUE(matched.first_store_draw_index.has_value());
    EXPECT_EQ(*matched.first_store_draw_index, 10);
    ASSERT_TRUE(matched.first_key8_store_draw_index.has_value());
    EXPECT_EQ(*matched.first_key8_store_draw_index, 11);
    ASSERT_EQ(matched.events.size(), 2u);
    EXPECT_EQ(matched.events[0].kind, SstActionCommandCheckpointKind::Case2Field6Store);
    EXPECT_EQ(matched.events[1].kind, SstActionCommandCheckpointKind::Case8Field6Store);
    ASSERT_TRUE(matched.events[1].source_matches_destination.has_value());
    EXPECT_TRUE(*matched.events[1].source_matches_destination);
    EXPECT_EQ(
        sst_action_command_checkpoint_status_name(matched.status),
        std::string("MatchesExpected"));
    EXPECT_NE(
        std::string_view(first_battle_sst_action_command_checkpoint_rule_detail()).find("8000c6e8"),
        std::string_view::npos);

    std::istringstream missing_input(
        "pc=8000c4c8 function=SST::Command::Dispatch_8000c19c "
        "checkpoint=sst_action_field6_case2_store_complete rng_draw_index_before=12 "
        "source_field6_0x06=4\n");
    const auto missing_parsed = parse_checkpoint_stream(missing_input);
    ASSERT_TRUE(missing_parsed.errors.empty());
    const auto missing = summarize_sst_action_command_checkpoints(missing_parsed.events);
    EXPECT_EQ(missing.status, SstActionCommandCheckpointStatus::MissingLiveFields);

    std::istringstream mismatch_input(
        "pc=8000c6e8 function=SST::Command::Dispatch_8000c19c "
        "checkpoint=sst_action_field6_case8_store_complete rng_draw_index_before=13 "
        "source_field6_0x0a=8 dest_field6_after_0x06=4\n");
    const auto mismatch_parsed = parse_checkpoint_stream(mismatch_input);
    ASSERT_TRUE(mismatch_parsed.errors.empty());
    const auto mismatch = summarize_sst_action_command_checkpoints(mismatch_parsed.events);
    EXPECT_EQ(mismatch.status, SstActionCommandCheckpointStatus::Field6Mismatch);
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

    std::istringstream split_enemy_setup_input(
        "pc=8008bc68 function=Battle::HandleECInst_8008b9e0 checkpoint=enemy_setup "
        "rng_draw_index_before=30 actor_slot=4 setup_rand=27 movement_flags=0xc0 "
        "slot4_instruction_0x0=3 slot4_target_0x4=0 slot4_instr_param_0x6=1\n"
        "pc=8008a660 function=Battle::HandleECInst_8008b9e0 checkpoint=soldier_ai_instr_param_set "
        "rng_draw_index_before=30 actor_slot=4 slot4_instruction_0x0=3 "
        "slot4_target_0x4=0 slot4_instr_param_0x6=0\n"
        "pc=8008bcb0 function=Battle::HandleECInst_8008b9e0 checkpoint=movement_helper_return "
        "rng_draw_index_before=30 actor_slot=4 helper_result=1\n"
        "pc=8008bccc function=Battle::HandleECInst_8008b9e0 checkpoint=movement_distance_return "
        "rng_draw_index_before=30 actor_slot=4 helper_result=0 target_distance=3\n"
        "pc=8008bdac function=Battle::HandleECInst_8008b9e0 checkpoint=worker_select "
        "rng_draw_index_before=30 actor_slot=4\n"
        "pc=80010bdc function=getAttackResult checkpoint=hit rng_draw_index_before=31\n");
    const auto split_enemy_setup_parsed = parse_checkpoint_stream(split_enemy_setup_input);
    ASSERT_TRUE(split_enemy_setup_parsed.errors.empty());
    const auto split_enemy_setup =
        summarize_action_setup_checkpoints(split_enemy_setup_parsed.events, 1);
    EXPECT_EQ(split_enemy_setup.status, ActionSetupCheckpointStatus::MatchesExpected);
    EXPECT_EQ(split_enemy_setup.observed_enemy_setup_draws, 1);
    EXPECT_EQ(split_enemy_setup.enemy_setup_draws_with_required_helper_fields, 1);
    EXPECT_EQ(split_enemy_setup.worker_matches, 1);
    ASSERT_EQ(split_enemy_setup.events.size(), 1u);
    ASSERT_TRUE(split_enemy_setup.events[0].final_instr_param_0x6.has_value());
    EXPECT_EQ(*split_enemy_setup.events[0].final_instr_param_0x6, 0);
    ASSERT_TRUE(split_enemy_setup.events[0].selected_worker_pc.has_value());
    EXPECT_EQ(*split_enemy_setup.events[0].selected_worker_pc, std::string("80087F6C"));

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
        "active_slot=1 source_slot=1 target_slot=4 source_field6_0x6=4 actor_field6_0x6=4 "
        "actor_subtype_0x8=0 gate_category_0x2f=2 gate_state_0x30=2 "
        "gate_active_slot_0x02=1 gate_target_slot_0x04=4 instruction_flags_0xf0=0x00000000 "
        "aux_list_root=0x80346bd8 query_arg0=4 query_arg1=0xFFFFFFFF query_arg2=0x2a query_arg3=3 "
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
    EXPECT_EQ(matched.events_with_selector_inputs, 1);
    EXPECT_EQ(matched.selector_model_comparisons, 1);
    EXPECT_EQ(matched.selector_query_args_match, 1);
    EXPECT_EQ(matched.selector_query_args_mismatch, 0);
    EXPECT_EQ(matched.selector_model_missing_expected_query, 0);
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
    ASSERT_TRUE(matched.events[0].query_arg1.has_value());
    EXPECT_EQ(*matched.events[0].query_arg1, -1);
    ASSERT_TRUE(matched.events[0].selector_expected_query.has_value());
    EXPECT_EQ(matched.events[0].selector_expected_query->action_key, 4);
    ASSERT_TRUE(matched.events[0].selector_query_args_match.has_value());
    EXPECT_TRUE(*matched.events[0].selector_query_args_match);
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

    std::istringstream split_input(
        "pc=8001331c function=FUN_80012f58 checkpoint=action_view_query rng_draw_index_before=18 "
        "action_sequence_id=7 active_slot=1 target_slot=4 actor_field6_0x6=4 actor_subtype_0x8=0 "
        "gate_category_0x2f=2 gate_state_0x30=2 gate_active_slot_0x02=1 "
        "instruction_flags_0xf0=0x00000000 aux_list_root=0x80346bd8 "
        "query_arg0=4 query_arg1=-1 query_arg2=0x2a query_arg3=3 "
        "aux_row00_location_code=0x2a aux_row00_opcode=3 "
        "aux_row00_payload_primary=4 aux_row00_payload_secondary=0 "
        "aux_row00_payload_direct_secondary=0 "
        "aux_row01_location_code=0xffff aux_row01_opcode=0\n"
        "pc=80013320 function=FUN_80012f58 checkpoint=action_view_query_result rng_draw_index_before=18 "
        "action_sequence_id=7 active_slot=1 target_slot=4 actor_field6_0x6=4 actor_subtype_0x8=0 "
        "gate_category_0x2f=2 gate_state_0x30=2 gate_active_slot_0x02=1 "
        "instruction_flags_0xf0=0x00000000 query_result=1\n");
    const auto split_parsed = parse_checkpoint_stream(split_input);
    ASSERT_TRUE(split_parsed.errors.empty());
    const auto split = summarize_action_view_gate_checkpoints(split_parsed.events);
    EXPECT_EQ(split.status, ActionViewGateCheckpointStatus::MatchesExpected);
    EXPECT_EQ(split.observed_gate_events, 2);
    EXPECT_EQ(split.legacy_gate_events, 0);
    EXPECT_EQ(split.query_call_events, 1);
    EXPECT_EQ(split.query_result_events, 1);
    EXPECT_EQ(split.query_call_events_with_query_args, 1);
    EXPECT_EQ(split.query_result_events_with_query_result, 1);
    EXPECT_EQ(split.events_with_selector_inputs, 2);
    EXPECT_EQ(split.selector_model_comparisons, 1);
    EXPECT_EQ(split.selector_query_args_match, 1);
    EXPECT_EQ(split.selector_model_missing_expected_query, 0);
    EXPECT_EQ(split.events_with_aux_table_fingerprint, 1);
    EXPECT_EQ(split.events_with_aux_table_count, 1);
    EXPECT_EQ(split.aux_table_count_matches_query_result, 1);
    EXPECT_EQ(split.aux_table_count_mismatches_query_result, 0);
    ASSERT_EQ(split.events.size(), 2u);
    ASSERT_TRUE(split.events[0].sampled_aux_table_count.has_value());
    EXPECT_EQ(*split.events[0].sampled_aux_table_count, 1);
    ASSERT_TRUE(split.events[0].matched_query_result_count.has_value());
    EXPECT_EQ(*split.events[0].matched_query_result_count, 1);
    ASSERT_TRUE(split.events[0].sampled_aux_table_count_matches_query_result.has_value());
    EXPECT_TRUE(*split.events[0].sampled_aux_table_count_matches_query_result);

    std::ostringstream row12_input_text;
    row12_input_text
        << "pc=800133cc function=FUN_80012f58 checkpoint=action_view_query rng_draw_index_before=18 "
        << "action_sequence_id=7 active_slot=1 target_slot=4 actor_field6_0x6=5 actor_subtype_0x8=0 "
        << "gate_category_0x2f=3 gate_state_0x30=2 gate_active_slot_0x02=1 "
        << "instruction_flags_0xf0=0x00000000 aux_list_root=0x80346bd8 "
        << "query_arg0=5 query_arg1=-1 query_arg2=0x2a query_arg3=3 ";
    for (int row = 0; row <= 13; ++row) {
        row12_input_text
            << "aux_row" << std::setw(2) << std::setfill('0') << row
            << "_location_code=" << (row == 13 ? "0xffff" : (row == 12 ? "0x2a" : "0x01")) << " ";
        row12_input_text
            << "aux_row" << std::setw(2) << std::setfill('0') << row
            << "_opcode=3 ";
        row12_input_text
            << "aux_row" << std::setw(2) << std::setfill('0') << row
            << "_payload_primary=" << (row == 12 ? "5" : "4") << " ";
        row12_input_text
            << "aux_row" << std::setw(2) << std::setfill('0') << row
            << "_payload_secondary=0 ";
        row12_input_text
            << "aux_row" << std::setw(2) << std::setfill('0') << row
            << "_payload_direct_secondary=0 ";
    }
    row12_input_text
        << "\n"
        << "pc=800133d0 function=FUN_80012f58 checkpoint=action_view_query_result rng_draw_index_before=18 "
        << "action_sequence_id=7 active_slot=1 target_slot=4 actor_field6_0x6=5 actor_subtype_0x8=0 "
        << "gate_category_0x2f=3 gate_state_0x30=2 gate_active_slot_0x02=1 "
        << "instruction_flags_0xf0=0x00000000 query_result=1\n";
    std::istringstream row12_input(row12_input_text.str());
    const auto row12_parsed = parse_checkpoint_stream(row12_input);
    ASSERT_TRUE(row12_parsed.errors.empty());
    const auto row12_summary = summarize_action_view_gate_checkpoints(row12_parsed.events);
    EXPECT_EQ(row12_summary.status, ActionViewGateCheckpointStatus::MatchesExpected);
    ASSERT_EQ(row12_summary.events.size(), 2u);
    ASSERT_TRUE(row12_summary.events[0].sampled_aux_table_count.has_value());
    EXPECT_EQ(*row12_summary.events[0].sampled_aux_table_count, 1);
    ASSERT_TRUE(row12_summary.events[0].matched_query_result_count.has_value());
    EXPECT_EQ(*row12_summary.events[0].matched_query_result_count, 1);
    ASSERT_TRUE(row12_summary.events[0].sampled_aux_table_count_matches_query_result.has_value());
    EXPECT_TRUE(*row12_summary.events[0].sampled_aux_table_count_matches_query_result);

    std::istringstream query_mismatch_input(
        "pc=80012f58 function=FUN_80012f58 checkpoint=action_view_gate rng_draw_index_before=18 "
        "aux_list_root=0x80346bd8 query_arg0=4 query_arg1=-1 query_arg2=0x2b query_arg3=3 "
        "query_result=0x81234567 selected_record_mode=0\n");
    const auto query_mismatch_parsed = parse_checkpoint_stream(query_mismatch_input);
    ASSERT_TRUE(query_mismatch_parsed.errors.empty());
    const auto query_mismatch = summarize_action_view_gate_checkpoints(query_mismatch_parsed.events);
    EXPECT_EQ(query_mismatch.status, ActionViewGateCheckpointStatus::QueryArgsMismatch);

    std::istringstream selector_mismatch_input(
        "pc=80012f58 function=FUN_80012f58 checkpoint=action_view_gate rng_draw_index_before=18 "
        "action_sequence_id=7 active_slot=1 target_slot=4 actor_field6_0x6=5 actor_subtype_0x8=0 "
        "gate_category_0x2f=3 gate_state_0x30=2 gate_active_slot_0x02=1 "
        "instruction_flags_0xf0=0x00000000 aux_list_root=0x80346bd8 "
        "query_arg0=4 query_arg1=-1 query_arg2=0x2a query_arg3=3 "
        "query_result=1 selected_record_mode=0 "
        "action_child_thread=0x81230000 child_payload=0x81231000 nested_payload=0x81232000 "
        "child_thread_state_byte=1 mode0_fallback_reached=0\n");
    const auto selector_mismatch_parsed = parse_checkpoint_stream(selector_mismatch_input);
    ASSERT_TRUE(selector_mismatch_parsed.errors.empty());
    const auto selector_mismatch =
        summarize_action_view_gate_checkpoints(selector_mismatch_parsed.events);
    EXPECT_EQ(selector_mismatch.status, ActionViewGateCheckpointStatus::SelectorModelMismatch);
    EXPECT_EQ(selector_mismatch.selector_model_comparisons, 1);
    EXPECT_EQ(selector_mismatch.selector_query_args_mismatch, 1);
    ASSERT_EQ(selector_mismatch.events.size(), 1u);
    ASSERT_TRUE(selector_mismatch.events[0].selector_expected_query.has_value());
    EXPECT_EQ(selector_mismatch.events[0].selector_expected_query->action_key, 5);

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

TEST(SavorPredictCheckpointTrace, IdentifiesSampledActionViewAuxTableFromStdJsonDirectory) {
    const auto temp_dir =
        std::filesystem::temp_directory_path() / "savor_predict_trace_std_identity_test";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto write_table = [&](std::string_view filename, int action_key) {
        std::ofstream file(temp_dir / std::string(filename));
        ASSERT_TRUE(file.good());
        file << R"json({
  "schema": "spice_std_ir_v1",
  "layoutKind": "entry_table",
  "parseOk": true,
  "entryTable": {
    "records": [
      {
        "index": 0,
        "isSentinel": false,
        "locationCode": 42,
        "opcode": 3,
        "payloadInBounds": true,
        "payloadBytesHex": ")json"
             << std::hex << std::setw(4) << std::setfill('0') << action_key
             << R"json(00000000"
      },
      {
        "index": 1,
        "isSentinel": true,
        "locationCode": -1,
        "opcode": 0,
        "payloadInBounds": false,
        "payloadBytesHex": ""
      }
    ]
  }
})json";
    };
    write_table("ma0000.std.json", 4);
    write_table("ma0010.std.json", 5);
    write_table("mb0000.std.json", 8);

    std::istringstream input(
        "pc=8001331c function=FUN_80012f58 checkpoint=action_view_query rng_draw_index_before=18 "
        "action_sequence_id=7 active_slot=0 target_slot=4 actor_field6_0x6=4 actor_subtype_0x8=0 "
          "gate_category_0x2f=2 gate_state_0x30=2 gate_active_slot_0x02=0 "
          "instruction_flags_0xf0=0x00000000 aux_list_root=0x80346bd8 "
          "query_arg0=4 query_arg1=-1 query_arg2=0x2a query_arg3=3 "
          "aux_row00_location_code=0 aux_row00_opcode=1 "
          "aux_row00_payload_primary=2 aux_row00_payload_secondary=1 "
          "aux_row00_payload_direct_secondary=8 "
          "aux_row01_location_code=0x2a aux_row01_opcode=3 "
          "aux_row01_payload_primary=4 aux_row01_payload_secondary=0 "
          "aux_row01_payload_direct_secondary=0 "
          "aux_row02_location_code=0xffff aux_row02_opcode=0\n"
        "pc=80013320 function=FUN_80012f58 checkpoint=action_view_query_result rng_draw_index_before=18 "
        "action_sequence_id=7 active_slot=0 target_slot=4 actor_field6_0x6=4 actor_subtype_0x8=0 "
        "gate_category_0x2f=2 gate_state_0x30=2 gate_active_slot_0x02=0 "
        "instruction_flags_0xf0=0x00000000 query_result=1\n");
    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());
    const auto summary = summarize_action_view_gate_checkpoints(
        parsed.events,
        ActionViewGateCheckpointOptions{.action_view_std_json_dir = temp_dir});

    EXPECT_EQ(summary.status, ActionViewGateCheckpointStatus::MatchesExpected);
    EXPECT_EQ(summary.aux_table_fingerprint_matches_known_std0, 1);
    EXPECT_EQ(summary.aux_table_fingerprint_ambiguous_known_std0, 0);
    EXPECT_EQ(summary.aux_table_fingerprint_matches_actor_slot_std0, 1);
    EXPECT_EQ(summary.aux_table_fingerprint_mismatches_actor_slot_std0, 0);
    ASSERT_EQ(summary.events.size(), 2u);
    EXPECT_EQ(summary.events[0].matched_std0_candidate_count, 1);
    ASSERT_TRUE(summary.events[0].matched_resource_stem.has_value());
    EXPECT_EQ(*summary.events[0].matched_resource_stem, "ma000");
    ASSERT_TRUE(summary.events[0].matched_std_filename.has_value());
    EXPECT_EQ(*summary.events[0].matched_std_filename, "ma000.std");
      ASSERT_TRUE(summary.events[0].matched_std0_filename.has_value());
      EXPECT_EQ(*summary.events[0].matched_std0_filename, "ma0000.std");
      ASSERT_TRUE(summary.events[0].matched_std0_sample_row_offset.has_value());
      EXPECT_EQ(*summary.events[0].matched_std0_sample_row_offset, 1);
      ASSERT_TRUE(summary.events[0].actor_slot_expected_std0_filename.has_value());
      EXPECT_EQ(*summary.events[0].actor_slot_expected_std0_filename, "ma0000.std");
      ASSERT_TRUE(summary.events[0].actor_slot_expected_std0_matches_sample.has_value());
      EXPECT_TRUE(*summary.events[0].actor_slot_expected_std0_matches_sample);
      ASSERT_TRUE(summary.events[0].actor_slot_expected_std0_sample_row_offset.has_value());
      EXPECT_EQ(*summary.events[0].actor_slot_expected_std0_sample_row_offset, 1);

    std::filesystem::remove_all(temp_dir);
}

TEST(SavorPredictCheckpointTrace, ComparesActionViewHelperCallAgainstSelectorPrediction) {
    std::istringstream input(
        "pc=80013334 function=FUN_80053f38 checkpoint=action_view_spawn rng_draw_index_before=18 "
        "action_sequence_id=7 active_slot=5 target_slot=0 actor_field6_0x6=4 actor_subtype_0x8=0 "
        "gate_category_0x2f=2 gate_state_0x30=2 gate_active_slot_0x02=5 "
        "instruction_flags_0xf0=0x00000000 spawn_slot_arg=5 spawn_mode_arg=0 "
        "aux_row00_location_code=0xffff aux_row00_opcode=0\n");
    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());

    const auto summary = summarize_action_view_gate_checkpoints(parsed.events);

    EXPECT_EQ(summary.status, ActionViewGateCheckpointStatus::MatchesExpected);
    EXPECT_EQ(summary.observed_gate_events, 0);
    EXPECT_EQ(summary.observed_helper_call_events, 1);
    EXPECT_EQ(summary.helper_call_events_with_selector_inputs, 1);
    EXPECT_EQ(summary.selector_helper_call_comparisons, 1);
    EXPECT_EQ(summary.selector_helper_call_matches, 1);
    EXPECT_EQ(summary.selector_helper_call_mismatches, 0);
    ASSERT_EQ(summary.events.size(), 1u);
    const auto& event = summary.events.front();
    EXPECT_TRUE(event.helper_call_event);
    ASSERT_TRUE(event.helper_call_site_pc.has_value());
    EXPECT_EQ(*event.helper_call_site_pc, 0x80013334u);
    ASSERT_TRUE(event.selector_expected_helper_role.has_value());
    EXPECT_EQ(*event.selector_expected_helper_role, "mode2_count_zero_spawn_mode0");
    ASSERT_TRUE(event.selector_expected_helper_mode_arg.has_value());
    EXPECT_EQ(*event.selector_expected_helper_mode_arg, 0);
    ASSERT_TRUE(event.selector_expected_spawned_record_mode.has_value());
    EXPECT_EQ(*event.selector_expected_spawned_record_mode, 0xe);
    ASSERT_TRUE(event.selector_helper_call_matches.has_value());
    EXPECT_TRUE(*event.selector_helper_call_matches);
}

TEST(SavorPredictCheckpointTrace, FlagsActionViewHelperCallModeMismatch) {
    std::istringstream input(
        "pc=80013334 function=FUN_80053f38 checkpoint=action_view_spawn rng_draw_index_before=18 "
        "action_sequence_id=7 active_slot=5 target_slot=0 actor_field6_0x6=4 actor_subtype_0x8=0 "
        "gate_category_0x2f=2 gate_state_0x30=2 gate_active_slot_0x02=5 "
        "instruction_flags_0xf0=0x00000000 spawn_slot_arg=5 spawn_mode_arg=1 "
        "aux_row00_location_code=0xffff aux_row00_opcode=0\n");
    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());

    const auto summary = summarize_action_view_gate_checkpoints(parsed.events);

    EXPECT_EQ(summary.status, ActionViewGateCheckpointStatus::SelectorModelMismatch);
    EXPECT_EQ(summary.observed_helper_call_events, 1);
    EXPECT_EQ(summary.selector_helper_call_comparisons, 1);
    EXPECT_EQ(summary.selector_helper_call_matches, 0);
    EXPECT_EQ(summary.selector_helper_call_mismatches, 1);
    ASSERT_EQ(summary.events.size(), 1u);
    const auto& event = summary.events.front();
    ASSERT_TRUE(event.helper_mode_arg.has_value());
    EXPECT_EQ(*event.helper_mode_arg, 1);
    ASSERT_TRUE(event.selector_expected_helper_mode_arg.has_value());
    EXPECT_EQ(*event.selector_expected_helper_mode_arg, 0);
    ASSERT_TRUE(event.selector_helper_call_matches.has_value());
    EXPECT_FALSE(*event.selector_helper_call_matches);
}

TEST(SavorPredictCheckpointTrace, LinksActionViewAuxRootToStd0CacheResource) {
    std::istringstream input(
        "pc=8006df8c function=Battle::Resource::ProcessQueuedBattleResourceFile_8006ddbc "
        "checkpoint=std0_cache_producer_materialize capture_sequence=1 rng_draw_index_before=0 "
        "loaded_file_ptr_arg=0x81230000\n"
        "pc=8006dfa4 function=Battle::Resource::ProcessQueuedBattleResourceFile_8006ddbc "
        "checkpoint=std0_cache_producer_table_store capture_sequence=2 rng_draw_index_before=0 "
        "materialized_table_ptr=0x81240000\n"
        "pc=8006dfc4 function=Battle::Resource::ProcessQueuedBattleResourceFile_8006ddbc "
        "checkpoint=std0_cache_producer_key_store capture_sequence=3 rng_draw_index_before=0 "
        "filename_key=0x00989680\n"
        "pc=80035d80 function=STD::LoadStd0EntryTable_80035d4c "
        "checkpoint=std0_cache_lookup capture_sequence=4 rng_draw_index_before=0 "
        "root_field_ptr=0x81230030 cache_key_expected=0x00989680 cache_slot_index=0\n"
        "pc=80035da8 function=STD::LoadStd0EntryTable_80035d4c "
        "checkpoint=std0_cache_table_read capture_sequence=5 rng_draw_index_before=0 "
        "root_field_ptr=0x81230030 cache_slot_index=0\n"
        "pc=80035e0c function=STD::LoadStd0EntryTable_80035d4c "
        "checkpoint=std0_cache_result_store capture_sequence=6 rng_draw_index_before=0 "
        "root_field_ptr=0x81230030 cached_table_ptr=0x81240000\n"
        "pc=8001331c function=FUN_80012f58 checkpoint=action_view_query "
        "capture_sequence=7 rng_draw_index_before=18 active_slot=0 target_slot=4 "
        "actor_field6_0x6=4 actor_subtype_0x8=0 gate_category_0x2f=2 "
        "gate_state_0x30=2 gate_active_slot_0x02=0 instruction_flags_0xf0=0x00000000 "
        "aux_list_root=0x81240000 query_arg0=4 query_arg1=-1 query_arg2=0x2a query_arg3=3 "
        "action_view_chain_payload_0x24=0x8122ff00 "
        "action_view_chain_loaded_resource_0x10=0x81230000 "
        "action_view_chain_aux_root_0x30=0x81240000 "
        "aux_row00_location_code=0x2a aux_row00_opcode=3 "
        "aux_row00_payload_primary=4 aux_row00_payload_secondary=0 "
        "aux_row00_payload_direct_secondary=0 "
        "aux_row01_location_code=0xffff aux_row01_opcode=0\n"
        "pc=80013320 function=FUN_80012f58 checkpoint=action_view_query_result "
        "capture_sequence=8 rng_draw_index_before=18 active_slot=0 target_slot=4 "
        "actor_field6_0x6=4 actor_subtype_0x8=0 gate_category_0x2f=2 "
        "gate_state_0x30=2 gate_active_slot_0x02=0 instruction_flags_0xf0=0x00000000 "
        "query_result=1\n");

    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());
    const auto gate_summary = summarize_action_view_gate_checkpoints(parsed.events);
    const auto resource_summary = summarize_action_view_resource_checkpoints(
        parsed.events,
        gate_summary);

    EXPECT_EQ(
        resource_summary.status,
        ActionViewResourceCheckpointStatus::MatchesSelectorRoots);
    EXPECT_EQ(resource_summary.complete_cache_producers, 1);
    EXPECT_EQ(resource_summary.complete_cache_hits, 1);
    EXPECT_EQ(resource_summary.selector_aux_roots, 1);
    EXPECT_EQ(resource_summary.selector_aux_roots_linked_to_cache_hits, 1);
    EXPECT_EQ(resource_summary.selector_aux_roots_linked_to_cache_producers, 1);
    EXPECT_EQ(resource_summary.selector_aux_roots_with_chain_samples, 1);
    EXPECT_EQ(resource_summary.selector_aux_roots_with_matching_chain, 1);
    EXPECT_EQ(resource_summary.selector_aux_roots_with_mismatching_chain, 0);
    EXPECT_EQ(resource_summary.selector_aux_roots_with_loaded_resource_root_field_match, 1);
    ASSERT_EQ(resource_summary.selector_root_links.size(), 1u);
    const auto& link = resource_summary.selector_root_links[0];
    ASSERT_TRUE(link.capture_sequence.has_value());
    EXPECT_EQ(*link.capture_sequence, 7);
    ASSERT_TRUE(link.cache_expected_key.has_value());
    EXPECT_EQ(*link.cache_expected_key, "0x00989680");
    ASSERT_TRUE(link.cache_slot.has_value());
    EXPECT_EQ(*link.cache_slot, 0);
    ASSERT_TRUE(link.cache_root_field_ptr.has_value());
    EXPECT_EQ(*link.cache_root_field_ptr, "0x81230030");
    ASSERT_TRUE(link.chain_aux_root_matches_query.has_value());
    EXPECT_TRUE(*link.chain_aux_root_matches_query);
    ASSERT_TRUE(link.cache_root_field_matches_loaded_resource_plus_0x30.has_value());
    EXPECT_TRUE(*link.cache_root_field_matches_loaded_resource_plus_0x30);
    ASSERT_TRUE(link.producer_loaded_file_ptr.has_value());
    EXPECT_EQ(*link.producer_loaded_file_ptr, "0x81230000");
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

TEST(SavorPredictCheckpointTrace, DoesNotCountDiagnosticKnownPcRowsAsRngOwners) {
    std::istringstream input(
        "pc=800513d4 function=memory_watchpoint_delta checkpoint=unattributed_delta "
        "rng_draw_index_before=127 owns_rng_draw=false\n"
        "pc=800513d4 function=memory_watchpoint_delta checkpoint=unattributed_delta "
        "rng_draw_index_before=127 owns_rng_draw=false\n"
        "pc=800513d4 function=UpdateActionViewRecord checkpoint=mode0_fallback "
        "rng_draw_index_before=127 owns_rng_draw=true "
        "rng_seed_before=0xAABBCCDD rng_seed_after=0x01020304\n");

    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.events.size(), 3u);
    EXPECT_TRUE(parsed.events[0].known_rng_owner.empty());
    EXPECT_TRUE(parsed.events[1].known_rng_owner.empty());
    EXPECT_EQ(parsed.events[2].known_rng_owner, "mode0_action_view_camera_fallback");

    const auto checkpoint_path =
        std::filesystem::temp_directory_path() / "savor_predict_trace_owner_override_test.txt";
    {
        std::ofstream file(checkpoint_path);
        ASSERT_TRUE(file.good());
        file
            << "pc=800513d4 function=memory_watchpoint_delta checkpoint=unattributed_delta "
               "rng_draw_index_before=127 owns_rng_draw=false\n"
            << "pc=800513d4 function=memory_watchpoint_delta checkpoint=unattributed_delta "
               "rng_draw_index_before=127 owns_rng_draw=false\n"
            << "pc=800513d4 function=UpdateActionViewRecord checkpoint=mode0_fallback "
               "rng_draw_index_before=127 owns_rng_draw=true "
               "rng_seed_before=0xAABBCCDD rng_seed_after=0x01020304\n";
    }

    TraceCheckpointsOptions options{};
    options.checkpoint_file = checkpoint_path;

    std::ostringstream out;
    std::ostringstream err;
    const int rc = run_trace_checkpoints(options, out, err);
    std::filesystem::remove(checkpoint_path);

    EXPECT_EQ(rc, 0) << err.str();
    EXPECT_NE(out.str().find("mode0_action_view_camera_fallback: 1"), std::string::npos);
    EXPECT_EQ(out.str().find("mode0_action_view_camera_fallback: 3"), std::string::npos);
}

TEST(SavorPredictCheckpointTrace, ReportsLiveAttackParamComparisonChains) {
    const auto checkpoint_path =
        std::filesystem::temp_directory_path() / "savor_predict_trace_attack_param_chain_test.txt";
    {
        std::ofstream file(checkpoint_path);
        ASSERT_TRUE(file.good());
        file
            << "pc=80081b94 function=Battle::AtkMethods::performAttack_80081b94 "
               "checkpoint=attack_begin rng_draw_index_before=13 "
               "actor_slot_arg=1 target_slot_arg=4 "
               "slot1_instr_param_0x6=0\n"
            << "pc=800856c4 function=FUN_800855ac checkpoint=instr_param_set "
               "rng_draw_index_before=13 actor_slot=1 target_slot=4 "
               "instr_param_0x6=1\n"
            << "pc=80085ce0 function=FUN_80085ce0 checkpoint=worker_select "
               "rng_draw_index_before=13 actor_slot=1 target_slot=4 "
               "instr_param_0x6=1\n"
            << "pc=80010bdc function=getAttackResult checkpoint=hit "
               "rng_draw_index_before=14 active_slot=1 target_slot=4 "
               "instr_param_0x6=1 attack_result=2 rand_value=30\n";
    }

    TraceCheckpointsOptions options{};
    options.checkpoint_file = checkpoint_path;

    std::ostringstream out;
    std::ostringstream err;
    const int rc = run_trace_checkpoints(options, out, err);
    std::filesystem::remove(checkpoint_path);

    EXPECT_EQ(rc, 0) << err.str();
    const auto text = out.str();
    EXPECT_NE(text.find("Predictor/live attack parameter comparison"), std::string::npos);
    EXPECT_NE(text.find("live_chains: 1"), std::string::npos);
    EXPECT_NE(text.find("predictor_status: not_requested"), std::string::npos);
    EXPECT_NE(text.find("live_setpoint_pc=80085CE0"), std::string::npos);
    EXPECT_NE(text.find("live_consumer_instr_param_0x6=1"), std::string::npos);
    EXPECT_NE(text.find("live_worker=PcFallbackAttack_80085ce0"), std::string::npos);
    EXPECT_NE(text.find("crit_draw_observed=false"), std::string::npos);
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
        "pc=8007140c function=setupTurn checkpoint=qsort_output rng_draw_index_before=20 "
        "queue_index=0 slot=4 assigned_priority=21\n"
        "pc=8007140c function=setupTurn checkpoint=qsort_output rng_draw_index_before=20 "
        "queue_index=1 slot=0 assigned_priority=27\n"
        "pc=8007140c function=setupTurn checkpoint=qsort_output rng_draw_index_before=20 "
        "queue_index=2 slot=1 assigned_priority=30\n"
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
    EXPECT_EQ(matched.observed_qsort_output_entries, 3);
    EXPECT_EQ(matched.observed_execution_order_entries, 3);
    EXPECT_TRUE(matched.qsort_output_compared);
    EXPECT_TRUE(matched.qsort_output_exact);
    EXPECT_EQ(matched.qsort_output_matches, 3);
    EXPECT_EQ(matched.qsort_output_mismatches, 0);
    EXPECT_TRUE(matched.execution_order_compared);
    EXPECT_TRUE(matched.execution_order_exact);
    EXPECT_FALSE(matched.priority_ties_observed);
    EXPECT_EQ(matched.execution_order_matches, 3);
    EXPECT_EQ(matched.execution_order_mismatches, 0);
    ASSERT_EQ(matched.expected_qsort_slots.size(), 3u);
    EXPECT_EQ(matched.expected_qsort_slots[0], 4);
    EXPECT_EQ(matched.expected_qsort_slots[1], 0);
    EXPECT_EQ(matched.expected_qsort_slots[2], 1);
    ASSERT_EQ(matched.observed_qsort_slots.size(), 3u);
    EXPECT_EQ(matched.observed_qsort_slots[0], 4);
    EXPECT_EQ(matched.observed_qsort_slots[1], 0);
    EXPECT_EQ(matched.observed_qsort_slots[2], 1);
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

    std::istringstream sentinel_tail_input(
        "pc=80070c18 function=setupTurn checkpoint=queued_entry rng_draw_index_before=20 "
        "queue_index=0 slot=0 fixed_priority_result=0 assigned_priority=22\n"
        "pc=80070c18 function=setupTurn checkpoint=queued_entry rng_draw_index_before=20 "
        "queue_index=1 slot=1 fixed_priority_result=0 assigned_priority=27\n"
        "pc=80070c18 function=setupTurn checkpoint=queued_entry rng_draw_index_before=20 "
        "queue_index=2 slot=4 fixed_priority_result=0 assigned_priority=21\n"
        "pc=80070c18 function=setupTurn checkpoint=queued_entry rng_draw_index_before=20 "
        "queue_index=3 slot=5 fixed_priority_result=0 assigned_priority=22\n"
        "pc=80070c18 function=setupTurn checkpoint=queued_entry rng_draw_index_before=20 "
        "queue_index=4 slot=255 fixed_priority_result=0 assigned_priority=2147483647\n"
        "pc=80070c18 function=setupTurn checkpoint=queued_entry rng_draw_index_before=20 "
        "queue_index=5 slot=255 fixed_priority_result=0 assigned_priority=2147483647\n"
        "pc=80070c18 function=setupTurn checkpoint=queued_entry rng_draw_index_before=20 "
        "queue_index=6 slot=255 fixed_priority_result=0 assigned_priority=2147483647\n"
        "pc=80070c18 function=setupTurn checkpoint=queued_entry rng_draw_index_before=20 "
        "queue_index=7 slot=255 fixed_priority_result=0 assigned_priority=2147483647\n"
        "pc=8007140c function=setupTurn checkpoint=qsort_output rng_draw_index_before=20 "
        "queue_index=0 slot=4 assigned_priority=21\n"
        "pc=8007140c function=setupTurn checkpoint=qsort_output rng_draw_index_before=20 "
        "queue_index=1 slot=0 assigned_priority=22\n"
        "pc=8007140c function=setupTurn checkpoint=qsort_output rng_draw_index_before=20 "
        "queue_index=2 slot=5 assigned_priority=22\n"
        "pc=8007140c function=setupTurn checkpoint=qsort_output rng_draw_index_before=20 "
        "queue_index=3 slot=1 assigned_priority=27\n"
        "pc=8007140c function=setupTurn checkpoint=qsort_output rng_draw_index_before=20 "
        "queue_index=4 slot=255 assigned_priority=2147483647\n"
        "pc=8007140c function=setupTurn checkpoint=qsort_output rng_draw_index_before=20 "
        "queue_index=5 slot=255 assigned_priority=2147483647\n"
        "pc=8007140c function=setupTurn checkpoint=qsort_output rng_draw_index_before=20 "
        "queue_index=6 slot=255 assigned_priority=2147483647\n"
        "pc=8007140c function=setupTurn checkpoint=qsort_output rng_draw_index_before=20 "
        "queue_index=7 slot=255 assigned_priority=2147483647\n"
        "pc=8007154c function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=0 slot=1\n"
        "pc=8007154c function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=1 slot=5\n"
        "pc=8007154c function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=2 slot=0\n"
        "pc=8007154c function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=3 slot=4\n"
        "pc=8007154c function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=4 slot=255\n"
        "pc=8007154c function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=5 slot=255\n"
        "pc=8007154c function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=6 slot=255\n"
        "pc=8007154c function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=7 slot=255\n");
    const auto sentinel_tail_parsed = parse_checkpoint_stream(sentinel_tail_input);
    ASSERT_TRUE(sentinel_tail_parsed.errors.empty());
    const auto sentinel_tail = summarize_turn_order_checkpoints(sentinel_tail_parsed.events, std::nullopt);
    EXPECT_EQ(sentinel_tail.status, TurnOrderCheckpointStatus::MatchesExpected);
    EXPECT_TRUE(sentinel_tail.qsort_output_exact);
    EXPECT_TRUE(sentinel_tail.execution_order_exact);
    EXPECT_EQ(sentinel_tail.observed_execution_order_entries, 8);
    ASSERT_EQ(sentinel_tail.expected_execution_slots.size(), 4u);
    EXPECT_EQ(sentinel_tail.expected_execution_slots[0], 1);
    EXPECT_EQ(sentinel_tail.expected_execution_slots[1], 5);
    EXPECT_EQ(sentinel_tail.expected_execution_slots[2], 0);
    EXPECT_EQ(sentinel_tail.expected_execution_slots[3], 4);
    ASSERT_EQ(sentinel_tail.observed_execution_slots.size(), 4u);
    EXPECT_EQ(sentinel_tail.observed_execution_slots[0], 1);
    EXPECT_EQ(sentinel_tail.observed_execution_slots[1], 5);
    EXPECT_EQ(sentinel_tail.observed_execution_slots[2], 0);
    EXPECT_EQ(sentinel_tail.observed_execution_slots[3], 4);

    std::istringstream live_tie_433890_input(
        "pc=80071408 function=setupTurn checkpoint=qsort_call rng_draw_index_before=20 "
        "queued_count=3 qsort_elem_size_arg=12 qsort_comparator_arg=123\n"
        "pc=80071408 function=setupTurn checkpoint=qsort_input rng_draw_index_before=20 "
        "queue_index=0 slot=0 assigned_priority=26\n"
        "pc=80071408 function=setupTurn checkpoint=qsort_input rng_draw_index_before=20 "
        "queue_index=1 slot=1 assigned_priority=26\n"
        "pc=80071408 function=setupTurn checkpoint=qsort_input rng_draw_index_before=20 "
        "queue_index=2 slot=4 assigned_priority=28\n"
        "pc=80071408 function=setupTurn checkpoint=qsort_input rng_draw_index_before=20 "
        "queue_index=3 slot=255 assigned_priority=2147483647\n"
        "pc=8007140c function=setupTurn checkpoint=qsort_output rng_draw_index_before=20 "
        "queue_index=0 slot=1 assigned_priority=26\n"
        "pc=8007140c function=setupTurn checkpoint=qsort_output rng_draw_index_before=20 "
        "queue_index=1 slot=0 assigned_priority=26\n"
        "pc=8007140c function=setupTurn checkpoint=qsort_output rng_draw_index_before=20 "
        "queue_index=2 slot=4 assigned_priority=28\n"
        "pc=8007140c function=setupTurn checkpoint=qsort_output rng_draw_index_before=20 "
        "queue_index=3 slot=255 assigned_priority=2147483647\n"
        "pc=8007154c function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=0 slot=4\n"
        "pc=8007154c function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=1 slot=0\n"
        "pc=8007154c function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=2 slot=1\n"
        "pc=8007154c function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=3 slot=255\n");
    const auto live_tie_433890_parsed = parse_checkpoint_stream(live_tie_433890_input);
    ASSERT_TRUE(live_tie_433890_parsed.errors.empty());
    const auto live_tie_433890 = summarize_turn_order_checkpoints(live_tie_433890_parsed.events, std::nullopt);
    EXPECT_EQ(live_tie_433890.status, TurnOrderCheckpointStatus::MatchesExpected);
    ASSERT_TRUE(live_tie_433890.qsort_call_count.has_value());
    EXPECT_EQ(*live_tie_433890.qsort_call_count, 3);
    EXPECT_TRUE(live_tie_433890.qsort_output_exact);
    EXPECT_TRUE(live_tie_433890.execution_order_exact);
    ASSERT_EQ(live_tie_433890.expected_qsort_slots.size(), 4u);
    EXPECT_EQ(live_tie_433890.expected_qsort_slots[0], 1);
    EXPECT_EQ(live_tie_433890.expected_qsort_slots[1], 0);
    EXPECT_EQ(live_tie_433890.expected_qsort_slots[2], 4);
    EXPECT_EQ(live_tie_433890.expected_qsort_slots[3], 255);

    std::istringstream live_tie_433896_input(
        "pc=80071408 function=setupTurn checkpoint=qsort_call rng_draw_index_before=20 "
        "queued_count=3 qsort_elem_size_arg=12 qsort_comparator_arg=123\n"
        "pc=80071408 function=setupTurn checkpoint=qsort_input rng_draw_index_before=20 "
        "queue_index=0 slot=0 assigned_priority=27\n"
        "pc=80071408 function=setupTurn checkpoint=qsort_input rng_draw_index_before=20 "
        "queue_index=1 slot=1 assigned_priority=36\n"
        "pc=80071408 function=setupTurn checkpoint=qsort_input rng_draw_index_before=20 "
        "queue_index=2 slot=5 assigned_priority=27\n"
        "pc=80071408 function=setupTurn checkpoint=qsort_input rng_draw_index_before=20 "
        "queue_index=3 slot=255 assigned_priority=2147483647\n"
        "pc=8007140c function=setupTurn checkpoint=qsort_output rng_draw_index_before=20 "
        "queue_index=0 slot=0 assigned_priority=27\n"
        "pc=8007140c function=setupTurn checkpoint=qsort_output rng_draw_index_before=20 "
        "queue_index=1 slot=5 assigned_priority=27\n"
        "pc=8007140c function=setupTurn checkpoint=qsort_output rng_draw_index_before=20 "
        "queue_index=2 slot=1 assigned_priority=36\n"
        "pc=8007140c function=setupTurn checkpoint=qsort_output rng_draw_index_before=20 "
        "queue_index=3 slot=255 assigned_priority=2147483647\n"
        "pc=8007154c function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=0 slot=1\n"
        "pc=8007154c function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=1 slot=5\n"
        "pc=8007154c function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=2 slot=0\n"
        "pc=8007154c function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=3 slot=255\n");
    const auto live_tie_433896_parsed = parse_checkpoint_stream(live_tie_433896_input);
    ASSERT_TRUE(live_tie_433896_parsed.errors.empty());
    const auto live_tie_433896 = summarize_turn_order_checkpoints(live_tie_433896_parsed.events, std::nullopt);
    EXPECT_EQ(live_tie_433896.status, TurnOrderCheckpointStatus::MatchesExpected);
    ASSERT_TRUE(live_tie_433896.qsort_call_count.has_value());
    EXPECT_EQ(*live_tie_433896.qsort_call_count, 3);
    EXPECT_TRUE(live_tie_433896.qsort_output_exact);
    EXPECT_TRUE(live_tie_433896.execution_order_exact);
    ASSERT_EQ(live_tie_433896.expected_qsort_slots.size(), 4u);
    EXPECT_EQ(live_tie_433896.expected_qsort_slots[0], 0);
    EXPECT_EQ(live_tie_433896.expected_qsort_slots[1], 5);
    EXPECT_EQ(live_tie_433896.expected_qsort_slots[2], 1);
    EXPECT_EQ(live_tie_433896.expected_qsort_slots[3], 255);

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

    std::istringstream qsort_mismatch_input(
        "pc=80070c18 function=setupTurn checkpoint=queued_entry rng_draw_index_before=20 "
        "queue_index=0 slot=0 quick=22 fixed_priority_result=0 assigned_priority=27\n"
        "pc=80070c18 function=setupTurn checkpoint=queued_entry rng_draw_index_before=20 "
        "queue_index=1 slot=1 quick=24 fixed_priority_result=0 assigned_priority=30\n"
        "pc=8007140c function=setupTurn checkpoint=qsort_output rng_draw_index_before=20 "
        "queue_index=0 slot=1 assigned_priority=30\n"
        "pc=8007140c function=setupTurn checkpoint=qsort_output rng_draw_index_before=20 "
        "queue_index=1 slot=0 assigned_priority=27\n");
    const auto qsort_mismatch_parsed = parse_checkpoint_stream(qsort_mismatch_input);
    ASSERT_TRUE(qsort_mismatch_parsed.errors.empty());
    const auto qsort_mismatch = summarize_turn_order_checkpoints(qsort_mismatch_parsed.events, std::nullopt);
    EXPECT_EQ(qsort_mismatch.status, TurnOrderCheckpointStatus::QSortOutputMismatch);
    EXPECT_TRUE(qsort_mismatch.qsort_output_compared);
    EXPECT_FALSE(qsort_mismatch.qsort_output_exact);
    EXPECT_EQ(
        turn_order_checkpoint_status_name(qsort_mismatch.status),
        std::string("QSortOutputMismatch"));

    std::istringstream tied_input(
        "pc=80070c18 function=setupTurn checkpoint=queued_entry rng_draw_index_before=20 "
        "queue_index=0 slot=0 quick=22 fixed_priority_result=0 assigned_priority=27 "
        "rand_value=5 jitter_modulus=10 sum_quick=40 queued_count=2\n"
        "pc=80070c18 function=setupTurn checkpoint=queued_entry rng_draw_index_before=20 "
        "queue_index=1 slot=4 quick=18 fixed_priority_result=0 assigned_priority=27 "
        "rand_value=9 jitter_modulus=10 sum_quick=40 queued_count=2\n"
        "pc=80071280 function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=0 slot=0\n"
        "pc=80071280 function=setupTurn checkpoint=execution_order rng_draw_index_before=20 "
        "execution_index=1 slot=4\n");
    const auto tied_parsed = parse_checkpoint_stream(tied_input);
    ASSERT_TRUE(tied_parsed.errors.empty());
    const auto tied = summarize_turn_order_checkpoints(tied_parsed.events, std::nullopt);
    EXPECT_EQ(tied.status, TurnOrderCheckpointStatus::MatchesExpected);
    EXPECT_TRUE(tied.priority_ties_observed);
    EXPECT_TRUE(tied.execution_order_exact);
    EXPECT_TRUE(tied.execution_order_compared);
    EXPECT_EQ(tied.execution_order_matches, 2);
    EXPECT_EQ(tied.execution_order_mismatches, 0);
    ASSERT_EQ(tied.expected_execution_slots.size(), 2u);
    EXPECT_EQ(tied.expected_execution_slots[0], 0);
    EXPECT_EQ(tied.expected_execution_slots[1], 4);
    EXPECT_EQ(tied.priority_tie_groups, 1);
    EXPECT_EQ(tied.priority_tied_entries, 2);
    EXPECT_EQ(tied.tie_groups_with_observed_execution_order, 1);
    EXPECT_EQ(tied.tie_groups_matching_queue_ascending, 1);
    EXPECT_EQ(tied.tie_groups_matching_queue_descending, 0);
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
    EXPECT_EQ(group.observed_execution_slots[0], 0);
    EXPECT_EQ(group.observed_execution_slots[1], 4);
    EXPECT_TRUE(group.observed_order_compared);
    EXPECT_TRUE(group.observed_order_matches_queue_ascending);
    EXPECT_FALSE(group.observed_order_matches_queue_descending);
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
        "attacker_attack=10 attacker_hit=100 attacker_agile=50 "
        "attacker_element=0 target_defense=5 target_dodge=0 "
        "target_element_effectiveness_tenths=10 target_status_flags=0 instr_param_0x6=0 ";

    std::istringstream input(
        "pc=80081b94 function=Battle::AtkMethods::performAttack_80081b94 "
        "checkpoint=attack_begin rng_draw_index_before=20 "
        "actor_slot_arg=0x00000000 target_slot_arg=0x00000004\n"
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
    EXPECT_EQ(matched.observed_attack_begin_events, 1);
    EXPECT_EQ(matched.attack_begins_with_actor_slot, 1);
    EXPECT_EQ(matched.attack_begins_with_target_slot, 1);
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
    EXPECT_EQ(matched.attacks[0].active_slot, std::optional<int>(0));
    EXPECT_EQ(matched.attacks[0].target_slot, std::optional<int>(4));
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

    std::istringstream split_gate_input(
        "pc=800819d0 function=shouldCounter_800819d0 checkpoint=counter_gate "
        "rng_draw_index_before=40 attacker_slot=1 target_slot=4\n"
        "pc=800819fc function=shouldCounter_800819d0 checkpoint=counter_gate_inputs "
        "rng_draw_index_before=40 attacker_slot=1 target_slot=4 "
        "target_status_flags=0x0 target_movement_flags=0xc0 "
        "target_base_counter_chance=10 target_current_counter_chance=10 "
        "slot1_action_marker_0x0=0 slot4_critical_marker_0x8=0\n"
        "pc=80081a88 function=shouldCounter_800819d0 checkpoint=counter_roll "
        "rng_draw_index_before=40 counter_rand=7\n"
        "pc=80081b80 function=shouldCounter_800819d0 checkpoint=counter_gate_result "
        "rng_draw_index_before=40 counter_result=1 slot4_attack_result_0xc=0 "
        "target_current_counter_chance=0\n"
        "pc=80081d80 function=performAttack checkpoint=counter_follow_up "
        "rng_draw_index_before=40 attacker_slot=1 target_slot=4 counter_follow_up=1\n");
    const auto split_gate_parsed = parse_checkpoint_stream(split_gate_input);
    ASSERT_TRUE(split_gate_parsed.errors.empty());
    const auto split_gate = summarize_counter_checkpoints(split_gate_parsed.events, std::nullopt);
    EXPECT_EQ(split_gate.status, CounterCheckpointStatus::MatchesLiveGate);
    EXPECT_EQ(split_gate.observed_counter_gate_attempts, 1);
    EXPECT_EQ(split_gate.observed_counter_rolls, 1);
    EXPECT_EQ(split_gate.observed_counter_follow_up_events, 1);
    EXPECT_EQ(split_gate.expected_counter_follow_up_events, 1);
    ASSERT_EQ(split_gate.draws.size(), 3u);
    EXPECT_EQ(split_gate.draws[0].kind, CounterCheckpointKind::GateAttempt);
    EXPECT_EQ(split_gate.draws[1].kind, CounterCheckpointKind::CounterRoll);
    ASSERT_TRUE(split_gate.draws[1].counter_result.has_value());
    EXPECT_EQ(*split_gate.draws[1].counter_result, 1);
    ASSERT_TRUE(split_gate.draws[1].updated_current_counter_chance.has_value());
    EXPECT_EQ(*split_gate.draws[1].updated_current_counter_chance, 0);
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
        "drop_threshold=1 drop_amount=1 rand_value=44 rand_mod100=44 drop_success=0\n"
        "pc=80081a88 function=shouldCounter checkpoint=roll rng_draw_index_before=25\n"
        "pc=80010984 function=rollDamage checkpoint=bonus rng_draw_index_before=26\n"
        "pc=8002bad8 function=enemyDropItem checkpoint=row rng_draw_index_before=27 "
        "target_slot=4 enemy_entry_id=0 drop_row_index=2 drop_item_id=258 "
        "drop_threshold=1 drop_amount=1 rand_value=0 rand_mod100=0 drop_success=1\n");

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

    std::istringstream live_fields_input(
        "pc=8002bad8 function=enemyDropItem checkpoint=row rng_draw_index_before=24 "
        "rng_seed_before=0x00000000 target_slot=4 enemy_id_slot4=0 "
        "drop_row_index_zero_based=0 drop_threshold=1 drop_item_id=273 drop_amount=1\n");
    const auto live_fields_parsed = parse_checkpoint_stream(live_fields_input);
    ASSERT_TRUE(live_fields_parsed.errors.empty());
    const auto live_fields =
        summarize_drop_checkpoints(live_fields_parsed.events, std::nullopt);
    EXPECT_EQ(live_fields.status, DropCheckpointStatus::MatchesExpected);
    EXPECT_EQ(live_fields.drop_rolls_with_live_outcome_fields, 1);
    EXPECT_EQ(live_fields.draws_with_rand_value, 1);
    EXPECT_EQ(live_fields.successful_drop_rolls, 1);
    ASSERT_EQ(live_fields.draws.size(), 1u);
    EXPECT_EQ(live_fields.draws[0].enemy_entry_id, std::optional<int>(0));
    EXPECT_EQ(live_fields.draws[0].drop_row_index, std::optional<int>(1));
    EXPECT_EQ(live_fields.draws[0].drop_success, std::optional<int>(1));
    EXPECT_EQ(live_fields.draws[0].expected_drop_success, std::optional<int>(1));

    std::istringstream outcome_mismatch_input(
        "pc=8002bad8 function=enemyDropItem checkpoint=row rng_draw_index_before=24 "
        "target_slot=4 enemy_entry_id=0 drop_row_index=1 drop_item_id=273 "
        "drop_threshold=1 drop_amount=1 rand_value=44 rand_mod100=44 drop_success=1\n");
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
        "drop_threshold=1 drop_amount=1 rand_value=44 rand_mod100=44 drop_success=0\n");
    const auto table_mismatch_parsed = parse_checkpoint_stream(table_mismatch_input);
    ASSERT_TRUE(table_mismatch_parsed.errors.empty());
    const auto table_mismatch =
        summarize_drop_checkpoints(table_mismatch_parsed.events, std::nullopt);
    EXPECT_EQ(table_mismatch.status, DropCheckpointStatus::DropTableMismatch);
    EXPECT_EQ(table_mismatch.drop_table_mismatches, 1);

    std::istringstream disabled_row_input(
        "pc=8002bad8 function=enemyDropItem checkpoint=row rng_draw_index_before=24 "
        "target_slot=4 enemy_entry_id=0 drop_row_index=3 drop_item_id=0 "
        "drop_threshold=0 drop_amount=0 rand_value=44 rand_mod100=44 drop_success=0\n");
    const auto disabled_row_parsed = parse_checkpoint_stream(disabled_row_input);
    ASSERT_TRUE(disabled_row_parsed.errors.empty());
    const auto disabled_row =
        summarize_drop_checkpoints(disabled_row_parsed.events, std::nullopt);
    EXPECT_EQ(disabled_row.status, DropCheckpointStatus::DropTableMismatch);
    EXPECT_EQ(disabled_row.disabled_first_battle_rows_observed, 1);

    std::istringstream continuation_input(
        "pc=8002bad8 function=enemyDropItem checkpoint=row rng_draw_index_before=24 "
        "target_slot=4 enemy_entry_id=0 drop_row_index=1 drop_item_id=273 "
        "drop_threshold=1 drop_amount=1 rand_value=0 rand_mod100=0 drop_success=1\n"
        "pc=8002bad8 function=enemyDropItem checkpoint=row rng_draw_index_before=25 "
        "target_slot=4 enemy_entry_id=0 drop_row_index=2 drop_item_id=258 "
        "drop_threshold=1 drop_amount=1 rand_value=44 rand_mod100=44 drop_success=0\n");
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
        "pc=8002bc80 function=HandleCombatantDeath checkpoint=death_handler_gate rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 cur_hp=0\n"
        "pc=8002bd20 function=HandleCombatantDeath checkpoint=enemy_drop_call rng_draw_index_before=22 "
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
    EXPECT_EQ(lethal.observed_drop_path_events, 1);
    EXPECT_EQ(lethal.observed_drop_entry_events, 1);
    EXPECT_EQ(lethal.observed_drop_rolls, 1);
    EXPECT_EQ(lethal.damage_events_with_live_death_fields, 1);
    EXPECT_EQ(lethal.lethal_damage_events, 1);
    EXPECT_EQ(lethal.damage_events_with_death_handler, 1);
    EXPECT_EQ(lethal.lethal_events_with_drop_path, 1);
    EXPECT_EQ(lethal.lethal_events_with_drop_entry, 1);
    EXPECT_EQ(lethal.lethal_events_with_drop_roll, 1);
    EXPECT_EQ(lethal.drop_rolls_after_drop_entry, 1);
    ASSERT_EQ(lethal.damage_flows.size(), 1u);
    EXPECT_TRUE(lethal.damage_flows[0].observed_death_handler);
    EXPECT_TRUE(lethal.damage_flows[0].observed_drop_path);
    EXPECT_TRUE(lethal.damage_flows[0].observed_drop_entry);

    std::istringstream legacy_drop_path_input(
        "pc=8002dd14 function=zzDealDamage checkpoint=damage_apply rng_draw_index_before=22 "
        "attacker_slot=0 target_slot=4 enemy_entry_id=0 damage=18 hp_before=18 hp_after=0\n"
        "pc=8002bd20 function=HandleCombatantDeath checkpoint=death_handler rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 cur_hp=0 entered_enemy_reward=1 called_enemy_drop=1\n"
        "pc=8002ba8c function=enemyDropItem checkpoint=enemy_drop_entry rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0\n"
        "pc=8002bad8 function=enemyDropItem checkpoint=row rng_draw_index_before=23 "
        "target_slot=4 enemy_entry_id=0 drop_row_index=1 rand_value=0 rand_mod100=0 "
        "drop_success=1\n");
    const auto legacy_drop_path_parsed = parse_checkpoint_stream(legacy_drop_path_input);
    ASSERT_TRUE(legacy_drop_path_parsed.errors.empty());
    const auto legacy_drop_path =
        summarize_death_drop_checkpoints(legacy_drop_path_parsed.events);
    EXPECT_EQ(legacy_drop_path.status, DeathDropCheckpointStatus::MatchesExpectedFlow);
    EXPECT_EQ(legacy_drop_path.observed_death_handler_events, 0);
    EXPECT_EQ(legacy_drop_path.observed_drop_path_events, 1);
    EXPECT_EQ(legacy_drop_path.damage_events_with_death_handler, 0);

    std::istringstream nonlethal_input(
        "pc=8002dd14 function=zzDealDamage checkpoint=damage_apply rng_draw_index_before=22 "
        "attacker_slot=0 target_slot=4 enemy_entry_id=0 damage=5 hp_before=18 hp_after=13\n"
        "pc=8002bc80 function=HandleCombatantDeath checkpoint=death_handler_gate rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 cur_hp=13\n");
    const auto nonlethal_parsed = parse_checkpoint_stream(nonlethal_input);
    ASSERT_TRUE(nonlethal_parsed.errors.empty());
    const auto nonlethal = summarize_death_drop_checkpoints(nonlethal_parsed.events);
    EXPECT_EQ(nonlethal.status, DeathDropCheckpointStatus::MatchesExpectedFlow);
    EXPECT_EQ(nonlethal.nonlethal_damage_events, 1);
    EXPECT_EQ(nonlethal.observed_drop_path_events, 0);
    EXPECT_EQ(nonlethal.observed_drop_entry_events, 0);

    std::istringstream missing_fields_input(
        "pc=8002dd14 function=zzDealDamage checkpoint=damage_apply rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 damage=5\n"
        "pc=8002bc80 function=HandleCombatantDeath checkpoint=death_handler_gate rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 cur_hp=13\n");
    const auto missing_fields_parsed = parse_checkpoint_stream(missing_fields_input);
    ASSERT_TRUE(missing_fields_parsed.errors.empty());
    const auto missing_fields = summarize_death_drop_checkpoints(missing_fields_parsed.events);
    EXPECT_EQ(
        death_drop_checkpoint_status_name(missing_fields.status),
        std::string("MissingLiveDeathFields"));

    std::istringstream missing_handler_input(
        "pc=8002dd14 function=zzDealDamage checkpoint=damage_apply rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 damage=18 hp_before=18 hp_after=0\n"
        "pc=8002bc80 function=HandleCombatantDeath checkpoint=death_handler_gate rng_draw_index_before=22 "
        "target_slot=5 enemy_entry_id=0 cur_hp=0\n");
    const auto missing_handler_parsed = parse_checkpoint_stream(missing_handler_input);
    ASSERT_TRUE(missing_handler_parsed.errors.empty());
    const auto missing_handler = summarize_death_drop_checkpoints(missing_handler_parsed.events);
    EXPECT_EQ(missing_handler.status, DeathDropCheckpointStatus::MissingDeathHandler);

    std::istringstream unexpected_drop_input(
        "pc=8002dd14 function=zzDealDamage checkpoint=damage_apply rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 damage=5 hp_before=18 hp_after=13\n"
        "pc=8002bc80 function=HandleCombatantDeath checkpoint=death_handler_gate rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 cur_hp=13\n"
        "pc=8002bd20 function=HandleCombatantDeath checkpoint=enemy_drop_call rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 cur_hp=13 entered_enemy_reward=0 called_enemy_drop=1\n"
        "pc=8002ba8c function=enemyDropItem checkpoint=enemy_drop_entry rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0\n");
    const auto unexpected_drop_parsed = parse_checkpoint_stream(unexpected_drop_input);
    ASSERT_TRUE(unexpected_drop_parsed.errors.empty());
    const auto unexpected_drop = summarize_death_drop_checkpoints(unexpected_drop_parsed.events);
    EXPECT_EQ(unexpected_drop.status, DeathDropCheckpointStatus::UnexpectedDropForNonlethalDamage);

    std::istringstream missing_drop_path_input(
        "pc=8002dd14 function=zzDealDamage checkpoint=damage_apply rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 damage=18 hp_before=18 hp_after=0\n"
        "pc=8002bc80 function=HandleCombatantDeath checkpoint=death_handler_gate rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 cur_hp=0\n");
    const auto missing_drop_path_parsed = parse_checkpoint_stream(missing_drop_path_input);
    ASSERT_TRUE(missing_drop_path_parsed.errors.empty());
    const auto missing_drop_path =
        summarize_death_drop_checkpoints(missing_drop_path_parsed.events);
    EXPECT_EQ(missing_drop_path.status, DeathDropCheckpointStatus::MissingDropPath);

    std::istringstream missing_drop_entry_input(
        "pc=8002dd14 function=zzDealDamage checkpoint=damage_apply rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 damage=18 hp_before=18 hp_after=0\n"
        "pc=8002bc80 function=HandleCombatantDeath checkpoint=death_handler_gate rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 cur_hp=0\n"
        "pc=8002bd20 function=HandleCombatantDeath checkpoint=enemy_drop_call rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 cur_hp=0 entered_enemy_reward=1 called_enemy_drop=1\n");
    const auto missing_drop_entry_parsed = parse_checkpoint_stream(missing_drop_entry_input);
    ASSERT_TRUE(missing_drop_entry_parsed.errors.empty());
    const auto missing_drop_entry =
        summarize_death_drop_checkpoints(missing_drop_entry_parsed.events);
    EXPECT_EQ(missing_drop_entry.status, DeathDropCheckpointStatus::MissingDropEntry);

    std::istringstream missing_drop_roll_input(
        "pc=8002dd14 function=zzDealDamage checkpoint=damage_apply rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 damage=18 hp_before=18 hp_after=0\n"
        "pc=8002bc80 function=HandleCombatantDeath checkpoint=death_handler_gate rng_draw_index_before=22 "
        "target_slot=4 enemy_entry_id=0 cur_hp=0\n"
        "pc=8002bd20 function=HandleCombatantDeath checkpoint=enemy_drop_call rng_draw_index_before=22 "
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
