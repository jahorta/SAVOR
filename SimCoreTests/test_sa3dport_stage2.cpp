#include "gtest/gtest.h"

#include "Testing/Slice2TestApi.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace {

using namespace Sa3Dport::Testing::Slice2;
namespace F = Sa3Dport::File;
namespace S = Sa3Dport::Structs;

void WriteU32Le(std::vector<std::byte>& data, std::uint32_t offset, std::uint32_t value) {
    if (data.size() < offset + 4u) {
        data.resize(offset + 4u);
    }

    data[offset] = std::byte(value & 0xFFu);
    data[offset + 1u] = std::byte((value >> 8u) & 0xFFu);
    data[offset + 2u] = std::byte((value >> 16u) & 0xFFu);
    data[offset + 3u] = std::byte((value >> 24u) & 0xFFu);
}

void WriteU32Be(std::vector<std::byte>& data, std::uint32_t offset, std::uint32_t value) {
    if (data.size() < offset + 4u) {
        data.resize(offset + 4u);
    }

    data[offset] = std::byte((value >> 24u) & 0xFFu);
    data[offset + 1u] = std::byte((value >> 16u) & 0xFFu);
    data[offset + 2u] = std::byte((value >> 8u) & 0xFFu);
    data[offset + 3u] = std::byte(value & 0xFFu);
}

void WriteString(std::vector<std::byte>& data, std::uint32_t offset, std::string_view value) {
    if (data.size() < offset + value.size() + 1u) {
        data.resize(offset + value.size() + 1u);
    }

    for (std::size_t i = 0; i < value.size(); ++i) {
        data[offset + i] = std::byte(static_cast<unsigned char>(value[i]));
    }

    data[offset + value.size()] = std::byte {0};
}

TEST(Sa3DportStage2, ScansNjBlocksInOrderAndClassifiesRoles) {
    std::vector<std::byte> data(36);
    WriteU32Le(data, 0, F::FileHeaders::TextureListBlockHeaders[0]);
    WriteU32Le(data, 4, 4);
    WriteU32Le(data, 12, F::FileHeaders::ModelBlockHeaders[0]);
    WriteU32Le(data, 16, 8);
    WriteU32Le(data, 28, F::FileHeaders::AnimationBlockHeaders[0]);
    WriteU32Le(data, 32, 4);

    const auto result = ScanNjBlocks(data);

    ASSERT_EQ(result.blocks.size(), 3u);
    EXPECT_EQ(result.blocks[0].offset, 0u);
    EXPECT_EQ(result.blocks[0].role, NJBlockRole::Texture);
    EXPECT_EQ(result.blocks[1].offset, 12u);
    EXPECT_EQ(result.blocks[1].role, NJBlockRole::Model);
    EXPECT_EQ(result.blocks[2].offset, 28u);
    EXPECT_EQ(result.blocks[2].role, NJBlockRole::Animation);
    EXPECT_EQ(RoleName(result.blocks[2].role), "animation");
    ASSERT_TRUE(FindFirstBlock(result.blocks, ModelBlockHeaders).has_value());
    EXPECT_EQ(*FindFirstBlock(result.blocks, ModelBlockHeaders), 12u);
}

TEST(Sa3DportStage2, DetectsBigEndianBlockSizesButKeepsHeaderLittleEndian) {
    std::vector<std::byte> data(12);
    WriteU32Le(data, 0, F::FileHeaders::ModelBlockHeaders[1]);
    WriteU32Be(data, 4, 4);

    const auto result = ScanNjBlocks(data);

    EXPECT_EQ(result.size_endian, S::Endian::Big);
    ASSERT_EQ(result.blocks.size(), 1u);
    EXPECT_EQ(result.blocks[0].header, F::FileHeaders::ModelBlockHeaders[1]);
    EXPECT_EQ(result.blocks[0].size, 4u);
}

TEST(Sa3DportStage2, DoesNotClassifyGJHeadersForSkiesScope) {
    constexpr std::uint32_t gjcm = (F::FileHeaders::NJCM & 0xFFFF0000u) | 0x4A47u;
    constexpr std::uint32_t gjtl = (F::FileHeaders::NJTL & 0xFFFF0000u) | 0x4A47u;

    EXPECT_EQ(ClassifyHeader(gjcm), NJBlockRole::None);
    EXPECT_EQ(ClassifyHeader(gjtl), NJBlockRole::None);
}

TEST(Sa3DportStage2, StopsOnZeroHeaderOrSize) {
    std::vector<std::byte> data(24);
    WriteU32Le(data, 0, F::FileHeaders::ModelBlockHeaders[0]);
    WriteU32Le(data, 4, 4);
    WriteU32Le(data, 12, F::FileHeaders::AnimationBlockHeaders[0]);
    WriteU32Le(data, 16, 0);

    const auto result = ScanNjBlocks(data);

    ASSERT_EQ(result.blocks.size(), 1u);
    EXPECT_TRUE(result.diagnostics.empty());
}

TEST(Sa3DportStage2, ReportsTruncatedBlockHeader) {
    std::vector<std::byte> data(14);
    WriteU32Le(data, 0, F::FileHeaders::ModelBlockHeaders[0]);
    WriteU32Le(data, 4, 4);

    const auto result = ScanNjBlocks(data);

    ASSERT_EQ(result.blocks.size(), 1u);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0], "truncated_block_header");
}

TEST(Sa3DportStage2, ReadsMetadataVersion3Shell) {
    std::vector<std::byte> data(120);
    WriteU32Le(data, 0, 4);

    WriteU32Le(data, 4, static_cast<std::uint32_t>(F::MetaBlockType::Label));
    WriteU32Le(data, 8, 24);
    WriteU32Le(data, 12, 0x12345678u);
    WriteU32Le(data, 16, 40);
    WriteU32Le(data, 20, UINT32_MAX);
    WriteU32Le(data, 24, UINT32_MAX);
    WriteString(data, 28, "root");

    WriteU32Le(data, 36, static_cast<std::uint32_t>(F::MetaBlockType::Author));
    WriteU32Le(data, 40, 8);
    WriteString(data, 44, "tool");

    WriteU32Le(data, 52, 0xDEADBEEFu);
    WriteU32Le(data, 56, 4);
    WriteU32Le(data, 60, 0xCAFEBABEu);

    WriteU32Le(data, 64, static_cast<std::uint32_t>(F::MetaBlockType::End));
    WriteU32Le(data, 68, 0);

    const auto meta = ReadMetadataShell(data, 0, 3, false);
    const auto summary = SummarizeMetadata(meta);

    ASSERT_EQ(meta.labels.size(), 1u);
    EXPECT_EQ(meta.labels.at(0x12345678u), "root");
    EXPECT_EQ(meta.author, "tool");
    EXPECT_EQ(summary.label_count, 1u);
    EXPECT_TRUE(summary.has_author);
    EXPECT_EQ(summary.unknown_block_count, 1u);
    EXPECT_TRUE(meta.other.contains(0xDEADBEEFu));
}

TEST(Sa3DportStage2, ReadsMetadataWeightCounts) {
    std::vector<std::byte> data(96);
    WriteU32Le(data, 0, 4);

    WriteU32Le(data, 4, static_cast<std::uint32_t>(F::MetaBlockType::Weight));
    WriteU32Le(data, 8, 52);
    WriteU32Le(data, 12, 0x1000u);
    WriteU32Le(data, 16, 1);
    WriteU32Le(data, 20, 7);
    WriteU32Le(data, 24, 2);
    WriteU32Le(data, 28, 0x2000u);
    WriteU32Le(data, 32, 3);
    WriteU32Le(data, 36, 0x3F000000u);
    WriteU32Le(data, 40, 0x3000u);
    WriteU32Le(data, 44, 4);
    WriteU32Le(data, 48, 0x3F800000u);
    WriteU32Le(data, 52, UINT32_MAX);

    WriteU32Le(data, 64, static_cast<std::uint32_t>(F::MetaBlockType::End));
    WriteU32Le(data, 68, 0);

    const auto summary = SummarizeMetadata(ReadMetadataShell(data, 0, 3, false));

    EXPECT_EQ(summary.meta_weight_node_count, 1u);
    EXPECT_EQ(summary.meta_weight_vertex_count, 1u);
    EXPECT_EQ(summary.meta_weight_count, 2u);
}

} // namespace
