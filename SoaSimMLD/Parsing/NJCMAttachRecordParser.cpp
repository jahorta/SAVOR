#include "NJCMAttachRecordParser.h"

#include "NJCMPolyRecordParser.h"
#include "NJCMVertexRecordParser.h"

namespace soasim::mld::parsing {

std::optional<model::NjAttachRecord> parseAttachRecord(const NjcmDecodeContext& ctx, const std::size_t attachOffset) {
    if (attachOffset + 8 > ctx.decoded.size()) {
        ctx.out->diagnostics.push_back("Attach pointer out of range; skipping attach decode.");
        return std::nullopt;
    }

    model::NjAttachRecord attach{};
    attach.offset = attachOffset;
    const auto vlistRaw = readU32At(ctx.decoded, attachOffset, ctx.littleEndian);
    const auto plistRaw = readU32At(ctx.decoded, attachOffset + 4, ctx.littleEndian);
    if (!vlistRaw.has_value() || !plistRaw.has_value()) {
        ctx.out->diagnostics.push_back("Attach record truncated while reading list pointers.");
        return attach;
    }

    if (const auto vlistOff = resolvePointer(*vlistRaw, ctx.imageBase, ctx.decoded.size()); vlistOff.has_value()) {
        parseVertexRecords(ctx, *vlistOff, attach);
    }
    if (const auto plistOff = resolvePointer(*plistRaw, ctx.imageBase, ctx.decoded.size()); plistOff.has_value()) {
        parsePolyRecords(ctx, *plistOff, attach);
    }

    return attach;
}

} // namespace soasim::mld::parsing
