#include "SaToolsParityObjectParser.h"

namespace soasim::mld::parsing::satools_parity {

std::optional<model::NjObjectRecord> parseObject(const NjcmDecodeContext& ctx,
    const std::size_t objOff,
    std::vector<std::size_t>& traversalStack) {
    if (objOff + 0x34 > ctx.decoded.size()) {
        return std::nullopt;
    }

    model::NjObjectRecord obj{};
    obj.offset = objOff;

    const auto attachRaw = readU32At(ctx.decoded, objOff + 0x4, ctx.littleEndian);
    const auto childRaw = readU32At(ctx.decoded, objOff + 0x2C, ctx.littleEndian);
    const auto siblingRaw = readU32At(ctx.decoded, objOff + 0x30, ctx.littleEndian);
    if (!attachRaw.has_value() || !childRaw.has_value() || !siblingRaw.has_value()) {
        ctx.out->diagnostics.push_back("SA-parity object truncated while reading pointer fields.");
        return std::nullopt;
    }

    if (const auto childOff = resolvePointer(*childRaw, ctx.imageBase, ctx.decoded.size()); childOff.has_value()) {
        obj.hasChild = true;
        obj.childOffset = *childOff;
        traversalStack.push_back(*childOff);
    }
    if (const auto siblingOff = resolvePointer(*siblingRaw, ctx.imageBase, ctx.decoded.size()); siblingOff.has_value()) {
        obj.hasSibling = true;
        obj.siblingOffset = *siblingOff;
        traversalStack.push_back(*siblingOff);
    }
    if (const auto attachOff = resolvePointer(*attachRaw, ctx.imageBase, ctx.decoded.size()); attachOff.has_value()) {
        obj.hasAttach = true;
        obj.attachOffset = *attachOff;
    }

    return obj;
}

} // namespace soasim::mld::parsing::satools_parity
