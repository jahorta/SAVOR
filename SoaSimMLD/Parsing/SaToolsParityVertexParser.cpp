#include "SaToolsParityVertexParser.h"

namespace soasim::mld::parsing::satools_parity {
namespace {

[[nodiscard]] std::optional<model::NjVertexChunkRecord> parseVertexChunkHeader(const NjcmDecodeContext& ctx, const std::size_t offset) {
    const auto h1 = readU32At(ctx.decoded, offset, ctx.littleEndian);
    if (!h1.has_value()) {
        return std::nullopt;
    }

    model::NjVertexChunkRecord vc{};
    vc.offset = offset;
    vc.type = static_cast<std::uint8_t>(*h1 & 0xFFU);
    vc.sizeWords32 = static_cast<std::uint16_t>((*h1 >> 16) & 0xFFFFU);

    const auto h2 = readU32At(ctx.decoded, offset + 4, ctx.littleEndian);
    if (h2.has_value()) {
        vc.indexOffset = static_cast<std::uint16_t>(*h2 & 0xFFFFU);
        vc.vertexCount = static_cast<std::uint16_t>((*h2 >> 16) & 0xFFFFU);
    }
    return vc;
}

void decodeVertexChunkSemantics(const NjcmDecodeContext& ctx,
    const model::NjVertexChunkRecord& vc,
    const std::size_t step,
    std::vector<model::NjSemanticVertex>& indexedVertices) {
    if (vc.vertexCount == 0) {
        return;
    }

    const std::size_t payloadOff = vc.offset + 8;
    if (payloadOff > ctx.decoded.size() || step < 8) {
        return;
    }

    const std::size_t payloadBytes = step - 8;
    std::size_t stride = 0;
    const auto wordsPerVertex = vertexWordsPerVertexByType(vc.type);
    if (wordsPerVertex > 0) {
        stride = wordsPerVertex * 4U;
    } else {
        stride = payloadBytes / static_cast<std::size_t>(vc.vertexCount);
    }

    if (stride >= 12) {
        for (std::uint16_t vi = 0; vi < vc.vertexCount; ++vi) {
            const std::size_t vOff = payloadOff + static_cast<std::size_t>(vi) * stride;
            model::NjSemanticVertex sv{};
            const auto px = readF32At(ctx.decoded, vOff, ctx.littleEndian);
            const auto py = readF32At(ctx.decoded, vOff + 4, ctx.littleEndian);
            const auto pz = readF32At(ctx.decoded, vOff + 8, ctx.littleEndian);
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

} // namespace

void parseVertexChunks(const NjcmDecodeContext& ctx, const std::size_t vertexListOffset, model::NjAttachRecord& attach) {
    attach.vertexListOffset = vertexListOffset;
    std::size_t cur = vertexListOffset;
    std::vector<model::NjSemanticVertex> indexedVertices{};

    for (std::size_t i = 0; i < 4096 && cur + 4 <= ctx.decoded.size(); ++i) {
        const auto header = readU32At(ctx.decoded, cur, ctx.littleEndian);
        if (!header.has_value()) {
            break;
        }

        const std::uint8_t type = static_cast<std::uint8_t>(*header & 0xFFU);
        if (type == 255U) {
            break;
        }

        const std::uint16_t szWords32 = static_cast<std::uint16_t>((*header >> 16) & 0xFFFFU);
        const std::size_t step = 4U + static_cast<std::size_t>(szWords32) * 4U;
        if (step == 0 || cur + step > ctx.decoded.size()) {
            ctx.out->diagnostics.push_back("SA-parity vertex chunk step exceeded buffer.");
            break;
        }

        const auto maybeVc = parseVertexChunkHeader(ctx, cur);
        if (!maybeVc.has_value()) {
            break;
        }

        model::NjVertexChunkRecord vc = *maybeVc;
        attach.decodedVertexCount += vc.vertexCount;
        decodeVertexChunkSemantics(ctx, vc, step, indexedVertices);
        attach.vertexChunks.push_back(vc);
        cur += step;
    }

    attach.semanticVertices = std::move(indexedVertices);
}

} // namespace soasim::mld::parsing::satools_parity
