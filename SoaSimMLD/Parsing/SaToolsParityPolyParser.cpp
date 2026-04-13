#include "SaToolsParityPolyParser.h"

#include "SaToolsParityStripParser.h"
#include "SaToolsParityVolumeParser.h"

#include <unordered_map>
#include <unordered_set>

namespace soasim::mld::parsing::satools_parity {
namespace {

void parsePolyListInternal(const NjcmDecodeContext& ctx,
    const std::size_t polyListOffset,
    model::NjAttachRecord& attach,
    std::unordered_map<std::uint8_t, std::size_t>& cacheStarts,
    std::unordered_set<std::size_t>& recursionGuard,
    const bool recordChunkRecords) {
    if (!recursionGuard.insert(polyListOffset).second) {
        ctx.out->diagnostics.push_back("SA-parity polygon cache recursion detected; skipping replay.");
        return;
    }

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

        std::size_t step = 0;
        std::uint16_t sizeWords16 = 0;
        if (type <= 5U) { // bits chunks (null/blend/mipmap/spec/cache/draw)
            step = 2;
        } else if (type == 8U || type == 9U) {
            step = 4;
        } else {
            const auto sw = readU16At(ctx.decoded, cur + 2, ctx.littleEndian);
            if (!sw.has_value()) {
                ctx.out->diagnostics.push_back("SA-parity polygon chunk truncated while reading size field.");
                break;
            }
            sizeWords16 = *sw;
            step = 4U + static_cast<std::size_t>(*sw) * 2U;
        }

        if (step == 0 || cur + step > ctx.decoded.size()) {
            ctx.out->diagnostics.push_back("SA-parity polygon chunk step exceeded buffer.");
            break;
        }

        model::NjPolyChunkRecord pc{};
        pc.offset = cur;
        pc.type = type;
        pc.sizeWords16 = sizeWords16;

        model::NjSemanticPolygon sp{};
        sp.type = type;

        if (type >= 64U && type <= 75U && step >= 6U) {
            parseStripChunk(ctx, cur, cur + step, type, attach, pc, sp, attach.decodedTriangleCount);
        } else if (type == 56U || type == 57U || type == 58U) {
            parseVolumeChunk(ctx, cur, cur + step, type, attach, pc, sp, attach.decodedTriangleCount);
        } else if (type == 4U) { // Bits_CachePolygonList
            cacheStarts[flags] = cur + step;
            if (recordChunkRecords) {
                attach.semanticPolygons.push_back(std::move(sp));
                attach.polyChunks.push_back(std::move(pc));
            }
            break; // matches sa_tools ProcessPolyList early return on cache chunk
        } else if (type == 5U) { // Bits_DrawPolygonList
            if (const auto it = cacheStarts.find(flags); it != cacheStarts.end()) {
                parsePolyListInternal(ctx, it->second, attach, cacheStarts, recursionGuard, false);
            }
        }

        pc.estimatedTriangleCount = sp.estimatedTriangleCount;
        if (sp.indices.empty() && step >= 4U) {
            const std::size_t words = (step - 4U) / 2U;
            for (std::size_t wi = 0; wi < words; ++wi) {
                const auto word = readU16At(ctx.decoded, cur + 4U + wi * 2U, ctx.littleEndian);
                if (!word.has_value()) {
                    break;
                }
                pc.rawIndexWords.push_back(*word);
            }
        }

        if (!recordChunkRecords && (type >= 56U && type <= 58U || type >= 64U && type <= 75U)) {
            // Replay path: inject semantic geometry so downstream rendering sees cache-draw output
            attach.semanticPolygons.push_back(std::move(sp));
        } else if (recordChunkRecords) {
            attach.semanticPolygons.push_back(std::move(sp));
            attach.polyChunks.push_back(std::move(pc));
        }

        cur += step;
    }

    recursionGuard.erase(polyListOffset);
}

} // namespace

void parsePolyChunks(const NjcmDecodeContext& ctx, const std::size_t polyListOffset, model::NjAttachRecord& attach) {
    attach.polyListOffset = polyListOffset;
    std::unordered_map<std::uint8_t, std::size_t> cacheStarts{};
    std::unordered_set<std::size_t> recursionGuard{};
    parsePolyListInternal(ctx, polyListOffset, attach, cacheStarts, recursionGuard, true);
}

} // namespace soasim::mld::parsing::satools_parity
