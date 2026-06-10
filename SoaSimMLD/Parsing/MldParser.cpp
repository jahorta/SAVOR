#include "MldParser.h"

#include "../Export/BlenderIrJsonExporter.h"
#include "Sa3dBlenderIrBuilder.h"
#include "../../Compression/Aklz.h"
#include "../Model/IndexEntry.h"
#include "../common/ByteUtils.h"
#include "EntryHandlers.h"
#include "GrndParser.h"
#include "MldTextureArchiveParser.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <iostream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace soasim::mld::parsing {

namespace {

using soasim::mld::model::EncounterOrTriggerRegion;
using soasim::mld::model::GrndSurface;
using soasim::mld::model::UnknownEntry;
using soasim::mld::model::Vec3;
using soasim::mld::model::WalkSurfaceNode;

constexpr std::uint32_t makeTag(const char a, const char b, const char c, const char d) {
    return static_cast<std::uint32_t>(a) |
        (static_cast<std::uint32_t>(b) << 8) |
        (static_cast<std::uint32_t>(c) << 16) |
        (static_cast<std::uint32_t>(d) << 24);
}

[[nodiscard]] std::string tagToString(const std::uint32_t tag) {
    std::array<char, 5> out{};
    out[0] = static_cast<char>(tag & 0xFFU);
    out[1] = static_cast<char>((tag >> 8) & 0xFFU);
    out[2] = static_cast<char>((tag >> 16) & 0xFFU);
    out[3] = static_cast<char>((tag >> 24) & 0xFFU);
    out[4] = '\0';

    for (std::size_t i = 0; i < 4; ++i) {
        const unsigned char c = static_cast<unsigned char>(out[i]);
        if (c < 32U || c > 126U) {
            out[i] = '?';
        }
    }
    return std::string(out.data());
}

[[nodiscard]] Vec3 applyCoordinates(const Vec3& value, const CoordinatePolicy& policy) {
    Vec3 out = value;
    if (policy.swapYZ) {
        std::swap(out.y, out.z);
    }
    if (policy.negateX) {
        out.x = -out.x;
    }
    if (policy.negateY) {
        out.y = -out.y;
    }
    if (policy.negateZ) {
        out.z = -out.z;
    }
    out.x *= policy.uniformScale;
    out.y *= policy.uniformScale;
    out.z *= policy.uniformScale;
    return out;
}

void addHistogram(std::unordered_map<std::string, std::size_t>& histogram, const ParseOptions& options, const std::string& fxnName) {
    if (!options.emitFxnHistogram) {
        return;
    }
    ++histogram[fxnName];
}

[[nodiscard]] std::string normalizeFxnName(std::string_view fxnName) {
    std::string normalized{};
    normalized.reserve(fxnName.size());
    for (const auto ch : fxnName) {
        if (std::isalnum(static_cast<unsigned char>(ch)) == 0) {
            continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return normalized;
}

[[nodiscard]] std::vector<ExtractedNjBlock> buildExtractedNjBlocks(
    std::span<const std::uint8_t> payload,
    const std::unordered_set<std::uint32_t>& objectAddresses,
    const std::unordered_set<std::uint32_t>& motionAddresses,
    const std::unordered_set<std::uint32_t>& textureAddresses) {
    struct CandidateAddress {
        enum class Kind {
            TextureList,
            Object,
            Motion,
        };

        std::uint32_t offset = 0;
        Kind kind = Kind::Object;
    };

    const auto readTag = [&](const std::uint32_t offset) -> std::optional<std::uint32_t> {
        const auto idx = static_cast<std::size_t>(offset);
        if (idx + 4U > payload.size()) {
            return std::nullopt;
        }
        return common::readU32AtBE(payload, idx);
    };

    std::vector<CandidateAddress> candidates{};
    candidates.reserve(objectAddresses.size() + motionAddresses.size() + textureAddresses.size());
    for (const auto offset : textureAddresses) {
        if (offset > 0U && static_cast<std::size_t>(offset) < payload.size()) {
            candidates.push_back(CandidateAddress{
                .offset = offset,
                .kind = CandidateAddress::Kind::TextureList,
            });
        }
    }
    for (const auto offset : objectAddresses) {
        if (offset > 0U && static_cast<std::size_t>(offset) < payload.size()) {
            candidates.push_back(CandidateAddress{
                .offset = offset,
                .kind = CandidateAddress::Kind::Object,
            });
        }
    }
    for (const auto offset : motionAddresses) {
        if (offset > 0U && static_cast<std::size_t>(offset) < payload.size()) {
            candidates.push_back(CandidateAddress{
                .offset = offset,
                .kind = CandidateAddress::Kind::Motion,
            });
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const CandidateAddress& a, const CandidateAddress& b) {
        if (a.offset != b.offset) {
            return a.offset < b.offset;
        }
        return static_cast<int>(a.kind) < static_cast<int>(b.kind);
    });
    candidates.erase(std::unique(candidates.begin(), candidates.end(), [](const CandidateAddress& a, const CandidateAddress& b) {
        return a.offset == b.offset;
    }), candidates.end());

    std::vector<ExtractedNjBlock> blocks{};
    blocks.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto candidate = candidates[i];
        const auto begin = static_cast<std::size_t>(candidate.offset);
        const auto end = (i + 1 < candidates.size())
            ? static_cast<std::size_t>(candidates[i + 1].offset)
            : payload.size();
        if (begin >= end || begin >= payload.size()) {
            continue;
        }

        bool includesNjtlPrefix = false;
        std::size_t effectiveEnd = std::min(end, payload.size());
        if (candidate.kind == CandidateAddress::Kind::TextureList && i + 1 < candidates.size()) {
            const auto currentTag = readTag(candidate.offset).value_or(0U);
            const auto nextTag = readTag(candidates[i + 1].offset).value_or(0U);
            const bool currentIsNjtl = currentTag == makeTag('N', 'J', 'T', 'L') || currentTag == makeTag('G', 'J', 'T', 'L');
            const bool nextIsNjcm = nextTag == makeTag('N', 'J', 'C', 'M') || nextTag == makeTag('G', 'J', 'C', 'M');
            const bool nextIsObject = candidates[i + 1].kind == CandidateAddress::Kind::Object;
            if (currentIsNjtl && nextIsNjcm && nextIsObject) {
                const auto objectEnd = (i + 2 < candidates.size())
                    ? static_cast<std::size_t>(candidates[i + 2].offset)
                    : payload.size();
                effectiveEnd = std::min(objectEnd, payload.size());
                includesNjtlPrefix = true;
                ++i; // consume the NJCM start with its preceding NJTL as one extracted NJ block.
            }
        }

        ExtractedNjBlock block{};
        block.kind = candidate.kind == CandidateAddress::Kind::Motion
            ? ExtractedNjBlock::Kind::Motion
            : ExtractedNjBlock::Kind::Object;
        block.offset = candidate.offset;
        block.size = effectiveEnd - begin;
        block.includesNjtlPrefix = includesNjtlPrefix;
        block.bytes.assign(
            payload.begin() + static_cast<std::ptrdiff_t>(begin),
            payload.begin() + static_cast<std::ptrdiff_t>(effectiveEnd));
        blocks.push_back(std::move(block));
    }

    return blocks;
}

using SpatialOwnerMap = std::unordered_map<std::uint32_t, std::vector<BlockOwnerRef>>;

[[nodiscard]] std::optional<std::uint16_t> readU16AtLE(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    if (offset + 2U > bytes.size()) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8);
}

[[nodiscard]] std::optional<std::uint16_t> readU16AtBE(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    if (offset + 2U > bytes.size()) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset]) << 8) |
        static_cast<std::uint16_t>(bytes[offset + 1U]);
}

[[nodiscard]] std::optional<std::int32_t> readI32AtLE(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    const auto value = common::readU32AtLE(bytes, offset);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(*value);
}

[[nodiscard]] std::optional<std::int32_t> readI32AtBE(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    const auto value = common::readU32AtBE(bytes, offset);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(*value);
}

[[nodiscard]] std::string hex32(std::uint32_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "0x00000000";
    for (int i = 9; i >= 2; --i) {
        result[static_cast<std::size_t>(i)] = digits[value & 0xFu];
        value >>= 4;
    }
    return result;
}

[[nodiscard]] std::string asciiTagAt(std::span<const std::uint8_t> payload, const std::size_t offset) {
    if (offset + 4U > payload.size()) {
        return {};
    }

    std::string tag{};
    tag.reserve(4U);
    for (std::size_t i = 0; i < 4U; ++i) {
        const unsigned char c = payload[offset + i];
        tag.push_back((c >= 32U && c <= 126U) ? static_cast<char>(c) : '?');
    }
    return tag;
}

[[nodiscard]] std::string readFixedAsciiName(std::span<const std::uint8_t> bytes,
    const std::size_t offset,
    const std::size_t maxLength) {
    std::string out{};
    if (offset >= bytes.size()) {
        return out;
    }

    const auto end = std::min(bytes.size(), offset + maxLength);
    for (std::size_t i = offset; i < end; ++i) {
        const auto ch = bytes[i];
        if (ch == 0U) {
            break;
        }
        if (std::isprint(static_cast<unsigned char>(ch)) == 0) {
            break;
        }
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

[[nodiscard]] std::vector<std::string> parseEntryTextureNames(std::span<const std::uint8_t> payload,
    const std::uint32_t texturesPointer) {
    constexpr std::uint32_t kNjtlTag = 0x4E4A544CU;
    constexpr std::uint32_t kGjtlTag = 0x474A544CU;
    constexpr std::size_t textureRecordStride = 12U;
    std::vector<std::string> names{};
    std::size_t njtlOffset = static_cast<std::size_t>(texturesPointer);
    if (njtlOffset + 16U > payload.size()) {
        return names;
    }

    auto tag = common::readU32AtBE(payload, njtlOffset).value_or(0U);
    if (tag != kNjtlTag && tag != kGjtlTag) {
        const auto count = common::readU32AtBE(payload, njtlOffset + 4U);
        if (count.has_value() && *count <= 4096U &&
            njtlOffset + 8U + (static_cast<std::size_t>(*count) * textureRecordStride) <= payload.size()) {
            names.reserve(*count);
            for (std::uint32_t i = 0; i < *count; ++i) {
                const auto namePointer = common::readU32AtBE(
                    payload,
                    njtlOffset + 8U + (static_cast<std::size_t>(i) * textureRecordStride));
                if (!namePointer.has_value() || static_cast<std::size_t>(*namePointer) >= payload.size()) {
                    names.push_back({});
                    continue;
                }
                names.push_back(readFixedAsciiName(payload, *namePointer, payload.size() - *namePointer));
            }
            return names;
        }
    }

    if (tag != kNjtlTag && tag != kGjtlTag) {
        const auto wrappedNjtlPointer = common::readU32AtBE(payload, njtlOffset + 0x08U).value_or(0U);
        if (wrappedNjtlPointer == 0U || static_cast<std::size_t>(wrappedNjtlPointer) + 16U > payload.size()) {
            return names;
        }
        njtlOffset = static_cast<std::size_t>(wrappedNjtlPointer);
        tag = common::readU32AtBE(payload, njtlOffset).value_or(0U);
    }
    if (tag != kNjtlTag && tag != kGjtlTag) {
        return names;
    }

    const auto blockSize = common::readU32AtBE(payload, njtlOffset + 4U);
    const auto count = common::readU32AtBE(payload, njtlOffset + 12U);
    if (!blockSize.has_value() || !count.has_value() || *count > 4096U) {
        return names;
    }

    constexpr std::size_t blockHeaderSize = 8U;
    constexpr std::size_t textureRecordTableOffset = 8U;
    constexpr std::size_t textureRecordSize = 12U;
    const std::size_t payloadStart = njtlOffset + blockHeaderSize;
    if (payloadStart >= payload.size()) {
        return names;
    }

    const std::size_t payloadSize = std::min<std::size_t>(*blockSize, payload.size() - payloadStart);
    const auto texturePayload = std::span<const std::uint8_t>(
        payload.data() + static_cast<std::ptrdiff_t>(payloadStart),
        payloadSize);

    names.reserve(*count);
    for (std::uint32_t i = 0; i < *count; ++i) {
        const std::size_t recordOffset = textureRecordTableOffset + (static_cast<std::size_t>(i) * textureRecordSize);
        const auto namePointer = common::readU32AtBE(texturePayload, recordOffset);
        if (!namePointer.has_value() || static_cast<std::size_t>(*namePointer) >= texturePayload.size()) {
            names.push_back({});
            continue;
        }
        names.push_back(readFixedAsciiName(texturePayload, *namePointer, texturePayload.size() - *namePointer));
    }

    return names;
}

[[nodiscard]] ParsedEntryListItem makeEntryListItem(std::span<const std::uint8_t> payload,
    const model::IndexEntry& entry) {
    ParsedEntryListItem item{};
    item.tableIndex = entry.tableIndex;
    item.entryId = entry.entryId;
    item.tblId = entry.tblId;
    item.fxnName = entry.fxnName;
    item.objectCount = entry.objectCount;
    item.groundCount = entry.groundCount;
    item.motionCount = entry.motionCount;
    item.texturesPointer = entry.texturesPointer;
    item.groundLinks = entry.groundLinks ? entry.groundLinks->values : std::vector<std::uint32_t>{};
    item.paramList2 = entry.paramList2 ? entry.paramList2->values : std::vector<std::uint32_t>{};
    item.functionParameters = entry.functionParameters ? entry.functionParameters->values : std::vector<std::uint32_t>{};
    item.objectAddresses = entry.objectAddresses ? entry.objectAddresses->values : std::vector<std::uint32_t>{};
    item.groundAddresses = entry.groundAddresses ? entry.groundAddresses->values : std::vector<std::uint32_t>{};
    item.motionAddresses = entry.motionAddresses ? entry.motionAddresses->values : std::vector<std::uint32_t>{};
    item.textureNames = parseEntryTextureNames(payload, entry.texturesPointer);
    item.textureCount = item.textureNames.size();
    return item;
}

[[nodiscard]] bool isNjLikeTag(const std::string& tag) {
    return tag == "NJCM" ||
        tag == "GJCM" ||
        tag == "NJTL" ||
        tag == "GJTL";
}

[[nodiscard]] bool isPlausibleBlockSize(
    std::span<const std::uint8_t> payload,
    const std::size_t offset,
    const std::uint32_t size) {
    if (size < 0x10U || offset > payload.size()) {
        return false;
    }
    return static_cast<std::size_t>(size) <= payload.size() - offset;
}

[[nodiscard]] std::optional<std::size_t> nextCandidateAfter(
    const std::vector<std::uint32_t>& candidateOffsets,
    const std::uint32_t offset) {
    const auto it = std::upper_bound(candidateOffsets.begin(), candidateOffsets.end(), offset);
    if (it == candidateOffsets.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(*it);
}

void appendProbeValue(
    std::vector<std::pair<std::string, std::string>>& probe,
    std::string key,
    std::string value) {
    probe.emplace_back(std::move(key), std::move(value));
}

void appendProbeValue(
    std::vector<std::pair<std::string, std::string>>& probe,
    std::string key,
    const std::optional<std::uint32_t>& value) {
    appendProbeValue(probe, std::move(key), value.has_value() ? hex32(*value) : std::string("unreadable"));
}

void appendProbeValue(
    std::vector<std::pair<std::string, std::string>>& probe,
    std::string key,
    const std::optional<std::int32_t>& value) {
    appendProbeValue(probe, std::move(key), value.has_value() ? std::to_string(*value) : std::string("unreadable"));
}

void appendProbeValue(
    std::vector<std::pair<std::string, std::string>>& probe,
    std::string key,
    const std::optional<std::uint16_t>& value) {
    appendProbeValue(probe, std::move(key), value.has_value() ? std::to_string(*value) : std::string("unreadable"));
}

void appendProbeValue(
    std::vector<std::pair<std::string, std::string>>& probe,
    std::string key,
    const std::optional<float>& value) {
    appendProbeValue(probe, std::move(key), value.has_value() ? std::to_string(*value) : std::string("unreadable"));
}

[[nodiscard]] bool offsetInBlock(
    const std::size_t target,
    const std::size_t blockOffset,
    const std::size_t blockSize) {
    return target >= blockOffset && target < blockOffset + blockSize;
}

void appendGobjNodeProbe(
    const std::span<const std::uint8_t> payload,
    ExtractedMldSpatialBlock& block,
    const std::size_t nodeOffset,
    const std::string& prefix) {
    appendProbeValue(block.headerProbe, prefix + ".offset", hex32(static_cast<std::uint32_t>(nodeOffset)));
    if (nodeOffset + 0x34U > payload.size()) {
        appendProbeValue(block.headerProbe, prefix + ".status", "out_of_bounds");
        return;
    }

    const auto dataPtrLe = common::readU32AtLE(payload, nodeOffset);
    const auto dataPtrBe = common::readU32AtBE(payload, nodeOffset);
    const auto childPtrLe = common::readU32AtLE(payload, nodeOffset + 0x2CU);
    const auto childPtrBe = common::readU32AtBE(payload, nodeOffset + 0x2CU);
    const auto siblingPtrLe = common::readU32AtLE(payload, nodeOffset + 0x30U);
    const auto siblingPtrBe = common::readU32AtBE(payload, nodeOffset + 0x30U);

    appendProbeValue(block.headerProbe, prefix + ".data_ptr_le", dataPtrLe);
    appendProbeValue(block.headerProbe, prefix + ".data_ptr_be", dataPtrBe);
    appendProbeValue(block.headerProbe, prefix + ".child_ptr_le", childPtrLe);
    appendProbeValue(block.headerProbe, prefix + ".child_ptr_be", childPtrBe);
    appendProbeValue(block.headerProbe, prefix + ".sibling_ptr_le", siblingPtrLe);
    appendProbeValue(block.headerProbe, prefix + ".sibling_ptr_be", siblingPtrBe);

    if (dataPtrLe.has_value() && *dataPtrLe > 0U) {
        const auto target = nodeOffset + static_cast<std::size_t>(*dataPtrLe);
        appendProbeValue(block.headerProbe, prefix + ".data_target_le", hex32(static_cast<std::uint32_t>(target)));
        appendProbeValue(block.headerProbe, prefix + ".data_target_le_in_block",
            offsetInBlock(target, block.offset, block.size) ? "true" : "false");
    }
    if (childPtrLe.has_value() && *childPtrLe > 0U) {
        const auto target = nodeOffset + 0x2CU + static_cast<std::size_t>(*childPtrLe);
        appendProbeValue(block.headerProbe, prefix + ".child_target_le", hex32(static_cast<std::uint32_t>(target)));
        appendProbeValue(block.headerProbe, prefix + ".child_target_le_in_block",
            offsetInBlock(target, block.offset, block.size) ? "true" : "false");
    }
    if (siblingPtrLe.has_value() && *siblingPtrLe > 0U) {
        const auto target = nodeOffset + 0x2CU + static_cast<std::size_t>(*siblingPtrLe);
        appendProbeValue(block.headerProbe, prefix + ".sibling_target_le", hex32(static_cast<std::uint32_t>(target)));
        appendProbeValue(block.headerProbe, prefix + ".sibling_target_le_in_block",
            offsetInBlock(target, block.offset, block.size) ? "true" : "false");
    }
    if (dataPtrBe.has_value() && *dataPtrBe > 0U) {
        const auto target = nodeOffset + 4U + static_cast<std::size_t>(*dataPtrBe);
        appendProbeValue(block.headerProbe, prefix + ".data_target_be_rel_field", hex32(static_cast<std::uint32_t>(target)));
        appendProbeValue(block.headerProbe, prefix + ".data_target_be_rel_field_in_block",
            offsetInBlock(target, block.offset, block.size) ? "true" : "false");
    }
    if (childPtrBe.has_value() && *childPtrBe > 0U) {
        const auto target = nodeOffset + 0x2CU + static_cast<std::size_t>(*childPtrBe);
        appendProbeValue(block.headerProbe, prefix + ".child_target_be_rel_field", hex32(static_cast<std::uint32_t>(target)));
        appendProbeValue(block.headerProbe, prefix + ".child_target_be_rel_field_in_block",
            offsetInBlock(target, block.offset, block.size) ? "true" : "false");
    }
    if (siblingPtrBe.has_value() && *siblingPtrBe > 0U) {
        const auto target = nodeOffset + 0x30U + static_cast<std::size_t>(*siblingPtrBe);
        appendProbeValue(block.headerProbe, prefix + ".sibling_target_be_rel_field", hex32(static_cast<std::uint32_t>(target)));
        appendProbeValue(block.headerProbe, prefix + ".sibling_target_be_rel_field_in_block",
            offsetInBlock(target, block.offset, block.size) ? "true" : "false");
    }
}

void appendSpatialHeaderProbe(
    const std::span<const std::uint8_t> payload,
    ExtractedMldSpatialBlock& block,
    const std::optional<std::size_t> nextCandidate) {
    appendProbeValue(block.headerProbe, "tag", block.tag.empty() ? std::string("unreadable") : block.tag);
    appendProbeValue(block.headerProbe, "size_le", common::readU32AtLE(payload, static_cast<std::size_t>(block.offset) + 4U));
    appendProbeValue(block.headerProbe, "size_be", common::readU32AtBE(payload, static_cast<std::size_t>(block.offset) + 4U));
    if (nextCandidate.has_value()) {
        appendProbeValue(block.headerProbe, "next_candidate_offset", hex32(static_cast<std::uint32_t>(*nextCandidate)));
        appendProbeValue(block.headerProbe, "next_candidate_delta", std::to_string(*nextCandidate - block.offset));
    }

    if (block.kind == ExtractedMldSpatialBlock::Kind::Grnd) {
        const std::size_t inner = static_cast<std::size_t>(block.offset) + 0x10U;
        appendProbeValue(block.headerProbe, "grnd.inner_header_offset", hex32(static_cast<std::uint32_t>(inner)));
        if (inner + 0x1CU <= payload.size()) {
            const auto relTriSets = readI32AtBE(payload, inner);
            const auto relQuads = readI32AtBE(payload, inner + 4U);
            appendProbeValue(block.headerProbe, "grnd.rel_tri_sets", relTriSets);
            appendProbeValue(block.headerProbe, "grnd.rel_quad_registry", relQuads);
            if (relTriSets.has_value()) {
                appendProbeValue(block.headerProbe, "grnd.tri_sets_offset",
                    hex32(static_cast<std::uint32_t>(static_cast<std::int64_t>(inner) + *relTriSets)));
            }
            if (relQuads.has_value()) {
                appendProbeValue(block.headerProbe, "grnd.quad_registry_offset",
                    hex32(static_cast<std::uint32_t>(static_cast<std::int64_t>(inner) + *relQuads + 4)));
            }
            appendProbeValue(block.headerProbe, "grnd.center_x", common::readF32AtBE(payload, inner + 8U));
            appendProbeValue(block.headerProbe, "grnd.center_z", common::readF32AtBE(payload, inner + 0x0CU));
            appendProbeValue(block.headerProbe, "grnd.grid_x", readU16AtBE(payload, inner + 0x10U));
            appendProbeValue(block.headerProbe, "grnd.grid_z", readU16AtBE(payload, inner + 0x12U));
            appendProbeValue(block.headerProbe, "grnd.cell_size_x", readU16AtBE(payload, inner + 0x14U));
            appendProbeValue(block.headerProbe, "grnd.cell_size_z", readU16AtBE(payload, inner + 0x16U));
            appendProbeValue(block.headerProbe, "grnd.triangle_set_count", readU16AtBE(payload, inner + 0x18U));
            appendProbeValue(block.headerProbe, "grnd.quad_count", readU16AtBE(payload, inner + 0x1AU));
        } else {
            appendProbeValue(block.headerProbe, "grnd.inner_header_status", "out_of_bounds");
        }
    }

    if (block.kind == ExtractedMldSpatialBlock::Kind::Gobj ||
        block.kind == ExtractedMldSpatialBlock::Kind::UnknownObject) {
        appendGobjNodeProbe(payload, block, block.offset, "node_at_block_start");
        appendGobjNodeProbe(payload, block, static_cast<std::size_t>(block.offset) + 0x10U, "node_at_0x10");
    }
}

[[nodiscard]] std::vector<ExtractedMldSpatialBlock> buildExtractedSpatialBlocks(
    std::span<const std::uint8_t> payload,
    const std::unordered_set<std::uint32_t>& groundAddresses,
    const std::unordered_set<std::uint32_t>& objectAddresses,
    const std::unordered_set<std::uint32_t>& motionAddresses,
    const std::unordered_set<std::uint32_t>& textureAddresses,
    const SpatialOwnerMap& groundOwners,
    const SpatialOwnerMap& objectOwners) {
    std::vector<std::uint32_t> candidateOffsets{};
    candidateOffsets.reserve(groundAddresses.size() + objectAddresses.size() + motionAddresses.size() + textureAddresses.size());
    const auto appendCandidate = [&](const std::uint32_t offset) {
        if (offset > 0U && static_cast<std::size_t>(offset) < payload.size()) {
            candidateOffsets.push_back(offset);
        }
    };
    for (const auto offset : groundAddresses) {
        appendCandidate(offset);
    }
    for (const auto offset : objectAddresses) {
        appendCandidate(offset);
    }
    for (const auto offset : motionAddresses) {
        appendCandidate(offset);
    }
    for (const auto offset : textureAddresses) {
        appendCandidate(offset);
    }
    std::sort(candidateOffsets.begin(), candidateOffsets.end());
    candidateOffsets.erase(std::unique(candidateOffsets.begin(), candidateOffsets.end()), candidateOffsets.end());

    std::vector<ExtractedMldSpatialBlock> blocks{};
    blocks.reserve(groundAddresses.size() + objectAddresses.size());

    const auto appendBlock = [&](const std::uint32_t offset, const bool isGround) {
        if (offset == 0U || static_cast<std::size_t>(offset) >= payload.size()) {
            return;
        }

        const auto begin = static_cast<std::size_t>(offset);
        const std::string tag = asciiTagAt(payload, begin);
        if (!isGround && isNjLikeTag(tag)) {
            return;
        }

        const auto nextCandidate = nextCandidateAfter(candidateOffsets, offset);
        std::size_t size = 0U;
        std::string sizeSource{};

        if (tag == "GRND" || tag == "GOBJ") {
            const auto leSize = common::readU32AtLE(payload, begin + 4U);
            if (leSize.has_value() && isPlausibleBlockSize(payload, begin, *leSize)) {
                size = static_cast<std::size_t>(*leSize);
                sizeSource = "header_le";
            } else {
                const auto beSize = common::readU32AtBE(payload, begin + 4U);
                if (beSize.has_value() && isPlausibleBlockSize(payload, begin, *beSize)) {
                    size = static_cast<std::size_t>(*beSize);
                    sizeSource = "header_be";
                }
            }
        }

        if (size == 0U && nextCandidate.has_value() && *nextCandidate > begin) {
            size = *nextCandidate - begin;
            sizeSource = "next_candidate";
        }
        if (size == 0U) {
            return;
        }

        ExtractedMldSpatialBlock block{};
        block.offset = offset;
        block.size = size;
        block.tag = tag;
        block.sizeSource = sizeSource;
        if (tag == "GOBJ") {
            block.kind = ExtractedMldSpatialBlock::Kind::Gobj;
            if (isGround) {
                if (const auto owners = groundOwners.find(offset); owners != groundOwners.end()) {
                    block.owners = owners->second;
                }
            } else if (const auto owners = objectOwners.find(offset); owners != objectOwners.end()) {
                block.owners = owners->second;
            }
        } else if (isGround) {
            block.kind = tag == "GRND"
                ? ExtractedMldSpatialBlock::Kind::Grnd
                : ExtractedMldSpatialBlock::Kind::UnknownGround;
            if (const auto owners = groundOwners.find(offset); owners != groundOwners.end()) {
                block.owners = owners->second;
            }
        } else {
            block.kind = tag == "GOBJ"
                ? ExtractedMldSpatialBlock::Kind::Gobj
                : ExtractedMldSpatialBlock::Kind::UnknownObject;
            if (const auto owners = objectOwners.find(offset); owners != objectOwners.end()) {
                block.owners = owners->second;
            }
        }

        block.bytes.assign(
            payload.begin() + static_cast<std::ptrdiff_t>(begin),
            payload.begin() + static_cast<std::ptrdiff_t>(begin + size));
        appendSpatialHeaderProbe(payload, block, nextCandidate);
        blocks.push_back(std::move(block));
    };

    for (const auto offset : groundAddresses) {
        appendBlock(offset, true);
    }
    for (const auto offset : objectAddresses) {
        appendBlock(offset, false);
    }

    std::sort(blocks.begin(), blocks.end(), [](const auto& a, const auto& b) {
        if (a.offset != b.offset) {
            return a.offset < b.offset;
        }
        return static_cast<int>(a.kind) < static_cast<int>(b.kind);
    });
    return blocks;
}

class CollisionEntryHandler final : public EntryHandler {
public:
    [[nodiscard]] bool canHandle(const std::string_view fxnName) const override {
        const auto normalized = normalizeFxnName(fxnName);
        return normalized == "wall" ||
            normalized == "walluv" ||
            normalized == "hasigo1" ||
            normalized == "hasigo2";
    }

    void parse(const RawEntry& entry, model::WorldModel& out) const override {
        model::CollisionVolume collision{};
        collision.sourceEntryId = entry.sourceEntryId;
        collision.fxnName = std::string(entry.fxnName);
        collision.tblId = entry.tblId;
        collision.transform = entry.transform;
        collision.objectAddresses = entry.objectAddresses;
        out.collisions.push_back(std::move(collision));
    }
};

class TriggerEntryHandler final : public EntryHandler {
public:
    [[nodiscard]] bool canHandle(const std::string_view fxnName) const override {
        const auto normalized = normalizeFxnName(fxnName);
        return normalized == "treasure" ||
            normalized == "goscript" ||
            normalized == "wallmot";
    }

    void parse(const RawEntry& entry, model::WorldModel& out) const override {
        model::TriggerVolume trigger{};
        trigger.sourceEntryId = entry.sourceEntryId;
        trigger.fxnName = std::string(entry.fxnName);
        trigger.tblId = entry.tblId;
        trigger.transform = entry.transform;
        trigger.objectAddresses = entry.objectAddresses;
        out.triggers.push_back(std::move(trigger));
    }
};

} // namespace

ParseResult MldParser::parse(std::span<const std::uint8_t> mldBytes, const ParseOptions& options) const {
    std::cout << "[SoaSimMLD] Step 1/5: Starting parse (" << mldBytes.size() << " bytes).\n";
    ParseResult result{};

    std::vector<std::uint8_t> decoded;
    std::span<const std::uint8_t> payload = mldBytes;
    if (soasim::compression::aklz::isAklz(mldBytes)) {
        std::cout << "[SoaSimMLD] Step 2/5: Input is AKLZ-compressed, decompressing...\n";
        auto decodedResult = soasim::compression::aklz::decompress(mldBytes);
        if (!decodedResult.ok()) {
            result.diagnostics.push_back(ParseDiagnostic{
                .severity = ParseDiagnostic::Severity::Error,
                .message = "AKLZ decompression failed: " + std::string(soasim::compression::aklz::errorToString(decodedResult.error)),
            });
            return result;
        }

        decoded = std::move(decodedResult.bytes);
        payload = std::span<const std::uint8_t>(decoded.data(), decoded.size());
    }
    else {
        std::cout << "[SoaSimMLD] Step 2/5: Input is not AKLZ-compressed.\n";
    }

    if (payload.empty()) {
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Error,
            .message = "Input buffer is empty.",
        });
        return result;
    }

    std::unordered_map<std::string, std::size_t> histogram{};
    std::unordered_map<std::uint32_t, std::size_t> chunkTypeCounts{};
    if (payload.size() < 0x14) {
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Error,
            .message = "MLD header too small.",
        });
        return result;
    }

    const auto entryCountOpt = common::readU32AtBE(payload, 0x00);
    const auto ptrIndexOpt = common::readU32AtBE(payload, 0x04);
    const auto ptrFxnParamsOpt = common::readU32AtBE(payload, 0x08);
    const auto ptrRealDataOpt = common::readU32AtBE(payload, 0x0C);
    const auto ptrTextureTableOpt = common::readU32AtBE(payload, 0x10);
    if (!entryCountOpt.has_value() || !ptrIndexOpt.has_value() || !ptrFxnParamsOpt.has_value() ||
        !ptrRealDataOpt.has_value() || !ptrTextureTableOpt.has_value()) {
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Error,
            .message = "Failed to read required MLD header pointers.",
        });
        return result;
    }

    constexpr std::size_t entrySize = 0x68;
    const std::size_t entryCount = static_cast<std::size_t>(*entryCountOpt);
    const std::size_t entryTableOffset = static_cast<std::size_t>(*ptrIndexOpt);
    const std::size_t entryTableEnd = entryTableOffset + (entryCount * entrySize);
    if (entryTableOffset >= payload.size() || entryTableEnd > payload.size()) {
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Error,
            .message = "MLD entry table is out of bounds (count=" + std::to_string(entryCount) +
                ", ptr=" + std::to_string(entryTableOffset) + ").",
        });
        return result;
    }

    result.diagnostics.push_back(ParseDiagnostic{
        .severity = ParseDiagnostic::Severity::Info,
        .message = "Index-based parse: entries=" + std::to_string(entryCount) +
            ", entryTable=0x" + std::to_string(entryTableOffset) +
            ", fxnParams=0x" + std::to_string(static_cast<std::size_t>(*ptrFxnParamsOpt)) +
            ", realData=0x" + std::to_string(static_cast<std::size_t>(*ptrRealDataOpt)) +
            ", textureTable=0x" + std::to_string(static_cast<std::size_t>(*ptrTextureTableOpt)),
    });
    result.textureArchive = parseMldTextureArchive(payload, static_cast<std::size_t>(*ptrTextureTableOpt));
    if (result.textureArchive.has_value()) {
        for (const auto& textureDiag : result.textureArchive->diagnostics) {
            result.diagnostics.push_back(ParseDiagnostic{
                .severity = ParseDiagnostic::Severity::Info,
                .message = textureDiag,
            });
        }
    }
    if (!options.filterEntryIdList.empty()) {
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Info,
            .message = "Entry filter enabled by ID list (" + std::to_string(options.filterEntryIdList.size()) + " IDs).",
        });
    } else if (!options.filterFxnName.empty()) {
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Info,
            .message = "Entry filter enabled by fxnName=\"" + options.filterFxnName + "\".",
        });
    }

    std::vector<model::IndexEntry> entries{};
    entries.reserve(entryCount);
    std::cout << "[SoaSimMLD] Step 3/5: Reading index entries (" << entryCount << " total)...\n";

    for (std::size_t i = 0; i < entryCount; ++i) {
        const std::size_t entryOffset = entryTableOffset + (i * entrySize);

        auto entryOpt = model::parseIndexEntry(payload, i, entryOffset,
            [&](const Vec3& value) {
                return applyCoordinates(value, options.coordinates);
            },
            [&](const std::string& message) {
                result.diagnostics.push_back(ParseDiagnostic{
                    .severity = ParseDiagnostic::Severity::Warning,
                    .message = message,
                });
            });
        if (!entryOpt.has_value()) {
            result.diagnostics.push_back(ParseDiagnostic{
                .severity = ParseDiagnostic::Severity::Warning,
                .message = "Entry " + std::to_string(i) + " malformed or truncated.",
            });
            continue;
        }

        entries.push_back(std::move(*entryOpt));
    }

    result.entryList.reserve(entries.size());
    for (const auto& entry : entries) {
        result.entryList.push_back(makeEntryListItem(payload, entry));
    }

    if (options.entryListOnly) {
        for (const auto& entry : result.entryList) {
            addHistogram(histogram, options, entry.fxnName);
        }
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Info,
            .message = "Entry list-only mode parsed " + std::to_string(result.entryList.size()) + " MLD index entries.",
        });
        if (options.emitFxnHistogram) {
            result.fxnHistogram.reserve(histogram.size());
            for (const auto& [fxn, count] : histogram) {
                result.fxnHistogram.emplace_back(fxn, count);
            }
            std::sort(result.fxnHistogram.begin(), result.fxnHistogram.end(),
                [](const auto& a, const auto& b) {
                    return a.first < b.first;
                });
        }
        std::cout << "[SoaSimMLD] Step 4/5: Entry list-only mode skipping payload/chunk decode.\n";
        std::cout << "[SoaSimMLD] Step 5/5: Parse complete. Entries=" << entries.size()
                  << ", GRND=0.\n";
        return result;
    }

    std::cout << "[SoaSimMLD] Step 4/5: Decoding entry payloads/chunks...\n";

    std::unordered_set<std::uint32_t> uniqueGroundAddresses{};
    std::unordered_set<std::uint32_t> uniqueObjectAddresses{};
    std::unordered_set<std::uint32_t> uniqueMotionAddresses{};
    std::unordered_set<std::uint32_t> uniqueTextureAddresses{};
    std::unordered_map<std::uint32_t, const model::IndexEntry*> groundAddressOwners{};
    SpatialOwnerMap groundBlockOwners{};
    SpatialOwnerMap objectBlockOwners{};
    const std::array<std::unique_ptr<EntryHandler>, 2> handlers{
        std::make_unique<CollisionEntryHandler>(),
        std::make_unique<TriggerEntryHandler>(),
    };
    const bool useEntryIdFilter = !options.filterEntryIdList.empty();
    std::unordered_set<std::uint32_t> selectedEntryIds{};
    if (useEntryIdFilter) {
        selectedEntryIds.insert(options.filterEntryIdList.begin(), options.filterEntryIdList.end());
    }
    const std::string normalizedFilterFxn = useEntryIdFilter ? std::string{} : normalizeFxnName(options.filterFxnName);
    const bool useFxnFilter = !useEntryIdFilter && !normalizedFilterFxn.empty();

    for (const auto& entry : entries) {
        const auto normalizedFxnName = normalizeFxnName(entry.fxnName);
        const bool selected = useEntryIdFilter
            ? (selectedEntryIds.find(entry.entryId) != selectedEntryIds.end())
            : (!useFxnFilter || normalizedFxnName == normalizedFilterFxn);
        if (!selected) {
            continue;
        }

        addHistogram(histogram, options, entry.fxnName);

        const std::size_t entryOffset = entryTableOffset + (entry.tableIndex * entrySize);
        std::vector<std::uint32_t> objectAddresses{};
        objectAddresses.reserve(entry.objectAddresses->values.size());
        for (const auto objectAddress : entry.objectAddresses->values) {
            if (objectAddress == 0U) {
                continue;
            }
            objectAddresses.push_back(objectAddress);
        }
        std::vector<std::uint32_t> groundAddresses{};
        groundAddresses.reserve(entry.groundAddresses->values.size());
        for (const auto groundAddress : entry.groundAddresses->values) {
            if (groundAddress == 0U) {
                continue;
            }
            groundAddresses.push_back(groundAddress);
        }
        RawEntry rawEntry{
            .sourceEntryId = entry.entryId,
            .fxnName = entry.fxnName,
            .tblId = entry.tblId,
            .transform = entry.transform,
            .objectAddresses = objectAddresses,
            .payload = std::span<const std::uint8_t>(payload.data() + static_cast<std::ptrdiff_t>(entryOffset), entrySize),
        };
        result.rawEntries.push_back(ParsedRawEntry{
            .sourceEntryId = entry.entryId,
            .fxnName = entry.fxnName,
            .tblId = entry.tblId,
            .transform = entry.transform,
            .objectAddresses = objectAddresses,
            .groundAddresses = groundAddresses,
            .payload = std::vector<std::uint8_t>(
                payload.begin() + static_cast<std::ptrdiff_t>(entryOffset),
                payload.begin() + static_cast<std::ptrdiff_t>(entryOffset + entrySize)),
        });

        bool classified = false;
        bool classifiedAsTrigger = false;
        for (const auto& handler : handlers) {
            if (!handler->canHandle(rawEntry.fxnName)) {
                continue;
            }
            handler->parse(rawEntry, result.world);
            classified = true;
            classifiedAsTrigger = normalizedFxnName == "treasure" ||
                normalizedFxnName == "goscript" ||
                normalizedFxnName == "wallmot";
            break;
        }

        if (!classified && options.preserveUnknownEntries) {
            UnknownEntry unknown{};
            unknown.sourceEntryId = entry.entryId;
            unknown.fxnName = entry.fxnName;
            unknown.tblId = entry.tblId;
            unknown.transform = entry.transform;
            unknown.rawPayload.assign(payload.begin() + static_cast<std::ptrdiff_t>(entryOffset),
                payload.begin() + static_cast<std::ptrdiff_t>(entryOffset + entrySize));
            result.world.unknownEntries.push_back(std::move(unknown));
        }

        if (classifiedAsTrigger) {
            result.searchWorld.regions.push_back(EncounterOrTriggerRegion{
                .sourceEntryId = entry.entryId,
                .fxnName = entry.fxnName,
                .tblId = entry.tblId,
                .transform = entry.transform,
            });
        }

        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Info,
            .message = "Entry " + std::to_string(entry.tableIndex) + ": id=" + std::to_string(entry.entryId) +
                ", tblId=0x" + std::to_string(entry.tblId) +
                ", fxn=\"" + entry.fxnName + "\"" +
                ", groundLinks=" + std::to_string(entry.groundLinks->values.size()) +
                ", params2=" + std::to_string(entry.paramList2->values.size()) +
                ", functionParams=" + std::to_string(entry.functionParameters->values.size()) +
                ", objects=" + std::to_string(entry.objectCount) +
                ", grounds=" + std::to_string(entry.groundCount) +
                ", motions=" + std::to_string(entry.motionCount),
        });
        for (const auto objectAddress : entry.objectAddresses->values) {
            if (objectAddress == 0U) {
                continue;
            }
            uniqueObjectAddresses.insert(objectAddress);
            objectBlockOwners[objectAddress].push_back(BlockOwnerRef{
                .sourceEntryId = entry.entryId,
                .tableIndex = entry.tableIndex,
                .fxnName = entry.fxnName,
                .role = "object",
            });
        }
        
        for (const auto groundAddress : entry.groundAddresses->values) {
            if (groundAddress == 0U) {
                continue;
            }
            if (uniqueGroundAddresses.insert(groundAddress).second) {
                groundAddressOwners.emplace(groundAddress, &entry);
            }
            groundBlockOwners[groundAddress].push_back(BlockOwnerRef{
                .sourceEntryId = entry.entryId,
                .tableIndex = entry.tableIndex,
                .fxnName = entry.fxnName,
                .role = "ground",
            });
        }

        for (const auto motionAddress : entry.motionAddresses->values) {
            if (motionAddress == 0U) {
                continue;
            }
            uniqueMotionAddresses.insert(motionAddress);
        }

        if (static_cast<std::size_t>(entry.texturesPointer) < payload.size()) {
            ++chunkTypeCounts[makeTag('N', 'J', 'T', 'L')];
            uniqueTextureAddresses.insert(entry.texturesPointer);
        }
    }

    std::vector<uint32_t> objectAddressesAll{};
    for (const auto addr : uniqueObjectAddresses) {
        objectAddressesAll.push_back(addr);
    }
    std::sort(objectAddressesAll.begin(), objectAddressesAll.end());
    result.extractedNjBlocks = buildExtractedNjBlocks(payload, uniqueObjectAddresses, uniqueMotionAddresses, uniqueTextureAddresses);
    result.diagnostics.push_back(ParseDiagnostic{
        .severity = ParseDiagnostic::Severity::Info,
        .message = "Extracted NJ blocks from MLD payload: " + std::to_string(result.extractedNjBlocks.size()),
    });
    if (options.extractGrndGobjBlocks || options.buildBlenderIntermediateIr) {
        result.extractedSpatialBlocks = buildExtractedSpatialBlocks(
            payload,
            uniqueGroundAddresses,
            uniqueObjectAddresses,
            uniqueMotionAddresses,
            uniqueTextureAddresses,
            groundBlockOwners,
            objectBlockOwners);
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Info,
            .message = "Scanned GRND/GOBJ spatial blocks from MLD payload: " + std::to_string(result.extractedSpatialBlocks.size()),
        });
    }
    result.diagnostics.push_back(ParseDiagnostic{
        .severity = ParseDiagnostic::Severity::Info,
        .message = "Legacy Ninja parsing modules (NJCM/NJTL) have been removed from MLD parsing. Object addresses are retained for SA3D IR migration.",
    });

    GrndParser grndParser{};
    for (const auto groundAddress : uniqueGroundAddresses) {
        const std::size_t grndOffset = static_cast<std::size_t>(groundAddress);
        if (grndOffset + 0x10U > payload.size()) {
            continue;
        }
        if (common::readU32AtBE(payload, grndOffset).value_or(0U) != 0x47524E44U) {
            continue;
        }

        const auto declaredSize = common::readU32AtBE(payload, grndOffset + 4U);
        if (!declaredSize.has_value() || *declaredSize < 0x10U ||
            static_cast<std::size_t>(*declaredSize) > payload.size() - grndOffset) {
            result.diagnostics.push_back(ParseDiagnostic{
                .severity = ParseDiagnostic::Severity::Warning,
                .message = "GRND block at " + std::to_string(groundAddress) + " has an invalid declared size.",
            });
            continue;
        }

        auto decoded = grndParser.decode(payload.subspan(grndOffset, static_cast<std::size_t>(*declaredSize)), groundAddress);
        for (const auto& diagnostic : decoded.diagnostics) {
            result.diagnostics.push_back(ParseDiagnostic{
                .severity = decoded.decoded ? ParseDiagnostic::Severity::Info : ParseDiagnostic::Severity::Warning,
                .message = diagnostic,
            });
        }
        if (!decoded.decoded) {
            continue;
        }

        GrndSurface surface{};
        surface.id = groundAddress;
        surface.sourceOffset = groundAddress;
        surface.mesh = std::move(decoded.mesh);
        for (auto& vertex : surface.mesh.vertices) {
            vertex.position = applyCoordinates(vertex.position, options.coordinates);
        }
        if (options.coordinates.reverseTriangleWinding && surface.mesh.indices.size() >= 3) {
            for (std::size_t ii = 0; ii + 2 < surface.mesh.indices.size(); ii += 3) {
                std::swap(surface.mesh.indices[ii + 1], surface.mesh.indices[ii + 2]);
            }
        }
        if (const auto ownerIt = groundAddressOwners.find(groundAddress); ownerIt != groundAddressOwners.end()) {
            const auto* owner = ownerIt->second;
            surface.transform = owner->transform;
            surface.linkedGrndIds.reserve(owner->groundLinks->values.size());
            for (const auto link : owner->groundLinks->values) {
                surface.linkedGrndIds.push_back(link);
            }
        }
        result.world.grndSurfaces.push_back(std::move(surface));
    }

    result.diagnostics.push_back(ParseDiagnostic{
        .severity = ParseDiagnostic::Severity::Info,
        .message = "Unique chunk address counts: grounds=" + std::to_string(uniqueGroundAddresses.size()) +
            ", objects=" + std::to_string(uniqueObjectAddresses.size()) +
            ", motions=" + std::to_string(uniqueMotionAddresses.size()),
    });

    if (options.buildBlenderIntermediateIr) {
        Sa3dBlenderIrBuilder blenderIrBuilder{};
        result.blenderIrScene = blenderIrBuilder.build(result);
        if (result.blenderIrScene.has_value()) {
            result.blenderIrDiagnostics = result.blenderIrScene->diagnostics;

            if (options.exportBlenderIrJson && !options.blenderIrOutputDir.empty()) {
                std::error_code ec{};
                std::filesystem::create_directories(options.blenderIrOutputDir, ec);
                if (ec) {
                    result.diagnostics.push_back(ParseDiagnostic{
                        .severity = ParseDiagnostic::Severity::Warning,
                        .message = "Failed to create Blender IR output directory: " + options.blenderIrOutputDir,
                    });
                } else {
                    exporting::BlenderIrJsonExporter exporter{};
                    const auto path = options.blenderIrOutputDir + "/blender_ir_scene.json";
                    std::ofstream os(path, std::ios::binary);
                    if (!os) {
                        result.diagnostics.push_back(ParseDiagnostic{
                            .severity = ParseDiagnostic::Severity::Warning,
                            .message = "Failed to open Blender IR JSON output file: " + path,
                        });
                    } else {
                        os << exporter.toJson(*result.blenderIrScene);
                        result.blenderIrArtifactPaths.push_back(path);
                    }
                }
            }
        }
    }

    result.searchWorld.surfaces.reserve(result.world.grndSurfaces.size());
    for (const auto& grnd : result.world.grndSurfaces) {
        result.searchWorld.surfaces.push_back(WalkSurfaceNode{
            .grndId = grnd.id,
            .neighborGrndIds = grnd.linkedGrndIds,
            .mesh = grnd.mesh,
        });
    }

    if (options.emitFxnHistogram) {
        result.fxnHistogram.reserve(histogram.size());
        for (const auto& [fxn, count] : histogram) {
            result.fxnHistogram.emplace_back(fxn, count);
        }
        std::sort(result.fxnHistogram.begin(), result.fxnHistogram.end(),
            [](const auto& a, const auto& b) {
                return a.first < b.first;
            });
    }

    result.chunkTypeHistogram.reserve(chunkTypeCounts.size());
    for (const auto& [tag, count] : chunkTypeCounts) {
        result.chunkTypeHistogram.emplace_back(tagToString(tag), count);
    }
    std::sort(result.chunkTypeHistogram.begin(), result.chunkTypeHistogram.end(),
        [](const auto& a, const auto& b) {
            return a.first < b.first;
        });

    result.diagnostics.push_back(ParseDiagnostic{
        .severity = ParseDiagnostic::Severity::Info,
        .message = "Parsed MLD bytes: GRND=" + std::to_string(result.world.grndSurfaces.size()) +
            ", collisions=" + std::to_string(result.world.collisions.size()) +
            ", triggers=" + std::to_string(result.world.triggers.size()) +
            ", unknownEntries=" + std::to_string(result.world.unknownEntries.size()) +
            ", chunkTypes=" + std::to_string(result.chunkTypeHistogram.size()),
    });

    if (result.world.grndSurfaces.empty()) {
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Warning,
            .message = "No GRND records decoded. This likely means container layout assumptions still need refinement.",
        });
    }

    std::cout << "[SoaSimMLD] Step 5/5: Parse complete. Entries=" << entries.size()
              << ", GRND=" << result.world.grndSurfaces.size()
              << ".\n";

    return result;
}

std::vector<ExtractedNjBlock> MldParser::extractNjBlocks(
    std::span<const std::uint8_t> mldBytes,
    const ParseOptions& options) const {
    const auto parsed = parse(mldBytes, options);
    return parsed.extractedNjBlocks;
}

std::vector<ExtractedMldSpatialBlock> MldParser::extractGrndGobjBlocks(
    std::span<const std::uint8_t> mldBytes,
    const ParseOptions& options) const {
    ParseOptions extractOptions = options;
    extractOptions.extractGrndGobjBlocks = true;
    const auto parsed = parse(mldBytes, extractOptions);
    return parsed.extractedSpatialBlocks;
}

std::string formatParseSummary(const ParseResult& parseResult) {
    std::ostringstream out;
    out << "grndSurfaces=" << parseResult.world.grndSurfaces.size() << '\n';
    out << "collisions=" << parseResult.world.collisions.size() << '\n';
    out << "triggers=" << parseResult.world.triggers.size() << '\n';
    out << "unknownEntries=" << parseResult.world.unknownEntries.size() << '\n';
    out << "entryList=" << parseResult.entryList.size() << '\n';
    out << "searchSurfaces=" << parseResult.searchWorld.surfaces.size() << '\n';
    out << "searchRegions=" << parseResult.searchWorld.regions.size() << '\n';
    out << "extractedNjBlocks=" << parseResult.extractedNjBlocks.size() << '\n';
    out << "extractedSpatialBlocks=" << parseResult.extractedSpatialBlocks.size() << '\n';
    out << "blenderIrMeshes=" << (parseResult.blenderIrScene.has_value() ? parseResult.blenderIrScene->meshes.size() : 0) << '\n';
    out << "blenderIrIndexEntries=" << (parseResult.blenderIrScene.has_value() ? parseResult.blenderIrScene->indexEntries.size() : 0) << '\n';

    if (!parseResult.chunkTypeHistogram.empty()) {
        out << "chunkTypes:" << '\n';
        for (const auto& [tag, count] : parseResult.chunkTypeHistogram) {
            out << "  - " << tag << ": " << count << '\n';
        }
    }

    if (!parseResult.fxnHistogram.empty()) {
        out << "fxnHistogram:" << '\n';
        for (const auto& [fxn, count] : parseResult.fxnHistogram) {
            out << "  - " << fxn << ": " << count << '\n';
        }
    }

    if (!parseResult.diagnostics.empty()) {
        out << "diagnostics:" << '\n';
        for (const auto& diagnostic : parseResult.diagnostics) {
            const char* severity = "info";
            switch (diagnostic.severity) {
            case ParseDiagnostic::Severity::Info:
                severity = "info";
                break;
            case ParseDiagnostic::Severity::Warning:
                severity = "warning";
                break;
            case ParseDiagnostic::Severity::Error:
                severity = "error";
                break;
            }
            out << "  - [" << severity << "] " << diagnostic.message << '\n';
        }
    }

    return out.str();
}

} // namespace soasim::mld::parsing
