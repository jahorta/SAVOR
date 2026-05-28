#pragma once

#include "Mesh/Attach.h"
#include "Mesh/Chunk/PolyChunkDispatch.h"
#include "Mesh/Chunk/VertexChunk.h"
#include "Mesh/Enums.h"
#include "Structs/Bounds.h"
#include "Structs/EndianStackReader.h"
#include "Structs/Vector3.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace Sa3Dport::Mesh::Chunk {

class ChunkAttach final : public Attach {
public:
    std::vector<std::optional<VertexChunk>> vertex_chunks;
    std::vector<std::optional<PolyChunkPtr>> poly_chunks;
    std::uint32_t vertex_chunks_address = 0;
    std::uint32_t poly_chunks_address = 0;
    bool poly_chunks_deferred = false;

    [[nodiscard]] AttachFormat format() const override {
        return AttachFormat::CHUNK;
    }

    [[nodiscard]] bool check_has_weights() const override {
        for (const auto& chunk : vertex_chunks) {
            if (chunk.has_value() && chunk->has_weight()) {
                return true;
            }
        }
        return false;
    }

    void recalculate_bounds_from_vertices() {
        if (vertex_chunks.empty() || check_has_weights()) {
            mesh_bounds = {};
            return;
        }

        std::vector<Sa3Dport::Structs::Vector3> points;
        for (const auto& chunk : vertex_chunks) {
            if (!chunk.has_value()) {
                continue;
            }
            for (const auto& vertex : chunk->vertices) {
                points.push_back(vertex.position);
            }
        }

        mesh_bounds = points.empty() ? Sa3Dport::Structs::Bounds{} : Sa3Dport::Structs::Bounds::from_points(points);
    }

    [[nodiscard]] static std::shared_ptr<ChunkAttach> read(const Sa3Dport::Structs::EndianStackReader& reader,
                                                           std::uint32_t address,
                                                           AttachReadContext& context) {
        if (const auto cached = context.attaches.find(address); cached != context.attaches.end()) {
            return std::static_pointer_cast<ChunkAttach>(cached->second);
        }

        auto result = std::make_shared<ChunkAttach>();
        result->source_address = address;
        result->label = "attach_0x" + hex_address(address);
        context.attaches.emplace(address, result);

        if (const auto vertexAddress = read_pointer(reader, address, context.image_base)) {
            result->vertex_chunks_address = *vertexAddress;
            result->vertex_chunks = VertexChunk::read_array(reader, *vertexAddress);
        }

        if (const auto polyAddress = read_pointer(reader, address + 4u, context.image_base)) {
            result->poly_chunks_address = *polyAddress;
            result->poly_chunks = PolyChunk::read_array(reader, *polyAddress);
        }

        result->mesh_bounds = Sa3Dport::Structs::Bounds::read(reader, address + 8u);
        return result;
    }

private:
    static std::string hex_address(std::uint32_t address) {
        constexpr char digits[] = "0123456789abcdef";
        std::string result(8, '0');
        for (int i = 7; i >= 0; --i) {
            result[static_cast<std::size_t>(i)] = digits[address & 0xFu];
            address >>= 4;
        }
        return result;
    }
};

} // namespace Sa3Dport::Mesh::Chunk
