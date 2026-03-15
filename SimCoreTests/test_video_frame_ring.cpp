#include "gtest/gtest.h"

#include "Runner/Debug/VideoFrameRing.h"

TEST(VideoFrameRing, ProducerConsumerRoundTripLatest) {
    const std::string mapping = simcore::debug::MakeVideoRingMappingName(9001, "tok");

    simcore::debug::VideoFrameRingProducer p;
    simcore::debug::VideoRingConfig cfg{};
    cfg.mapping_name = mapping;
    cfg.slot_count = 3;
    cfg.max_frame_bytes = 64 * 64 * 4;
    ASSERT_TRUE(p.Open(cfg));

    simcore::debug::VideoFrameRingConsumer c;
    ASSERT_TRUE(c.Open(mapping));

    std::vector<uint8_t> pixels(16 * 16 * 4, 0x22);
    simcore::debug::VideoFrameDesc d{};
    d.frame_id = 1;
    d.timestamp_sec = 10;
    d.width = 16;
    d.height = 16;
    d.stride = 16 * 4;
    d.format = simcore::debug::VideoPixelFormat::BGRA8;
    d.color_space = 1;
    d.data_bytes = static_cast<uint32_t>(pixels.size());

    ASSERT_TRUE(p.WriteFrame(d, pixels.data(), pixels.size()));

    simcore::debug::VideoFrameDesc out{};
    std::vector<uint8_t> out_pixels;
    ASSERT_TRUE(c.ReadLatest(out, out_pixels));
    EXPECT_EQ(out.frame_id, 1u);
    EXPECT_EQ(out.width, 16u);
    EXPECT_EQ(out.height, 16u);
    ASSERT_EQ(out_pixels.size(), pixels.size());
    EXPECT_EQ(out_pixels[0], 0x22);

    // No new frame should return false.
    EXPECT_FALSE(c.ReadLatest(out, out_pixels));

    // Newer frame replaces latest.
    std::fill(pixels.begin(), pixels.end(), 0x7F);
    d.frame_id = 2;
    ASSERT_TRUE(p.WriteFrame(d, pixels.data(), pixels.size()));
    ASSERT_TRUE(c.ReadLatest(out, out_pixels));
    EXPECT_EQ(out.frame_id, 2u);
    EXPECT_EQ(out_pixels[3], 0x7F);
}
