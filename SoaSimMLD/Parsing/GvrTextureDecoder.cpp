#include "GvrTextureDecoder.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>

namespace soasim::mld::parsing {
namespace {

[[nodiscard]] std::size_t compact1By1(std::size_t x) {
    x &= 0x55555555U;
    x = (x ^ (x >> 1U)) & 0x33333333U;
    x = (x ^ (x >> 2U)) & 0x0F0F0F0FU;
    x = (x ^ (x >> 4U)) & 0x00FF00FFU;
    x = (x ^ (x >> 8U)) & 0x0000FFFFU;
    return x;
}

[[nodiscard]] std::size_t mortonX(std::size_t morton) {
    return compact1By1(morton);
}

[[nodiscard]] std::size_t mortonY(std::size_t morton) {
    return compact1By1(morton >> 1U);
}

void writeArgb1555(const std::uint16_t px, std::uint8_t* out) {
    out[0] = static_cast<std::uint8_t>(((px >> 10U) & 0x1FU) * 255U / 31U);
    out[1] = static_cast<std::uint8_t>(((px >> 5U) & 0x1FU) * 255U / 31U);
    out[2] = static_cast<std::uint8_t>((px & 0x1FU) * 255U / 31U);
    out[3] = static_cast<std::uint8_t>(((px >> 15U) & 0x1U) ? 255U : 0U);
}

void writeRgb565(const std::uint16_t px, std::uint8_t* out) {
    out[0] = static_cast<std::uint8_t>(((px >> 11U) & 0x1FU) * 255U / 31U);
    out[1] = static_cast<std::uint8_t>(((px >> 5U) & 0x3FU) * 255U / 63U);
    out[2] = static_cast<std::uint8_t>((px & 0x1FU) * 255U / 31U);
    out[3] = 255U;
}

void writeArgb4444(const std::uint16_t px, std::uint8_t* out) {
    out[0] = static_cast<std::uint8_t>(((px >> 8U) & 0xFU) * 17U);
    out[1] = static_cast<std::uint8_t>(((px >> 4U) & 0xFU) * 17U);
    out[2] = static_cast<std::uint8_t>((px & 0xFU) * 17U);
    out[3] = static_cast<std::uint8_t>(((px >> 12U) & 0xFU) * 17U);
}

[[nodiscard]] bool isSupportedPixelFormat(const std::uint8_t pf) {
    return pf == 0U || pf == 1U || pf == 2U;
}

[[nodiscard]] bool isSupportedDataFormat(const std::uint8_t df) {
    return df == 1U || df == 9U;
}

} // namespace

DecodedRgbaTexture decodeGvrToRgba8(const model::MldTextureEntry& entry) {
    DecodedRgbaTexture out{};
    out.width = entry.width;
    out.height = entry.height;

    if (entry.width == 0U || entry.height == 0U) {
        out.diagnostics.push_back("GVR decode skipped: missing width/height.");
        return out;
    }

    if (!isSupportedPixelFormat(entry.pixelFormat)) {
        out.diagnostics.push_back("GVR decode skipped: unsupported pixel format " + std::to_string(entry.pixelFormat) + ".");
        return out;
    }
    if (!isSupportedDataFormat(entry.dataFormat)) {
        out.diagnostics.push_back("GVR decode skipped: unsupported data format " + std::to_string(entry.dataFormat) + ".");
        return out;
    }

    if (entry.imageDataOffset >= entry.gvrData.size()) {
        out.diagnostics.push_back("GVR decode skipped: image data offset out of range.");
        return out;
    }
    const auto imageData = std::span<const std::uint8_t>(entry.gvrData.data() + static_cast<std::ptrdiff_t>(entry.imageDataOffset),
        entry.gvrData.size() - entry.imageDataOffset);

    const std::size_t pixelCount = static_cast<std::size_t>(entry.width) * static_cast<std::size_t>(entry.height);
    const std::size_t expectedBytes = pixelCount * 2U;
    if (imageData.size() < expectedBytes) {
        out.diagnostics.push_back("GVR decode skipped: image payload shorter than expected for 16bpp texture.");
        return out;
    }

    out.rgba8.assign(pixelCount * 4U, 0U);
    for (std::size_t i = 0; i < pixelCount; ++i) {
        std::size_t srcIndex = i;
        if (entry.dataFormat == 1U) {
            const auto tx = mortonX(i);
            const auto ty = mortonY(i);
            if (tx >= entry.width || ty >= entry.height) {
                continue;
            }
            srcIndex = (ty * static_cast<std::size_t>(entry.width)) + tx;
        }

        const std::size_t srcByte = srcIndex * 2U;
        const std::uint16_t px = static_cast<std::uint16_t>(imageData[srcByte] | (static_cast<std::uint16_t>(imageData[srcByte + 1U]) << 8U));
        auto* dst = out.rgba8.data() + (i * 4U);

        switch (entry.pixelFormat) {
        case 0U: writeArgb1555(px, dst); break;
        case 1U: writeRgb565(px, dst); break;
        case 2U: writeArgb4444(px, dst); break;
        default: break;
        }
    }

    out.decoded = true;
    out.diagnostics.push_back("GVR texture decoded to RGBA8.");
    return out;
}

} // namespace soasim::mld::parsing
