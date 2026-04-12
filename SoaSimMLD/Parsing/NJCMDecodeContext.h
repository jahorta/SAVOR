#pragma once

#include "../Model/NjcmModel.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace soasim::mld::parsing {

struct NjcmDecodeContext {
    std::span<const std::uint8_t> decoded{};
    std::uint32_t imageBase = 0;
    bool littleEndian = false;
    model::NjcmDecodedChunk* out = nullptr;
};

[[nodiscard]] std::optional<std::uint16_t> readU16At(std::span<const std::uint8_t> bytes, std::size_t off, bool littleEndian);
[[nodiscard]] std::optional<std::uint32_t> readU32At(std::span<const std::uint8_t> bytes, std::size_t off, bool littleEndian);
[[nodiscard]] std::optional<float> readF32At(std::span<const std::uint8_t> bytes, std::size_t off, bool littleEndian);
[[nodiscard]] std::optional<std::size_t> resolvePointer(std::uint32_t rawPtr, std::uint32_t imageBase, std::size_t payloadSize);
[[nodiscard]] std::size_t vertexWordsPerVertexByType(std::uint8_t type);

} // namespace soasim::mld::parsing
