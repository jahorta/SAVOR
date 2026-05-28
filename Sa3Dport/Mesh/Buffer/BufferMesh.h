#pragma once

#include "Mesh/Chunk/Structs/ChunkCorner.h"
#include "Mesh/Chunk/Structs/ChunkVertex.h"
#include "Mesh/Chunk/VertexChunk.h"
#include "Mesh/Chunk/WeightStatus.h"
#include "Structs/Color.h"
#include "Structs/Vector2.h"
#include "Structs/Vector3.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace Sa3Dport::Mesh::Buffer {

struct BufferVertex {
    Structs::Vector3 position {};
    Structs::Vector3 normal = Structs::Vector3::unit_y();
    std::uint16_t index = 0;
    float weight = 1.0f;
};

struct BufferCorner {
    std::uint16_t vertex_index = 0;
    Structs::Color color = Structs::Color(0xFF, 0xFF, 0xFF, 0xFF);
    Structs::Vector2 texcoord {};
};

struct BufferMaterial {
    std::uint32_t texture_index = 0;
    std::uint32_t flags = 0;
};

struct BufferMesh {
    std::vector<BufferVertex> vertices;
    std::vector<BufferCorner> corners;
    BufferMaterial material {};
    bool strippified = false;
    bool continue_weight = false;
    bool has_normals = false;
    bool has_colors = false;
    std::uint16_t vertex_write_offset = 0;
    std::uint16_t vertex_read_offset = 0;

    [[nodiscard]] bool has_vertices() const { return !vertices.empty(); }
    [[nodiscard]] bool has_corners() const { return !corners.empty(); }

    [[nodiscard]] std::vector<BufferCorner> corner_triangle_list() const {
        if (!strippified) {
            return corners;
        }

        std::vector<BufferCorner> result;
        bool reversed = false;
        for (std::size_t i = 2; i < corners.size(); ++i, reversed = !reversed) {
            const auto& c1 = corners[i - 2];
            const auto& c2 = corners[i - 1];
            const auto& c3 = corners[i];
            if (c1.vertex_index == c2.vertex_index ||
                c2.vertex_index == c3.vertex_index ||
                c3.vertex_index == c1.vertex_index) {
                continue;
            }

            if (reversed) {
                result.push_back(c2);
                result.push_back(c1);
                result.push_back(c3);
            } else {
                result.push_back(c1);
                result.push_back(c2);
                result.push_back(c3);
            }
        }
        return result;
    }
};

inline std::vector<BufferCorner> join_strips(const std::vector<std::vector<BufferCorner>>& strips,
                                             const std::vector<bool>& reversed) {
    std::vector<BufferCorner> result;
    bool realRev = false;
    for (std::size_t i = 0; i < strips.size(); ++i) {
        const auto& strip = strips[i];
        if (strip.empty()) {
            continue;
        }

        if (i > 0) {
            result.push_back(strip.front());
            realRev = !realRev;
        }

        if (realRev != (i < reversed.size() && reversed[i])) {
            result.push_back(strip.front());
            realRev = !realRev;
        }

        for (const auto& item : strip) {
            result.push_back(item);
            realRev = !realRev;
        }

        if (i + 1 < strips.size()) {
            result.push_back(strip.back());
            realRev = !realRev;
        }
    }
    return result;
}

inline std::vector<BufferMesh> compress_layout(const std::vector<BufferMesh>& input) {
    std::vector<BufferMesh> result;
    for (const auto& mesh : input) {
        if (!mesh.has_vertices() && !mesh.has_corners()) {
            continue;
        }

        if (mesh.has_vertices()) {
            result.push_back(mesh);
            continue;
        }

        if (!result.empty() && !result.back().has_corners()) {
            result.back().corners = mesh.corners;
            result.back().material = mesh.material;
            result.back().has_colors = mesh.has_colors;
            result.back().strippified = mesh.strippified;
            result.back().vertex_read_offset = mesh.vertex_read_offset;
        } else {
            result.push_back(mesh);
        }
    }
    return result;
}

} // namespace Sa3Dport::Mesh::Buffer
