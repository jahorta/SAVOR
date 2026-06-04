#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace soasim::gvm::model {

enum class TextureFormat : std::uint8_t {
    I4 = 0x00,
    I8 = 0x01,
    IA4 = 0x02,
    IA8 = 0x03,
    RGB565 = 0x04,
    RGB5A3 = 0x05,
    RGBA8 = 0x06,
    CI4 = 0x08,
    CI8 = 0x09,
    CI14X2 = 0x0A,
    CMPR = 0x0E,
    Unknown = 0xFF,
};

enum class PaletteFormat : std::uint8_t {
    IA8 = 0x00,
    RGB565 = 0x01,
    RGB5A3 = 0x02,
    None = 0xFF,
};

struct RgbaImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba8{};
};

struct GvrTexture {
    std::size_t sourceOffset = 0;
    std::size_t sourceSize = 0;
    bool hasGlobalIndex = false;
    std::uint32_t globalIndex = 0;
    std::uint8_t rawFlags = 0;
    std::uint8_t rawDataFormat = 0;
    TextureFormat textureFormat = TextureFormat::Unknown;
    PaletteFormat paletteFormat = PaletteFormat::None;
    bool hasMipmaps = false;
    bool hasInternalPalette = false;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::size_t imageDataOffset = 0;
    std::size_t imageDataSize = 0;
    std::vector<std::uint8_t> imageData{};
    std::vector<std::uint8_t> paletteData{};
    std::optional<RgbaImage> decodedBaseLevel{};
    std::vector<std::string> diagnostics{};
};

struct GvmArchive {
    std::size_t sourceOffset = 0;
    std::vector<GvrTexture> textures{};
    std::vector<std::string> diagnostics{};
};

[[nodiscard]] std::string to_string(TextureFormat format);
[[nodiscard]] std::string to_string(PaletteFormat format);

} // namespace soasim::gvm::model
