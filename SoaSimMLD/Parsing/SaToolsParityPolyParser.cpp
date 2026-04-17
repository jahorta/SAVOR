#include "SaToolsParityPolyParser.h"

#include "SaToolsParityStripParser.h"
#include "SaToolsParityVolumeParser.h"

#include <optional>
#include <unordered_map>

namespace soasim::mld::parsing::satools_parity {
namespace {

struct MaterialState {
    std::uint8_t blendFlags = 0;
    std::uint8_t mipmapFlags = 0;
    std::uint8_t specularFlags = 0;
    std::uint16_t textureId = 0xFFFFU;

    [[nodiscard]] std::uint32_t key() const {
        return static_cast<std::uint32_t>(blendFlags) |
            (static_cast<std::uint32_t>(mipmapFlags) << 8U) |
            (static_cast<std::uint32_t>(specularFlags) << 16U);
    }
};

struct RawPolyChunk {
    std::size_t offset = 0;
    std::size_t stepBytes = 0;
    std::uint8_t type = 0;
    std::uint8_t flags = 0;
    std::uint16_t sizeWords16 = 0;
};

struct ResolvedPolyChunk {
    RawPolyChunk chunk{};
    bool fromCacheReplay = false;
};

[[nodiscard]] bool isSaToolsSupportedStripType(const std::uint8_t type) {
    switch (type) {
    case 64U: // Strip_Strip
    case 65U: // Strip_StripUVN
    case 66U: // Strip_StripUVH
    case 70U: // Strip_StripColor
    case 71U: // Strip_StripUVNColor
    case 72U: // Strip_StripUVHColor
    case 73U: // Strip_Strip2
    case 74U: // Strip_StripUVN2
    case 75U: // Strip_StripUVH2
        return true;
    default:
        return false;
    }
}

void runRawChunkParsePass(const NjcmDecodeContext& ctx,
    const std::size_t polyListOffset,
    std::vector<RawPolyChunk>& rawChunks) {
    std::size_t cur = polyListOffset;
    for (std::size_t i = 0; i < 8192 && cur + 2 <= ctx.decoded.size(); ++i) {
        const auto header = readU16At(ctx.decoded, cur, ctx.littleEndian);
        if (!header.has_value()) {
            break;
        }

        const std::uint8_t type = static_cast<std::uint8_t>(*header & 0xFFU);
        const std::uint8_t flags = static_cast<std::uint8_t>((*header >> 8) & 0xFFU);
        if (type == 255U) {
            break;
        }

        std::size_t stepBytes = 0;
        std::uint16_t sizeWords16 = 0;
        if (type <= 5U) { // bits chunks (null/blend/mipmap/spec/cache/draw)
            stepBytes = 2;
        } else if (type == 8U || type == 9U) {
            stepBytes = 4;
        } else {
            const auto sw = readU16At(ctx.decoded, cur + 2, ctx.littleEndian);
            if (!sw.has_value()) {
                ctx.out->diagnostics.push_back("SA-parity polygon chunk truncated while reading size field.");
                break;
            }
            sizeWords16 = *sw;
            stepBytes = 4U + static_cast<std::size_t>(*sw) * 2U;
        }

        if (stepBytes == 0 || cur + stepBytes > ctx.decoded.size()) {
            ctx.out->diagnostics.push_back("SA-parity polygon chunk step exceeded buffer.");
            break;
        }

        RawPolyChunk parsedChunk{};
        parsedChunk.offset = cur;
        parsedChunk.stepBytes = stepBytes;
        parsedChunk.type = type;
        parsedChunk.flags = flags;
        parsedChunk.sizeWords16 = sizeWords16;
        rawChunks.push_back(parsedChunk);
        cur += stepBytes;
    }
}

void runActiveStreamResolutionPass(const NjcmDecodeContext& ctx,
    const std::vector<RawPolyChunk>& rawChunks,
    std::vector<ResolvedPolyChunk>& activeStream) {
    std::unordered_map<std::uint8_t, std::vector<RawPolyChunk>> cacheBuckets{};
    std::optional<std::uint8_t> activeCacheTarget{};

    for (const auto& chunk : rawChunks) {
        if (chunk.type == 4U) { // Bits_CachePolygonList
            cacheBuckets[chunk.flags].clear();
            activeCacheTarget = chunk.flags;
            continue;
        }

        if (chunk.type == 5U) { // Bits_DrawPolygonList
            if (const auto it = cacheBuckets.find(chunk.flags); it != cacheBuckets.end()) {
                for (const auto& cachedChunk : it->second) {
                    ResolvedPolyChunk replayChunk{};
                    replayChunk.chunk = cachedChunk;
                    replayChunk.fromCacheReplay = true;
                    activeStream.push_back(std::move(replayChunk));
                }
            } else {
                ctx.out->diagnostics.push_back("SA-parity draw polygon list referenced missing cache bucket " +
                    std::to_string(chunk.flags) + ".");
            }
            continue;
        }

        if (activeCacheTarget.has_value()) {
            cacheBuckets[*activeCacheTarget].push_back(chunk);
        } else {
            ResolvedPolyChunk directChunk{};
            directChunk.chunk = chunk;
            directChunk.fromCacheReplay = false;
            activeStream.push_back(std::move(directChunk));
        }
    }
}

void decodeResolvedStream(const NjcmDecodeContext& ctx,
    const std::vector<ResolvedPolyChunk>& activeStream,
    model::NjAttachRecord& attach) {
    MaterialState materialState{};

    for (const auto& resolved : activeStream) {
        const auto& chunk = resolved.chunk;

        model::NjPolyChunkRecord pc{};
        pc.offset = chunk.offset;
        pc.type = chunk.type;
        pc.sizeWords16 = chunk.sizeWords16;

        model::NjSemanticPolygon sp{};
        sp.type = chunk.type;
        sp.sourceChunkFlags = chunk.flags;
        sp.sourceChunkOffset = chunk.offset;
        sp.fromCacheReplay = resolved.fromCacheReplay;
        sp.materialStateKey = materialState.key();
        sp.textureId = materialState.textureId;

        if (chunk.type >= 64U && chunk.type <= 75U && chunk.stepBytes >= 6U) {
            if (isSaToolsSupportedStripType(chunk.type)) {
                parseStripChunk(ctx,
                    chunk.offset,
                    chunk.offset + chunk.stepBytes,
                    chunk.type,
                    attach,
                    pc,
                    sp,
                    attach.decodedTriangleCount);
            } else {
                ctx.out->diagnostics.push_back("SA-parity unsupported strip chunk type " +
                    std::to_string(chunk.type) + " at offset " + std::to_string(chunk.offset) +
                    "; chunk metadata preserved and geometry decode skipped.");
            }
        } else if (chunk.type == 56U || chunk.type == 57U || chunk.type == 58U) {
            parseVolumeChunk(ctx,
                chunk.offset,
                chunk.offset + chunk.stepBytes,
                chunk.type,
                attach,
                pc,
                sp,
                attach.decodedTriangleCount);
        } else if (chunk.type == 1U) {
            materialState.blendFlags = chunk.flags;
        } else if (chunk.type == 2U) {
            materialState.mipmapFlags = chunk.flags;
        } else if (chunk.type == 3U) {
            materialState.specularFlags = chunk.flags;
        } else if (chunk.type == 8U || chunk.type == 9U) {
            materialState.textureId = readU16At(ctx.decoded, chunk.offset + 2U, ctx.littleEndian).value_or(materialState.textureId);
        }

        pc.estimatedTriangleCount = sp.estimatedTriangleCount;
        if (sp.indices.empty() && chunk.stepBytes >= 4U) {
            const std::size_t words = (chunk.stepBytes - 4U) / 2U;
            for (std::size_t wi = 0; wi < words; ++wi) {
                const auto word = readU16At(ctx.decoded, chunk.offset + 4U + wi * 2U, ctx.littleEndian);
                if (!word.has_value()) {
                    break;
                }
                pc.rawIndexWords.push_back(*word);
            }
        }

        if (resolved.fromCacheReplay && (chunk.type == 56U || chunk.type == 57U || chunk.type == 58U ||
            (chunk.type >= 64U && chunk.type <= 75U && isSaToolsSupportedStripType(chunk.type)))) {
            // Replay path: inject semantic geometry so downstream rendering sees cache-draw output
            attach.semanticPolygons.push_back(std::move(sp));
            continue;
        }

        attach.semanticPolygons.push_back(std::move(sp));
        attach.polyChunks.push_back(std::move(pc));
    }
}

} // namespace

void parsePolyChunks(const NjcmDecodeContext& ctx, const std::size_t polyListOffset, model::NjAttachRecord& attach) {
    attach.polyListOffset = polyListOffset;

    std::vector<RawPolyChunk> rawChunks{};
    rawChunks.reserve(128);
    runRawChunkParsePass(ctx, polyListOffset, rawChunks);

    std::vector<ResolvedPolyChunk> activeStream{};
    activeStream.reserve(rawChunks.size());
    runActiveStreamResolutionPass(ctx, rawChunks, activeStream);

    decodeResolvedStream(ctx, activeStream, attach);
}

} // namespace soasim::mld::parsing::satools_parity
