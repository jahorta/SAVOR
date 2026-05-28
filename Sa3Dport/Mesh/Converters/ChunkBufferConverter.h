#pragma once

#include "Mesh/Buffer/BufferMesh.h"
#include "Mesh/Chunk/ChunkAttach.h"
#include "Mesh/Chunk/PolyChunks/MaterialBumpChunk.h"
#include "Mesh/Chunk/PolyChunks/MaterialChunk.h"
#include "Mesh/Chunk/PolyChunks/StripChunk.h"
#include "Mesh/Chunk/PolyChunks/TextureChunk.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace Sa3Dport::Mesh::Converters {

inline std::vector<Buffer::BufferCorner> convert_strip_chunk(
    const Chunk::PolyChunks::StripChunk& chunk,
    const std::vector<Chunk::Structs::ChunkVertex>& vertexCache) {
    std::vector<std::vector<Buffer::BufferCorner>> strips;
    std::vector<bool> reversed;
    strips.reserve(chunk.strips.size());
    reversed.reserve(chunk.strips.size());

    const bool hasColor = chunk.has_colors();
    for (const auto& strip : chunk.strips) {
        std::vector<Buffer::BufferCorner> bufferStrip;
        bufferStrip.reserve(strip.corners.size());
        for (const auto& corner : strip.corners) {
            Buffer::BufferCorner out;
            out.vertex_index = corner.index;
            out.color = hasColor ? corner.color : vertexCache[corner.index].diffuse;
            out.texcoord = corner.texcoord;
            bufferStrip.push_back(out);
        }
        strips.push_back(std::move(bufferStrip));
        reversed.push_back(strip.reversed);
    }

    return Buffer::join_strips(strips, reversed);
}

inline std::vector<Buffer::BufferMesh> buffer_chunk_attach(
    const Chunk::ChunkAttach& attach,
    const std::vector<std::optional<Chunk::PolyChunkPtr>>& polyChunks) {
    Buffer::BufferMaterial material;
    std::vector<Chunk::Structs::ChunkVertex> vertexCache(0x10000);
    std::vector<Buffer::BufferMesh> meshes;

    std::vector<Buffer::BufferVertex> pendingVertices;
    bool continueWeight = false;
    bool hasVertexNormals = false;
    bool hasVertexColors = false;
    std::uint16_t vertexWriteOffset = 0;

    for (std::size_t i = 0; i < attach.vertex_chunks.size(); ++i) {
        const auto& maybeChunk = attach.vertex_chunks[i];
        if (!maybeChunk.has_value()) {
            continue;
        }

        const auto& chunk = *maybeChunk;
        std::vector<Buffer::BufferVertex> vertices;
        vertices.reserve(chunk.vertices.size());

        if (!chunk.has_weight()) {
            for (std::size_t j = 0; j < chunk.vertices.size(); ++j) {
                const auto& source = chunk.vertices[j];
                vertexCache[j + chunk.index_offset] = source;
                vertices.push_back({source.position, source.normal, static_cast<std::uint16_t>(j), 1.0f});
            }
        } else {
            for (const auto& source : chunk.vertices) {
                vertexCache[source.index() + chunk.index_offset] = source;
                vertices.push_back({source.position, source.normal, source.index(), source.weight()});
            }
        }

        pendingVertices = std::move(vertices);
        continueWeight = chunk.weight_status() != Chunk::WeightStatus::Start;
        hasVertexNormals = chunk.has_normals();
        hasVertexColors = hasVertexColors || chunk.has_diffuse_colors();
        vertexWriteOffset = chunk.index_offset;

        if (i + 1 < attach.vertex_chunks.size() && !pendingVertices.empty()) {
            Buffer::BufferMesh mesh;
            mesh.vertices = pendingVertices;
            mesh.continue_weight = continueWeight;
            mesh.has_normals = hasVertexNormals;
            mesh.vertex_write_offset = vertexWriteOffset;
            meshes.push_back(std::move(mesh));
        }
    }

    for (const auto& maybePolyChunk : polyChunks) {
        if (!maybePolyChunk.has_value()) {
            continue;
        }

        const auto& chunk = *maybePolyChunk;
        if (const auto texture = std::dynamic_pointer_cast<Chunk::PolyChunks::TextureChunk>(chunk)) {
            material.texture_index = texture->texture_id();
            material.flags ^= (texture->mirror_u() ? 0x1u : 0u);
            material.flags ^= (texture->mirror_v() ? 0x2u : 0u);
            material.flags ^= (texture->clamp_u() ? 0x4u : 0u);
            material.flags ^= (texture->clamp_v() ? 0x8u : 0u);
            continue;
        }

        if (const auto materialChunk = std::dynamic_pointer_cast<Chunk::PolyChunks::MaterialChunk>(chunk)) {
            material.flags ^= (static_cast<std::uint32_t>(materialChunk->attributes) << 8u);
            continue;
        }

        const auto strip = std::dynamic_pointer_cast<Chunk::PolyChunks::StripChunk>(chunk);
        if (!strip) {
            continue;
        }

        const auto corners = convert_strip_chunk(*strip, vertexCache);
        if (corners.empty()) {
            continue;
        }

        Buffer::BufferMesh mesh;
        mesh.material = material;
        mesh.corners = corners;
        mesh.strippified = true;
        mesh.has_colors = strip->has_colors() || hasVertexColors;
        mesh.vertex_read_offset = 0;
        if (!pendingVertices.empty()) {
            mesh.vertices = pendingVertices;
            mesh.continue_weight = continueWeight;
            mesh.has_normals = hasVertexNormals;
            mesh.vertex_write_offset = vertexWriteOffset;
            pendingVertices.clear();
        }
        meshes.push_back(std::move(mesh));
    }

    if (!pendingVertices.empty()) {
        Buffer::BufferMesh mesh;
        mesh.vertices = pendingVertices;
        mesh.continue_weight = continueWeight;
        mesh.has_normals = hasVertexNormals;
        mesh.vertex_write_offset = vertexWriteOffset;
        meshes.push_back(std::move(mesh));
    }

    return Buffer::compress_layout(meshes);
}

inline std::vector<Buffer::BufferMesh> buffer_chunk_attach(const Chunk::ChunkAttach& attach) {
    return buffer_chunk_attach(attach, attach.poly_chunks);
}

} // namespace Sa3Dport::Mesh::Converters
