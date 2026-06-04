#pragma once

#include "Mesh/Buffer/BufferMesh.h"
#include "Mesh/Chunk/ChunkAttach.h"
#include "Mesh/Chunk/PolyChunks/BitsChunk.h"
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
            out.normal = chunk.has_normals() ? corner.normal : vertexCache[corner.index].normal;
            out.has_normal = chunk.has_normals();
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
        if (const auto bits = std::dynamic_pointer_cast<Chunk::PolyChunks::BitsChunk>(chunk)) {
            switch (bits->type) {
            case Chunk::PolyChunkType::BlendAlpha:
                material.source_blend_mode = bits->source_alpha();
                material.destination_blend_mode = bits->destination_alpha();
                break;
            case Chunk::PolyChunkType::MipmapDistanceMultiplier:
                material.mipmap_distance_multiplier = bits->mipmap_distance_multiplier();
                break;
            case Chunk::PolyChunkType::SpecularExponent:
                material.specular_exponent = static_cast<float>(bits->specular_exponent());
                break;
            default:
                break;
            }
            continue;
        }

        if (const auto texture = std::dynamic_pointer_cast<Chunk::PolyChunks::TextureChunk>(chunk)) {
            material.texture_index = texture->texture_id();
            material.mipmap_distance_multiplier = texture->mipmap_distance_multiplier();
            material.texture_filtering = texture->filter_mode();
            material.anisotropic_filtering = texture->super_sample();
            material.mirror_u = texture->mirror_u();
            material.mirror_v = texture->mirror_v();
            material.clamp_u = texture->clamp_u();
            material.clamp_v = texture->clamp_v();
            continue;
        }

        if (const auto materialChunk = std::dynamic_pointer_cast<Chunk::PolyChunks::MaterialChunk>(chunk)) {
            material.source_blend_mode = materialChunk->source_alpha();
            material.destination_blend_mode = materialChunk->destination_alpha();
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
        mesh.has_normals = hasVertexNormals || strip->has_normals();
        mesh.flat_shading = strip->flat_shading();
        mesh.material.flat = strip->flat_shading();
        mesh.material.no_ambient = strip->ignore_ambient();
        mesh.material.no_lighting = strip->ignore_light();
        mesh.material.no_specular = strip->ignore_specular();
        mesh.material.normal_mapping = strip->environment_mapping();
        mesh.material.use_texture = strip->texcoord_count() > 0 || strip->environment_mapping();
        mesh.material.use_alpha = strip->use_alpha();
        mesh.material.backface_culling = !strip->double_side();
        mesh.material.no_alpha_test = strip->no_alpha_test();
        mesh.vertex_read_offset = 0;
        if (!pendingVertices.empty()) {
            mesh.vertices = pendingVertices;
            mesh.continue_weight = continueWeight;
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
