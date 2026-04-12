#include "SctParser.h"
#include "SctOpcodeMetadata.h"
#include "SctScptDecodeHelpers.h"

#include "../Compression/Aklz.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iostream>
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
constexpr std::uint32_t kScptStopCode = 0x0000001d;

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

[[nodiscard]] bool isScptNoLoopValue(std::uint32_t value) {
    return value == 0x7f7fffff || value == 0x00800000 || value == 0x7fffffff || value == kScptStopCode;
}

[[nodiscard]] bool isStringSection(std::span<const std::uint8_t> sectionBytes, Endian baseEndian) {
    if (sectionBytes.size() < 12u) {
        return false;
    }

    const auto otherEndian = baseEndian == Endian::Big ? Endian::Little : Endian::Big;
    const auto firstWordBase = readU32(sectionBytes, 0, baseEndian);
    const auto firstWordOther = readU32(sectionBytes, 0, otherEndian);

    Endian chosenEndian = baseEndian;
    if (firstWordBase != 0x00000009u) {
        if (firstWordOther != 0x00000009u) {
            return false;
        }
        chosenEndian = otherEndian;
    }

    std::uint32_t cursor = 0;
    while (cursor + 4u <= sectionBytes.size()) {
        const auto currentWord = readU32(sectionBytes, cursor, chosenEndian);
        if (currentWord == kScptStopCode) {
            break;
        }
        cursor += 4u;
    }

    if (cursor + 8u > sectionBytes.size()) {
        return false;
    }

    const auto nextWord = readU32(sectionBytes, cursor + 4u, chosenEndian);
    return nextWord > kMaxOpcodeProbe;
}

[[nodiscard]] std::uint32_t scptInputActionPrefix(std::uint32_t value) {
    if (value >= 0x50000000) {
        return 0x50000000;
    }
    if (value >= 0x40000000) {
        return 0x40000000;
    }
    if (value >= 0x20000000) {
        return 0x20000000;
    }
    if (value >= 0x10000000) {
        return 0x10000000;
    }
    if (value >= 0x08000000) {
        return 0x08000000;
    }
    if (value >= 0x04000000) {
        return 0x04000000;
    }
    return 0;
}

[[nodiscard]] bool isScptCompareCode(std::uint32_t value) {
    switch (value) {
    case 0x00000000:
    case 0x00000001:
    case 0x00000002:
    case 0x00000003:
    case 0x00000004:
    case 0x00000005:
    case 0x00000006:
    case 0x00000007:
    case 0x00000008:
    case 0x00000009:
    case 0x0000000a:
    case 0x00000010:
    case 0x00000011:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool isScptArithmeticCode(std::uint32_t value) {
    switch (value) {
    case 0x0000000b:
    case 0x0000000c:
    case 0x0000000d:
    case 0x0000000e:
    case 0x0000000f:
    case 0x00000012:
    case 0x00000013:
    case 0x00000014:
    case 0x00000015:
    case 0x00000016:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] std::uint32_t consumeScptParameterWords(
    std::span<const std::uint8_t> sectionBytes,
    std::uint32_t wordOffset,
    Endian endian,
    std::uint32_t instructionOffset,
    std::vector<SctDiagnostic>& diagnostics,
    SctInstruction::ScptParameterValueRecord* record) {

    if (wordOffset + 4u > sectionBytes.size()) {
        diagnostics.push_back({"SCPT parameter decode failed: out-of-bounds read.", instructionOffset});
        return 0;
    }

    const auto firstWord = readU32(sectionBytes, wordOffset, endian);
    if (isScptNoLoopValue(firstWord)) {
        if (record != nullptr) {
            record->resolvedValue = detail::toHexWord(firstWord);
            record->evaluationTrace.push_back({firstWord, record->resolvedValue});
        }
        return 1;
    }

    std::uint32_t consumedWords = 0;
    std::uint32_t cursor = wordOffset;
    std::array<std::string, 20> resultStack{};

    // Mirror SALSA's _SCPT_analyze stack-driven loop semantics.
    // In Python this starts as `stack_index = 0`, `max_index = 18` and bails
    // if stack_index >= max_index before processing the current word.
    std::int32_t stackIndex = 0;
    constexpr std::int32_t kScptMaxIndex = 18;

    while (cursor + 4u <= sectionBytes.size()) {
        const auto currentWord = readU32(sectionBytes, cursor, endian);

        if (stackIndex >= kScptMaxIndex) {
            diagnostics.push_back({"SCPT parameter decode stopped at stack overflow threshold.", instructionOffset});
            break;
        }

        ++consumedWords;

        if (currentWord == kScptStopCode) {
            if (record != nullptr) {
                record->hitStopCode = true;
                if (!resultStack[2].empty()) {
                    record->resolvedValue = resultStack[2];
                } else {
                    record->resolvedValue = "return values (0x1d)";
                }
                record->evaluationTrace.push_back({currentWord, "return values (0x1d)"});
            }
            return consumedWords;
        }

        if (isScptCompareCode(currentWord)) {
            const auto lhs = detail::stackValue(resultStack, stackIndex);
            const auto rhs = detail::stackValue(resultStack, stackIndex + 1);
            const auto expr = "(" + lhs + " " + detail::compareSymbol(currentWord) + " " + rhs + ")";
            std::int32_t nones = 0;
            if (lhs == "?") {
                ++nones;
            }
            if (rhs == "?") {
                ++nones;
            }
            resultStack[std::clamp<std::int32_t>(stackIndex + nones, 0, 19)] = expr;
            if (record != nullptr) {
                record->evaluationTrace.push_back({currentWord, expr});
            }
            --stackIndex;
            if (currentWord == 0x0000000a) {
                ++stackIndex;
            }
            cursor += 4u;
            continue;
        }

        if (isScptArithmeticCode(currentWord)) {
            const auto lhs = detail::stackValue(resultStack, stackIndex);
            const auto rhs = detail::stackValue(resultStack, stackIndex + 1);
            const auto expr = "(" + lhs + " " + detail::arithmeticSymbol(currentWord) + " " + rhs + ")";
            std::int32_t nones = 0;
            if (lhs == "?") {
                ++nones;
            }
            if (rhs == "?") {
                ++nones;
            }
            resultStack[std::clamp<std::int32_t>(stackIndex + nones, 0, 19)] = expr;
            if (record != nullptr) {
                record->evaluationTrace.push_back({currentWord, expr});
            }
            --stackIndex;
            cursor += 4u;
            continue;
        }

        const auto action = scptInputActionPrefix(currentWord);
        if (action != 0x50000000u) {
            if (action == 0x04000000u) {
                if (cursor + 8u > sectionBytes.size()) {
                    diagnostics.push_back({"SCPT float literal payload exceeds section bounds.", instructionOffset});
                    return consumedWords;
                }
                const auto floatPayload = readU32(sectionBytes, cursor + 4u, endian);
                const auto floatValue = detail::floatFromWordBits(floatPayload);
                const auto value = std::string{detail::inputPrefix(action)} + std::to_string(floatValue);
                resultStack[std::clamp<std::int32_t>(stackIndex + 2, 0, 19)] = value;
                if (record != nullptr) {
                    record->evaluationTrace.push_back({currentWord, detail::toHexWord(currentWord)});
                    record->evaluationTrace.push_back({floatPayload, value});
                }
                ++consumedWords;
                cursor += 8u;
            } else {
                std::string value;
                if (action == 0x08000000u) {
                    const auto whole = (currentWord & 0x00ffff00u) >> 8u;
                    const auto frac = currentWord & 0x000000ffu;
                    value = std::string{detail::inputPrefix(action)} + std::to_string(whole) + "+" + std::to_string(frac) + "/256";
                } else {
                    value = std::string{detail::inputPrefix(action)} + std::to_string(currentWord & 0x00ffffffu);
                }
                resultStack[std::clamp<std::int32_t>(stackIndex + 2, 0, 19)] = value;
                if (record != nullptr) {
                    record->evaluationTrace.push_back({currentWord, value});
                }
                cursor += 4u;
            }
        } else {
            const auto masked = currentWord & 0x00ffffffu;
            auto value = detail::secondaryLabel(masked);
            if (value.empty()) {
                value = std::string{detail::inputPrefix(action)} + std::to_string(masked);
            }
            resultStack[std::clamp<std::int32_t>(stackIndex + 2, 0, 19)] = value;
            if (record != nullptr) {
                record->evaluationTrace.push_back({currentWord, value});
            }
            cursor += 4u;
        }
        ++stackIndex;
    }

    if (record != nullptr && record->resolvedValue.empty() && !resultStack[2].empty()) {
        record->resolvedValue = resultStack[2];
    }
    diagnostics.push_back({"SCPT parameter decode reached section end before stop code (0x1d).", instructionOffset});
    return consumedWords;
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

    if (opcode < kSalsaOpcodeParamPatterns.size()) {
        const auto& paramPattern = kSalsaOpcodeParamPatterns[opcode];
        std::uint32_t totalParamSlots = paramPattern.paramCount;
        std::uint32_t consumedOperandWords = 0;
        std::uint32_t iterations = 0;

        auto consumeParamSlot = [&](std::uint32_t paramIndex) -> bool {
            const auto paramWordOffset = offset + 4u + (consumedOperandWords * 4u);
            if (paramWordOffset + 4u > sectionBytes.size()) {
                diagnostics.push_back({"Instruction payload exceeds section bounds.", offset});
                return false;
            }

            const bool isScptParam = paramIndex < 64u && ((paramPattern.scptAnalyzeMask >> paramIndex) & 1ull) != 0ull;
            std::uint32_t wordsForParam = 1;
            SctInstruction::ScptParameterValueRecord scptRecord{};
            if (isScptParam) {
                scptRecord.parameterIndex = static_cast<std::uint8_t>(paramIndex);
                scptRecord.operandStartWordIndex = consumedOperandWords;
                wordsForParam = consumeScptParameterWords(
                    sectionBytes, paramWordOffset, chosenEndian, offset, diagnostics, &scptRecord);
                if (wordsForParam == 0) {
                    return false;
                }
                decoded.inst.scptAnalyzeOperandIndexes.push_back(static_cast<std::uint8_t>(paramIndex));
                scptRecord.operandWordCount = wordsForParam;
            }

            if (paramPattern.iterationCountParam >= 0
                && paramIndex == static_cast<std::uint32_t>(paramPattern.iterationCountParam)) {
                iterations = readU32(sectionBytes, paramWordOffset, chosenEndian);
            }

            for (std::uint32_t i = 0; i < wordsForParam; ++i) {
                const auto operandOffset = paramWordOffset + (i * 4u);
                if (operandOffset + 4u > sectionBytes.size()) {
                    diagnostics.push_back({"Instruction payload exceeds section bounds.", offset});
                    return false;
                }
                decoded.inst.operands.push_back(readU32(sectionBytes, operandOffset, chosenEndian));
            }

            consumedOperandWords += wordsForParam;
            if (isScptParam) {
                decoded.inst.scptParameterValueRecords.push_back(std::move(scptRecord));
            }
            return true;
        };

        for (std::uint32_t paramIndex = 0; paramIndex < paramPattern.paramCount; ++paramIndex) {
            if (!consumeParamSlot(paramIndex)) {
                decoded.blockTerminator = true;
                return decoded;
            }
        }

        if (paramPattern.loopStartParam >= 0 && paramPattern.loopEndParam >= paramPattern.loopStartParam
            && paramPattern.iterationCountParam >= 0 && iterations > 1u) {
            const auto loopWidth = static_cast<std::uint32_t>(paramPattern.loopEndParam - paramPattern.loopStartParam + 1);
            totalParamSlots += (iterations - 1u) * loopWidth;
            for (std::uint32_t paramIndex = paramPattern.paramCount; paramIndex < totalParamSlots; ++paramIndex) {
                if (!consumeParamSlot(paramIndex)) {
                    decoded.blockTerminator = true;
                    return decoded;
                }
            }
        }

        decoded.inst.sizeBytes = 4u + (consumedOperandWords * 4u);

        if (opcode == 0 || opcode == 10 || opcode == 3) {
            decoded.blockTerminator = true;

            if (opcode == 3) {
                decoded.isSwitch = true;
                const auto nextOffsetBase = static_cast<std::int64_t>(offset + decoded.inst.sizeBytes);
                if (paramPattern.switchJumpParam >= 0 && paramPattern.loopStartParam >= 0
                    && paramPattern.loopEndParam >= paramPattern.loopStartParam) {
                    const auto loopWidth = static_cast<std::size_t>(paramPattern.loopEndParam - paramPattern.loopStartParam + 1);
                    const auto start = static_cast<std::size_t>(paramPattern.switchJumpParam);
                    for (std::size_t i = start; i < decoded.inst.operands.size(); i += loopWidth) {
                        const auto rel = static_cast<std::int32_t>(decoded.inst.operands[i]);
                        const auto jumpTarget = nextOffsetBase + rel;
                        if (jumpTarget >= 0) {
                            decoded.successors.push_back(static_cast<std::uint32_t>(jumpTarget));
                        }
                    }
                }
            } else  {
                const auto rel = static_cast<std::int32_t>(decoded.inst.operands.back());
                const auto jumpTarget = static_cast<std::int64_t>(offset + decoded.inst.sizeBytes) + rel + -4;
                if (jumpTarget >= 0) {
                    decoded.successors.push_back(static_cast<std::uint32_t>(jumpTarget));
                }
                if (opcode == 0) {
                    decoded.successors.push_back(offset + decoded.inst.sizeBytes);
                }
            }
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

void fillUnknownRegions(
    std::unordered_map<std::uint32_t, std::uint32_t> visitedRegions, std::span<const std::uint8_t> sectionBytes, SctSection& section) 
{
    std::uint32_t cursor = 0;
    while (cursor < sectionBytes.size()) {
        const bool visited = visitedRegions.contains(cursor);
        if (visited) {
            cursor += visitedRegions[cursor];
            continue;
        }

        const auto start = cursor;
        while (cursor < sectionBytes.size() && !visitedRegions.contains(cursor)) {
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
    std::cout << "[SoaSimSCT] Step 1/5: Starting parse (" << bytes.size() << " bytes).\n";
    SctParseResult result{};
    result.file.sourcePath = std::move(sourcePath);

    std::vector<std::uint8_t> decoded;
    std::span<const std::uint8_t> payload = bytes;
    if (soasim::compression::aklz::isAklz(bytes)) {
        std::cout << "[SoaSimSCT] Step 2/5: Input is AKLZ-compressed, decompressing...\n";
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
    else {
        std::cout << "[SoaSimSCT] Step 2/5: Input is not AKLZ-compressed.\n";
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
    std::cout << "[SoaSimSCT] Step 3/5: Reading section index (" << sectionCount << " sections)...\n";

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

    std::cout << "[SoaSimSCT] Step 4/5: Walking section instructions...\n";
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

        if (isStringSection(sectionBytes, indexEndian)) {
            section.isStringSection = true;
            section.heuristicEvidence.notes.push_back(
                "Detected string section from SALSA-style pattern (9 ... 0x1d followed by non-opcode payload); skipped decode.");
            result.file.sections.push_back(std::move(section));
            continue;
        }

        // Control-flow guided pass starting at section offset 0.
        std::deque<std::uint32_t> worklist;
        std::set<std::uint32_t> enqueued;
        std::unordered_map<std::uint32_t, std::uint32_t> visited;
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

                std::vector<SctDiagnostic> inst_diagnostics{};
                auto decoded = decodeInstruction(sectionBytes, cursor, indexEndian, inst_diagnostics);
                if (decoded.inst.sizeBytes == 0) {
                    break;
                }
                for (auto& diag : inst_diagnostics) {
                    diag.section = section.id.name;
                }
                
                result.diagnostics.insert(result.diagnostics.end(), inst_diagnostics.begin(), inst_diagnostics.end());

                instructionByOffset[cursor] = section.instructions.size();
                visited.try_emplace(cursor, decoded.inst.sizeBytes);
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

                if (!decoded.successors.empty()) {
                    break;
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
    std::cout << "[SoaSimSCT] Step 5/5: Parse complete. ParsedSections="
              << result.file.sections.size()
              << ", diagnostics=" << result.diagnostics.size() << ".\n";
    result.diagnostics.push_back(
        {"Initial SCT parser pass completed (index + control-flow-guided section walk with placeholder opcode semantics).", 0});
    return result;
}

} // namespace soasim::sct
