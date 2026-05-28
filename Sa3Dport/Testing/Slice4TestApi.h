#pragma once

#include "Mesh/AttachDispatch.h"
#include "Mesh/Chunk/ChunkAttach.h"
#include "Mesh/Chunk/Structs/ChunkStrip.h"
#include "Mesh/Chunk/VertexChunk.h"
#include "ObjectData/Node.h"

namespace Sa3Dport::Testing::Slice4 {

using AttachFormat = Mesh::AttachFormat;
using AttachPtr = Mesh::AttachPtr;
using AttachReadContext = Mesh::AttachReadContext;
using ChunkAttach = Mesh::Chunk::ChunkAttach;
using ChunkCorner = Mesh::Chunk::Structs::ChunkCorner;
using ChunkStrip = Mesh::Chunk::Structs::ChunkStrip;
using ChunkVertex = Mesh::Chunk::Structs::ChunkVertex;
using ModelFormat = ObjectData::Enums::ModelFormat;
using Node = ObjectData::Node;
using NodeReadContext = ObjectData::NodeReadContext;
using VertexChunk = Mesh::Chunk::VertexChunk;
using VertexChunkType = Mesh::Chunk::VertexChunkType;
using WeightStatus = Mesh::Chunk::WeightStatus;

inline AttachPtr ReadAttach(const Structs::EndianStackReader& reader,
                            std::uint32_t address,
                            ModelFormat format,
                            AttachReadContext& context) {
    return Mesh::Attach::read(reader, address, format, context);
}

inline std::shared_ptr<ChunkAttach> ReadChunkAttach(const Structs::EndianStackReader& reader,
                                                    std::uint32_t address,
                                                    AttachReadContext& context) {
    return Mesh::Chunk::ChunkAttach::read(reader, address, context);
}

} // namespace Sa3Dport::Testing::Slice4
