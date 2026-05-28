#pragma once

#include "Mesh/Buffer/BufferMesh.h"

#include <cstdint>
#include <set>
#include <vector>

namespace Sa3Dport::Mesh::Weighted {

struct WeightedMeshSummary {
    std::uint32_t root_index = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t triangle_set_count = 0;
    std::uint32_t triangle_corner_count = 0;
    std::uint32_t depending_node_count = 0;
    bool has_colors = false;
    bool has_normals = false;
};

inline WeightedMeshSummary summarize_buffer_meshes(std::uint32_t rootIndex,
                                                   const std::vector<Buffer::BufferMesh>& meshes) {
    WeightedMeshSummary result;
    result.root_index = rootIndex;

    std::set<std::uint16_t> usedVertices;
    bool hasWeights = false;
    for (const auto& mesh : meshes) {
        if (mesh.has_normals) {
            result.has_normals = true;
        }
        if (mesh.has_colors) {
            result.has_colors = true;
        }
        for (const auto& vertex : mesh.vertices) {
            if (mesh.continue_weight || vertex.weight != 1.0f) {
                hasWeights = true;
            }
        }
        if (!mesh.has_corners()) {
            continue;
        }

        ++result.triangle_set_count;
        const auto triangles = mesh.corner_triangle_list();
        result.triangle_corner_count += static_cast<std::uint32_t>(triangles.size());
        for (const auto& corner : mesh.corners) {
            usedVertices.insert(static_cast<std::uint16_t>(corner.vertex_index + mesh.vertex_read_offset));
        }
    }

    result.vertex_count = static_cast<std::uint32_t>(usedVertices.size());
    result.depending_node_count = hasWeights ? 1u : 0u;
    return result;
}

} // namespace Sa3Dport::Mesh::Weighted
