#include "SaToolsParityAttachParser.h"

#include "SaToolsParityPolyParser.h"
#include "SaToolsParityVertexParser.h"

namespace soasim::mld::parsing::satools_parity {

std::optional<model::NjAttachRecord> parseAttach(const NjcmDecodeContext& ctx, const std::size_t attachOffset) {
    if (attachOffset + 8 > ctx.decoded.size()) {
        ctx.out->diagnostics.push_back("SA-parity attach pointer out of range.");
        return std::nullopt;
    }

    model::NjAttachRecord attach{};
    attach.offset = attachOffset;

    const auto vlistRaw = readU32At(ctx.decoded, attachOffset, ctx.littleEndian);
    const auto plistRaw = readU32At(ctx.decoded, attachOffset + 4, ctx.littleEndian);
    if (!vlistRaw.has_value() || !plistRaw.has_value()) {
        ctx.out->diagnostics.push_back("SA-parity attach record truncated while reading list pointers.");
        return attach;
    }

    if (const auto vlistOff = resolvePointer(*vlistRaw, ctx.imageBase, ctx.decoded.size()); vlistOff.has_value()) {
        parseVertexChunks(ctx, *vlistOff, attach);
    }
    if (const auto plistOff = resolvePointer(*plistRaw, ctx.imageBase, ctx.decoded.size()); plistOff.has_value()) {
        parsePolyChunks(ctx, *plistOff, attach);
    }

    return attach;
}

} // namespace soasim::mld::parsing::satools_parity
