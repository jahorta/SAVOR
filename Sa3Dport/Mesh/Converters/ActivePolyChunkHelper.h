#pragma once

#include "Mesh/Chunk/ChunkAttach.h"
#include "Mesh/Chunk/PolyChunks/BitsChunk.h"
#include "Mesh/Converters/ChunkBufferConverter.h"
#include "ObjectData/Node.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace Sa3Dport::Mesh::Converters {

using ActivePolyChunkList = std::vector<std::optional<Mesh::Chunk::PolyChunkPtr>>;

inline std::vector<std::optional<ActivePolyChunkList>> get_active_poly_chunks(
    const std::vector<ObjectData::NodePtr>& nodes) {
    std::vector<std::optional<ActivePolyChunkList>> result(nodes.size());
    std::vector<ActivePolyChunkList> cache;

    for (std::size_t nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex) {
        const auto chunkAttach = std::dynamic_pointer_cast<Mesh::Chunk::ChunkAttach>(nodes[nodeIndex]->attach);
        if (!chunkAttach || chunkAttach->poly_chunks.empty()) {
            continue;
        }

        ActivePolyChunkList active;
        int cacheId = -1;
        for (const auto& maybePolyChunk : chunkAttach->poly_chunks) {
            if (!maybePolyChunk.has_value()) {
                if (cacheId >= 0) {
                    cache[static_cast<std::size_t>(cacheId)].push_back(std::nullopt);
                } else {
                    active.push_back(std::nullopt);
                }
                continue;
            }

            const auto& polyChunk = *maybePolyChunk;
            if (const auto bits = std::dynamic_pointer_cast<Mesh::Chunk::PolyChunks::BitsChunk>(polyChunk)) {
                if (bits->type == Mesh::Chunk::PolyChunkType::CacheList) {
                    cacheId = bits->list();
                    if (cache.size() <= static_cast<std::size_t>(cacheId)) {
                        cache.resize(static_cast<std::size_t>(cacheId) + 1u);
                    }
                    cache[static_cast<std::size_t>(cacheId)].clear();
                    continue;
                }

                if (bits->type == Mesh::Chunk::PolyChunkType::DrawList) {
                    const auto listId = static_cast<std::size_t>(bits->list());
                    if (listId < cache.size()) {
                        active.insert(active.end(), cache[listId].begin(), cache[listId].end());
                    }
                    continue;
                }
            }

            if (cacheId >= 0) {
                cache[static_cast<std::size_t>(cacheId)].push_back(maybePolyChunk);
            } else {
                active.push_back(maybePolyChunk);
            }
        }

        if (!active.empty()) {
            result[nodeIndex] = std::move(active);
        }
    }

    return result;
}

inline std::vector<Buffer::BufferMesh> buffer_chunk_attach_with_active_poly_chunks(
    const Mesh::Chunk::ChunkAttach& attach,
    const std::optional<ActivePolyChunkList>& activePolyChunks) {
    return activePolyChunks.has_value()
        ? buffer_chunk_attach(attach, *activePolyChunks)
        : buffer_chunk_attach(attach, {});
}

} // namespace Sa3Dport::Mesh::Converters
