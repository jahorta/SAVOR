#include "gtest/gtest.h"

#include "Testing/Slice4TestApi.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

using namespace Sa3Dport::Testing::Slice4;
namespace S = Sa3Dport::Structs;

void WriteU16(std::vector<std::byte>& data, std::uint32_t offset, std::uint16_t value) {
    if (data.size() < offset + 2u) {
        data.resize(offset + 2u);
    }
    data[offset] = std::byte(value & 0xFFu);
    data[offset + 1u] = std::byte((value >> 8u) & 0xFFu);
}

void WriteU32(std::vector<std::byte>& data, std::uint32_t offset, std::uint32_t value) {
    if (data.size() < offset + 4u) {
        data.resize(offset + 4u);
    }
    data[offset] = std::byte(value & 0xFFu);
    data[offset + 1u] = std::byte((value >> 8u) & 0xFFu);
    data[offset + 2u] = std::byte((value >> 16u) & 0xFFu);
    data[offset + 3u] = std::byte((value >> 24u) & 0xFFu);
}

void WriteF32(std::vector<std::byte>& data, std::uint32_t offset, float value) {
    WriteU32(data, offset, std::bit_cast<std::uint32_t>(value));
}

void WriteVec3(std::vector<std::byte>& data, std::uint32_t offset, S::Vector3 value) {
    WriteF32(data, offset, value.x);
    WriteF32(data, offset + 4u, value.y);
    WriteF32(data, offset + 8u, value.z);
}

void WriteBlankVertexChunk(std::vector<std::byte>& data, std::uint32_t offset) {
    WriteU32(data, offset, static_cast<std::uint32_t>(VertexChunkType::Blank) | (4u << 16u));
    WriteU32(data, offset + 4u, (2u << 16u) | 3u);
    WriteVec3(data, offset + 8u, {1.0f, 2.0f, 3.0f});
    WriteVec3(data, offset + 20u, {4.0f, 5.0f, 6.0f});
    WriteU32(data, offset + 32u, static_cast<std::uint32_t>(VertexChunkType::End));
    WriteU32(data, offset + 36u, 0);
}

void WriteChunkAttach(std::vector<std::byte>& data, std::uint32_t offset, std::uint32_t vertexAddress, std::uint32_t polyAddress) {
    WriteU32(data, offset, vertexAddress);
    WriteU32(data, offset + 4u, polyAddress);
    WriteVec3(data, offset + 8u, {7.0f, 8.0f, 9.0f});
    WriteF32(data, offset + 20u, 10.0f);
}

TEST(Sa3DportStage4, ReadsChunkAttachHeaderBoundsAndVertexChunks) {
    std::vector<std::byte> data(0x80);
    WriteChunkAttach(data, 0, 0x20, 0x70);
    WriteBlankVertexChunk(data, 0x20);
    WriteU16(data, 0x70, 0xFF);

    S::EndianStackReader reader(data, S::Endian::Little);
    AttachReadContext context;
    const auto attach = ReadChunkAttach(reader, 0, context);

    ASSERT_NE(attach, nullptr);
    EXPECT_EQ(attach->format(), AttachFormat::CHUNK);
    EXPECT_EQ(attach->vertex_chunks_address, 0x20u);
    EXPECT_EQ(attach->poly_chunks_address, 0x70u);
    EXPECT_FALSE(attach->poly_chunks_deferred);
    EXPECT_TRUE(attach->poly_chunks.empty());
    ASSERT_EQ(attach->vertex_chunks.size(), 1u);
    ASSERT_TRUE(attach->vertex_chunks[0].has_value());
    EXPECT_EQ(attach->vertex_chunks[0]->type, VertexChunkType::Blank);
    EXPECT_EQ(attach->vertex_chunks[0]->index_offset, 3u);
    ASSERT_EQ(attach->vertex_chunks[0]->vertices.size(), 2u);
    EXPECT_FLOAT_EQ(attach->vertex_chunks[0]->vertices[1].position.z, 6.0f);
    EXPECT_FLOAT_EQ(attach->mesh_bounds.position().x, 7.0f);
    EXPECT_FLOAT_EQ(attach->mesh_bounds.radius(), 10.0f);
}

TEST(Sa3DportStage4, AttachDispatchAllowsOnlySA2ChunkForThisSlice) {
    std::vector<std::byte> data(0x40);
    WriteChunkAttach(data, 0, 0, 0);

    S::EndianStackReader reader(data, S::Endian::Little);
    AttachReadContext context;
    EXPECT_EQ(ReadAttach(reader, 0, ModelFormat::SA2, context)->format(), AttachFormat::CHUNK);
    EXPECT_THROW(ReadAttach(reader, 0, ModelFormat::SA1, context), std::invalid_argument);
}

TEST(Sa3DportStage4, NodeReadCanResolveChunkAttachWhenRequested) {
    std::vector<std::byte> data(0xA0);
    WriteU32(data, 0, 0);
    WriteU32(data, 4, 0x34);
    WriteVec3(data, 8, {});
    WriteVec3(data, 20, {});
    WriteVec3(data, 32, S::Vector3::one());
    WriteU32(data, 44, 0);
    WriteU32(data, 48, 0);
    WriteU32(data, 52, 0);
    WriteChunkAttach(data, 0x34, 0x60, 0);
    WriteBlankVertexChunk(data, 0x60);

    S::EndianStackReader reader(data, S::Endian::Little);
    NodeReadContext context;
    context.read_attach = true;
    const auto node = Node::read(reader, 0, ModelFormat::SA2, context);

    ASSERT_NE(node->attach, nullptr);
    EXPECT_EQ(node->attach->format(), AttachFormat::CHUNK);
    const auto chunkAttach = std::dynamic_pointer_cast<ChunkAttach>(node->attach);
    ASSERT_NE(chunkAttach, nullptr);
    ASSERT_EQ(chunkAttach->vertex_chunks.size(), 1u);
}

TEST(Sa3DportStage4, ReadsChunkStripCornersWithTexcoordsAndAttributes) {
    std::vector<std::byte> data(32);
    WriteU16(data, 0, static_cast<std::uint16_t>(-3));
    WriteU16(data, 2, 1);
    WriteU16(data, 4, 128);
    WriteU16(data, 6, 64);
    WriteU16(data, 8, 2);
    WriteU16(data, 10, 256);
    WriteU16(data, 12, 128);
    WriteU16(data, 14, 3);
    WriteU16(data, 16, 512);
    WriteU16(data, 18, 256);
    WriteU16(data, 20, 0xAA55);

    S::EndianStackReader reader(data, S::Endian::Little);
    std::uint32_t address = 0;
    const auto strip = ChunkStrip::read(reader, address, 1, false, false, false, 1);

    EXPECT_TRUE(strip.reversed);
    ASSERT_EQ(strip.corners.size(), 3u);
    EXPECT_EQ(strip.corners[2].index, 3u);
    EXPECT_FLOAT_EQ(strip.corners[2].texcoord.x, 2.0f);
    EXPECT_EQ(strip.corners[2].attributes1, 0xAA55u);
    EXPECT_EQ(address, 22u);
}

} // namespace
