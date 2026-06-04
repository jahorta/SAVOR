#include "MldTextureArchiveParser.h"

#include "../../SoaSimGvm/SoaSimGvm.h"

#include "../common/ByteUtils.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace soasim::mld::parsing {
namespace {

[[nodiscard]] std::string readFixedAsciiName(std::span<const std::uint8_t> bytes,
    const std::size_t offset,
    const std::size_t maxLength) {
    std::string out{};
    if (offset >= bytes.size()) {
        return out;
    }

    const auto end = std::min(bytes.size(), offset + maxLength);
    for (std::size_t i = offset; i < end; ++i) {
        const auto ch = bytes[i];
        if (ch == 0U) {
            break;
        }
        if (std::isprint(static_cast<unsigned char>(ch)) == 0) {
            break;
        }
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

[[nodiscard]] std::vector<std::string> parseTextureArchiveNames(std::span<const std::uint8_t> bytes,
    const std::size_t textureTableOffset,
    std::vector<std::string>& diagnostics) {
    std::vector<std::string> names{};
    const auto count = common::readU32AtBE(bytes, textureTableOffset);
    if (!count.has_value()) {
        diagnostics.push_back("Texture archive name table count is unreadable.");
        return names;
    }

    constexpr std::size_t recordSize = 44U;
    constexpr std::size_t nameSize = 32U;
    constexpr std::size_t hardCap = 4096U;
    if (*count == 0U || *count > hardCap) {
        diagnostics.push_back("Texture archive name table count is suspicious: " + std::to_string(*count) + ".");
        return names;
    }

    const std::size_t tableBegin = textureTableOffset + 4U;
    const std::size_t requiredEnd = tableBegin + (static_cast<std::size_t>(*count) * recordSize);
    if (requiredEnd > bytes.size()) {
        diagnostics.push_back("Texture archive name table overruns file bounds.");
        return names;
    }

    names.reserve(*count);
    for (std::uint32_t i = 0; i < *count; ++i) {
        names.push_back(readFixedAsciiName(bytes, tableBegin + (static_cast<std::size_t>(i) * recordSize), nameSize));
    }
    diagnostics.push_back("Texture archive name table extracted " + std::to_string(names.size()) + " texture name(s).");
    return names;
}

} // namespace

model::MldTextureArchive parseMldTextureArchive(std::span<const std::uint8_t> bytes,
    const std::size_t textureTableOffset) {
    model::MldTextureArchive out{};
    out.tableOffset = textureTableOffset;
    const auto archiveNames = parseTextureArchiveNames(bytes, textureTableOffset, out.diagnostics);

    soasim::gvm::parsing::ParseOptions options{};
    options.decodeBaseLevel = true;
    options.keepRawEncodedPayload = false;
    auto archive = soasim::gvm::parsing::parseGvmArchive(bytes, textureTableOffset, options);

    out.diagnostics.insert(out.diagnostics.end(), archive.diagnostics.begin(), archive.diagnostics.end());
    out.entries.reserve(archive.textures.size());
    for (std::size_t i = 0; i < archive.textures.size(); ++i) {
        const auto& texture = archive.textures[i];

        model::MldTextureEntry entry{};
        entry.archiveOffset = texture.sourceOffset;
        entry.gvrDataOffset = texture.sourceOffset;
        entry.gvrDataSize = texture.sourceSize;
        entry.hasGlobalIndex = true;
        entry.globalIndex = static_cast<std::uint32_t>(i);
        if (i < archiveNames.size()) {
            entry.textureName = archiveNames[i];
        }
        entry.pixelFormat = texture.rawFlags;
        entry.dataFormat = texture.rawDataFormat;
        entry.sourceFormat = soasim::gvm::model::to_string(texture.textureFormat);
        entry.sourcePaletteFormat = soasim::gvm::model::to_string(texture.paletteFormat);
        entry.width = texture.width;
        entry.height = texture.height;
        entry.imageDataOffset = texture.imageDataOffset;
        entry.imageDataSize = texture.imageDataSize;
        entry.diagnostics = texture.diagnostics;

        if (texture.decodedBaseLevel.has_value()) {
            entry.decoded = !texture.decodedBaseLevel->rgba8.empty();
            entry.width = static_cast<std::uint16_t>(texture.decodedBaseLevel->width);
            entry.height = static_cast<std::uint16_t>(texture.decodedBaseLevel->height);
            entry.rgba8 = texture.decodedBaseLevel->rgba8;
        }

        out.entries.push_back(std::move(entry));
    }

    out.diagnostics.push_back("Texture archive parse extracted " + std::to_string(out.entries.size()) + " GVR texture chunk(s).");
    return out;
}

} // namespace soasim::mld::parsing
