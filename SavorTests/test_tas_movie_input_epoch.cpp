#include "../SavorCore/Phases/Programs/TasMovieInputEpoch/TasMovieInputEpochModule.h"
#include "../SavorCore/Runner/Runtime/ProgramKind.h"
#include "../SavorCore/Tas/DtmFile.h"

#include <gtest/gtest.h>

namespace {

using namespace savor;
using namespace savor::runtime::program;
using namespace savor::runtime::tasmovie::inputepoch;

TEST(TasMovieInputEpoch, DecodesDolphinControllerStateBitfield)
{
    tas::DtmGCPoll poll{};
    poll.button_bits = static_cast<std::uint16_t>(
        (1u << 1) | (1u << 2) | (1u << 5) | (1u << 14));
    poll.trigger_l = 12;
    poll.trigger_r = 34;
    poll.stick_x = 101;
    poll.stick_y = 102;
    poll.cstick_x = 103;
    poll.cstick_y = 104;
    tas::DtmControllerStateMetadata metadata{};
    const auto decoded = tas::decode_gc_controller_state(poll, &metadata);
    EXPECT_EQ(decoded.buttons, static_cast<std::uint16_t>(GC_A | GC_B | GC_Z));
    EXPECT_EQ(decoded.trig_l, 12);
    EXPECT_EQ(decoded.trig_r, 34);
    EXPECT_EQ(decoded.main_x, 101);
    EXPECT_EQ(decoded.main_y, 102);
    EXPECT_EQ(decoded.c_x, 103);
    EXPECT_EQ(decoded.c_y, 104);
    EXPECT_TRUE(metadata.connected);
}

TEST(TasMovieInputEpoch, DecodesGuestPadStatusLayout)
{
    EXPECT_EQ(DecodeGuestPadStatusV1(0), GCInputFrame{});
    EXPECT_EQ(
        DecodeGuestPadStatusV1(0x0100000000000000ull),
        GCInputFrame::new_btns(GC_A));
    EXPECT_EQ(
        DecodeGuestPadStatusV1(0x0200000000000000ull),
        GCInputFrame::new_btns(GC_B));
}

TEST(TasMovieInputEpoch, ScheduleCodecAllowsRepeatedAndJumpedCursors)
{
    TasMovieInputEpochScheduleV1 schedule{
        .source_dtm_sha256 = std::string(64, 'a'),
        .source_poll_count = 9,
        .epochs = {
            { .movie_input_cursor = 1, .input = GCInputFrame::new_btns(GC_A) },
            { .movie_input_cursor = 1, .input = GCInputFrame::new_btns(GC_A) },
            { .movie_input_cursor = 7, .input = GCInputFrame::new_btns(GC_B) },
        },
    };
    std::string diagnostic;
    const auto encoded = EncodeInputEpochScheduleArtifactV1(schedule, &diagnostic);
    ASSERT_FALSE(encoded.empty()) << diagnostic;
    TasMovieInputEpochScheduleV1 decoded;
    ASSERT_TRUE(DecodeInputEpochScheduleArtifactV1(encoded, decoded, &diagnostic))
        << diagnostic;
    EXPECT_EQ(decoded, schedule);
    EXPECT_EQ(encoded.size(), 92u + schedule.epochs.size() * 16u);
}

TEST(TasMovieInputEpoch, ScheduleRejectsMalformedCursors)
{
    TasMovieInputEpochScheduleV1 schedule{
        .source_dtm_sha256 = std::string(64, 'b'),
        .source_poll_count = 2,
        .epochs = {
            { .movie_input_cursor = 2 },
            { .movie_input_cursor = 1 },
        },
    };
    std::string diagnostic;
    EXPECT_FALSE(ValidateInputEpochScheduleV1(schedule, &diagnostic));
    EXPECT_FALSE(diagnostic.empty());
}

TEST(TasMovieInputEpoch, RewriteRunsMergeInsertedAndSourceRepeatedInputs)
{
    const auto neutral = GCInputFrame{};
    const auto b = GCInputFrame::new_btns(GC_B);
    TasMovieInputEpochScheduleV1 schedule{
        .source_dtm_sha256 = std::string(64, 'c'),
        .source_poll_count = 5,
        .epochs = {
            {.movie_input_cursor = 1, .input = GCInputFrame::new_btns(GC_A)},
            {.movie_input_cursor = 2, .input = neutral},
            {.movie_input_cursor = 3, .input = neutral},
            {.movie_input_cursor = 4, .input = b},
            {.movie_input_cursor = 5, .input = b},
        },
    };

    const auto runs = BuildRewriteInputRunsV1(schedule, 1, 2);
    ASSERT_EQ(runs.size(), 2u);
    EXPECT_EQ(runs[0].input, neutral);
    EXPECT_EQ(runs[0].epoch_count, 4u);
    EXPECT_EQ(runs[1].input, b);
    EXPECT_EQ(runs[1].epoch_count, 2u);
}

TEST(TasMovieInputEpoch, RewriteRunsAcceptExactZeroAndFiveNeutralEpochs)
{
    const auto a = GCInputFrame::new_btns(GC_A);
    TasMovieInputEpochScheduleV1 schedule{
        .source_dtm_sha256 = std::string(64, 'd'),
        .source_poll_count = 2,
        .epochs = {
            {.movie_input_cursor = 1, .input = a},
            {.movie_input_cursor = 2, .input = a},
        },
    };

    const auto unchanged = BuildRewriteInputRunsV1(schedule, 1, 0);
    ASSERT_EQ(unchanged.size(), 1u);
    EXPECT_EQ(unchanged.front().input, a);
    EXPECT_EQ(unchanged.front().epoch_count, 1u);

    const auto delayed = BuildRewriteInputRunsV1(schedule, 1, 5);
    ASSERT_EQ(delayed.size(), 2u);
    EXPECT_EQ(delayed.front().input, GCInputFrame{});
    EXPECT_EQ(delayed.front().epoch_count, 5u);
    EXPECT_EQ(delayed.back().input, a);
    EXPECT_EQ(delayed.back().epoch_count, 1u);
}

TEST(TasMovieInputEpoch, ProductionDefinitionsHaveIndependentCanonicalIdentities)
{
    const auto annotation = AnnotationFullPhaseDefinitionV1();
    const auto rewrite = RewriteFullPhaseDefinitionV1();
    const auto cutscene = CutsceneFullPhaseDefinitionV1();
    ASSERT_TRUE(annotation);
    ASSERT_TRUE(rewrite);
    ASSERT_TRUE(cutscene);
    EXPECT_EQ(annotation->identity().program_kind, PK_TasMovieAnnotate);
    EXPECT_EQ(rewrite->identity().program_kind, PK_TasMovieRevise);
    EXPECT_EQ(cutscene->identity().program_kind, PK_TasMovieCutscene);
    EXPECT_NE(annotation->identity().canonical_id, rewrite->identity().canonical_id);
    EXPECT_NE(rewrite->identity().canonical_id, cutscene->identity().canonical_id);
    EXPECT_FALSE(annotation->module_envelope().payload.empty());
    EXPECT_FALSE(rewrite->module_envelope().payload.empty());
    EXPECT_FALSE(cutscene->module_envelope().payload.empty());
    EXPECT_EQ(cutscene->runtime_contract().state_policy,
        InvocationStatePolicy::RestoreBaseline);
    EXPECT_TRUE(HasInvocationHandlerFlag(
        cutscene->runtime_contract().execution.handler_flags,
        InvocationHandlerFlag::DialogueAdvance));
}

} // namespace
