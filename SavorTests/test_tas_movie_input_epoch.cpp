#include "../SavorCore/Phases/Programs/TasMovieInputEpoch/TasMovieInputEpochModule.h"
#include "../SavorCore/Runner/Runtime/ProgramKind.h"
#include "../SavorCore/Tas/DtmFile.h"

#include <gtest/gtest.h>

namespace {

using namespace savor;
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

TEST(TasMovieInputEpoch, ProductionDefinitionsHaveIndependentCanonicalIdentities)
{
    const auto annotation = AnnotationFullPhaseDefinitionV1();
    const auto rewrite = RewriteFullPhaseDefinitionV1();
    ASSERT_TRUE(annotation);
    ASSERT_TRUE(rewrite);
    EXPECT_EQ(annotation->identity().program_kind, PK_TasMovieAnnotateInputEpochs);
    EXPECT_EQ(rewrite->identity().program_kind, PK_TasMovieRewriteInputEpochs);
    EXPECT_NE(annotation->identity().canonical_id, rewrite->identity().canonical_id);
    EXPECT_FALSE(annotation->module_envelope().payload.empty());
    EXPECT_FALSE(rewrite->module_envelope().payload.empty());
}

} // namespace
