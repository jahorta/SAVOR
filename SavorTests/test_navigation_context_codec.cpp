#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Input/InputPlan.h"
#include "Core/Memory/MemView.h"
#include "Core/Memory/Soa/Navigation/NavigationContext.h"
#include "Core/Memory/Soa/Navigation/NavigationContextCodec.h"
#include "Phases/Programs/NavigationContext/NavigationContextPayload.h"
#include "Phases/Programs/NavigationContext/NavigationContextResult.h"
#include "Runner/IPC/Wire.h"
#include "Runner/Script/CtxRegistry.h"

namespace {

namespace nav = soa::navigation::ctx;
namespace codec = soa::navigation::ctx::codec;

class Mem1Image {
public:
    Mem1Image()
        : bytes_(savor::MemView::kMem1Size, 0)
    {
    }

    savor::MemView view() const
    {
        return savor::MemView(bytes_.data(), bytes_.size());
    }

    void PutU8(std::uint32_t address, std::uint8_t value)
    {
        bytes_.at(Offset(address)) = value;
    }

    void PutU16(std::uint32_t address, std::uint16_t value)
    {
        PutU8(address, static_cast<std::uint8_t>(value >> 8));
        PutU8(address + 1, static_cast<std::uint8_t>(value));
    }

    void PutU32(std::uint32_t address, std::uint32_t value)
    {
        PutU8(address, static_cast<std::uint8_t>(value >> 24));
        PutU8(address + 1, static_cast<std::uint8_t>(value >> 16));
        PutU8(address + 2, static_cast<std::uint8_t>(value >> 8));
        PutU8(address + 3, static_cast<std::uint8_t>(value));
    }

    void PutF32Bits(std::uint32_t address, std::uint32_t bits)
    {
        PutU32(address, bits);
    }

private:
    static std::size_t Offset(std::uint32_t address)
    {
        return static_cast<std::size_t>(
            address - savor::MemView::kMem1Base);
    }

    std::vector<std::uint8_t> bytes_;
};

struct SeededMemory {
    static constexpr std::uint32_t Worksheet = 0x80350000u;
    static constexpr std::uint32_t Ground = 0x80360000u;
    static constexpr std::uint16_t GroundTableId = 0x1357u;
    static constexpr std::uint16_t GroundTblId = 0xa1b2u;

    Mem1Image image;

    explicit SeededMemory(bool with_ground = true)
    {
        image.PutU32(nav::PlayerWorksheetPointerAddress, Worksheet);
        image.PutU32(nav::AreaAddress, 0x12345678u);
        image.PutU8(nav::SubareaAddress, 0x9au);
        image.PutU32(nav::PostInputMovementSuppressAddress, 0xa1b2c3d4u);
        image.PutF32Bits(nav::StepDistanceCarryInAddress, 0x80000000u);

        image.PutF32Bits(Worksheet + 0x38u, 0x3f800000u);
        image.PutF32Bits(Worksheet + 0x3cu, 0xc0200000u);
        image.PutF32Bits(Worksheet + 0x40u, 0x7fc01234u);
        image.PutU32(Worksheet + 0x44u, 0x11111111u);
        image.PutU32(Worksheet + 0x48u, 0x22222222u);
        image.PutU32(Worksheet + 0x4cu, 0x33333333u);

        image.PutF32Bits(Worksheet + 0x104u, 0x40400000u);
        image.PutF32Bits(Worksheet + 0x108u, 0xc0880000u);
        image.PutF32Bits(Worksheet + 0x10cu, 0x00000001u);
        image.PutU32(Worksheet + 0x110u, 0x44444444u);
        image.PutU32(Worksheet + 0x114u, 0x55555555u);
        image.PutU32(Worksheet + 0x118u, 0x66666666u);

        image.PutU16(Worksheet + 0x170u, 1u);
        image.PutU16(Worksheet + 0x172u, 0x7788u);
        image.PutU32(
            Worksheet + nav::GroundSelectorPointerOffset,
            with_ground ? Ground : 0u);
        if (with_ground) {
            image.PutU16(Ground + 0x28u, GroundTableId);
            image.PutU16(Ground + 0x2au, GroundTblId);
        }
    }
};

std::uint32_t FloatBits(float value)
{
    return std::bit_cast<std::uint32_t>(value);
}

void ExpectContextFields(
    const nav::NavigationContext& value,
    bool with_ground)
{
    EXPECT_EQ(value.capture_pc, nav::CapturePc);
    EXPECT_EQ(value.player_worksheet, SeededMemory::Worksheet);
    EXPECT_EQ(value.area, 0x12345678u);
    EXPECT_EQ(value.subarea, 0x9au);
    EXPECT_EQ(value.motion_state, 1u);
    EXPECT_EQ(value.motion_substate, 0x7788u);
    EXPECT_EQ(value.post_input_movement_suppress, 0xa1b2c3d4u);
    EXPECT_EQ(FloatBits(value.position_x), 0x3f800000u);
    EXPECT_EQ(FloatBits(value.position_y), 0xc0200000u);
    EXPECT_EQ(FloatBits(value.position_z), 0x7fc01234u);
    EXPECT_EQ(value.rotation_x_raw, 0x11111111u);
    EXPECT_EQ(value.rotation_y_raw, 0x22222222u);
    EXPECT_EQ(value.rotation_z_raw, 0x33333333u);
    EXPECT_EQ(FloatBits(value.previous_position_x), 0x40400000u);
    EXPECT_EQ(FloatBits(value.previous_position_y), 0xc0880000u);
    EXPECT_EQ(FloatBits(value.previous_position_z), 0x00000001u);
    EXPECT_EQ(value.previous_rotation_x_raw, 0x44444444u);
    EXPECT_EQ(value.previous_rotation_y_raw, 0x55555555u);
    EXPECT_EQ(value.previous_rotation_z_raw, 0x66666666u);
    EXPECT_EQ(FloatBits(value.step_distance_carry_in), 0x80000000u);
    EXPECT_EQ(value.has_ground, with_ground);
    EXPECT_EQ(
        value.ground_tbl_id,
        with_ground ? SeededMemory::GroundTblId : 0u);
}

nav::NavigationContext ExtractValidContext(bool with_ground)
{
    SeededMemory seeded(with_ground);
    nav::NavigationContext value{};
    codec::ExtractFailure failure = codec::ExtractFailure::InvalidMem1;
    EXPECT_TRUE(codec::extract_from_mem1(
        seeded.image.view(),
        nav::CapturePc,
        value,
        &failure));
    EXPECT_EQ(failure, codec::ExtractFailure::None);
    return value;
}

} // namespace

TEST(NavigationContextCapture, ExtractsExactFieldsAndGroundTblId)
{
    SeededMemory seeded(true);
    nav::NavigationContext value{};
    codec::ExtractFailure failure = codec::ExtractFailure::InvalidMem1;
    ASSERT_TRUE(codec::extract_from_mem1(
        seeded.image.view(),
        nav::CapturePc,
        value,
        &failure));
    EXPECT_EQ(failure, codec::ExtractFailure::None);
    ExpectContextFields(value, true);
}

TEST(NavigationContextCapture, NullGroundSelectorIsAValidCapture)
{
    SeededMemory seeded(false);
    nav::NavigationContext value{};
    codec::ExtractFailure failure = codec::ExtractFailure::InvalidMem1;
    ASSERT_TRUE(codec::extract_from_mem1(
        seeded.image.view(),
        nav::CapturePc,
        value,
        &failure));
    EXPECT_EQ(failure, codec::ExtractFailure::None);
    ExpectContextFields(value, false);
}

TEST(NavigationContextCapture, RejectsInvalidMemoryPcWorksheetStateAndGround)
{
    nav::NavigationContext value{};
    codec::ExtractFailure failure = codec::ExtractFailure::None;
    const std::array<std::uint8_t, 4> short_memory{};
    EXPECT_FALSE(codec::extract_from_mem1(
        savor::MemView(short_memory.data(), short_memory.size()),
        nav::CapturePc,
        value,
        &failure));
    EXPECT_EQ(failure, codec::ExtractFailure::InvalidMem1);

    SeededMemory seeded;
    EXPECT_FALSE(codec::extract_from_mem1(
        seeded.image.view(),
        nav::CapturePc - 4,
        value,
        &failure));
    EXPECT_EQ(failure, codec::ExtractFailure::CapturePcMismatch);

    seeded.image.PutU32(nav::PlayerWorksheetPointerAddress, 0u);
    EXPECT_FALSE(codec::extract_from_mem1(
        seeded.image.view(),
        nav::CapturePc,
        value,
        &failure));
    EXPECT_EQ(failure, codec::ExtractFailure::InvalidWorksheet);

    seeded.image.PutU32(
        nav::PlayerWorksheetPointerAddress,
        savor::MemView::kMem1Base + savor::MemView::kMem1Size - 4u);
    EXPECT_FALSE(codec::extract_from_mem1(
        seeded.image.view(),
        nav::CapturePc,
        value,
        &failure));
    EXPECT_EQ(failure, codec::ExtractFailure::InvalidWorksheet);

    seeded.image.PutU32(
        nav::PlayerWorksheetPointerAddress,
        SeededMemory::Worksheet);
    seeded.image.PutU16(SeededMemory::Worksheet + 0x170u, 2u);
    EXPECT_FALSE(codec::extract_from_mem1(
        seeded.image.view(),
        nav::CapturePc,
        value,
        &failure));
    EXPECT_EQ(failure, codec::ExtractFailure::MotionStateMismatch);

    seeded.image.PutU16(SeededMemory::Worksheet + 0x170u, 1u);
    seeded.image.PutU32(
        SeededMemory::Worksheet + nav::GroundSelectorPointerOffset,
        savor::MemView::kMem1Base + savor::MemView::kMem1Size
            - nav::GroundTblIdOffset - 2u);
    EXPECT_FALSE(codec::extract_from_mem1(
        seeded.image.view(),
        nav::CapturePc,
        value,
        &failure));
    EXPECT_EQ(failure, codec::ExtractFailure::InvalidGroundSelector);
}

TEST(NavigationContextCodec, RoundTripsPortableLayoutsAndPreservesBits)
{
    for (const bool with_ground : {false, true}) {
        const auto source = ExtractValidContext(with_ground);
        std::string encoded;
        ASSERT_TRUE(codec::encode(source, encoded));
        EXPECT_EQ(
            encoded.size(),
            codec::EncodedSize);
        EXPECT_EQ(encoded.substr(0, 4), "NCTX");
        EXPECT_EQ(static_cast<std::uint8_t>(encoded[4]), codec::Version);
        EXPECT_EQ(
            static_cast<std::uint8_t>(encoded[6]),
            with_ground ? codec::FlagGroundPresent : 0u);
        if (with_ground) {
            EXPECT_EQ(
                static_cast<std::uint8_t>(
                    encoded[84]),
                static_cast<std::uint8_t>(
                    SeededMemory::GroundTblId));
            EXPECT_EQ(
                static_cast<std::uint8_t>(
                    encoded[85]),
                static_cast<std::uint8_t>(
                    SeededMemory::GroundTblId >> 8));
        } else {
            EXPECT_EQ(static_cast<std::uint8_t>(encoded[84]), 0u);
            EXPECT_EQ(static_cast<std::uint8_t>(encoded[85]), 0u);
        }

        nav::NavigationContext decoded{};
        ASSERT_TRUE(codec::decode(encoded, decoded));
        ExpectContextFields(decoded, with_ground);
    }
}

TEST(NavigationContextCodec, RejectsMalformedAndInconsistentEncodings)
{
    const auto without_ground = ExtractValidContext(false);
    std::string encoded;
    ASSERT_TRUE(codec::encode(without_ground, encoded));

    const auto rejected = [](std::string bytes) {
        nav::NavigationContext decoded{};
        return !codec::decode(bytes, decoded);
    };

    auto bad_magic = encoded;
    bad_magic[0] = 'X';
    EXPECT_TRUE(rejected(std::move(bad_magic)));

    auto bad_version = encoded;
    bad_version[4] = 2;
    EXPECT_TRUE(rejected(std::move(bad_version)));

    auto unknown_flags = encoded;
    unknown_flags[6] = 2;
    EXPECT_TRUE(rejected(std::move(unknown_flags)));

    auto nonzero_reserved = encoded;
    nonzero_reserved[21] = 1;
    EXPECT_TRUE(rejected(std::move(nonzero_reserved)));

    auto ground_id_without_flag = encoded;
    ground_id_without_flag[84] = 1;
    EXPECT_TRUE(rejected(std::move(ground_id_without_flag)));

    auto truncated = encoded;
    truncated.pop_back();
    EXPECT_TRUE(rejected(std::move(truncated)));

    auto trailing = encoded;
    trailing.push_back('\0');
    EXPECT_TRUE(rejected(std::move(trailing)));

    const auto with_ground = ExtractValidContext(true);
    ASSERT_TRUE(codec::encode(with_ground, encoded));
    auto ground_without_presence = encoded;
    ground_without_presence[6] = 0;
    EXPECT_TRUE(rejected(std::move(ground_without_presence)));
}

TEST(NavigationContextCodec, EncodeFailsClosedForInvalidModels)
{
    auto value = ExtractValidContext(false);
    std::string encoded = "stale";
    value.capture_pc = 0;
    EXPECT_FALSE(codec::encode(value, encoded));
    EXPECT_TRUE(encoded.empty());

    value = ExtractValidContext(false);
    value.player_worksheet = 0;
    EXPECT_FALSE(codec::encode(value, encoded));

    value = ExtractValidContext(false);
    value.motion_state = 2;
    EXPECT_FALSE(codec::encode(value, encoded));

    value = ExtractValidContext(false);
    value.ground_tbl_id = SeededMemory::GroundTblId;
    EXPECT_FALSE(codec::encode(value, encoded));

    value = ExtractValidContext(true);
    value.ground_tbl_id = 0;
    EXPECT_TRUE(codec::encode(value, encoded));
    nav::NavigationContext decoded{};
    ASSERT_TRUE(codec::decode(encoded, decoded));
    EXPECT_TRUE(decoded.has_ground);
    EXPECT_EQ(decoded.ground_tbl_id, 0u);
}

TEST(NavigationContextPayload, RoundTripsStrictInputsAndInjectsNeutralController)
{
    using namespace phase::navigation::ctx;
    const std::string output_path = "C:\\capture\\navigation.sav";
    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(encode_payload(
        {
            .output_savestate_path = output_path,
        },
        payload));
    ASSERT_FALSE(payload.empty());
    EXPECT_EQ(payload.front(), savor::PK_NavigationContextRunner);

    savor::PSContext context;
    ASSERT_TRUE(decode_payload(payload, context));
    std::uint32_t scalar = 0;
    std::string text;
    savor::GCInputFrame neutral{};
    ASSERT_TRUE(context.get(
        savor::context::key::navigation::OUTPUT_SAVESTATE_PATH,
        text));
    EXPECT_EQ(text, output_path);
    ASSERT_TRUE(context.get(
        savor::context::key::navigation::NEUTRAL_INPUT,
        neutral));
    EXPECT_EQ(neutral.buttons, 0u);
    EXPECT_EQ(neutral.main_x, 128u);
    EXPECT_EQ(neutral.main_y, 128u);
    EXPECT_EQ(neutral.c_x, 128u);
    EXPECT_EQ(neutral.c_y, 128u);
    EXPECT_EQ(neutral.trig_l, 0u);
    EXPECT_EQ(neutral.trig_r, 0u);
    ASSERT_TRUE(context.get(
        savor::context::key::navigation::OUTCOME,
        scalar));
    EXPECT_EQ(
        scalar,
        static_cast<std::uint32_t>(Outcome::Failed));
    ASSERT_TRUE(context.get(
        savor::context::key::navigation::FAILURE,
        scalar));
    EXPECT_EQ(
        scalar,
        static_cast<std::uint32_t>(FailureCode::None));
}

TEST(NavigationContextPayload, RejectsEmptyPathOldVersionAndTrailingData)
{
    using namespace phase::navigation::ctx;
    std::vector<std::uint8_t> payload{1, 2, 3};
    EXPECT_FALSE(encode_payload(
        {.output_savestate_path = ""},
        payload));

    ASSERT_TRUE(encode_payload(
        {.output_savestate_path = "out.sav"},
        payload));
    savor::PSContext context;

    auto wrong_kind = payload;
    wrong_kind[0] = savor::PK_BattleTurnRunner;
    EXPECT_FALSE(decode_payload(wrong_kind, context));

    auto wrong_version = payload;
    wrong_version[1] = 1;
    EXPECT_FALSE(decode_payload(wrong_version, context));

    auto truncated = payload;
    truncated.pop_back();
    EXPECT_FALSE(decode_payload(truncated, context));

    auto trailing = payload;
    trailing.push_back(0);
    EXPECT_FALSE(decode_payload(trailing, context));
}
