#pragma once

#include "NjcmModel.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace soasim::mld::model {

struct NjtlBlock {
    std::size_t chunkOffset = 0;
    std::size_t chunkDataSize = 0;
    bool chunkSizeLittleEndian = false;
    std::uint32_t imageBase = 0;
    bool parseSucceeded = false;
    std::uint32_t firstTextureNamePointer = 0;
    std::uint32_t textureCount = 0;
    std::vector<std::string> diagnostics{};
    struct TextureNameRecord {
        std::size_t recordOffset = 0;
        std::uint32_t textureNamePointer = 0;
        std::uint32_t reserved0 = 0;
        std::uint32_t reserved1 = 0;
        std::string name{};
    };
    std::vector<TextureNameRecord> textureNames{};
};

struct NjObjectBlockModel {
    bool hasNjtlBeforeNjcm = false;
    std::optional<NjtlBlock> njtl{};
    NjcmDecodedChunk njcm{};
};

} // namespace soasim::mld::model
