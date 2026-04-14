#include "NJTLParser.h"

#include "../common/ByteUtils.h"

#include <string>

namespace soasim::mld::parsing {

namespace {

[[nodiscard]] std::uint32_t swapU32(std::uint32_t value) {
    return ((value & 0x000000FFU) << 24) |
        ((value & 0x0000FF00U) << 8) |
        ((value & 0x00FF0000U) >> 8) |
        ((value & 0xFF000000U) >> 24);
}

[[nodiscard]] std::string readAsciiCString(std::span<const std::uint8_t> bytes, std::size_t startOffset) {
    std::string out{};
    if (startOffset >= bytes.size()) {
        return out;
    }
    for (std::size_t i = startOffset; i < bytes.size(); ++i) {
        const auto ch = bytes[i];
        if (ch == 0U) {
            break;
        }
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

} // namespace

model::NjtlBlock parseNjtlBlock(std::span<const std::uint8_t> data,
    const std::size_t chunkOffset,
    const std::size_t chunkDataSize,
    const bool chunkSizeLittleEndian) {
    model::NjtlBlock out{};
    out.chunkOffset = chunkOffset;
    out.chunkDataSize = chunkDataSize;
    out.chunkSizeLittleEndian = chunkSizeLittleEndian;
    out.imageBase = static_cast<std::uint32_t>(chunkOffset + 8U);

    const auto firstPointerRaw = common::readU32AtLE(data, 0x00);
    const auto textureCountRaw = common::readU32AtLE(data, 0x04);
    if (!firstPointerRaw.has_value() || !textureCountRaw.has_value()) {
        out.diagnostics.push_back("NJTL header truncated.");
        return out;
    }

    out.firstTextureNamePointer = chunkSizeLittleEndian ? *firstPointerRaw : swapU32(*firstPointerRaw);
    out.textureCount = chunkSizeLittleEndian ? *textureCountRaw : swapU32(*textureCountRaw);
    if (out.firstTextureNamePointer < out.imageBase) {
        out.diagnostics.push_back("NJTL first texture-name pointer is below image base.");
        return out;
    }
    const std::size_t firstEntryOffset = static_cast<std::size_t>(out.firstTextureNamePointer - out.imageBase);

    if (firstEntryOffset > data.size()) {
        out.diagnostics.push_back("NJTL first texture-name entry offset out of bounds.");
        return out;
    }

    out.textureNames.reserve(out.textureCount);
    for (std::size_t i = 0; i < static_cast<std::size_t>(out.textureCount); ++i) {
        const std::size_t recordOffset = firstEntryOffset + (i * 0xCU);
        const auto ptrRaw = common::readU32AtLE(data, recordOffset + 0x0);
        const auto reserved0Raw = common::readU32AtLE(data, recordOffset + 0x4);
        const auto reserved1Raw = common::readU32AtLE(data, recordOffset + 0x8);
        if (!ptrRaw.has_value() || !reserved0Raw.has_value() || !reserved1Raw.has_value()) {
            out.diagnostics.push_back("NJTL texture-name record truncated at index " + std::to_string(i) + ".");
            break;
        }

        model::NjtlBlock::TextureNameRecord record{};
        record.recordOffset = recordOffset;
        record.textureNamePointer = chunkSizeLittleEndian ? *ptrRaw : swapU32(*ptrRaw);
        record.reserved0 = chunkSizeLittleEndian ? *reserved0Raw : swapU32(*reserved0Raw);
        record.reserved1 = chunkSizeLittleEndian ? *reserved1Raw : swapU32(*reserved1Raw);

        if (record.textureNamePointer >= out.imageBase) {
            const std::size_t nameOffset = static_cast<std::size_t>(record.textureNamePointer - out.imageBase);
            if (nameOffset < data.size()) {
                record.name = readAsciiCString(data, nameOffset);
            } else {
                out.diagnostics.push_back("NJTL texture-name pointer out of bounds at index " + std::to_string(i) + ".");
            }
        } else {
            out.diagnostics.push_back("NJTL texture-name pointer below image base at index " + std::to_string(i) + ".");
        }

        out.textureNames.push_back(std::move(record));
    }

    out.parseSucceeded = out.textureNames.size() == static_cast<std::size_t>(out.textureCount);
    if (!out.parseSucceeded && out.diagnostics.empty()) {
        out.diagnostics.push_back("NJTL parse incomplete.");
    }
    if (out.parseSucceeded && out.diagnostics.empty()) {
        out.diagnostics.push_back("NJTL parsed successfully.");
    }
    return out;
}

} // namespace soasim::mld::parsing
