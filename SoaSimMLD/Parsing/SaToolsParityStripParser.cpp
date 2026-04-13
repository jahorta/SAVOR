#include "SaToolsParityStripParser.h"

#include <algorithm>

namespace soasim::mld::parsing::satools_parity {

void parseStripChunk(const NjcmDecodeContext& ctx,
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
        ctx.out->diagnostics.push_back("SA-parity strip chunk missing header2.");
        return;
    }

    const std::uint16_t userOffset = static_cast<std::uint16_t>((*header2 >> 14) & 0x3U);
    const std::uint16_t stripCount = static_cast<std::uint16_t>(*header2 & 0x3FFFU);
    std::size_t pos = chunkStart + 6;
    std::size_t parsedStrips = 0;

    std::size_t wordsPerVertex = 1;
    switch (type) {
    case 65U: // StripUVN
    case 66U: // StripUVH
    case 71U: // StripUVN2
    case 72U: // StripUVH2
        wordsPerVertex = 3;
        break;
    case 74U:
    case 75U:
        wordsPerVertex = 5;
        break;
    default:
        wordsPerVertex = 1;
        break;
    }

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

    while (parsedStrips < stripCount && pos + 2 <= chunkEnd) {
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
            const std::size_t perVertexWords = wordsPerVertex + ((vi >= 2U) ? userOffset : 0U);
            if (pos + perVertexWords * 2U > chunkEnd) {
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
            pos += perVertexWords * 2U;
        }

        if (!stripOk) {
            ctx.out->diagnostics.push_back("SA-parity strip payload exceeded chunk bounds.");
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

        ++parsedStrips;
    }
}

} // namespace soasim::mld::parsing::satools_parity
