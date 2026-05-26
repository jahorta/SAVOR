#include "gtest/gtest.h"

#include "Testing/Slice1TestApi.h"
#include "Sa3Dport.h"

#include <array>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

using namespace Sa3Dport::Testing::Slice1;
namespace S = Sa3Dport::Structs;

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

TEST(Sa3DportStructs, RoundToEvenMatchesDotNetMidpoints) {
    EXPECT_EQ(S::MathCompat::round_to_even(0.5f), 0.0f);
    EXPECT_EQ(S::MathCompat::round_to_even(1.5f), 2.0f);
    EXPECT_EQ(S::MathCompat::round_to_even(2.5f), 2.0f);
    EXPECT_EQ(S::MathCompat::round_to_even(-0.5f), -0.0f);
    EXPECT_EQ(S::MathCompat::round_to_even(-1.5f), -2.0f);
}

TEST(Sa3DportStructs, VectorAndMatrixBasicsWork) {
    const S::Vector3 a {1.0f, 2.0f, 3.0f};
    const S::Vector3 b {4.0f, 5.0f, 6.0f};
    EXPECT_EQ(a + b, S::Vector3(5.0f, 7.0f, 9.0f));
    EXPECT_FLOAT_EQ(S::dot(a, b), 32.0f);

    const auto scaleTranslate = S::create_scale(2.0f) * S::create_translation({3.0f, 4.0f, 5.0f});
    EXPECT_FLOAT_EQ(scaleTranslate.m11, 2.0f);
    EXPECT_FLOAT_EQ(scaleTranslate.m22, 2.0f);
    EXPECT_FLOAT_EQ(scaleTranslate.m33, 2.0f);
    EXPECT_FLOAT_EQ(scaleTranslate.m41, 3.0f);
    EXPECT_FLOAT_EQ(scaleTranslate.m42, 4.0f);
    EXPECT_FLOAT_EQ(scaleTranslate.m43, 5.0f);

    S::Matrix4x4 inverse;
    ASSERT_TRUE(S::invert(S::identity(), inverse));
    EXPECT_EQ(inverse, S::identity());
}

TEST(Sa3DportStructs, EndianStackWriterAndReaderRoundTripBigEndian) {
    S::EndianStackWriter writer(S::Endian::Big);
    writer.write_u16(0x1234u);
    writer.write_i32(-4);
    writer.write_float(1.5f);

    const auto& bytes = writer.data();
    ASSERT_EQ(bytes.size(), 10u);
    EXPECT_EQ(bytes[0], std::byte {0x12});
    EXPECT_EQ(bytes[1], std::byte {0x34});

    S::EndianStackReader reader(bytes, S::Endian::Big);
    EXPECT_EQ(reader.read_u16(0), 0x1234u);
    EXPECT_EQ(reader.read_i32(2), -4);
    EXPECT_FLOAT_EQ(reader.read_float(6), 1.5f);
}

TEST(Sa3DportStructs, FloatIOTypeWritesAndReadsExpectedFormats) {
    S::EndianStackWriter writer(S::Endian::Little);
    S::write_float_as(writer, 2.5f, S::FloatIOType::Short);
    S::write_float_as(writer, S::MathHelper::HalfPi, S::FloatIOType::BAMS16);

    S::EndianStackReader reader(writer.data(), S::Endian::Little);
    EXPECT_FLOAT_EQ(S::read_float_as(reader, 0, S::FloatIOType::Short), 2.0f);
    EXPECT_NEAR(S::read_float_as(reader, 2, S::FloatIOType::BAMS16), S::MathHelper::HalfPi, 0.001f);
    EXPECT_EQ(S::print_float_as(1.25f, S::FloatIOType::Float), "1.25000f");
    EXPECT_EQ(S::BAMSFHelper::RadToBAMSF(S::MathHelper::HalfPi), 16384);
    EXPECT_EQ(S::BAMSFHelper::DegToBAMSF(180.0f), 32768);
}

TEST(Sa3DportStructs, ColorPackedFormatsAndHexRoundTrip) {
    S::Color color {0x12, 0x34, 0x56, 0x78};
    EXPECT_EQ(color.rgba(), 0x12345678u);
    EXPECT_EQ(color.argb(), 0x78123456u);

    S::Color parsed;
    parsed.set_hex("#12345678");
    EXPECT_EQ(parsed, color);
    EXPECT_EQ(parsed.hex(), "#12345678");

    EXPECT_EQ(S::Color(0xFF, 0x00, 0xFF).rgb565(), 0x001Fu);
    EXPECT_EQ(S::Color(0xF0, 0xA5, 0x5A, 0xCC).argb4(), 0x0005u);

    S::Color vectorColor;
    vectorColor.set_float_vector({1.0f, 0.25f, 0.5f, 1.0f});
    EXPECT_EQ(vectorColor.red, 0xFF);
    EXPECT_EQ(vectorColor.blue, 63);
    EXPECT_EQ(vectorColor.green, 127);
}

TEST(Sa3DportStructs, DebugStringsMatchCSharpFormatting) {
    EXPECT_EQ(S::DebugStringExtensions::debug_string(1.25f), " 1.250");
    EXPECT_EQ(S::DebugStringExtensions::debug_string(-1.25f), "-1.250");
    EXPECT_EQ(S::DebugStringExtensions::debug_string(S::Vector2(1.0f, -2.0f)), "( 1.000, -2.000)");
}

TEST(Sa3DportStructs, EndianIOExtensionsHandleColorsVectorsAndQuaternions) {
    S::EndianStackWriter writer(S::Endian::Little);
    S::EndianIOExtensions::write_color(writer, S::Color(0x12, 0x34, 0x56, 0x78), S::ColorIOType::ARGB8_16);
    S::EndianIOExtensions::write_vector3(writer, {1.0f, 2.0f, 3.0f});
    S::EndianIOExtensions::write_quaternion(writer, {1.0f, 2.0f, 3.0f, 4.0f});

    S::EndianStackReader reader(writer.data(), S::Endian::Little);
    std::uint32_t address = 0;
    const auto color = S::EndianIOExtensions::read_color(reader, address, S::ColorIOType::ARGB8_16);
    EXPECT_EQ(color.argb(), 0x78123456u);
    EXPECT_EQ(address, 4u);

    const auto vector = S::EndianIOExtensions::read_vector3(reader, address);
    EXPECT_EQ(vector, S::Vector3(1.0f, 2.0f, 3.0f));
    EXPECT_EQ(address, 16u);

    const auto quaternion = S::EndianIOExtensions::read_quaternion(reader, address);
    EXPECT_EQ(quaternion, S::Quaternion(1.0f, 2.0f, 3.0f, 4.0f));
    EXPECT_EQ(address, 32u);
}

TEST(Sa3DportStructs, BaseLutCachesReadValuesAndWriteAddresses) {
    S::BaseLUT lut;
    int factoryCalls = 0;
    const auto first = lut.get_add_value<int>(0x20, [&]() {
        ++factoryCalls;
        return std::vector<int> {1, 2, 3};
    });
    const auto second = lut.get_add_value<int>(0x20, [&]() {
        ++factoryCalls;
        return std::vector<int> {9};
    });

    EXPECT_EQ(factoryCalls, 1);
    EXPECT_EQ(first, second);

    std::vector<int> values {1, 2};
    const auto firstAddress = lut.get_add_address(&values, []() { return 0x100u; });
    const auto secondAddress = lut.get_add_address(&values, []() { return 0x200u; });
    EXPECT_EQ(firstAddress, 0x100u);
    EXPECT_EQ(secondAddress, 0x100u);

    S::BaseLUT labeled({{0x40u, "known_label"}});
    const auto labeledArray = labeled.get_add_labeled_value<int>(0x40u, "generated", []() {
        return std::vector<int> {4, 5};
    });
    EXPECT_EQ(labeledArray.label, "known_label");
}

TEST(Sa3DportStructs, BoundsRecalculatesMatrixAndPositionNormalHashesExactBits) {
    S::Bounds bounds({1.0f, 2.0f, 3.0f}, 4.0f);
    EXPECT_FLOAT_EQ(bounds.matrix().m11, 4.0f);
    EXPECT_FLOAT_EQ(bounds.matrix().m41, 1.0f);

    bounds.set_position({5.0f, 6.0f, 7.0f});
    bounds.set_radius(2.0f);
    EXPECT_FLOAT_EQ(bounds.matrix().m11, 2.0f);
    EXPECT_FLOAT_EQ(bounds.matrix().m41, 5.0f);

    const S::PositionNormal a {{1.0f, 2.0f, 3.0f}, {0.0f, 1.0f, 0.0f}};
    const S::PositionNormal b {{1.0f, 2.0f, 3.0f}, {0.0f, 1.0f, 0.0f}};
    EXPECT_EQ(a, b);
    EXPECT_EQ(S::PositionNormalHash{}(a), S::PositionNormalHash{}(b));
}

} // namespace
