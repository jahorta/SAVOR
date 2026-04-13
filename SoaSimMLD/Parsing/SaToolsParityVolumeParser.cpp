#include "SaToolsParityVolumeParser.h"

#include <algorithm>

namespace soasim::mld::parsing::satools_parity {

void parseVolumeChunk(const NjcmDecodeContext& ctx,
    const std::size_t chunkStart,
    const std::size_t chunkEnd,
    const std::uint8_t type,
    model::NjPolyChunkRecord& polyChunk,
    model::NjSemanticPolygon& semanticPolygon,
    std::size_t& attachTriangleCount) {
    const auto readWord = [&](const std::size_t off) -> std::optional<std::uint16_t> {
        return readU16At(ctx.decoded, off, ctx.littleEndian);
    };

    const auto header2 = readWord(chunkStart + 4);
    if (!header2.has_value()) {
        ctx.out->diagnostics.push_back("SA-parity volume chunk missing header2.");
        return;
    }

    const std::uint16_t userOffset = static_cast<std::uint16_t>((*header2 >> 14) & 0x3U);
    const std::uint16_t polyCount = static_cast<std::uint16_t>(*header2 & 0x3FFFU);
    std::size_t pos = chunkStart + 6;

    auto appendTriangle = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        if (a == b || b == c || a == c) {
            return;
        }
        semanticPolygon.indices.push_back(a);
        semanticPolygon.indices.push_back(b);
        semanticPolygon.indices.push_back(c);
        ++semanticPolygon.estimatedTriangleCount;
        ++attachTriangleCount;
    };

    for (std::size_t pi = 0; pi < polyCount && pos < chunkEnd; ++pi) {
        if (type == 56U) {
            if (pos + (3U + userOffset) * 2U > chunkEnd) {
                break;
            }
            const auto i0 = readWord(pos);
            const auto i1 = readWord(pos + 2);
            const auto i2 = readWord(pos + 4);
            if (!i0.has_value() || !i1.has_value() || !i2.has_value()) {
                break;
            }
            polyChunk.rawIndexWords.push_back(*i0);
            polyChunk.rawIndexWords.push_back(*i1);
            polyChunk.rawIndexWords.push_back(*i2);
            appendTriangle(static_cast<std::uint32_t>(*i0 & 0x7FFFU),
                static_cast<std::uint32_t>(*i1 & 0x7FFFU),
                static_cast<std::uint32_t>(*i2 & 0x7FFFU));
            pos += (3U + userOffset) * 2U;
        } else if (type == 57U) {
            if (pos + (4U + userOffset) * 2U > chunkEnd) {
                break;
            }
            const auto i0 = readWord(pos);
            const auto i1 = readWord(pos + 2);
            const auto i2 = readWord(pos + 4);
            const auto i3 = readWord(pos + 6);
            if (!i0.has_value() || !i1.has_value() || !i2.has_value() || !i3.has_value()) {
                break;
            }
            polyChunk.rawIndexWords.push_back(*i0);
            polyChunk.rawIndexWords.push_back(*i1);
            polyChunk.rawIndexWords.push_back(*i2);
            polyChunk.rawIndexWords.push_back(*i3);
            const std::uint32_t a = static_cast<std::uint32_t>(*i0 & 0x7FFFU);
            const std::uint32_t b = static_cast<std::uint32_t>(*i1 & 0x7FFFU);
            const std::uint32_t c = static_cast<std::uint32_t>(*i2 & 0x7FFFU);
            const std::uint32_t d = static_cast<std::uint32_t>(*i3 & 0x7FFFU);
            appendTriangle(a, b, c);
            appendTriangle(a, c, d);
            pos += (4U + userOffset) * 2U;
        } else {
            if (pos + 2 > chunkEnd) {
                break;
            }
            const auto flagLen = readWord(pos);
            if (!flagLen.has_value()) {
                break;
            }
            pos += 2;
            const bool reverse = ((*flagLen & 0x8000U) != 0U);
            const std::size_t len = static_cast<std::size_t>(*flagLen & 0x7FFFU);
            std::vector<std::uint32_t> stripIndices{};
            stripIndices.reserve(len);
            bool stripOk = true;

            for (std::size_t vi = 0; vi < len; ++vi) {
                const std::size_t words = 1U + ((vi >= 2U) ? userOffset : 0U);
                if (pos + words * 2U > chunkEnd) {
                    stripOk = false;
                    break;
                }
                const auto idxWord = readWord(pos);
                if (!idxWord.has_value()) {
                    stripOk = false;
                    break;
                }
                stripIndices.push_back(static_cast<std::uint32_t>(*idxWord & 0x7FFFU));
                polyChunk.rawIndexWords.push_back(*idxWord);
                pos += words * 2U;
            }
            if (!stripOk) {
                break;
            }

            for (std::size_t ii = 2; ii < stripIndices.size(); ++ii) {
                std::uint32_t a = stripIndices[ii - 2];
                std::uint32_t b = stripIndices[ii - 1];
                const std::uint32_t c = stripIndices[ii];
                if ((ii & 1U) != 0U) {
                    std::swap(a, b);
                }
                if (reverse) {
                    std::swap(a, b);
                }
                appendTriangle(a, b, c);
            }
        }
    }
}

} // namespace soasim::mld::parsing::satools_parity
