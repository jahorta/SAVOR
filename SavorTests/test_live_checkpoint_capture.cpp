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
        "\n"
        "[checkpoint.first]\n"
        "pc=0x80001000\n"
        "name=first_draw\n"
        "function=RNG\n"
        "checkpoint=draw\n"
        "owns_rng_draw=true\n"
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
    EXPECT_EQ(profile.name, "first_battle_rng_live_capture");
    ASSERT_EQ(profile.checkpoints.size(), known_rng_callsite_owners().size());
    ASSERT_FALSE(profile.checkpoints.empty());
    for (const auto& checkpoint : profile.checkpoints) {
        EXPECT_TRUE(checkpoint.owns_rng_draw) << checkpoint.id;
        ASSERT_FALSE(checkpoint.memory_samples.empty()) << checkpoint.id;
        EXPECT_EQ(checkpoint.memory_samples[0].name, "rng_seed_before") << checkpoint.id;
        EXPECT_EQ(checkpoint.memory_samples[0].width, SampleWidth::U32) << checkpoint.id;
    }
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
