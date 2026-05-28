#pragma once

#include "Mesh/Chunk/PolyChunkType.h"
#include "Structs/EndianStackReader.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace Sa3Dport::Mesh::Chunk {

class PolyChunk {
public:
    PolyChunkType type = PolyChunkType::Null;
    std::uint8_t attributes = 0;

    explicit PolyChunk(PolyChunkType chunkType) : type(chunkType) {}
    virtual ~PolyChunk() = default;

    [[nodiscard]] virtual std::uint32_t byte_size() const = 0;

    [[nodiscard]] static std::shared_ptr<PolyChunk> read(const Sa3Dport::Structs::EndianStackReader& reader,
                                                         std::uint32_t& address);

    [[nodiscard]] static std::vector<std::optional<std::shared_ptr<PolyChunk>>> read_array(
        const Sa3Dport::Structs::EndianStackReader& reader,
        std::uint32_t address);
};

using PolyChunkPtr = std::shared_ptr<PolyChunk>;

class SizedPolyChunk : public PolyChunk {
public:
    explicit SizedPolyChunk(PolyChunkType chunkType) : PolyChunk(chunkType) {}

    [[nodiscard]] virtual std::uint16_t size() const = 0;

    [[nodiscard]] std::uint32_t byte_size() const override {
        return (static_cast<std::uint32_t>(size()) * 2u) + 4u;
    }
};

} // namespace Sa3Dport::Mesh::Chunk
