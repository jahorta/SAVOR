#include "SaToolsParityObjectParser.h"

#include "../Model/IndexEntry.h"

#include <cstdint>

namespace soasim::mld::parsing::satools_parity {

std::optional<model::NjObjectRecord> parseObject(const NjcmDecodeContext& ctx,
    const std::size_t objOff,
    std::vector<std::size_t>& traversalStack) {
    if (objOff + 0x34 > ctx.decoded.size()) {
        return std::nullopt;
    }

    model::NjObjectRecord obj{};
    obj.offset = objOff;
    if (const auto evalFlags = readU32At(ctx.decoded, objOff, ctx.littleEndian); evalFlags.has_value()) {
        obj.evalFlags = *evalFlags;
    }

    const auto attachRaw = readU32At(ctx.decoded, objOff + 0x4, ctx.littleEndian);
    const auto childRaw = readU32At(ctx.decoded, objOff + 0x2C, ctx.littleEndian);
    const auto siblingRaw = readU32At(ctx.decoded, objOff + 0x30, ctx.littleEndian);
    if (!attachRaw.has_value() || !childRaw.has_value() || !siblingRaw.has_value()) {
        ctx.out->diagnostics.push_back("SA-parity object truncated while reading pointer fields.");
        return std::nullopt;
    }

    if (const auto siblingOff = resolvePointer(*siblingRaw, ctx.imageBase, ctx.decoded.size()); siblingOff.has_value()) {
        obj.hasSibling = true;
        obj.siblingOffset = *siblingOff;
        traversalStack.push_back(*siblingOff);
    }
    if (const auto childOff = resolvePointer(*childRaw, ctx.imageBase, ctx.decoded.size()); childOff.has_value()) {
        obj.hasChild = true;
        obj.childOffset = *childOff;
        traversalStack.push_back(*childOff);
    }
    if (const auto attachOff = resolvePointer(*attachRaw, ctx.imageBase, ctx.decoded.size()); attachOff.has_value()) {
        obj.hasAttach = true;
        obj.attachOffset = *attachOff;
    }

    const auto posX = readF32At(ctx.decoded, objOff + 0x8, ctx.littleEndian);
    const auto posY = readF32At(ctx.decoded, objOff + 0xC, ctx.littleEndian);
    const auto posZ = readF32At(ctx.decoded, objOff + 0x10, ctx.littleEndian);
    if (posX.has_value() && posY.has_value() && posZ.has_value()) {
        obj.localTransform.position = model::Vec3{ *posX, *posY, *posZ };
    }

    const auto rotX = readU32At(ctx.decoded, objOff + 0x14, ctx.littleEndian);
    const auto rotY = readU32At(ctx.decoded, objOff + 0x18, ctx.littleEndian);
    const auto rotZ = readU32At(ctx.decoded, objOff + 0x1C, ctx.littleEndian);
    if (rotX.has_value() && rotY.has_value() && rotZ.has_value()) {
        constexpr float kNinjaAngleToRadians = static_cast<float>((2.0 * 3.14159265358979323846) / 65536.0);
        obj.localTransform.rotationRaw = model::Vec3{
            static_cast<float>(static_cast<std::int32_t>(*rotX)) * kNinjaAngleToRadians,
            static_cast<float>(static_cast<std::int32_t>(*rotY)) * kNinjaAngleToRadians,
            static_cast<float>(static_cast<std::int32_t>(*rotZ)) * kNinjaAngleToRadians,
        };
        obj.localTransform.rotation = model::eulerRadiansToQuaternionXYZ(obj.localTransform.rotationRaw);
    }

    const auto sclX = readF32At(ctx.decoded, objOff + 0x20, ctx.littleEndian);
    const auto sclY = readF32At(ctx.decoded, objOff + 0x24, ctx.littleEndian);
    const auto sclZ = readF32At(ctx.decoded, objOff + 0x28, ctx.littleEndian);
    if (sclX.has_value() && sclY.has_value() && sclZ.has_value()) {
        obj.localTransform.scale = model::Vec3{ *sclX, *sclY, *sclZ };
    }

    return obj;
}

} // namespace soasim::mld::parsing::satools_parity
