#include "NJCMPolyRecordParser.h"

#include <algorithm>

namespace soasim::mld::parsing {
namespace {

[[nodiscard]] std::optional<model::NjPolyChunkRecord> parsePolyChunkHeader(const NjcmDecodeContext& ctx, const std::size_t offset, std::size_t& step) {
    const auto header = readU16At(ctx.decoded, offset, ctx.littleEndian);
    if (!header.has_value()) {
        return std::nullopt;
    }

    model::NjPolyChunkRecord pc{};
    pc.offset = offset;
    pc.type = static_cast<std::uint8_t>(*header & 0xFFU);

    if (pc.type <= 4U || pc.type == 0U) {
        step = 2;
    } else if (pc.type == 8U || pc.type == 9U) {
        step = 4;
    } else {
        const auto sw = readU16At(ctx.decoded, offset + 2, ctx.littleEndian);
        if (!sw.has_value()) {
            ctx.out->diagnostics.push_back("Polygon chunk truncated while reading size field.");
            return std::nullopt;
        }
        pc.sizeWords16 = *sw;
        step = 4U + static_cast<std::size_t>(*sw) * 2U;
    }

    return pc;
}

} // namespace

void parsePolyRecords(const NjcmDecodeContext& ctx, const std::size_t polyListOffset, model::NjAttachRecord& attach) {
    attach.polyListOffset = polyListOffset;
    std::size_t cur = polyListOffset;
    const auto readWord = [&](const std::size_t off) -> std::optional<std::uint16_t> {
        return readU16At(ctx.decoded, off, ctx.littleEndian);
    };

    for (std::size_t i = 0; i < 8192 && cur + 2 <= ctx.decoded.size(); ++i) {
        std::size_t step = 0;
        const auto maybePc = parsePolyChunkHeader(ctx, cur, step);
        if (!maybePc.has_value()) {
            break;
        }
        model::NjPolyChunkRecord pc = *maybePc;
        if (pc.type == 255U) {
            break;
        }
        if (step == 0 || cur + step > ctx.decoded.size()) {
            ctx.out->diagnostics.push_back("Polygon chunk step exceeded buffer; stopping attach polygon decode.");
            break;
        }

        model::NjSemanticPolygon sp{};
        sp.type = pc.type;
        auto appendTriangle = [&sp, &attach](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
            if (a == b || b == c || a == c) {
                return;
            }
            sp.indices.push_back(a);
            sp.indices.push_back(b);
            sp.indices.push_back(c);
            ++sp.estimatedTriangleCount;
            ++attach.decodedTriangleCount;
        };

        if (pc.type >= 64U && pc.type <= 75U && step >= 6) {
            const auto stripHeader = readWord(cur + 2);
            if (!stripHeader.has_value()) {
                ctx.out->diagnostics.push_back("Strip chunk truncated while reading strip header2.");
            } else {
                const std::uint16_t userOffset = static_cast<std::uint16_t>((*stripHeader >> 14) & 0x3U);
                const std::uint16_t stripCount = static_cast<std::uint16_t>(*stripHeader & 0x3FFFU);
                std::size_t pos = cur + 4;
                std::size_t parsedStrips = 0;

                std::size_t wordsPerVertex = 1;
                if (pc.type == 65U || pc.type == 66U || pc.type == 70U) {
                    wordsPerVertex = 3;
                } else if (pc.type == 71U || pc.type == 72U || pc.type == 74U || pc.type == 75U) {
                    wordsPerVertex = 5;
                }

                while (parsedStrips < stripCount && pos + 2 <= cur + step) {
                    const auto flagLen = readWord(pos);
                    if (!flagLen.has_value()) {
                        break;
                    }
                    pos += 2;
                    const bool reverse = ((*flagLen & 0x8000U) != 0);
                    const std::size_t len = static_cast<std::size_t>(*flagLen & 0x7FFFU);
                    std::vector<std::uint32_t> stripIndices{};
                    stripIndices.reserve(len);

                    bool stripOk = true;
                    for (std::size_t vi = 0; vi < len; ++vi) {
                        const std::size_t perVertexWords = wordsPerVertex + ((vi >= 2) ? userOffset : 0U);
                        if (pos + perVertexWords * 2 > cur + step) {
                            stripOk = false;
                            break;
                        }
                        const auto idxWord = readWord(pos);
                        if (!idxWord.has_value()) {
                            stripOk = false;
                            break;
                        }
                        stripIndices.push_back(static_cast<std::uint32_t>(*idxWord & 0x7FFFU));
                        pc.rawIndexWords.push_back(*idxWord);
                        pos += perVertexWords * 2;
                    }
                    if (!stripOk) {
                        ctx.out->diagnostics.push_back("Strip payload exceeded chunk bounds while parsing indices.");
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
        } else if (pc.type == 56U || pc.type == 57U || pc.type == 58U) {
            const auto volHeader = readWord(cur + 2);
            if (!volHeader.has_value()) {
                ctx.out->diagnostics.push_back("Volume chunk truncated while reading header2.");
            } else {
                const std::uint16_t userOffset = static_cast<std::uint16_t>((*volHeader >> 14) & 0x3U);
                const std::uint16_t polyCount = static_cast<std::uint16_t>(*volHeader & 0x3FFFU);
                std::size_t pos = cur + 4;
                for (std::size_t pi = 0; pi < polyCount && pos < cur + step; ++pi) {
                    if (pc.type == 56U) {
                        if (pos + (3U + userOffset) * 2U > cur + step) {
                            break;
                        }
                        const auto i0 = readWord(pos);
                        const auto i1 = readWord(pos + 2);
                        const auto i2 = readWord(pos + 4);
                        if (!i0.has_value() || !i1.has_value() || !i2.has_value()) {
                            break;
                        }
                        pc.rawIndexWords.push_back(*i0);
                        pc.rawIndexWords.push_back(*i1);
                        pc.rawIndexWords.push_back(*i2);
                        appendTriangle(static_cast<std::uint32_t>(*i0 & 0x7FFFU),
                            static_cast<std::uint32_t>(*i1 & 0x7FFFU),
                            static_cast<std::uint32_t>(*i2 & 0x7FFFU));
                        pos += (3U + userOffset) * 2U;
                    } else if (pc.type == 57U) {
                        if (pos + (4U + userOffset) * 2U > cur + step) {
                            break;
                        }
                        const auto i0 = readWord(pos);
                        const auto i1 = readWord(pos + 2);
                        const auto i2 = readWord(pos + 4);
                        const auto i3 = readWord(pos + 6);
                        if (!i0.has_value() || !i1.has_value() || !i2.has_value() || !i3.has_value()) {
                            break;
                        }
                        pc.rawIndexWords.push_back(*i0);
                        pc.rawIndexWords.push_back(*i1);
                        pc.rawIndexWords.push_back(*i2);
                        pc.rawIndexWords.push_back(*i3);
                        const std::uint32_t a = static_cast<std::uint32_t>(*i0 & 0x7FFFU);
                        const std::uint32_t b = static_cast<std::uint32_t>(*i1 & 0x7FFFU);
                        const std::uint32_t c = static_cast<std::uint32_t>(*i2 & 0x7FFFU);
                        const std::uint32_t d = static_cast<std::uint32_t>(*i3 & 0x7FFFU);
                        appendTriangle(a, b, c);
                        appendTriangle(a, c, d);
                        pos += (4U + userOffset) * 2U;
                    } else {
                        if (pos + 2 > cur + step) {
                            break;
                        }
                        const auto flagLen = readWord(pos);
                        if (!flagLen.has_value()) {
                            break;
                        }
                        pos += 2;
                        const bool reverse = ((*flagLen & 0x8000U) != 0);
                        const std::size_t len = static_cast<std::size_t>(*flagLen & 0x7FFFU);
                        std::vector<std::uint32_t> stripIndices{};
                        stripIndices.reserve(len);
                        bool stripOk = true;
                        for (std::size_t vi = 0; vi < len; ++vi) {
                            const std::size_t words = 1U + ((vi >= 2) ? userOffset : 0U);
                            if (pos + words * 2 > cur + step) {
                                stripOk = false;
                                break;
                            }
                            const auto idxWord = readWord(pos);
                            if (!idxWord.has_value()) {
                                stripOk = false;
                                break;
                            }
                            stripIndices.push_back(static_cast<std::uint32_t>(*idxWord & 0x7FFFU));
                            pc.rawIndexWords.push_back(*idxWord);
                            pos += words * 2;
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
        }

        pc.estimatedTriangleCount = sp.estimatedTriangleCount;
        if (sp.indices.empty() && step >= 4) {
            const std::size_t words = (step - 4) / 2;
            pc.rawIndexWords.reserve(words);
            for (std::size_t wi = 0; wi < words; ++wi) {
                const auto word = readU16At(ctx.decoded, cur + 4 + wi * 2, ctx.littleEndian);
                if (!word.has_value()) {
                    break;
                }
                pc.rawIndexWords.push_back(*word);
            }
        }

        attach.semanticPolygons.push_back(std::move(sp));
        attach.polyChunks.push_back(std::move(pc));
        cur += step;
    }
}

} // namespace soasim::mld::parsing
