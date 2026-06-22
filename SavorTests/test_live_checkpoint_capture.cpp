#include <gtest/gtest.h>

#include "CheckpointTrace.h"
#include "LiveCaptureProfile.h"
#include "Phases/Programs/BattleTurnRunner/BattleTurnRunnerPayload.h"
#include "Runner/Capture/CaptureJsonlWriter.h"
#include "Runner/Capture/CaptureProfile.h"
#include "Runner/Script/CtxRegistry.h"
#include "Runner/Script/PSContext.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace savor::capture;
using namespace savor::predict;

TEST(LiveCheckpointCaptureProfile, ParsesDefaultSamplesAndUniquePcs)
{
    const std::string text =
        "[profile]\n"
        "name=test_capture\n"
        "schema_version=1\n"
        "memory=rng_seed_before:0x803469A8:u32, wide_counter:0x80000000:u64\n"
        "gprs=return_value:3\n"
        "reg_memory=payload_mode:r3:0x22:u16, saved_mode:31:-0x10:u16\n"
        "\n"
        "[checkpoint.first]\n"
        "pc=0x80001000\n"
        "name=first_draw\n"
        "function=RNG\n"
        "checkpoint=draw\n"
        "owns_rng_draw=true\n"
        "reg_memory=payload_flags:r3:0x10:u32\n"
        "\n"
        "[checkpoint.second]\n"
        "pc=0x80001000\n"
        "name=second_label_same_pc\n"
        "function=RNG\n"
        "checkpoint=draw\n"
        "owns_rng_draw=false\n";

    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "test_capture");
    ASSERT_EQ(profile.checkpoints.size(), 2u);
    EXPECT_EQ(profile.checkpoints[0].memory_samples.size(), 2u);
    EXPECT_EQ(profile.checkpoints[0].memory_samples[0].name, "rng_seed_before");
    EXPECT_EQ(profile.checkpoints[0].memory_samples[0].width, SampleWidth::U32);
    EXPECT_EQ(profile.checkpoints[0].memory_samples[1].width, SampleWidth::U64);
    EXPECT_EQ(profile.checkpoints[0].gpr_samples.size(), 1u);
    ASSERT_EQ(profile.checkpoints[0].register_memory_samples.size(), 3u);
    EXPECT_EQ(profile.checkpoints[0].register_memory_samples[0].name, "payload_mode");
    EXPECT_EQ(profile.checkpoints[0].register_memory_samples[0].base_reg, 3u);
    EXPECT_EQ(profile.checkpoints[0].register_memory_samples[0].offset, 0x22);
    EXPECT_EQ(profile.checkpoints[0].register_memory_samples[1].base_reg, 31u);
    EXPECT_EQ(profile.checkpoints[0].register_memory_samples[1].offset, -0x10);
    EXPECT_EQ(profile.checkpoints[0].register_memory_samples[2].name, "payload_flags");
    EXPECT_TRUE(profile.checkpoints[0].owns_rng_draw);
    EXPECT_FALSE(profile.checkpoints[1].owns_rng_draw);

    const auto pcs = profile.pcs();
    ASSERT_EQ(pcs.size(), 1u);
    EXPECT_EQ(pcs[0], 0x80001000u);
}

TEST(LiveCheckpointCaptureJsonl, SerializesRequiredFieldsAndStableRepeatedHitOrder)
{
    CheckpointCaptureRecord first{};
    first.capture_sequence = 0;
    first.checkpoint_hit_count = 0;
    first.pc = 0x8008B428;
    first.checkpoint_id = "soldier_ai_action";
    first.checkpoint_name = "soldier_ai_action";
    first.function = "RNG";
    first.checkpoint = "soldier_ai_action";
    first.movie_input_count = 11;
    first.vi_field_count = 22;
    first.frame_count = 33;
    first.tbr_u64 = 0x1111111122222222ull;
    first.tbr_high = 0x11111111u;
    first.tbr_low = 0x22222222u;
    first.rng_draw_index_before = 5;
    first.owns_rng_draw = true;
    first.fields.push_back(CaptureField{ "rng_seed_before", "\"0x12345678\"", false });

    CheckpointCaptureRecord second = first;
    second.capture_sequence = 1;
    second.checkpoint_hit_count = 1;
    second.rng_draw_index_before = 6;
    second.tbr_u64 = 0x3333333344444444ull;
    second.tbr_high = 0x33333333u;
    second.tbr_low = 0x44444444u;
    second.fields.clear();
    second.fields.push_back(CaptureField{ "rng_seed_before", "\"0x23456789\"", false });

    const auto first_line = SerializeJsonlRecord(first);
    const auto second_line = SerializeJsonlRecord(second);
    EXPECT_EQ(first_line.find(",}"), std::string::npos);
    EXPECT_NE(first_line.find("\"capture_sequence\":0"), std::string::npos);
    EXPECT_NE(first_line.find("\"tbr_high\":286331153"), std::string::npos);
    EXPECT_NE(first_line.find("\"tbr_low\":572662306"), std::string::npos);
    EXPECT_NE(first_line.find("\"tbr_u64_hex\":\"0x1111111122222222\""), std::string::npos);

    std::istringstream input(first_line + "\n" + second_line + "\n");
    const auto parsed = parse_checkpoint_stream(input);
    EXPECT_TRUE(parsed.errors.empty()) << (parsed.errors.empty() ? "" : parsed.errors.front());
    ASSERT_EQ(parsed.events.size(), 2u);
    EXPECT_EQ(parsed.events[0].fields.at("capture_sequence"), "0");
    EXPECT_EQ(parsed.events[0].fields.at("checkpoint_hit_count"), "0");
    EXPECT_EQ(parsed.events[0].fields.at("tbr_high"), "286331153");
    EXPECT_EQ(parsed.events[0].fields.at("tbr_low"), "572662306");
    ASSERT_TRUE(parsed.events[0].rng_draw_index_before.has_value());
    ASSERT_TRUE(parsed.events[0].rng_seed_before.has_value());
    EXPECT_EQ(*parsed.events[0].rng_draw_index_before, 5);
    EXPECT_EQ(*parsed.events[0].rng_seed_before, 0x12345678u);
    EXPECT_EQ(parsed.events[1].fields.at("capture_sequence"), "1");
    EXPECT_EQ(parsed.events[1].fields.at("checkpoint_hit_count"), "1");
    ASSERT_TRUE(parsed.events[1].rng_draw_index_before.has_value());
    ASSERT_TRUE(parsed.events[1].rng_seed_before.has_value());
    EXPECT_EQ(*parsed.events[1].rng_draw_index_before, 6);
    EXPECT_EQ(*parsed.events[1].rng_seed_before, 0x23456789u);
}

TEST(SavorPredictLiveCaptureProfile, BuildsParseableFirstBattleRngProfile)
{
    const auto text = build_first_battle_capture_profile_ini();
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_live_capture");
    ASSERT_GT(profile.checkpoints.size(), known_rng_callsite_owners().size());
    ASSERT_FALSE(profile.checkpoints.empty());
    int rng_checkpoints = 0;
    int action_view_state_checkpoints = 0;
    bool found_default_targeting_camera = false;
    bool found_default_action_view_dispatch_state = false;
    for (const auto& checkpoint : profile.checkpoints) {
        ASSERT_FALSE(checkpoint.memory_samples.empty()) << checkpoint.id;
        EXPECT_EQ(checkpoint.memory_samples[0].name, "rng_seed_before") << checkpoint.id;
        EXPECT_EQ(checkpoint.memory_samples[0].width, SampleWidth::U32) << checkpoint.id;
        if (checkpoint.owns_rng_draw) {
            ++rng_checkpoints;
        }
        if (checkpoint.id == "pre_ai_attack_targeting_camera_800608DC") {
            found_default_targeting_camera = true;
        }
        if (checkpoint.id == "action_view_dispatch_state_80051424") {
            found_default_action_view_dispatch_state = true;
        }
        if (checkpoint.id.find("action_view") != std::string::npos
            || checkpoint.id.find("mode0") != std::string::npos) {
            ++action_view_state_checkpoints;
        }
    }
    EXPECT_EQ(rng_checkpoints, static_cast<int>(known_rng_callsite_owners().size()) - 1);
    EXPECT_FALSE(found_default_targeting_camera);
    EXPECT_FALSE(found_default_action_view_dispatch_state);
    EXPECT_GE(action_view_state_checkpoints, 7);

    const auto find_checkpoint = [&](std::string_view id)
        -> const CheckpointSpec* {
        for (const auto& checkpoint : profile.checkpoints) {
            if (checkpoint.id == id) {
                return &checkpoint;
            }
        }
        return nullptr;
    };
    const auto has_reg_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.register_memory_samples) {
            if (sample.name == name) {
                return true;
            }
        }
        return false;
    };
    const auto has_gpr_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.gpr_samples) {
            if (sample.name == name) {
                return true;
            }
        }
        return false;
    };

    const auto* combat_effect =
        find_checkpoint("combat_effect_spawn_scale_x_80043020");
    ASSERT_NE(combat_effect, nullptr);
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_source_key_0x28"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_source_subtype_0x2a"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_source_secondary_0x2c"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_source_resource_id_0x30"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_source_timing_raw_0x3c"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_source_pos_x_raw_0x50"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_source_pos_y_raw_0x54"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_source_pos_z_raw_0x58"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_loop_count_0x5c"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_flags_0x38"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_variant_count_0x5e"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_axis_mode_0x60"));

    const auto* emitter_source =
        find_checkpoint("effect_emitter_source_gate_80041F30");
    ASSERT_NE(emitter_source, nullptr);
    EXPECT_FALSE(emitter_source->owns_rng_draw);
    EXPECT_TRUE(has_reg_sample(*emitter_source, "emitter_outer_count_0x142"));
    EXPECT_TRUE(has_reg_sample(*emitter_source, "emitter_child_count_0x1c"));
    EXPECT_TRUE(has_reg_sample(*emitter_source, "emitter_variant_count_0x16"));
    EXPECT_TRUE(has_reg_sample(*emitter_source, "emitter_axis_mode_0x3c"));

    const auto* particle_tick =
        find_checkpoint("effect_particle_motion_gate_x_800425A0");
    ASSERT_NE(particle_tick, nullptr);

    const auto* action_view_query_call =
        find_checkpoint("action_view_category2_query_call_8001331C");
    ASSERT_NE(action_view_query_call, nullptr);
    EXPECT_FALSE(action_view_query_call->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*action_view_query_call, "aux_list_root"));
    EXPECT_TRUE(has_gpr_sample(*action_view_query_call, "query_arg1"));
    EXPECT_TRUE(has_reg_sample(*action_view_query_call, "active_slot"));
    EXPECT_TRUE(has_reg_sample(*action_view_query_call, "target_slot"));
    EXPECT_TRUE(has_reg_sample(*action_view_query_call, "actor_field6_0x6"));
    EXPECT_TRUE(has_reg_sample(*action_view_query_call, "gate_state_0x30"));

    const auto* action_view_query_result =
        find_checkpoint("action_view_category2_query_result_80013320");
    ASSERT_NE(action_view_query_result, nullptr);
    EXPECT_FALSE(action_view_query_result->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*action_view_query_result, "query_result"));
    EXPECT_TRUE(has_reg_sample(*action_view_query_result, "actor_field6_0x6"));

    const auto* action_view_spawn =
        find_checkpoint("action_view_category2_spawn_80013334");
    ASSERT_NE(action_view_spawn, nullptr);
    EXPECT_FALSE(action_view_spawn->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*action_view_spawn, "spawn_slot_arg"));
    EXPECT_TRUE(has_gpr_sample(*action_view_spawn, "spawn_mode_arg"));

    const auto* effect_record_copy =
        find_checkpoint("effect_record_copy_complete_8003BB24");
    ASSERT_NE(effect_record_copy, nullptr);
    EXPECT_FALSE(effect_record_copy->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*effect_record_copy, "r6_effect_buffer"));
    EXPECT_TRUE(has_gpr_sample(*effect_record_copy, "r30_parent_action_thread"));
    EXPECT_TRUE(has_gpr_sample(*effect_record_copy, "r31_source_record"));
    EXPECT_TRUE(has_reg_sample(*effect_record_copy, "effect_parent_action_thread_0x04"));
    EXPECT_TRUE(has_reg_sample(*effect_record_copy, "effect_source_key_0x28"));
    EXPECT_TRUE(has_reg_sample(*effect_record_copy, "source_record_key_0x00"));
    EXPECT_TRUE(has_reg_sample(*effect_record_copy, "source_record_loop_count_0x34"));
    EXPECT_TRUE(has_reg_sample(*particle_tick, "particle_payload_source_ptr_0x20"));
    EXPECT_TRUE(has_reg_sample(*particle_tick, "particle_payload_lifetime_0x28"));

    const auto* attack_begin =
        find_checkpoint("attack_resolution_begin_80081B94");
    ASSERT_NE(attack_begin, nullptr);
    EXPECT_FALSE(attack_begin->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*attack_begin, "target_slot_arg"));
    EXPECT_TRUE(has_gpr_sample(*attack_begin, "actor_slot_arg"));

    const auto* source_selection =
        find_checkpoint("action_source_selection_80067BD0");
    ASSERT_NE(source_selection, nullptr);
    EXPECT_FALSE(source_selection->owns_rng_draw);
    EXPECT_TRUE(has_reg_sample(*source_selection, "selected_source_slot"));
    EXPECT_TRUE(has_reg_sample(*source_selection, "controller_actor_slot_0x92"));

    const auto* source_bridge =
        find_checkpoint("action_source_field6_bridge_8006778C");
    ASSERT_NE(source_bridge, nullptr);
    EXPECT_FALSE(source_bridge->owns_rng_draw);
    EXPECT_TRUE(has_reg_sample(*source_bridge, "actor_slot"));
    EXPECT_TRUE(has_reg_sample(*source_bridge, "target_slot"));
    EXPECT_TRUE(has_reg_sample(*source_bridge, "source_slot"));
    EXPECT_TRUE(has_reg_sample(*source_bridge, "actor_field6_0x6"));
    EXPECT_TRUE(has_reg_sample(*source_bridge, "source_field6_0x6"));
    EXPECT_TRUE(has_reg_sample(*source_bridge, "handler_pc"));
    EXPECT_TRUE(has_reg_sample(*source_bridge, "callback_0xe0"));

    const auto* sst_case2 =
        find_checkpoint("sst_action_field6_case2_store_complete_8000C4C8");
    ASSERT_NE(sst_case2, nullptr);
    EXPECT_FALSE(sst_case2->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*sst_case2, "r0_written_field6"));
    EXPECT_TRUE(has_gpr_sample(*sst_case2, "r3_destination_worksheet"));
    EXPECT_TRUE(has_gpr_sample(*sst_case2, "r29_serialized_command"));
    EXPECT_TRUE(has_reg_sample(*sst_case2, "source_field6_0x06"));
    EXPECT_TRUE(has_reg_sample(*sst_case2, "dest_field6_after_0x06"));

    const auto* sst_case8 =
        find_checkpoint("sst_action_field6_case8_store_complete_8000C6E8");
    ASSERT_NE(sst_case8, nullptr);
    EXPECT_FALSE(sst_case8->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*sst_case8, "r0_written_field6"));
    EXPECT_TRUE(has_gpr_sample(*sst_case8, "r3_destination_worksheet"));
    EXPECT_TRUE(has_gpr_sample(*sst_case8, "r30_serialized_command"));
    EXPECT_TRUE(has_reg_sample(*sst_case8, "source_field6_0x0a"));
    EXPECT_TRUE(has_reg_sample(*sst_case8, "dest_field6_after_0x06"));
}

TEST(BattleTurnRunnerPayload, RoundTripsLiveCaptureContextPaths)
{
    phase::battle::turnrunner::EncodeSpec spec{};
    spec.run_ms = 120000;
    spec.vi_stall_ms = 5000;
    spec.capture_profile_path = "D:/SavorPredictDB/capture/first_battle.ini";
    spec.capture_output_path = "D:/SavorPredictDB/capture/job_1.jsonl";

    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(phase::battle::turnrunner::encode_payload(spec, payload));

    savor::PSContext ctx;
    ASSERT_TRUE(phase::battle::turnrunner::decode_payload(payload, ctx));

    std::string profile_path;
    std::string output_path;
    ASSERT_TRUE(ctx.get(savor::context::key::core::CAPTURE_PROFILE_PATH, profile_path));
    ASSERT_TRUE(ctx.get(savor::context::key::core::CAPTURE_OUTPUT_PATH, output_path));
    EXPECT_EQ(profile_path, spec.capture_profile_path);
    EXPECT_EQ(output_path, spec.capture_output_path);
}

} // namespace
