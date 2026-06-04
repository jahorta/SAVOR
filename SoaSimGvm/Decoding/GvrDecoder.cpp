#include "GvrDecoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace soasim::gvm::decoding {
namespace {

struct Rgba {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

[[nodiscard]] std::uint8_t expand4(const std::uint8_t value) {
    return static_cast<std::uint8_t>((value << 4U) | value);
}

[[nodiscard]] std::uint8_t expand5(const std::uint8_t value) {
    return static_cast<std::uint8_t>((value << 3U) | (value >> 2U));
}

[[nodiscard]] std::uint8_t expand6(const std::uint8_t value) {
    return static_cast<std::uint8_t>((value << 2U) | (value >> 4U));
}

[[nodiscard]] std::uint16_t readBe16(const std::vector<std::uint8_t>& bytes, const std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8U) | bytes[offset + 1U]);
}

[[nodiscard]] Rgba decodeRgb565(const std::uint16_t px) {
    return Rgba{
        .r = expand5(static_cast<std::uint8_t>((px >> 11U) & 0x1FU)),
        .g = expand6(static_cast<std::uint8_t>((px >> 5U) & 0x3FU)),
        .b = expand5(static_cast<std::uint8_t>(px & 0x1FU)),
        .a = 255U,
    };
}

[[nodiscard]] Rgba decodeRgb5A3(const std::uint16_t px) {
    if ((px & 0x8000U) != 0U) {
        return Rgba{
            .r = expand5(static_cast<std::uint8_t>((px >> 10U) & 0x1FU)),
            .g = expand5(static_cast<std::uint8_t>((px >> 5U) & 0x1FU)),
            .b = expand5(static_cast<std::uint8_t>(px & 0x1FU)),
            .a = 255U,
        };
    }
    return Rgba{
        .r = expand4(static_cast<std::uint8_t>((px >> 8U) & 0x0FU)),
        .g = expand4(static_cast<std::uint8_t>((px >> 4U) & 0x0FU)),
        .b = expand4(static_cast<std::uint8_t>(px & 0x0FU)),
        .a = static_cast<std::uint8_t>(((px >> 12U) & 0x7U) * 255U / 7U),
    };
}

[[nodiscard]] Rgba decodePaletteEntry(const std::uint16_t px, const model::PaletteFormat format) {
    switch (format) {
    case model::PaletteFormat::IA8:
        return Rgba{
            .r = static_cast<std::uint8_t>(px & 0xFFU),
            .g = static_cast<std::uint8_t>(px & 0xFFU),
            .b = static_cast<std::uint8_t>(px & 0xFFU),
            .a = static_cast<std::uint8_t>((px >> 8U) & 0xFFU),
        };
    case model::PaletteFormat::RGB565: return decodeRgb565(px);
    case model::PaletteFormat::RGB5A3: return decodeRgb5A3(px);
    case model::PaletteFormat::None: break;
    default: break;
    }
    return Rgba{ .r = 255U, .g = 0U, .b = 255U, .a = 255U };
}

void writePixel(model::RgbaImage& image, const std::uint32_t x, const std::uint32_t y, const Rgba px) {
    if (x >= image.width || y >= image.height) {
        return;
    }
    const std::size_t offset = (static_cast<std::size_t>(y) * image.width + x) * 4U;
    image.rgba8[offset + 0U] = px.r;
    image.rgba8[offset + 1U] = px.g;
    image.rgba8[offset + 2U] = px.b;
    image.rgba8[offset + 3U] = px.a;
}

[[nodiscard]] bool hasBytes(const std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::size_t count) {
    return offset <= bytes.size() && count <= bytes.size() - offset;
}

[[nodiscard]] std::vector<Rgba> decodePalette(const model::GvrTexture& texture, const std::size_t entryCount) {
    std::vector<Rgba> palette{};
    const auto count = std::min(entryCount, texture.paletteData.size() / 2U);
    palette.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        palette.push_back(decodePaletteEntry(readBe16(texture.paletteData, i * 2U), texture.paletteFormat));
    }
    return palette;
}

void decodeI4(const model::GvrTexture& texture, model::RgbaImage& image) {
    std::size_t src = 0;
    for (std::uint32_t by = 0; by < image.height; by += 8U) {
        for (std::uint32_t bx = 0; bx < image.width; bx += 8U) {
            for (std::uint32_t y = 0; y < 8U; ++y) {
                for (std::uint32_t x = 0; x < 8U; x += 2U) {
                    if (!hasBytes(texture.imageData, src, 1U)) {
                        return;
                    }
                    const auto packed = texture.imageData[src++];
                    const std::uint8_t hi = expand4(static_cast<std::uint8_t>((packed >> 4U) & 0x0FU));
                    const std::uint8_t lo = expand4(static_cast<std::uint8_t>(packed & 0x0FU));
                    writePixel(image, bx + x, by + y, Rgba{ .r = hi, .g = hi, .b = hi, .a = 255U });
                    writePixel(image, bx + x + 1U, by + y, Rgba{ .r = lo, .g = lo, .b = lo, .a = 255U });
                }
            }
        }
    }
}

void decodeI8(const model::GvrTexture& texture, model::RgbaImage& image) {
    std::size_t src = 0;
    for (std::uint32_t by = 0; by < image.height; by += 4U) {
        for (std::uint32_t bx = 0; bx < image.width; bx += 8U) {
            for (std::uint32_t y = 0; y < 4U; ++y) {
                for (std::uint32_t x = 0; x < 8U; ++x) {
                    if (!hasBytes(texture.imageData, src, 1U)) {
                        return;
                    }
                    const auto v = texture.imageData[src++];
                    writePixel(image, bx + x, by + y, Rgba{ .r = v, .g = v, .b = v, .a = 255U });
                }
            }
        }
    }
}

void decodeIA4(const model::GvrTexture& texture, model::RgbaImage& image) {
    std::size_t src = 0;
    for (std::uint32_t by = 0; by < image.height; by += 4U) {
        for (std::uint32_t bx = 0; bx < image.width; bx += 8U) {
            for (std::uint32_t y = 0; y < 4U; ++y) {
                for (std::uint32_t x = 0; x < 8U; ++x) {
                    if (!hasBytes(texture.imageData, src, 1U)) {
                        return;
                    }
                    const auto packed = texture.imageData[src++];
                    const auto a = expand4(static_cast<std::uint8_t>((packed >> 4U) & 0x0FU));
                    const auto i = expand4(static_cast<std::uint8_t>(packed & 0x0FU));
                    writePixel(image, bx + x, by + y, Rgba{ .r = i, .g = i, .b = i, .a = a });
                }
            }
        }
    }
}

void decodeIA8(const model::GvrTexture& texture, model::RgbaImage& image) {
    std::size_t src = 0;
    for (std::uint32_t by = 0; by < image.height; by += 4U) {
        for (std::uint32_t bx = 0; bx < image.width; bx += 4U) {
            for (std::uint32_t y = 0; y < 4U; ++y) {
                for (std::uint32_t x = 0; x < 4U; ++x) {
                    if (!hasBytes(texture.imageData, src, 2U)) {
                        return;
                    }
                    const auto a = texture.imageData[src++];
                    const auto i = texture.imageData[src++];
                    writePixel(image, bx + x, by + y, Rgba{ .r = i, .g = i, .b = i, .a = a });
                }
            }
        }
    }
}

void decode16Bit(const model::GvrTexture& texture, model::RgbaImage& image, const bool rgb5a3) {
    std::size_t src = 0;
    for (std::uint32_t by = 0; by < image.height; by += 4U) {
        for (std::uint32_t bx = 0; bx < image.width; bx += 4U) {
            for (std::uint32_t y = 0; y < 4U; ++y) {
                for (std::uint32_t x = 0; x < 4U; ++x) {
                    if (!hasBytes(texture.imageData, src, 2U)) {
                        return;
                    }
                    const auto raw = readBe16(texture.imageData, src);
                    src += 2U;
                    writePixel(image, bx + x, by + y, rgb5a3 ? decodeRgb5A3(raw) : decodeRgb565(raw));
                }
            }
        }
    }
}

void decodeRGBA8(const model::GvrTexture& texture, model::RgbaImage& image) {
    std::size_t src = 0;
    for (std::uint32_t by = 0; by < image.height; by += 4U) {
        for (std::uint32_t bx = 0; bx < image.width; bx += 4U) {
            std::array<std::uint8_t, 16> alpha{};
            std::array<std::uint8_t, 16> red{};
            std::array<std::uint8_t, 16> green{};
            std::array<std::uint8_t, 16> blue{};
            if (!hasBytes(texture.imageData, src, 64U)) {
                return;
            }
            for (std::size_t i = 0; i < 16U; ++i) {
                alpha[i] = texture.imageData[src++];
                red[i] = texture.imageData[src++];
            }
            for (std::size_t i = 0; i < 16U; ++i) {
                green[i] = texture.imageData[src++];
                blue[i] = texture.imageData[src++];
            }
            for (std::uint32_t y = 0; y < 4U; ++y) {
                for (std::uint32_t x = 0; x < 4U; ++x) {
                    const std::size_t i = y * 4U + x;
                    writePixel(image, bx + x, by + y, Rgba{ .r = red[i], .g = green[i], .b = blue[i], .a = alpha[i] });
                }
            }
        }
    }
}

void decodeCI4(const model::GvrTexture& texture, model::RgbaImage& image) {
    const auto palette = decodePalette(texture, 16U);
    if (palette.empty()) {
        return;
    }
    std::size_t src = 0;
    for (std::uint32_t by = 0; by < image.height; by += 8U) {
        for (std::uint32_t bx = 0; bx < image.width; bx += 8U) {
            for (std::uint32_t y = 0; y < 8U; ++y) {
                for (std::uint32_t x = 0; x < 8U; x += 2U) {
                    if (!hasBytes(texture.imageData, src, 1U)) {
                        return;
                    }
                    const auto packed = texture.imageData[src++];
                    writePixel(image, bx + x, by + y, palette[(packed >> 4U) & 0x0FU]);
                    writePixel(image, bx + x + 1U, by + y, palette[packed & 0x0FU]);
                }
            }
        }
    }
}

void decodeCI8(const model::GvrTexture& texture, model::RgbaImage& image) {
    const auto palette = decodePalette(texture, 256U);
    if (palette.empty()) {
        return;
    }
    std::size_t src = 0;
    for (std::uint32_t by = 0; by < image.height; by += 4U) {
        for (std::uint32_t bx = 0; bx < image.width; bx += 8U) {
            for (std::uint32_t y = 0; y < 4U; ++y) {
                for (std::uint32_t x = 0; x < 8U; ++x) {
                    if (!hasBytes(texture.imageData, src, 1U)) {
                        return;
                    }
                    writePixel(image, bx + x, by + y, palette[texture.imageData[src++]]);
                }
            }
        }
    }
}

void decodeCI14X2(const model::GvrTexture& texture, model::RgbaImage& image) {
    const auto palette = decodePalette(texture, 16384U);
    if (palette.empty()) {
        return;
    }
    std::size_t src = 0;
    for (std::uint32_t by = 0; by < image.height; by += 4U) {
        for (std::uint32_t bx = 0; bx < image.width; bx += 4U) {
            for (std::uint32_t y = 0; y < 4U; ++y) {
                for (std::uint32_t x = 0; x < 4U; ++x) {
                    if (!hasBytes(texture.imageData, src, 2U)) {
                        return;
                    }
                    const auto index = static_cast<std::size_t>(readBe16(texture.imageData, src) & 0x3FFFU);
                    src += 2U;
                    if (index < palette.size()) {
                        writePixel(image, bx + x, by + y, palette[index]);
                    }
                }
            }
        }
    }
}

[[nodiscard]] std::array<Rgba, 4> decodeDxt1Palette(const std::uint16_t c0, const std::uint16_t c1) {
    std::array<Rgba, 4> colors{ decodeRgb565(c0), decodeRgb565(c1), {}, {} };
    if (c0 > c1) {
        colors[2] = Rgba{
            .r = static_cast<std::uint8_t>((2U * colors[0].r + colors[1].r) / 3U),
            .g = static_cast<std::uint8_t>((2U * colors[0].g + colors[1].g) / 3U),
            .b = static_cast<std::uint8_t>((2U * colors[0].b + colors[1].b) / 3U),
            .a = 255U,
        };
        colors[3] = Rgba{
            .r = static_cast<std::uint8_t>((colors[0].r + 2U * colors[1].r) / 3U),
            .g = static_cast<std::uint8_t>((colors[0].g + 2U * colors[1].g) / 3U),
            .b = static_cast<std::uint8_t>((colors[0].b + 2U * colors[1].b) / 3U),
            .a = 255U,
        };
    } else {
        colors[2] = Rgba{
            .r = static_cast<std::uint8_t>((colors[0].r + colors[1].r) / 2U),
            .g = static_cast<std::uint8_t>((colors[0].g + colors[1].g) / 2U),
            .b = static_cast<std::uint8_t>((colors[0].b + colors[1].b) / 2U),
            .a = 255U,
        };
        colors[3] = Rgba{ .r = 0U, .g = 0U, .b = 0U, .a = 0U };
    }
    return colors;
}

void decodeCMPR(const model::GvrTexture& texture, model::RgbaImage& image) {
    std::size_t src = 0;
    for (std::uint32_t by = 0; by < image.height; by += 8U) {
        for (std::uint32_t bx = 0; bx < image.width; bx += 8U) {
            for (std::uint32_t sub = 0; sub < 4U; ++sub) {
                if (!hasBytes(texture.imageData, src, 8U)) {
                    return;
                }
                const auto c0 = readBe16(texture.imageData, src);
                const auto c1 = readBe16(texture.imageData, src + 2U);
                const auto colors = decodeDxt1Palette(c0, c1);
                const std::uint32_t codes =
                    (static_cast<std::uint32_t>(texture.imageData[src + 4U]) << 24U) |
                    (static_cast<std::uint32_t>(texture.imageData[src + 5U]) << 16U) |
                    (static_cast<std::uint32_t>(texture.imageData[src + 6U]) << 8U) |
                    static_cast<std::uint32_t>(texture.imageData[src + 7U]);
                src += 8U;

                const std::uint32_t subX = (sub & 1U) * 4U;
                const std::uint32_t subY = (sub >> 1U) * 4U;
                for (std::uint32_t y = 0; y < 4U; ++y) {
                    for (std::uint32_t x = 0; x < 4U; ++x) {
                        const auto shift = 30U - ((y * 4U + x) * 2U);
                        const auto index = (codes >> shift) & 0x3U;
                        writePixel(image, bx + subX + x, by + subY + y, colors[index]);
                    }
                }
            }
        }
    }
}

void drawGlyph(model::RgbaImage& image,
    const std::uint32_t x,
    const std::uint32_t y,
    const std::array<std::uint8_t, 7>& rows,
    const std::uint32_t scale) {
    for (std::uint32_t row = 0; row < 7U; ++row) {
        for (std::uint32_t col = 0; col < 5U; ++col) {
            if ((rows[row] & (1U << (4U - col))) == 0U) {
                continue;
            }
            for (std::uint32_t sy = 0; sy < scale; ++sy) {
                for (std::uint32_t sx = 0; sx < scale; ++sx) {
                    writePixel(image, x + col * scale + sx, y + row * scale + sy, Rgba{ .r = 255U, .g = 0U, .b = 0U, .a = 255U });
                }
            }
        }
    }
}

} // namespace

model::RgbaImage decodeBaseLevel(const model::GvrTexture& texture, std::vector<std::string>& diagnostics) {
    model::RgbaImage image{};
    image.width = texture.width;
    image.height = texture.height;
    if (image.width == 0U || image.height == 0U) {
        diagnostics.push_back("GVR decode failed: missing texture dimensions.");
        return {};
    }

    image.rgba8.assign(static_cast<std::size_t>(image.width) * image.height * 4U, 0U);
    switch (texture.textureFormat) {
    case model::TextureFormat::I4: decodeI4(texture, image); break;
    case model::TextureFormat::I8: decodeI8(texture, image); break;
    case model::TextureFormat::IA4: decodeIA4(texture, image); break;
    case model::TextureFormat::IA8: decodeIA8(texture, image); break;
    case model::TextureFormat::RGB565: decode16Bit(texture, image, false); break;
    case model::TextureFormat::RGB5A3: decode16Bit(texture, image, true); break;
    case model::TextureFormat::RGBA8: decodeRGBA8(texture, image); break;
    case model::TextureFormat::CI4: decodeCI4(texture, image); break;
    case model::TextureFormat::CI8: decodeCI8(texture, image); break;
    case model::TextureFormat::CI14X2: decodeCI14X2(texture, image); break;
    case model::TextureFormat::CMPR: decodeCMPR(texture, image); break;
    case model::TextureFormat::Unknown:
    default:
        diagnostics.push_back("GVR decode failed: unsupported texture format " + model::to_string(texture.textureFormat) + ".");
        return {};
    }

    if (texture.textureFormat == model::TextureFormat::CI4 ||
        texture.textureFormat == model::TextureFormat::CI8 ||
        texture.textureFormat == model::TextureFormat::CI14X2) {
        if (texture.paletteData.empty()) {
            diagnostics.push_back("GVR decode failed: indexed texture has no internal palette.");
            return {};
        }
    }

    diagnostics.push_back("GVR texture decoded as " + model::to_string(texture.textureFormat) + " to RGBA8.");
    return image;
}

model::RgbaImage makeErrorTexture(const std::uint32_t requestedWidth, const std::uint32_t requestedHeight) {
    model::RgbaImage image{};
    image.width = std::clamp<std::uint32_t>(requestedWidth == 0U ? 64U : requestedWidth, 32U, 256U);
    image.height = std::clamp<std::uint32_t>(requestedHeight == 0U ? 64U : requestedHeight, 32U, 256U);
    image.rgba8.assign(static_cast<std::size_t>(image.width) * image.height * 4U, 255U);

    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const bool purple = ((x / 8U) + (y / 8U)) % 2U == 0U;
            writePixel(image, x, y, purple
                ? Rgba{ .r = 255U, .g = 0U, .b = 255U, .a = 255U }
                : Rgba{ .r = 0U, .g = 0U, .b = 0U, .a = 255U });
        }
    }

    static constexpr std::array<std::uint8_t, 7> e{ 0x1FU, 0x10U, 0x10U, 0x1EU, 0x10U, 0x10U, 0x1FU };
    static constexpr std::array<std::uint8_t, 7> r{ 0x1EU, 0x11U, 0x11U, 0x1EU, 0x14U, 0x12U, 0x11U };
    static constexpr std::array<std::uint8_t, 7> o{ 0x0EU, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x0EU };
    const std::uint32_t scale = std::max<std::uint32_t>(1U, std::min(image.width / 36U, image.height / 12U));
    const std::uint32_t textWidth = 5U * 5U * scale + 4U * scale;
    const std::uint32_t startX = image.width > textWidth ? (image.width - textWidth) / 2U : 0U;
    const std::uint32_t startY = image.height > 7U * scale ? (image.height - 7U * scale) / 2U : 0U;
    drawGlyph(image, startX, startY, e, scale);
    drawGlyph(image, startX + 6U * scale, startY, r, scale);
    drawGlyph(image, startX + 12U * scale, startY, r, scale);
    drawGlyph(image, startX + 18U * scale, startY, o, scale);
    drawGlyph(image, startX + 24U * scale, startY, r, scale);
    return image;
}

} // namespace soasim::gvm::decoding
