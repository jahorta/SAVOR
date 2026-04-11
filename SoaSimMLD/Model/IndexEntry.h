#pragma once

#include "Types.h"
#include "U32List.h"
#include "../common/ByteUtils.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace soasim::mld::model {

struct IndexEntry {
    std::size_t tableIndex = 0;
    std::uint32_t entryId = 0;
    std::uint32_t tblId = 0;
    std::string fxnName{};
    Transform transform{};

    std::unique_ptr<U32List> groundLinks{};
    std::unique_ptr<U32List> paramList2{};
    std::unique_ptr<U32List> functionParameters{};
    std::unique_ptr<U32List> objectAddresses{};
    std::unique_ptr<U32List> groundAddresses{};
    std::unique_ptr<U32List> motionAddresses{};
    std::uint32_t texturesPointer = 0;
};

using IndexEntryCoordinateMapper = std::function<Vec3(const Vec3&)>;
using IndexEntryWarningSink = std::function<void(const std::string&)>;

[[nodiscard]] inline std::string readFxnString(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    constexpr std::size_t fxnLen = 0x14;
    std::string out{};
    if (offset + fxnLen > bytes.size()) {
        return out;
    }

    out.reserve(fxnLen);
    for (std::size_t i = 0; i < fxnLen; ++i) {
        const char c = static_cast<char>(bytes[offset + i]);
        if (c == '\0') {
            break;
        }
        const unsigned char uc = static_cast<unsigned char>(c);
        out.push_back((uc >= 32U && uc <= 126U) ? c : '?');
    }

    return out;
}

[[nodiscard]] inline std::optional<IndexEntry> parseIndexEntry(std::span<const std::uint8_t> bytes,
    const std::size_t tableIndex,
    const std::size_t entryOffset,
    const IndexEntryCoordinateMapper& coordinateMapper,
    const IndexEntryWarningSink& warningSink) {
    const auto entryId = common::readU32AtBE(bytes, entryOffset + 0x00);
    const auto tblId = common::readU32AtBE(bytes, entryOffset + 0x04);
    const auto ptrGroundLinks = common::readU32AtBE(bytes, entryOffset + 0x08);
    const auto ptrParamList2 = common::readU32AtBE(bytes, entryOffset + 0x0C);
    const auto ptrFunctionParameters = common::readU32AtBE(bytes, entryOffset + 0x10);
    const auto ptrObjects = common::readU32AtBE(bytes, entryOffset + 0x14);
    const auto ptrGrounds = common::readU32AtBE(bytes, entryOffset + 0x18);
    const auto ptrMotions = common::readU32AtBE(bytes, entryOffset + 0x1C);
    const auto ptrTextures = common::readU32AtBE(bytes, entryOffset + 0x20);
    if (!entryId.has_value() || !tblId.has_value() || !ptrGroundLinks.has_value() || !ptrParamList2.has_value() ||
        !ptrFunctionParameters.has_value() || !ptrObjects.has_value() || !ptrGrounds.has_value() ||
        !ptrMotions.has_value() || !ptrTextures.has_value()) {
        return std::nullopt;
    }

    IndexEntry entry{};
    entry.tableIndex = tableIndex;
    entry.entryId = *entryId;
    entry.tblId = *tblId;
    entry.texturesPointer = *ptrTextures;

    Transform transform{};
    const auto posX = common::readF32AtBE(bytes, entryOffset + 0x44);
    const auto posY = common::readF32AtBE(bytes, entryOffset + 0x48);
    const auto posZ = common::readF32AtBE(bytes, entryOffset + 0x4C);
    if (posX.has_value() && posY.has_value() && posZ.has_value()) {
        transform.position = coordinateMapper(Vec3{ *posX, *posY, *posZ });
    }
    entry.transform = transform;
    entry.fxnName = readFxnString(bytes, entryOffset + 0x24);

    const std::string indexPrefix = "entry[" + std::to_string(tableIndex) + "]";
    entry.groundLinks = makeU32List(bytes, *ptrGroundLinks, indexPrefix + ".groundLinks", warningSink);
    entry.paramList2 = makeU32List(bytes, *ptrParamList2, indexPrefix + ".paramList2", warningSink);
    entry.functionParameters = makeU32List(bytes, *ptrFunctionParameters, indexPrefix + ".functionParameters", warningSink);
    entry.objectAddresses = makeU32List(bytes, *ptrObjects, indexPrefix + ".objects", warningSink);
    entry.groundAddresses = makeU32List(bytes, *ptrGrounds, indexPrefix + ".grounds", warningSink);
    entry.motionAddresses = makeU32List(bytes, *ptrMotions, indexPrefix + ".motions", warningSink);

    return entry;
}


} // namespace soasim::mld::model
