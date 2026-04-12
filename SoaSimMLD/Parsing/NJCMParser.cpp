#include "NJCMParser.h"
#include "NJCMParityPath.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_set>

namespace soasim::mld::parsing {
namespace {

[[nodiscard]] std::optional<std::uint16_t> readU16At(std::span<const std::uint8_t> bytes, const std::size_t off, const bool littleEndian) {
    if (off + 2 > bytes.size()) {
        return std::nullopt;
    }
    if (littleEndian) {
        return static_cast<std::uint16_t>(bytes[off]) |
            (static_cast<std::uint16_t>(bytes[off + 1]) << 8);
    }
    return (static_cast<std::uint16_t>(bytes[off]) << 8) |
        static_cast<std::uint16_t>(bytes[off + 1]);
}

[[nodiscard]] std::optional<std::uint32_t> readU32At(std::span<const std::uint8_t> bytes, const std::size_t off, const bool littleEndian) {
    if (off + 4 > bytes.size()) {
        return std::nullopt;
    }
    if (littleEndian) {
        return static_cast<std::uint32_t>(bytes[off]) |
            (static_cast<std::uint32_t>(bytes[off + 1]) << 8) |
            (static_cast<std::uint32_t>(bytes[off + 2]) << 16) |
            (static_cast<std::uint32_t>(bytes[off + 3]) << 24);
    }
    return (static_cast<std::uint32_t>(bytes[off]) << 24) |
        (static_cast<std::uint32_t>(bytes[off + 1]) << 16) |
        (static_cast<std::uint32_t>(bytes[off + 2]) << 8) |
        static_cast<std::uint32_t>(bytes[off + 3]);
}

[[nodiscard]] std::optional<float> readF32At(std::span<const std::uint8_t> bytes, const std::size_t off, const bool littleEndian) {
    const auto u = readU32At(bytes, off, littleEndian);
    if (!u.has_value()) {
        return std::nullopt;
    }
    float out = 0.0f;
    const auto raw = *u;
    std::memcpy(&out, &raw, sizeof(float));
    return out;
}

[[nodiscard]] std::optional<std::size_t> resolvePointer(const std::uint32_t rawPtr, const std::uint32_t imageBase, const std::size_t payloadSize) {
    if (rawPtr == 0) {
        return std::nullopt;
    }
    if (rawPtr < imageBase) {
        return std::nullopt;
    }
    const std::size_t off = static_cast<std::size_t>(rawPtr - imageBase);
    if (off >= payloadSize) {
        return std::nullopt;
    }
    return off;
}

struct NjcmCandidateMetrics {
    std::size_t objectCount = 0;
    std::size_t attachCount = 0;
    std::size_t vertexChunkCount = 0;
    std::size_t polyChunkCount = 0;
    std::size_t decodedVertexCount = 0;
    std::size_t decodedTriangleCount = 0;
    std::size_t score = 0;
};

[[nodiscard]] std::size_t vertexWordsPerVertexByType(const std::uint8_t type) {
    switch (type) {
    case 32U: // Vertex_VertexSH
        return 4;
    case 33U: // Vertex_VertexNormalSH
        return 8;
    case 34U: // Vertex_Vertex
        return 3;
    case 35U: // Vertex_VertexDiffuse8
    case 36U: // Vertex_VertexUserFlags
    case 37U: // Vertex_VertexNinjaFlags
    case 38U: // Vertex_VertexDiffuseSpecular5
    case 39U: // Vertex_VertexDiffuseSpecular4
        return 4;
    case 40U: // Vertex_VertexDiffuseSpecular16
    case 41U: // Vertex_VertexNormal
    case 42U: // Vertex_VertexNormalDiffuse8
    case 43U: // Vertex_VertexNormalUserFlags
    case 44U: // Vertex_VertexNormalNinjaFlags
    case 45U: // Vertex_VertexNormalDiffuseSpecular5
    case 46U: // Vertex_VertexNormalDiffuseSpecular4
    case 47U: // Vertex_VertexNormalDiffuseSpecular16
        return 7;
    default:
        return 0;
    }
}

[[nodiscard]] NjcmCandidateMetrics scoreNjcmCandidate(std::span<const std::uint8_t> data, const bool littleEndian, const std::uint32_t imageBase) {
    NjcmCandidateMetrics m{};
    std::unordered_set<std::size_t> visitedObjects{};
    std::unordered_set<std::size_t> visitedAttaches{};
    std::vector<std::size_t> stack{};
    stack.push_back(0);

    while (!stack.empty()) {
        const std::size_t objOff = stack.back();
        stack.pop_back();
        if (objOff + 0x34 > data.size() || visitedObjects.find(objOff) != visitedObjects.end()) {
            continue;
        }
        visitedObjects.insert(objOff);
        ++m.objectCount;
        m.score += 4;

        const auto attachRaw = readU32At(data, objOff + 0x4, littleEndian);
        const auto childRaw = readU32At(data, objOff + 0x2C, littleEndian);
        const auto siblingRaw = readU32At(data, objOff + 0x30, littleEndian);
        if (!attachRaw.has_value() || !childRaw.has_value() || !siblingRaw.has_value()) {
            continue;
        }

        if (const auto childOff = resolvePointer(*childRaw, imageBase, data.size()); childOff.has_value()) {
            stack.push_back(*childOff);
            ++m.score;
        }
        if (const auto sibOff = resolvePointer(*siblingRaw, imageBase, data.size()); sibOff.has_value()) {
            stack.push_back(*sibOff);
            ++m.score;
        }

        const auto attachOff = resolvePointer(*attachRaw, imageBase, data.size());
        if (!attachOff.has_value() || *attachOff + 0x18 > data.size() || visitedAttaches.find(*attachOff) != visitedAttaches.end()) {
            continue;
        }
        visitedAttaches.insert(*attachOff);
        ++m.attachCount;
        m.score += 3;

        const auto vlistRaw = readU32At(data, *attachOff, littleEndian);
        const auto plistRaw = readU32At(data, *attachOff + 4, littleEndian);
        if (!vlistRaw.has_value() || !plistRaw.has_value()) {
            continue;
        }

        if (const auto vlistOff = resolvePointer(*vlistRaw, imageBase, data.size()); vlistOff.has_value()) {
            std::size_t cur = *vlistOff;
            for (std::size_t i = 0; i < 4096 && cur + 4 <= data.size(); ++i) {
                const auto h1 = readU32At(data, cur, littleEndian);
                if (!h1.has_value()) {
                    break;
                }
                const std::uint8_t type = static_cast<std::uint8_t>(*h1 & 0xFFU);
                if (type == 255U) {
                    ++m.score;
                    break;
                }
                const std::uint16_t szWords32 = static_cast<std::uint16_t>((*h1 >> 16) & 0xFFFFU);
                const std::size_t step = 4U + static_cast<std::size_t>(szWords32) * 4U;
                if (step == 0 || cur + step > data.size()) {
                    break;
                }
                ++m.vertexChunkCount;
                const auto h2 = readU32At(data, cur + 4, littleEndian);
                if (h2.has_value()) {
                    m.decodedVertexCount += static_cast<std::size_t>((*h2 >> 16) & 0xFFFFU);
                }
                m.score += 2;
                cur += step;
            }
        }

        if (const auto plistOff = resolvePointer(*plistRaw, imageBase, data.size()); plistOff.has_value()) {
            std::size_t cur = *plistOff;
            for (std::size_t i = 0; i < 8192 && cur + 2 <= data.size(); ++i) {
                const auto header = readU16At(data, cur, littleEndian);
                if (!header.has_value()) {
                    break;
                }
                const std::uint8_t type = static_cast<std::uint8_t>(*header & 0xFFU);
                if (type == 255U) {
                    ++m.score;
                    break;
                }

                std::size_t step = 0;
                if (type <= 4U || type == 0U) {
                    step = 2;
                } else if (type == 8U || type == 9U) {
                    step = 4;
                } else {
                    const auto sizeWords16 = readU16At(data, cur + 2, littleEndian);
                    if (!sizeWords16.has_value()) {
                        break;
                    }
                    step = 4U + static_cast<std::size_t>(*sizeWords16) * 2U;
                    if (type >= 64U && type <= 75U) {
                        const std::size_t stripDataWords = static_cast<std::size_t>(*sizeWords16);
                        m.decodedTriangleCount += stripDataWords > 6 ? (stripDataWords / 3) : 0;
                    }
                }

                if (step == 0 || cur + step > data.size()) {
                    break;
                }
                ++m.polyChunkCount;
                m.score += 2;
                cur += step;
            }
        }
    }

    return m;
}

} // namespace

std::vector<std::size_t> decodePof0Deltas(std::span<const std::uint8_t> pofData) {
    std::vector<std::size_t> out{};
    std::size_t pos = 0;
    while (pos < pofData.size()) {
        const std::uint8_t b0 = pofData[pos++];
        const std::uint8_t type = static_cast<std::uint8_t>(b0 & 0xC0U);
        std::size_t val = static_cast<std::size_t>(b0 & 0x3FU);
        if (type == 0x00U) {
            continue;
        }
        if (type == 0x80U) {
            if (pos >= pofData.size()) {
                break;
            }
            val = (val << 8) | pofData[pos++];
        } else if (type == 0xC0U) {
            if (pos + 2 >= pofData.size()) {
                break;
            }
            val = (val << 24) |
                (static_cast<std::size_t>(pofData[pos]) << 16) |
                (static_cast<std::size_t>(pofData[pos + 1]) << 8) |
                static_cast<std::size_t>(pofData[pos + 2]);
            pos += 3;
        }
        out.push_back(val * 4U);
    }
    return out;
}

void applyPof0Fixups(std::vector<std::uint8_t>& target,
    const std::vector<std::size_t>& deltas,
    const std::uint32_t imageBase,
    const bool littleEndian) {
    std::size_t cursor = 0;
    for (const auto delta : deltas) {
        cursor += delta;
        if (cursor + 4 > target.size()) {
            break;
        }
        const auto oldPtr = readU32At(target, cursor, littleEndian);
        if (!oldPtr.has_value() || *oldPtr == 0) {
            continue;
        }
        const std::uint32_t patched = *oldPtr + imageBase;
        if (littleEndian) {
            target[cursor] = static_cast<std::uint8_t>(patched & 0xFFU);
            target[cursor + 1] = static_cast<std::uint8_t>((patched >> 8) & 0xFFU);
            target[cursor + 2] = static_cast<std::uint8_t>((patched >> 16) & 0xFFU);
            target[cursor + 3] = static_cast<std::uint8_t>((patched >> 24) & 0xFFU);
        } else {
            target[cursor] = static_cast<std::uint8_t>((patched >> 24) & 0xFFU);
            target[cursor + 1] = static_cast<std::uint8_t>((patched >> 16) & 0xFFU);
            target[cursor + 2] = static_cast<std::uint8_t>((patched >> 8) & 0xFFU);
            target[cursor + 3] = static_cast<std::uint8_t>(patched & 0xFFU);
        }
    }
}

NjcmChunkSummary analyzeNjcmChunk(std::span<const std::uint8_t> njcmData,
    const std::size_t chunkOffset,
    const std::size_t chunkDataSize,
    const bool chunkSizeLittleEndian,
    const bool usedPof0Fixup,
    std::span<const std::uint8_t> pof0Data) {
    NjcmChunkSummary best{};
    best.chunkOffset = chunkOffset;
    best.chunkDataSize = chunkDataSize;
    best.chunkSizeLittleEndian = chunkSizeLittleEndian;
    best.usedPof0Fixup = usedPof0Fixup;

    const std::array<bool, 2> endianModes{ true, false };
    const std::array<std::uint32_t, 3> imageBases{
        0U,
        static_cast<std::uint32_t>(chunkOffset),
        static_cast<std::uint32_t>(chunkOffset + 8U),
    };

    const std::vector<std::size_t> deltas = pof0Data.empty() ? std::vector<std::size_t>{} : decodePof0Deltas(pof0Data);

    for (const bool payloadLe : endianModes) {
        for (const auto imageBase : imageBases) {
            std::vector<std::uint8_t> candidateData(njcmData.begin(), njcmData.end());
            if (!deltas.empty()) {
                applyPof0Fixups(candidateData, deltas, imageBase, payloadLe);
            }

            const auto metrics = scoreNjcmCandidate(std::span<const std::uint8_t>(candidateData.data(), candidateData.size()),
                payloadLe,
                0U);
            if (metrics.score < best.score) {
                continue;
            }
            best.payloadLittleEndian = payloadLe;
            best.imageBase = imageBase;
            best.objectCount = metrics.objectCount;
            best.attachCount = metrics.attachCount;
            best.vertexChunkCount = metrics.vertexChunkCount;
            best.polyChunkCount = metrics.polyChunkCount;
            best.decodedVertexCount = metrics.decodedVertexCount;
            best.decodedTriangleCount = metrics.decodedTriangleCount;
            best.score = metrics.score;
        }
    }
    return best;
}

model::NjcmDecodedChunk decodeNjcmChunkDeterministic(std::span<const std::uint8_t> njcmData,
    const std::size_t chunkOffset,
    const std::size_t chunkDataSize,
    const bool chunkSizeLittleEndian,
    const bool sawPof0Chunk,
    const NjcmParsePolicy& policy,
    std::span<const std::uint8_t> pof0Data) {
    if (policy.useSaToolsParityPath) {
        return decodeNjcmChunkSaToolsParity(njcmData,
            chunkOffset,
            chunkDataSize,
            chunkSizeLittleEndian,
            sawPof0Chunk,
            policy,
            pof0Data);
    }

    model::NjcmDecodedChunk out{};
    out.chunkOffset = chunkOffset;
    out.chunkDataSize = chunkDataSize;
    out.chunkSizeLittleEndian = chunkSizeLittleEndian;
    out.payloadLittleEndian = policy.payloadLittleEndian;
    const std::uint32_t effectiveImageBase = policy.imageBase;
    out.imageBase = policy.imageBase;
    out.sawPof0Chunk = sawPof0Chunk;
    out.usedPof0Fixup = false;

    std::vector<std::uint8_t> decoded(njcmData.begin(), njcmData.end());
    const bool shouldApplyPof0 = sawPof0Chunk && !pof0Data.empty() && policy.applyPof0Fixups;
    if (shouldApplyPof0) {
        const auto deltas = decodePof0Deltas(pof0Data);
        applyPof0Fixups(decoded, deltas, effectiveImageBase, policy.payloadLittleEndian);
        out.usedPof0Fixup = true;
        out.diagnostics.push_back("POF0 sidecar applied before deterministic decode.");
    } else if (sawPof0Chunk) {
        out.diagnostics.push_back("POF0 sidecar detected; skipped fixups per deterministic policy.");
    }

    std::unordered_set<std::size_t> visitedObjects{};
    std::unordered_set<std::size_t> visitedAttaches{};
    std::vector<std::size_t> stack{};
    stack.push_back(0);

    while (!stack.empty()) {
        const std::size_t objOff = stack.back();
        stack.pop_back();
        if (objOff + 0x34 > decoded.size() || visitedObjects.find(objOff) != visitedObjects.end()) {
            continue;
        }
        visitedObjects.insert(objOff);

        model::NjObjectRecord obj{};
        obj.offset = objOff;

        const auto attachRaw = readU32At(decoded, objOff + 0x4, policy.payloadLittleEndian);
        const auto childRaw = readU32At(decoded, objOff + 0x2C, policy.payloadLittleEndian);
        const auto siblingRaw = readU32At(decoded, objOff + 0x30, policy.payloadLittleEndian);
        if (!attachRaw.has_value() || !childRaw.has_value() || !siblingRaw.has_value()) {
            out.diagnostics.push_back("Object record truncated while reading pointer fields.");
            continue;
        }

        if (const auto childOff = resolvePointer(*childRaw, effectiveImageBase, decoded.size()); childOff.has_value()) {
            obj.hasChild = true;
            obj.childOffset = *childOff;
            stack.push_back(*childOff);
        }
        if (const auto siblingOff = resolvePointer(*siblingRaw, effectiveImageBase, decoded.size()); siblingOff.has_value()) {
            obj.hasSibling = true;
            obj.siblingOffset = *siblingOff;
            stack.push_back(*siblingOff);
        }

        if (const auto attachOff = resolvePointer(*attachRaw, effectiveImageBase, decoded.size()); attachOff.has_value()) {
            obj.hasAttach = true;
            obj.attachOffset = *attachOff;
            if (*attachOff + 8 > decoded.size()) {
                out.diagnostics.push_back("Attach pointer out of range; skipping attach decode.");
            } else if (visitedAttaches.find(*attachOff) == visitedAttaches.end()) {
                visitedAttaches.insert(*attachOff);
                model::NjAttachRecord attach{};
                attach.offset = *attachOff;
                std::vector<model::NjSemanticVertex> indexedVertices{};
                const auto vlistRaw = readU32At(decoded, *attachOff, policy.payloadLittleEndian);
                const auto plistRaw = readU32At(decoded, *attachOff + 4, policy.payloadLittleEndian);
                if (!vlistRaw.has_value() || !plistRaw.has_value()) {
                    out.diagnostics.push_back("Attach record truncated while reading list pointers.");
                } else {
                    if (const auto vlistOff = resolvePointer(*vlistRaw, effectiveImageBase, decoded.size()); vlistOff.has_value()) {
                        attach.vertexListOffset = *vlistOff;
                        std::size_t cur = *vlistOff;
                        for (std::size_t i = 0; i < 4096 && cur + 4 <= decoded.size(); ++i) {
                            const auto h1 = readU32At(decoded, cur, policy.payloadLittleEndian);
                            if (!h1.has_value()) {
                                break;
                            }
                            const std::uint8_t type = static_cast<std::uint8_t>(*h1 & 0xFFU);
                            if (type == 255U) {
                                break;
                            }
                            const std::uint16_t szWords32 = static_cast<std::uint16_t>((*h1 >> 16) & 0xFFFFU);
                            const std::size_t step = 4U + static_cast<std::size_t>(szWords32) * 4U;
                            if (step == 0 || cur + step > decoded.size()) {
                                out.diagnostics.push_back("Vertex chunk step exceeded buffer; stopping attach vertex decode.");
                                break;
                            }
                            model::NjVertexChunkRecord vc{};
                            vc.offset = cur;
                            vc.type = type;
                            vc.sizeWords32 = szWords32;
                            const auto h2 = readU32At(decoded, cur + 4, policy.payloadLittleEndian);
                            if (h2.has_value()) {
                                vc.indexOffset = static_cast<std::uint16_t>(*h2 & 0xFFFFU);
                                vc.vertexCount = static_cast<std::uint16_t>((*h2 >> 16) & 0xFFFFU);
                                attach.decodedVertexCount += vc.vertexCount;
                            }
                            attach.vertexChunks.push_back(vc);

                            if (vc.vertexCount > 0) {
                                const std::size_t payloadOff = cur + 8;
                                if (payloadOff <= decoded.size() && step >= 8) {
                                    const std::size_t payloadBytes = step - 8;
                                    std::size_t stride = 0;
                                    const auto wordsPerVertex = vertexWordsPerVertexByType(type);
                                    if (wordsPerVertex > 0) {
                                        stride = wordsPerVertex * 4U;
                                    } else {
                                        stride = payloadBytes / static_cast<std::size_t>(vc.vertexCount);
                                    }
                                    if (stride >= 12) {
                                        for (std::uint16_t vi = 0; vi < vc.vertexCount; ++vi) {
                                            const std::size_t vOff = payloadOff + static_cast<std::size_t>(vi) * stride;
                                            model::NjSemanticVertex sv{};
                                            const auto px = readF32At(decoded, vOff, policy.payloadLittleEndian);
                                            const auto py = readF32At(decoded, vOff + 4, policy.payloadLittleEndian);
                                            const auto pz = readF32At(decoded, vOff + 8, policy.payloadLittleEndian);
                                            if (px.has_value() && py.has_value() && pz.has_value()) {
                                                sv.position.x = *px;
                                                sv.position.y = *py;
                                                sv.position.z = *pz;
                                                sv.hasPosition = true;
                                            }
                                            const std::size_t outIdx = static_cast<std::size_t>(vc.indexOffset) + static_cast<std::size_t>(vi);
                                            if (outIdx >= indexedVertices.size()) {
                                                indexedVertices.resize(outIdx + 1);
                                            }
                                            indexedVertices[outIdx] = sv;
                                        }
                                    } else {
                                        const std::size_t required = static_cast<std::size_t>(vc.indexOffset) + static_cast<std::size_t>(vc.vertexCount);
                                        if (required > indexedVertices.size()) {
                                            indexedVertices.resize(required);
                                        }
                                    }
                                }
                            }
                            cur += step;
                        }
                    }
                    attach.semanticVertices = std::move(indexedVertices);

                    if (const auto plistOff = resolvePointer(*plistRaw, effectiveImageBase, decoded.size()); plistOff.has_value()) {
                        attach.polyListOffset = *plistOff;
                        std::size_t cur = *plistOff;
                        for (std::size_t i = 0; i < 8192 && cur + 2 <= decoded.size(); ++i) {
                            const auto header = readU16At(decoded, cur, policy.payloadLittleEndian);
                            if (!header.has_value()) {
                                break;
                            }
                            const std::uint8_t type = static_cast<std::uint8_t>(*header & 0xFFU);
                            if (type == 255U) {
                                break;
                            }
                            std::size_t step = 0;
                            std::uint16_t sizeWords16 = 0;
                            if (type <= 4U || type == 0U) {
                                step = 2;
                            } else if (type == 8U || type == 9U) {
                                step = 4;
                            } else {
                                const auto sw = readU16At(decoded, cur + 2, policy.payloadLittleEndian);
                                if (!sw.has_value()) {
                                    out.diagnostics.push_back("Polygon chunk truncated while reading size field.");
                                    break;
                                }
                                sizeWords16 = *sw;
                                step = 4U + static_cast<std::size_t>(sizeWords16) * 2U;
                            }

                            if (step == 0 || cur + step > decoded.size()) {
                                out.diagnostics.push_back("Polygon chunk step exceeded buffer; stopping attach polygon decode.");
                                break;
                            }
                            model::NjPolyChunkRecord pc{};
                            pc.offset = cur;
                            pc.type = type;
                            pc.sizeWords16 = sizeWords16;
                            attach.polyChunks.push_back(pc);

                            model::NjSemanticPolygon sp{};
                            sp.type = type;
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

                            const auto readWord = [&](const std::size_t off) -> std::optional<std::uint16_t> {
                                return readU16At(decoded, off, policy.payloadLittleEndian);
                            };

                            if (type >= 64U && type <= 75U && step >= 6) {
                                const auto stripHeader = readWord(cur + 2);
                                if (!stripHeader.has_value()) {
                                    out.diagnostics.push_back("Strip chunk truncated while reading strip header2.");
                                } else {
                                    const std::uint16_t userOffset = static_cast<std::uint16_t>((*stripHeader >> 14) & 0x3U);
                                    const std::uint16_t stripCount = static_cast<std::uint16_t>(*stripHeader & 0x3FFFU);
                                    std::size_t pos = cur + 4;
                                    std::size_t parsedStrips = 0;

                                    std::size_t wordsPerVertex = 1;
                                    if (type == 65U || type == 66U || type == 70U) {
                                        wordsPerVertex = 3;
                                    } else if (type == 71U || type == 72U || type == 74U || type == 75U) {
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
                                            out.diagnostics.push_back("Strip payload exceeded chunk bounds while parsing indices.");
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
                            } else if (type == 56U || type == 57U || type == 58U) {
                                const auto volHeader = readWord(cur + 2);
                                if (!volHeader.has_value()) {
                                    out.diagnostics.push_back("Volume chunk truncated while reading header2.");
                                } else {
                                    const std::uint16_t userOffset = static_cast<std::uint16_t>((*volHeader >> 14) & 0x3U);
                                    const std::uint16_t polyCount = static_cast<std::uint16_t>(*volHeader & 0x3FFFU);
                                    std::size_t pos = cur + 4;
                                    for (std::size_t pi = 0; pi < polyCount && pos < cur + step; ++pi) {
                                        if (type == 56U) {
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
                                        } else if (type == 57U) {
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
                                    const auto word = readU16At(decoded, cur + 4 + wi * 2, policy.payloadLittleEndian);
                                    if (!word.has_value()) {
                                        break;
                                    }
                                    pc.rawIndexWords.push_back(*word);
                                }
                            }
                            attach.semanticPolygons.push_back(std::move(sp));
                            if (!attach.polyChunks.empty()) {
                                attach.polyChunks.back() = pc;
                            }
                            cur += step;
                        }
                    }
                }
                out.attaches.push_back(std::move(attach));
            }
        }
        out.objects.push_back(std::move(obj));
    }

    out.parseSucceeded = !out.objects.empty();
    if (!out.parseSucceeded && policy.allowHeuristicFallback) {
        out.parsedWithHeuristicFallback = true;
        const auto fallback = analyzeNjcmChunk(njcmData, chunkOffset, chunkDataSize, chunkSizeLittleEndian, sawPof0Chunk, {});
        if (fallback.score > 0) {
            out.parseSucceeded = true;
            out.diagnostics.push_back("Deterministic decode produced no objects; legacy heuristic fallback reported score " + std::to_string(fallback.score) + ".");
        }
    }

    return out;
}

NjcmChunkSummary summarizeDecodedNjcmChunk(const model::NjcmDecodedChunk& decodedChunk) {
    NjcmChunkSummary out{};
    out.chunkOffset = decodedChunk.chunkOffset;
    out.chunkDataSize = decodedChunk.chunkDataSize;
    out.chunkSizeLittleEndian = decodedChunk.chunkSizeLittleEndian;
    out.payloadLittleEndian = decodedChunk.payloadLittleEndian;
    out.imageBase = decodedChunk.imageBase;
    out.usedPof0Fixup = decodedChunk.usedPof0Fixup;
    out.objectCount = decodedChunk.objects.size();
    out.attachCount = decodedChunk.attaches.size();
    for (const auto& attach : decodedChunk.attaches) {
        out.vertexChunkCount += attach.vertexChunks.size();
        out.polyChunkCount += attach.polyChunks.size();
        out.decodedVertexCount += attach.decodedVertexCount;
        out.decodedTriangleCount += attach.decodedTriangleCount;
    }
    out.score = out.objectCount * 4 + out.attachCount * 3 + out.vertexChunkCount * 2 + out.polyChunkCount * 2;
    return out;
}

} // namespace soasim::mld::parsing
