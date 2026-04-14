#include "MldTextureArchiveParser.h"

#include "../common/ByteUtils.h"

#include <algorithm>
#include <optional>
#include <string>

namespace soasim::mld::parsing {
namespace {

constexpr std::uint32_t makeTag(const char a, const char b, const char c, const char d) {
    return static_cast<std::uint32_t>(a) |
        (static_cast<std::uint32_t>(b) << 8) |
        (static_cast<std::uint32_t>(c) << 16) |
        (static_cast<std::uint32_t>(d) << 24);
}

[[nodiscard]] std::uint32_t swapU32(const std::uint32_t value) {
    return ((value & 0x000000FFU) << 24) |
        ((value & 0x0000FF00U) << 8) |
        ((value & 0x00FF0000U) >> 8) |
        ((value & 0xFF000000U) >> 24);
}

[[nodiscard]] std::optional<std::size_t> readChunkPayloadSize(std::span<const std::uint8_t> bytes, std::size_t at) {
    const auto raw = common::readU32AtLE(bytes, at + 0x4);
    if (!raw.has_value()) {
        return std::nullopt;
    }

    const auto le = static_cast<std::size_t>(*raw);
    if (at + 8U + le <= bytes.size()) {
        return le;
    }

    const auto be = static_cast<std::size_t>(swapU32(*raw));
    if (at + 8U + be <= bytes.size()) {
        return be;
    }

    return std::nullopt;
}

} // namespace

model::MldTextureArchive parseMldTextureArchive(std::span<const std::uint8_t> bytes,
    const std::size_t textureTableOffset) {
    model::MldTextureArchive out{};
    out.tableOffset = textureTableOffset;

    if (textureTableOffset >= bytes.size()) {
        out.diagnostics.push_back("Texture table pointer out of bounds.");
        return out;
    }

    constexpr std::uint32_t tagGbix = makeTag('G', 'B', 'I', 'X');
    constexpr std::uint32_t tagGvrt = makeTag('G', 'V', 'R', 'T');

    struct PendingGbix {
        std::size_t archiveOffset = 0;
        bool hasGlobalIndex = false;
        std::uint32_t globalIndex = 0;
    };

    std::optional<PendingGbix> pendingGbix{};
    std::size_t cursor = textureTableOffset;

    while (cursor + 8U <= bytes.size()) {
        const auto maybeTag = common::readU32AtLE(bytes, cursor);
        if (!maybeTag.has_value()) {
            break;
        }

        if (*maybeTag != tagGbix && *maybeTag != tagGvrt) {
            ++cursor;
            continue;
        }

        const auto maybeChunkSize = readChunkPayloadSize(bytes, cursor);
        if (!maybeChunkSize.has_value()) {
            ++cursor;
            continue;
        }

        const std::size_t chunkEnd = cursor + 8U + *maybeChunkSize;
        if (*maybeTag == tagGbix) {
            PendingGbix pending{};
            pending.archiveOffset = cursor;

            const auto globalRaw = common::readU32AtLE(bytes, cursor + 0x8);
            if (globalRaw.has_value()) {
                pending.hasGlobalIndex = true;
                pending.globalIndex = *globalRaw;
            }
            pendingGbix = pending;
        } else if (*maybeTag == tagGvrt) {
            model::MldTextureEntry entry{};
            entry.archiveOffset = pendingGbix.has_value() ? pendingGbix->archiveOffset : cursor;
            entry.gvrDataOffset = cursor;
            entry.gvrDataSize = chunkEnd - cursor;
            if (pendingGbix.has_value()) {
                entry.hasGlobalIndex = pendingGbix->hasGlobalIndex;
                entry.globalIndex = pendingGbix->globalIndex;
            }

            entry.gvrData.assign(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                bytes.begin() + static_cast<std::ptrdiff_t>(chunkEnd));

            if (entry.gvrData.size() >= 0x10U) {
                const auto payload = std::span<const std::uint8_t>(entry.gvrData.data() + 8U, entry.gvrData.size() - 8U);
                entry.pixelFormat = payload[0];
                entry.dataFormat = payload[1];
                entry.width = static_cast<std::uint16_t>(payload[4] | (static_cast<std::uint16_t>(payload[5]) << 8U));
                entry.height = static_cast<std::uint16_t>(payload[6] | (static_cast<std::uint16_t>(payload[7]) << 8U));
                entry.imageDataOffset = 16U;
                entry.imageDataSize = entry.gvrData.size() - 16U;
            } else {
                entry.diagnostics.push_back("GVRT chunk too small for texture header decode.");
            }
            out.entries.push_back(std::move(entry));
            pendingGbix.reset();
        }

        cursor = chunkEnd;
    }

    out.diagnostics.push_back("Texture archive parse extracted " + std::to_string(out.entries.size()) + " GVRT texture chunk(s).");
    return out;
}

} // namespace soasim::mld::parsing
