#include "GvmParser.h"

#include "../Decoding/GvrDecoder.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace soasim::gvm::parsing {
namespace {

constexpr std::uint32_t makeTag(const char a, const char b, const char c, const char d) {
    return (static_cast<std::uint32_t>(a) << 24U) |
        (static_cast<std::uint32_t>(b) << 16U) |
        (static_cast<std::uint32_t>(c) << 8U) |
        static_cast<std::uint32_t>(d);
}

constexpr std::uint32_t tagGbix = makeTag('G', 'B', 'I', 'X');
constexpr std::uint32_t tagGcix = makeTag('G', 'C', 'I', 'X');
constexpr std::uint32_t tagGvrt = makeTag('G', 'V', 'R', 'T');
constexpr std::uint32_t tagGvmh = makeTag('G', 'V', 'M', 'H');

[[nodiscard]] std::optional<std::uint32_t> readU32BE(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4U) {
        return std::nullopt;
    }
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
        static_cast<std::uint32_t>(bytes[offset + 3U]);
}

[[nodiscard]] std::optional<std::uint32_t> readU32LE(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4U) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

[[nodiscard]] std::optional<std::uint16_t> readU16BE(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2U) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8U) | bytes[offset + 1U]);
}

[[nodiscard]] std::optional<std::uint16_t> readU16LE(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2U) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(bytes[offset] | (static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

[[nodiscard]] bool plausibleDimension(const std::uint16_t value) {
    return value > 0U && value <= 8192U;
}

[[nodiscard]] bool plausibleDimensions(const std::uint16_t width, const std::uint16_t height) {
    return plausibleDimension(width) && plausibleDimension(height);
}

[[nodiscard]] model::TextureFormat mapTextureFormat(const std::uint8_t raw) {
    switch (raw) {
    case 0x00U: return model::TextureFormat::I4;
    case 0x01U: return model::TextureFormat::I8;
    case 0x02U: return model::TextureFormat::IA4;
    case 0x03U: return model::TextureFormat::IA8;
    case 0x04U: return model::TextureFormat::RGB565;
    case 0x05U: return model::TextureFormat::RGB5A3;
    case 0x06U: return model::TextureFormat::RGBA8;
    case 0x08U: return model::TextureFormat::CI4;
    case 0x09U: return model::TextureFormat::CI8;
    case 0x0AU: return model::TextureFormat::CI14X2;
    case 0x0EU: return model::TextureFormat::CMPR;
    default: break;
    }
    return model::TextureFormat::Unknown;
}

[[nodiscard]] model::TextureFormat mapGvrDataFormat(const std::uint8_t raw) {
    if (auto gx = mapTextureFormat(raw); gx != model::TextureFormat::Unknown) {
        return gx;
    }

    // GVR tools commonly use compact data-format ids rather than raw GX ids.
    switch (raw) {
    case 0x10U:
    case 0x11U:
    case 0x12U:
    case 0x13U:
        return model::TextureFormat::CI4;
    case 0x20U:
    case 0x21U:
    case 0x22U:
    case 0x23U:
        return model::TextureFormat::CI8;
    case 0x30U:
    case 0x31U:
    case 0x32U:
    case 0x33U:
        return model::TextureFormat::RGB565;
    case 0x40U:
    case 0x41U:
    case 0x42U:
    case 0x43U:
        return model::TextureFormat::RGB5A3;
    case 0x50U:
    case 0x51U:
    case 0x52U:
    case 0x53U:
        return model::TextureFormat::RGBA8;
    case 0x60U:
    case 0x61U:
    case 0x62U:
    case 0x63U:
        return model::TextureFormat::CMPR;
    default:
        break;
    }
    return model::TextureFormat::Unknown;
}

[[nodiscard]] model::PaletteFormat mapPaletteFormatFromFlags(const std::uint8_t flags, const std::uint8_t dataFormat) {
    const auto raw = static_cast<std::uint8_t>((flags >> 4U) & 0x0FU);
    switch (raw) {
    case 0x0U: return model::PaletteFormat::IA8;
    case 0x1U: return model::PaletteFormat::RGB565;
    case 0x2U: return model::PaletteFormat::RGB5A3;
    default: break;
    }

    switch (dataFormat & 0x0FU) {
    case 0x1U: return model::PaletteFormat::IA8;
    case 0x2U: return model::PaletteFormat::RGB565;
    case 0x3U: return model::PaletteFormat::RGB5A3;
    default: break;
    }
    return model::PaletteFormat::RGB5A3;
}

[[nodiscard]] bool usesPalette(const model::TextureFormat format) {
    return format == model::TextureFormat::CI4 ||
        format == model::TextureFormat::CI8 ||
        format == model::TextureFormat::CI14X2;
}

[[nodiscard]] std::size_t paletteEntryCount(const model::TextureFormat format) {
    switch (format) {
    case model::TextureFormat::CI4: return 16U;
    case model::TextureFormat::CI8: return 256U;
    case model::TextureFormat::CI14X2: return 16384U;
    default: return 0U;
    }
}

[[nodiscard]] std::size_t roundUp(const std::size_t value, const std::size_t multiple) {
    return multiple == 0U ? value : ((value + multiple - 1U) / multiple) * multiple;
}

struct Layout {
    std::size_t blockWidth = 0;
    std::size_t blockHeight = 0;
    std::size_t bytesPerBlock = 0;
};

[[nodiscard]] std::optional<Layout> layoutFor(const model::TextureFormat format) {
    switch (format) {
    case model::TextureFormat::I4: return Layout{ 8U, 8U, 32U };
    case model::TextureFormat::I8:
    case model::TextureFormat::IA4:
    case model::TextureFormat::CI8: return Layout{ 8U, 4U, 32U };
    case model::TextureFormat::IA8:
    case model::TextureFormat::RGB565:
    case model::TextureFormat::RGB5A3:
    case model::TextureFormat::CI14X2: return Layout{ 4U, 4U, 32U };
    case model::TextureFormat::RGBA8: return Layout{ 4U, 4U, 64U };
    case model::TextureFormat::CI4: return Layout{ 8U, 8U, 32U };
    case model::TextureFormat::CMPR: return Layout{ 8U, 8U, 32U };
    default: break;
    }
    return std::nullopt;
}

[[nodiscard]] std::size_t expectedBaseImageSize(const model::TextureFormat format,
    const std::uint16_t width,
    const std::uint16_t height) {
    const auto layout = layoutFor(format);
    if (!layout.has_value() || width == 0U || height == 0U) {
        return 0U;
    }
    const auto blocksX = roundUp(width, layout->blockWidth) / layout->blockWidth;
    const auto blocksY = roundUp(height, layout->blockHeight) / layout->blockHeight;
    return blocksX * blocksY * layout->bytesPerBlock;
}

[[nodiscard]] std::optional<std::size_t> readChunkPayloadSize(std::span<const std::uint8_t> bytes, const std::size_t at) {
    const auto le = readU32LE(bytes, at + 4U);
    if (le.has_value() && *le >= 8U && at + 8U + *le <= bytes.size()) {
        return static_cast<std::size_t>(*le);
    }
    const auto be = readU32BE(bytes, at + 4U);
    if (be.has_value() && *be >= 8U && at + 8U + *be <= bytes.size()) {
        return static_cast<std::size_t>(*be);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint32_t> readGlobalIndex(std::span<const std::uint8_t> bytes, const std::size_t chunkAt) {
    const auto payloadSize = readChunkPayloadSize(bytes, chunkAt);
    if (!payloadSize.has_value() || *payloadSize < 4U) {
        return std::nullopt;
    }
    if (const auto be = readU32BE(bytes, chunkAt + 8U); be.has_value()) {
        return *be;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::size_t> findNextTag(std::span<const std::uint8_t> bytes,
    const std::size_t start,
    const std::uint32_t tag) {
    for (std::size_t i = start; i + 4U <= bytes.size(); ++i) {
        if (readU32BE(bytes, i).value_or(0U) == tag) {
            return i;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<std::uint8_t> sliceBytes(std::span<const std::uint8_t> bytes,
    const std::size_t offset,
    const std::size_t size) {
    if (offset > bytes.size()) {
        return {};
    }
    const auto safeSize = std::min(size, bytes.size() - offset);
    return std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        bytes.begin() + static_cast<std::ptrdiff_t>(offset + safeSize));
}

struct HeaderCandidate {
    std::size_t headerPayloadOffset = 0;
    std::size_t imageDataOffset = 0;
    std::uint8_t flags = 0;
    std::uint8_t dataFormat = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
};

[[nodiscard]] std::optional<HeaderCandidate> readHeaderCandidate(std::span<const std::uint8_t> bytes,
    const std::size_t gvrtAt,
    const std::size_t payloadSize) {
    std::vector<HeaderCandidate> candidates{};
    const auto append = [&](const std::size_t headerPayloadOffset,
                            const std::size_t imageDataOffset,
                            const std::optional<std::uint16_t> width,
                            const std::optional<std::uint16_t> height,
                            const std::uint8_t flags,
                            const std::uint8_t dataFormat) {
        if (width.has_value() && height.has_value() && plausibleDimensions(*width, *height) &&
            imageDataOffset <= bytes.size() && imageDataOffset >= gvrtAt + 8U &&
            imageDataOffset <= gvrtAt + 8U + payloadSize) {
            candidates.push_back(HeaderCandidate{
                .headerPayloadOffset = headerPayloadOffset,
                .imageDataOffset = imageDataOffset,
                .flags = flags,
                .dataFormat = dataFormat,
                .width = *width,
                .height = *height,
            });
        }
    };

    if (payloadSize >= 8U && gvrtAt + 0x10U <= bytes.size()) {
        append(gvrtAt + 8U,
            gvrtAt + 0x10U,
            readU16BE(bytes, gvrtAt + 0x0CU),
            readU16BE(bytes, gvrtAt + 0x0EU),
            bytes[gvrtAt + 0x0AU],
            bytes[gvrtAt + 0x0BU]);
        append(gvrtAt + 8U,
            gvrtAt + 0x10U,
            readU16LE(bytes, gvrtAt + 0x0CU),
            readU16LE(bytes, gvrtAt + 0x0EU),
            bytes[gvrtAt + 0x08U],
            bytes[gvrtAt + 0x09U]);
    }

    if (payloadSize >= 16U && gvrtAt + 0x18U <= bytes.size()) {
        append(gvrtAt + 8U,
            gvrtAt + 0x18U,
            readU16BE(bytes, gvrtAt + 0x14U),
            readU16BE(bytes, gvrtAt + 0x16U),
            bytes[gvrtAt + 0x12U],
            bytes[gvrtAt + 0x13U]);
        append(gvrtAt + 8U,
            gvrtAt + 0x18U,
            readU16LE(bytes, gvrtAt + 0x14U),
            readU16LE(bytes, gvrtAt + 0x16U),
            bytes[gvrtAt + 0x10U],
            bytes[gvrtAt + 0x11U]);
    }

    if (candidates.empty()) {
        return std::nullopt;
    }

    std::sort(candidates.begin(), candidates.end(), [](const HeaderCandidate& left, const HeaderCandidate& right) {
        const auto leftKnown = mapGvrDataFormat(left.dataFormat) != model::TextureFormat::Unknown;
        const auto rightKnown = mapGvrDataFormat(right.dataFormat) != model::TextureFormat::Unknown;
        if (leftKnown != rightKnown) {
            return leftKnown;
        }
        return left.imageDataOffset < right.imageDataOffset;
    });
    return candidates.front();
}

void decodeOrFallback(model::GvrTexture& texture) {
    std::vector<std::string> decodeDiagnostics{};
    auto decoded = decoding::decodeBaseLevel(texture, decodeDiagnostics);
    texture.diagnostics.insert(texture.diagnostics.end(), decodeDiagnostics.begin(), decodeDiagnostics.end());
    if (decoded.rgba8.empty()) {
        texture.diagnostics.push_back("Using generated ERROR checkerboard texture.");
        decoded = decoding::makeErrorTexture(texture.width, texture.height);
    }
    texture.decodedBaseLevel = std::move(decoded);
}

} // namespace

model::GvrTexture parseGvrTexture(std::span<const std::uint8_t> bytes,
    const std::size_t sourceOffset,
    const ParseOptions& options) {
    model::GvrTexture out{};
    out.sourceOffset = sourceOffset;
    out.sourceSize = bytes.size();

    const auto gvrtAt = readU32BE(bytes, 0U).value_or(0U) == tagGvrt ? std::optional<std::size_t>(0U) : findNextTag(bytes, 0U, tagGvrt);
    if (!gvrtAt.has_value()) {
        out.diagnostics.push_back("GVR parse failed: GVRT chunk not found.");
        if (options.decodeBaseLevel) {
            decodeOrFallback(out);
        }
        return out;
    }

    if (auto gbixAt = findNextTag(bytes, 0U, tagGbix); gbixAt.has_value() && *gbixAt < *gvrtAt) {
        if (const auto global = readGlobalIndex(bytes, *gbixAt); global.has_value()) {
            out.hasGlobalIndex = true;
            out.globalIndex = *global;
        }
    } else if (auto gcixAt = findNextTag(bytes, 0U, tagGcix); gcixAt.has_value() && *gcixAt < *gvrtAt) {
        if (const auto global = readGlobalIndex(bytes, *gcixAt); global.has_value()) {
            out.hasGlobalIndex = true;
            out.globalIndex = *global;
        }
    }

    const auto payloadSize = readChunkPayloadSize(bytes, *gvrtAt);
    if (!payloadSize.has_value()) {
        out.diagnostics.push_back("GVR parse failed: GVRT payload size is invalid.");
        if (options.decodeBaseLevel) {
            decodeOrFallback(out);
        }
        return out;
    }
    out.sourceSize = *gvrtAt + 8U + *payloadSize;

    const auto candidate = readHeaderCandidate(bytes, *gvrtAt, *payloadSize);
    if (!candidate.has_value()) {
        out.diagnostics.push_back("GVR parse failed: no plausible GVRT texture header was found.");
        if (options.decodeBaseLevel) {
            decodeOrFallback(out);
        }
        return out;
    }

    out.rawFlags = candidate->flags;
    out.rawDataFormat = candidate->dataFormat;
    out.textureFormat = mapGvrDataFormat(out.rawDataFormat);
    out.width = candidate->width;
    out.height = candidate->height;
    out.hasMipmaps = (out.rawFlags & 0x01U) != 0U;
    if (usesPalette(out.textureFormat)) {
        out.hasInternalPalette = true;
        out.paletteFormat = mapPaletteFormatFromFlags(out.rawFlags, out.rawDataFormat);
    }

    const auto payloadEnd = *gvrtAt + 8U + *payloadSize;
    const auto expectedImageSize = expectedBaseImageSize(out.textureFormat, out.width, out.height);
    auto imageDataOffset = candidate->imageDataOffset;
    if (usesPalette(out.textureFormat)) {
        const auto paletteBytes = paletteEntryCount(out.textureFormat) * 2U;
        if (imageDataOffset < payloadEnd) {
            out.paletteData = sliceBytes(bytes, imageDataOffset, std::min(paletteBytes, payloadEnd - imageDataOffset));
        }
        imageDataOffset += out.paletteData.size();
        if (out.paletteData.empty()) {
            out.diagnostics.push_back("GVR indexed texture did not expose an obvious internal palette.");
        }
    }

    out.imageDataOffset = imageDataOffset;
    out.imageDataSize = expectedImageSize == 0U
        ? payloadEnd - imageDataOffset
        : std::min(expectedImageSize, payloadEnd - imageDataOffset);
    out.imageData = sliceBytes(bytes, out.imageDataOffset, out.imageDataSize);

    if (out.textureFormat == model::TextureFormat::Unknown) {
        out.diagnostics.push_back("GVR parse warning: unsupported raw data format " + std::to_string(out.rawDataFormat) + ".");
    }
    if (out.hasMipmaps) {
        out.diagnostics.push_back("GVR parse warning: mipmap flag is set; only base level is decoded.");
    }

    if (options.decodeBaseLevel) {
        decodeOrFallback(out);
    }
    return out;
}

model::GvmArchive parseGvmArchive(std::span<const std::uint8_t> bytes,
    const std::size_t startOffset,
    const ParseOptions& options) {
    model::GvmArchive out{};
    out.sourceOffset = startOffset;
    if (startOffset >= bytes.size()) {
        out.diagnostics.push_back("GVM parse skipped: start offset is out of bounds.");
        return out;
    }

    if (readU32BE(bytes, startOffset).value_or(0U) == tagGvmh) {
        out.diagnostics.push_back("GVMH archive header detected; texture chunk scan will be used for this first implementation.");
    }

    std::optional<std::size_t> pendingStart{};
    std::optional<std::uint32_t> pendingGlobalIndex{};
    std::size_t cursor = startOffset;
    while (cursor + 8U <= bytes.size()) {
        const auto tag = readU32BE(bytes, cursor).value_or(0U);
        if (tag != tagGbix && tag != tagGcix && tag != tagGvrt) {
            ++cursor;
            continue;
        }

        const auto payloadSize = readChunkPayloadSize(bytes, cursor);
        if (!payloadSize.has_value()) {
            ++cursor;
            continue;
        }
        const auto chunkEnd = cursor + 8U + *payloadSize;

        if (tag == tagGbix || tag == tagGcix) {
            pendingStart = cursor;
            pendingGlobalIndex = readGlobalIndex(bytes, cursor);
            cursor = chunkEnd;
            continue;
        }

        const auto textureStart = pendingStart.has_value() ? *pendingStart : cursor;
        const auto textureEnd = chunkEnd;
        auto texture = parseGvrTexture(bytes.subspan(textureStart, textureEnd - textureStart), textureStart, options);
        if (pendingGlobalIndex.has_value()) {
            texture.hasGlobalIndex = true;
            texture.globalIndex = *pendingGlobalIndex;
        }
        out.textures.push_back(std::move(texture));
        pendingStart.reset();
        pendingGlobalIndex.reset();
        cursor = chunkEnd;
    }

    out.diagnostics.push_back("GVM parse extracted " + std::to_string(out.textures.size()) + " GVR texture chunk(s).");
    return out;
}

} // namespace soasim::gvm::parsing
