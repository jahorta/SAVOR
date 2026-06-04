#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace soasim::mld::model {

struct MldTextureEntry {
    std::size_t archiveOffset = 0;
    std::size_t gvrDataOffset = 0;
    std::size_t gvrDataSize = 0;
    bool hasGlobalIndex = false;
    std::uint32_t globalIndex = 0;
    std::string textureName{};
    std::uint8_t pixelFormat = 0;
    std::uint8_t dataFormat = 0;
    std::string sourceFormat{};
    std::string sourcePaletteFormat{};
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::size_t imageDataOffset = 0;
    std::size_t imageDataSize = 0;
    std::vector<std::uint8_t> gvrData{};
    bool decoded = false;
    std::vector<std::uint8_t> rgba8{};
    std::vector<std::string> diagnostics{};
};

struct MldTextureArchive {
    std::size_t tableOffset = 0;
    std::vector<MldTextureEntry> entries{};
    std::vector<std::string> diagnostics{};
};

} // namespace soasim::mld::model
