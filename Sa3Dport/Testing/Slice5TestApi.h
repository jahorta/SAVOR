#pragma once

#include "Mesh/AttachDispatch.h"
#include "Mesh/Chunk/ChunkAttach.h"
#include "Mesh/Chunk/PolyChunkDispatch.h"
#include "Mesh/Chunk/PolyChunks/BitsChunk.h"
#include "Mesh/Chunk/PolyChunks/MaterialBumpChunk.h"
#include "Mesh/Chunk/PolyChunks/MaterialChunk.h"
#include "Mesh/Chunk/PolyChunks/StripChunk.h"
#include "Mesh/Chunk/PolyChunks/TextureChunk.h"
#include "ObjectData/Node.h"

namespace Sa3Dport::Testing::Slice5 {

using AttachReadContext = Mesh::AttachReadContext;
using ChunkAttach = Mesh::Chunk::ChunkAttach;
using BitsChunk = Mesh::Chunk::PolyChunks::BitsChunk;
using MaterialBumpChunk = Mesh::Chunk::PolyChunks::MaterialBumpChunk;
using MaterialChunk = Mesh::Chunk::PolyChunks::MaterialChunk;
using PolyChunk = Mesh::Chunk::PolyChunk;
using PolyChunkPtr = Mesh::Chunk::PolyChunkPtr;
using PolyChunkType = Mesh::Chunk::PolyChunkType;
using StripChunk = Mesh::Chunk::PolyChunks::StripChunk;
using TextureChunk = Mesh::Chunk::PolyChunks::TextureChunk;

inline PolyChunkPtr ReadPolyChunk(const Structs::EndianStackReader& reader, std::uint32_t& address) {
    return Mesh::Chunk::PolyChunk::read(reader, address);
}

inline std::vector<std::optional<PolyChunkPtr>> ReadPolyChunks(const Structs::EndianStackReader& reader,
                                                               std::uint32_t address) {
    return Mesh::Chunk::PolyChunk::read_array(reader, address);
}

inline std::shared_ptr<ChunkAttach> ReadChunkAttach(const Structs::EndianStackReader& reader,
                                                    std::uint32_t address,
                                                    AttachReadContext& context) {
    return Mesh::Chunk::ChunkAttach::read(reader, address, context);
}

} // namespace Sa3Dport::Testing::Slice5
