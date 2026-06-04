#pragma once

#include "Mesh/Converters/ActivePolyChunkHelper.h"
#include "Mesh/Weighted/WeightedMesh.h"
#include "ObjectData/Node.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace Sa3Dport::Testing::Slice9 {

struct NormalizationSummary {
    std::uint32_t attach_count = 0;
    std::uint32_t buffer_mesh_count = 0;
    std::uint32_t buffer_vertex_count = 0;
    std::uint32_t buffer_corner_count = 0;
    std::uint32_t buffer_triangle_corner_count = 0;
    std::uint32_t weighted_mesh_count = 0;
    std::uint32_t weighted_vertex_count = 0;
    std::uint32_t weighted_triangle_set_count = 0;
    std::uint32_t weighted_triangle_corner_count = 0;
};

inline std::vector<std::optional<Mesh::Converters::ActivePolyChunkList>>
GetActivePolyChunks(const std::vector<ObjectData::NodePtr>& nodes) {
    return Mesh::Converters::get_active_poly_chunks(nodes);
}

inline NormalizationSummary SummarizeNodeTree(const ObjectData::NodePtr& root) {
    NormalizationSummary result;
    if (!root) {
        return result;
    }

    const auto nodes = root->tree_nodes();
    const auto activePolyChunks = GetActivePolyChunks(nodes);
    for (std::size_t nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex) {
        const auto chunkAttach = std::dynamic_pointer_cast<Mesh::Chunk::ChunkAttach>(nodes[nodeIndex]->attach);
        if (!chunkAttach) {
            continue;
        }

        const auto bufferMeshes = Mesh::Converters::buffer_chunk_attach_with_active_poly_chunks(
            *chunkAttach,
            activePolyChunks[nodeIndex]);
        if (bufferMeshes.empty()) {
            continue;
        }

        ++result.attach_count;
        result.buffer_mesh_count += static_cast<std::uint32_t>(bufferMeshes.size());
        for (const auto& mesh : bufferMeshes) {
            result.buffer_vertex_count += static_cast<std::uint32_t>(mesh.vertices.size());
            result.buffer_corner_count += static_cast<std::uint32_t>(mesh.corners.size());
            result.buffer_triangle_corner_count += static_cast<std::uint32_t>(mesh.corner_triangle_list().size());
        }

        const auto weighted = Mesh::Weighted::summarize_buffer_meshes(static_cast<std::uint32_t>(nodeIndex), bufferMeshes);
        if (weighted.vertex_count > 0 || weighted.triangle_corner_count > 0) {
            ++result.weighted_mesh_count;
            result.weighted_vertex_count += weighted.vertex_count;
            result.weighted_triangle_set_count += weighted.triangle_set_count;
            result.weighted_triangle_corner_count += weighted.triangle_corner_count;
        }
    }

    return result;
}

} // namespace Sa3Dport::Testing::Slice9
