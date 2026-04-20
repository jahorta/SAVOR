#include "gtest/gtest.h"

#include "File/FileHeaders.h"
#include "Structs/BAMSFHelper.h"
#include "Structs/EndianIOExtensions.h"
#include "Structs/PointerLUT.h"

#include <array>
#include <cstddef>
#include <cmath>
#include <memory>

namespace {

using namespace Sa3Dport;

TEST(Sa3DportStage1, FileHeadersRecognizeNjcmMagic) {
    constexpr std::array<char, 4> candidate {'N', 'J', 'C', 'M'};
    EXPECT_TRUE(File::FileHeaders::MatchesMagic(candidate, File::FileHeaders::kNjcmMagic));
    EXPECT_FALSE(File::FileHeaders::MatchesMagic(candidate, File::FileHeaders::kNjtlMagic));
}

TEST(Sa3DportStage1, EndianReaderReadsLittleEndianPrimitives) {
    constexpr std::array<std::byte, 8> bytes {
        std::byte {0x78}, std::byte {0x56}, std::byte {0x34}, std::byte {0x12},
        std::byte {0xFC}, std::byte {0xFF}, std::byte {0xFF}, std::byte {0xFF},
    };

    Structs::EndianReader reader(bytes, Structs::Endianness::Little);
    EXPECT_EQ(reader.ReadU32(), 0x12345678u);
    EXPECT_EQ(reader.ReadI32(), -4);
}

TEST(Sa3DportStage1, EndianReaderAppliesImageBaseForPointerOffsets) {
    constexpr std::array<std::byte, 4> bytes {
        std::byte {0x20}, std::byte {0x10}, std::byte {0x00}, std::byte {0x00},
    };

    Structs::EndianReader reader(bytes, Structs::Endianness::Little, 0x1000);
    EXPECT_EQ(reader.ReadPointerOffset(), 0x20u);
}

TEST(Sa3DportStage1, PointerLutMemoizesByAddress) {
    Structs::PointerLUT<int> lut;
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
    const auto bams = Structs::BAMSFHelper::DegreesToBams(degrees);

    EXPECT_EQ(bams, 16384);
    EXPECT_NEAR(Structs::BAMSFHelper::BamsToDegrees(bams), degrees, 0.001f);

    constexpr float radians = Structs::BAMSFHelper::kPi * 0.5f;
    EXPECT_NEAR(Structs::BAMSFHelper::RadiansToBams(radians), 16384.0f, 0.01f);
    EXPECT_NEAR(Structs::BAMSFHelper::BamsToRadians(16384), radians, 0.001f);
}

} // namespace
