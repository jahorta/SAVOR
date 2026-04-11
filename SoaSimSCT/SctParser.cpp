#include "SctParser.h"

#include "../Compression/Aklz.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <set>
#include <unordered_map>
#include <utility>

namespace soasim::sct {
namespace {

constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kIndexEntrySize = 0x14;
constexpr std::size_t kIndexNameOffset = 4;
constexpr std::size_t kIndexNameMaxLen = 0x10;
constexpr std::uint32_t kMaxOpcodeProbe = 265;

enum class Endian {
    Big,
    Little,
};

[[nodiscard]] std::uint32_t readU32(std::span<const std::uint8_t> bytes, std::size_t offset, Endian endian) {
    if (offset + sizeof(std::uint32_t) > bytes.size()) {
        return 0;
    }

    const std::uint32_t b0 = bytes[offset + 0];
    const std::uint32_t b1 = bytes[offset + 1];
    const std::uint32_t b2 = bytes[offset + 2];
    const std::uint32_t b3 = bytes[offset + 3];

    if (endian == Endian::Big) {
        return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
    }

    return (b3 << 24) | (b2 << 16) | (b1 << 8) | b0;
}

[[nodiscard]] std::string readIndexName(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset >= bytes.size()) {
        return {};
    }

    const std::size_t limit = std::min<std::size_t>(bytes.size(), offset + kIndexNameMaxLen);
    std::string name;
    for (std::size_t i = offset; i < limit; ++i) {
        if (bytes[i] == 0) {
            break;
        }
        name.push_back(static_cast<char>(bytes[i]));
    }
    return name;
}

[[nodiscard]] Endian detectIndexEndian(std::span<const std::uint8_t> bytes) {
    const auto big = readU32(bytes, 8, Endian::Big);
    const auto little = readU32(bytes, 8, Endian::Little);
    return big <= little ? Endian::Big : Endian::Little;
}

struct DecodedInstruction {
    SctInstruction inst;
    std::vector<std::uint32_t> successors;
    bool blockTerminator = false;
    bool touchesFlag = false;
    bool writesFlag = false;
    bool testedFlag = false;
    bool isSwitch = false;
};

[[nodiscard]] bool isOffsetInBounds(std::uint32_t offset, std::uint32_t sectionSize) {
    return offset < sectionSize && (offset % 4u == 0u);
}

[[nodiscard]] std::uint32_t clampToSection(std::uint32_t raw, std::uint32_t sectionSize) {
    if (sectionSize == 0) {
        return 0;
    }
    if (raw >= sectionSize) {
        return sectionSize - (sectionSize % 4u == 0u ? 4u : sectionSize % 4u);
    }
    return raw - (raw % 4u);
}

[[nodiscard]] DecodedInstruction decodeInstruction(
    std::span<const std::uint8_t> sectionBytes,
    std::uint32_t offset,
    Endian baseEndian,
    std::vector<SctDiagnostic>& diagnostics) {

    DecodedInstruction decoded{};
    decoded.inst.offset = offset;

    if (offset + 4u > sectionBytes.size()) {
        decoded.inst.decodeOk = false;
        decoded.inst.sizeBytes = 0;
        diagnostics.push_back({"Instruction decode failed: out-of-bounds read.", offset});
        decoded.blockTerminator = true;
        return decoded;
    }

    const auto wordBase = readU32(sectionBytes, offset, baseEndian);
    const auto otherEndian = baseEndian == Endian::Big ? Endian::Little : Endian::Big;
    const auto wordOther = readU32(sectionBytes, offset, otherEndian);

    const auto baseOpcode = static_cast<std::uint16_t>(wordBase & 0xffffu);
    const auto otherOpcode = static_cast<std::uint16_t>(wordOther & 0xffffu);

    Endian chosenEndian = baseEndian;
    std::uint32_t word = wordBase;

    if (baseOpcode > kMaxOpcodeProbe && otherOpcode <= kMaxOpcodeProbe) {
        chosenEndian = otherEndian;
        word = wordOther;
    }

    const auto opcode = static_cast<std::uint16_t>(word & 0xffffu);
    decoded.inst.opcode = opcode;
    decoded.inst.decodeOk = opcode <= kMaxOpcodeProbe;
    decoded.inst.sizeBytes = 4;

    // This is intentionally conservative and only supports a tiny "known" set.
    // All offsets are interpreted as section-relative word offsets from the next instruction.
    if (opcode == 0 || opcode == 10 || opcode == 3) {
        if (offset + 8u > sectionBytes.size()) {
            diagnostics.push_back({"Control-flow instruction missing payload word.", offset});
            decoded.blockTerminator = true;
            return decoded;
        }

        const auto rawArg = readU32(sectionBytes, offset + 4u, chosenEndian);
        decoded.inst.operands.push_back(rawArg);
        decoded.inst.sizeBytes = 8;

        if (opcode == 3) {
            decoded.isSwitch = true;
            decoded.blockTerminator = true;

            const auto caseCount = rawArg & 0xffu;
            const auto tableStart = offset + decoded.inst.sizeBytes;
            for (std::uint32_t i = 0; i < caseCount; ++i) {
                const auto caseEntryOffset = tableStart + (i * 4u);
                if (caseEntryOffset + 4u > sectionBytes.size()) {
                    diagnostics.push_back({"Switch table truncated.", offset});
                    break;
                }
                const auto rel = static_cast<std::int32_t>(readU32(sectionBytes, caseEntryOffset, chosenEndian));
                const auto nextOffset = static_cast<std::int64_t>(offset + decoded.inst.sizeBytes) + rel;
                if (nextOffset >= 0) {
                    decoded.successors.push_back(static_cast<std::uint32_t>(nextOffset));
                }
            }
            decoded.inst.sizeBytes += caseCount * 4u;
        } else {
            const auto rel = static_cast<std::int32_t>(rawArg);
            const auto jumpTarget = static_cast<std::int64_t>(offset + decoded.inst.sizeBytes) + rel;
            if (jumpTarget >= 0) {
                decoded.successors.push_back(static_cast<std::uint32_t>(jumpTarget));
            }

            if (opcode == 0) {
                decoded.successors.push_back(offset + decoded.inst.sizeBytes);
            }

            decoded.blockTerminator = true;
        }
    }

    if (opcode == 12) {
        decoded.blockTerminator = true;
    }

    // Placeholder flag heuristics for early indexing.
    if (opcode == 52 || opcode == 53 || opcode == 54) {
        decoded.touchesFlag = true;
        decoded.testedFlag = true;
        if (!decoded.inst.operands.empty()) {
            decoded.inst.operands.push_back(decoded.inst.operands.front());
        }
    }

    if (opcode == 55 || opcode == 56) {
        decoded.touchesFlag = true;
        decoded.writesFlag = true;
    }

    return decoded;
}

void fillUnknownRegions(const std::set<std::uint32_t>& visitedOffsets, std::span<const std::uint8_t> sectionBytes, SctSection& section) {
    std::uint32_t cursor = 0;
    while (cursor < sectionBytes.size()) {
        const bool visited = visitedOffsets.contains(cursor);
        if (visited) {
            cursor += 4;
            continue;
        }

        const auto start = cursor;
        while (cursor < sectionBytes.size() && !visitedOffsets.contains(cursor)) {
            cursor += 4;
        }

        SctUnknownRegion region{};
        region.startOffset = start;
        region.endOffset = cursor;
        region.reason = "Not reached by control-flow-guided pass.";
        region.rawBytes.insert(
            region.rawBytes.end(),
            sectionBytes.begin() + start,
            sectionBytes.begin() + std::min<std::size_t>(cursor, sectionBytes.size()));

        section.unknownRegions.push_back(std::move(region));
    }
}

} // namespace

SctParseResult SctParser::parseFile(const std::string& sourcePath) const {
    SctParseResult result{};
    result.file.sourcePath = sourcePath;

    if (sourcePath.empty()) {
        result.diagnostics.push_back({"SCT parse skipped: source path is empty.", 0});
        return result;
    }

    std::ifstream in(sourcePath, std::ios::binary);
    if (!in) {
        result.diagnostics.push_back({"Unable to open SCT file on disk.", 0});
        return result;
    }

    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    in.seekg(0, std::ios::beg);

    if (size <= 0) {
        result.diagnostics.push_back({"SCT parse skipped: file is empty.", 0});
        return result;
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

    return parse(bytes, sourcePath);
}

SctParseResult SctParser::parse(std::span<const std::uint8_t> bytes, std::string sourcePath) const {
    SctParseResult result{};
    result.file.sourcePath = std::move(sourcePath);

    std::vector<std::uint8_t> decoded;
    std::span<const std::uint8_t> payload = bytes;
    if (soasim::compression::aklz::isAklz(bytes)) {
        auto decodedResult = soasim::compression::aklz::decompress(bytes);
        if (!decodedResult.ok()) {
            result.diagnostics.push_back({
                "AKLZ decompression failed: " + std::string(soasim::compression::aklz::errorToString(decodedResult.error)),
                0
            });
            return result;
        }

        decoded = std::move(decodedResult.bytes);
        payload = std::span<const std::uint8_t>(decoded.data(), decoded.size());
    }

    if (payload.empty()) {
        result.diagnostics.push_back({"SCT parse skipped: input byte buffer is empty.", 0});
        return result;
    }

    if (payload.size() < kHeaderSize) {
        result.diagnostics.push_back({"SCT parse failed: file too small for header and index count.", 0});
        return result;
    }

    const auto indexEndian = detectIndexEndian(payload);
    const auto sectionCount = readU32(payload, 8, indexEndian);
    const std::size_t indexSize = static_cast<std::size_t>(sectionCount) * kIndexEntrySize;

    if (kHeaderSize + indexSize > payload.size()) {
        result.diagnostics.push_back({"SCT parse failed: index table exceeds file bounds.", 8});
        return result;
    }

    struct SectionRow {
        std::uint32_t start = 0;
        std::string name;
    };

    std::vector<SectionRow> rows;
    rows.reserve(sectionCount);

    for (std::uint32_t i = 0; i < sectionCount; ++i) {
        const auto rowOffset = kHeaderSize + (static_cast<std::size_t>(i) * kIndexEntrySize);
        const auto start = readU32(payload, rowOffset, indexEndian);
        auto name = readIndexName(payload, rowOffset + kIndexNameOffset);
        if (name.empty()) {
            name = "section_" + std::to_string(i);
        }
        rows.push_back({start, std::move(name)});
    }

    const auto dataStart = static_cast<std::uint32_t>(kHeaderSize + indexSize);
    const auto dataSize = static_cast<std::uint32_t>(payload.size() - dataStart);
    const auto dataBytes = payload.subspan(dataStart);

    for (std::uint32_t i = 0; i < rows.size(); ++i) {
        const auto sectionStart = rows[i].start;
        const auto sectionEnd = (i + 1u < rows.size()) ? rows[i + 1u].start : dataSize;

        if (sectionStart > dataSize || sectionEnd > dataSize || sectionEnd < sectionStart) {
            result.diagnostics.push_back({"Invalid section bounds in SCT index.", sectionStart});
            continue;
        }

        SctSection section{};
        section.id.index = i;
        section.id.name = rows[i].name;
        section.startOffset = dataStart + sectionStart;
        section.endOffset = dataStart + sectionEnd;

        const auto sectionBytes = dataBytes.subspan(sectionStart, sectionEnd - sectionStart);
        if (sectionBytes.empty()) {
            result.file.sections.push_back(std::move(section));
            continue;
        }

        // Control-flow guided pass starting at section offset 0.
        std::deque<std::uint32_t> worklist;
        std::set<std::uint32_t> enqueued;
        std::set<std::uint32_t> visited;
        std::unordered_map<std::uint32_t, std::size_t> instructionByOffset;

        worklist.push_back(0);
        enqueued.insert(0);

        while (!worklist.empty()) {
            const auto blockStart = worklist.front();
            worklist.pop_front();

            if (!isOffsetInBounds(blockStart, static_cast<std::uint32_t>(sectionBytes.size()))) {
                continue;
            }

            SctBasicBlock block{};
            block.startOffset = blockStart;

            std::uint32_t cursor = blockStart;
            while (isOffsetInBounds(cursor, static_cast<std::uint32_t>(sectionBytes.size()))) {
                if (visited.contains(cursor)) {
                    break;
                }

                auto decoded = decodeInstruction(sectionBytes, cursor, indexEndian, result.diagnostics);
                if (decoded.inst.sizeBytes == 0) {
                    break;
                }

                instructionByOffset[cursor] = section.instructions.size();
                visited.insert(cursor);
                section.instructions.push_back(decoded.inst);
                block.instructionOffsets.push_back(cursor);

                if (decoded.touchesFlag) {
                    section.heuristicEvidence.touchesFlags = true;
                }
                if (decoded.testedFlag) {
                    section.heuristicEvidence.branchesOnFlags = true;
                }
                if (decoded.writesFlag) {
                    section.heuristicEvidence.writesFlags = true;
                }
                if (decoded.isSwitch) {
                    section.heuristicEvidence.hasSwitch = true;
                }

                for (auto successor : decoded.successors) {
                    successor = clampToSection(successor, static_cast<std::uint32_t>(sectionBytes.size()));
                    block.successorOffsets.push_back(successor);
                    if (isOffsetInBounds(successor, static_cast<std::uint32_t>(sectionBytes.size())) && !enqueued.contains(successor)) {
                        worklist.push_back(successor);
                        enqueued.insert(successor);
                    }
                }

                const auto nextCursor = cursor + decoded.inst.sizeBytes;
                if (decoded.blockTerminator) {
                    break;
                }

                if (!isOffsetInBounds(nextCursor, static_cast<std::uint32_t>(sectionBytes.size()))) {
                    break;
                }

                cursor = nextCursor;
            }

            if (!block.instructionOffsets.empty()) {
                const auto last = block.instructionOffsets.back();
                const auto& lastInst = section.instructions[instructionByOffset[last]];
                block.endOffset = last + lastInst.sizeBytes;
                section.blocks.push_back(std::move(block));
            }
        }

        std::sort(section.instructions.begin(), section.instructions.end(), [](const auto& a, const auto& b) {
            return a.offset < b.offset;
        });

        fillUnknownRegions(visited, sectionBytes, section);

        if (section.instructions.size() >= 16) {
            section.heuristicEvidence.hasLongLinearSequence = true;
        }

        if (section.heuristicEvidence.hasSwitch || section.heuristicEvidence.branchesOnFlags) {
            section.heuristicEvidence.likelyTrigger = true;
            section.heuristicEvidence.notes.push_back("Contains switch/branch patterns compatible with trigger checks.");
        }

        if (section.heuristicEvidence.hasLongLinearSequence && !section.heuristicEvidence.likelyTrigger) {
            section.heuristicEvidence.likelyCutscene = true;
            section.heuristicEvidence.notes.push_back("Long reachable sequence without strong trigger indicators.");
        }

        result.file.sections.push_back(std::move(section));
    }

    if (result.file.sections.empty()) {
        result.diagnostics.push_back({"No sections were parsed from SCT index.", 0});
        return result;
    }

    result.parseOk = true;
    result.diagnostics.push_back(
        {"Initial SCT parser pass completed (index + control-flow-guided section walk with placeholder opcode semantics).", 0});
    return result;
}

} // namespace soasim::sct
