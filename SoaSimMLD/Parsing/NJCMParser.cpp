#include "NJCMParser.h"

#include "NJCMAttachRecordParser.h"
#include "NJCMDecodeContext.h"
#include "NJCMObjectRecordParser.h"
#include "NJCMParityPath.h"

#include <array>
#include <optional>
#include <string>
#include <unordered_set>

namespace soasim::mld::parsing {
namespace {

struct NjcmCandidateMetrics {
    std::size_t objectCount = 0;
    std::size_t attachCount = 0;
    std::size_t vertexChunkCount = 0;
    std::size_t polyChunkCount = 0;
    std::size_t decodedVertexCount = 0;
    std::size_t decodedTriangleCount = 0;
    std::size_t score = 0;
};

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
    const NjcmDecodeContext ctx{
        .decoded = std::span<const std::uint8_t>(decoded.data(), decoded.size()),
        .imageBase = effectiveImageBase,
        .littleEndian = policy.payloadLittleEndian,
        .out = &out,
    };

    while (!stack.empty()) {
        const std::size_t objOff = stack.back();
        stack.pop_back();
        if (visitedObjects.find(objOff) != visitedObjects.end()) {
            continue;
        }
        visitedObjects.insert(objOff);

        const auto maybeObj = parseObjectRecord(ctx, objOff, stack);
        if (!maybeObj.has_value()) {
            continue;
        }
        model::NjObjectRecord obj = *maybeObj;

        if (obj.hasAttach && visitedAttaches.find(obj.attachOffset) == visitedAttaches.end()) {
            visitedAttaches.insert(obj.attachOffset);
            if (const auto maybeAttach = parseAttachRecord(ctx, obj.attachOffset); maybeAttach.has_value()) {
                out.attaches.push_back(*maybeAttach);
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
