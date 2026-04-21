#include "gtest/gtest.h"

#include "Testing/Slice1TestApi.h"

#include <array>
#include <cstddef>
#include <cmath>
#include <memory>

namespace {

using namespace Sa3Dport::Testing::Slice1;

TEST(Sa3DportStage1, FileHeadersRecognizeNjcmMagic) {
    constexpr std::array<char, 4> candidate {'N', 'J', 'C', 'M'};
    EXPECT_TRUE(MatchesMagic(candidate, kNjcmMagic));
    EXPECT_FALSE(MatchesMagic(candidate, kNjtlMagic));
}

TEST(Sa3DportStage1, EndianReaderReadsLittleEndianPrimitives) {
    constexpr std::array<std::byte, 8> bytes {
        std::byte {0x78}, std::byte {0x56}, std::byte {0x34}, std::byte {0x12},
        std::byte {0xFC}, std::byte {0xFF}, std::byte {0xFF}, std::byte {0xFF},
    };

    auto reader = MakeReader(bytes, Endianness::Little);
    EXPECT_EQ(reader.ReadU32(), 0x12345678u);
    EXPECT_EQ(reader.ReadI32(), -4);
}

TEST(Sa3DportStage1, EndianReaderAppliesImageBaseForPointerOffsets) {
    constexpr std::array<std::byte, 4> bytes {
        std::byte {0x20}, std::byte {0x10}, std::byte {0x00}, std::byte {0x00},
    };

    auto reader = MakeReader(bytes, Endianness::Little, 0x1000);
    EXPECT_EQ(reader.ReadPointerOffset(), 0x20u);
}

TEST(Sa3DportStage1, PointerLutMemoizesByAddress) {
    PointerLUT<int> lut;
    auto first = std::make_shared<int>(7);
    auto second = std::make_shared<int>(9);

    auto stored = lut.GetOrAdd(0x1010, first);
    auto duplicate = lut.GetOrAdd(0x1010, second);

    EXPECT_EQ(lut.Size(), 1u);
    EXPECT_EQ(stored.get(), duplicate.get());
    ASSERT_NE(lut.TryGet(0x1010), nullptr);
    EXPECT_EQ(*lut.TryGet(0x1010), 7);
}

TEST(Sa3DportStage1, BamsConversionsRoundTripDegreesAndRadians) {
    constexpr float degrees = 90.0f;
    const auto bams = DegreesToBams(degrees);

    EXPECT_EQ(bams, 16384);
    EXPECT_NEAR(BamsToDegrees(bams), degrees, 0.001f);

    constexpr float radians = kPi * 0.5f;
    EXPECT_NEAR(RadiansToBams(radians), 16384.0f, 0.01f);
    EXPECT_NEAR(BamsToRadians(16384), radians, 0.001f);
}

} // namespace
