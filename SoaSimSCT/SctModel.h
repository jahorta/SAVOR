#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace soasim::sct {

struct SctSectionId {
    std::uint32_t index = 0;
    std::string name;
};

struct SctInstruction {
    struct ScptTraceEntry {
        std::uint32_t rawWord = 0;
        std::string interpretedValue;
    };

    struct ScptParameterValueRecord {
        std::uint8_t parameterIndex = 0;
        std::uint32_t operandStartWordIndex = 0;
        std::uint32_t operandWordCount = 0;
        bool hitStopCode = false;
        std::string resolvedValue;
        std::vector<ScptTraceEntry> evaluationTrace;
    };

    std::uint32_t offset = 0;
    std::uint16_t opcode = 0;
    std::vector<std::uint32_t> operands;
    std::vector<std::uint8_t> scptAnalyzeOperandIndexes;
    std::vector<ScptParameterValueRecord> scptParameterValueRecords;
    std::uint32_t sizeBytes = 0;
    bool decodeOk = false;
};

struct SctBasicBlock {
    std::uint32_t startOffset = 0;
    std::uint32_t endOffset = 0;
    std::vector<std::uint32_t> instructionOffsets;
    std::vector<std::uint32_t> successorOffsets;
};

struct SctUnknownRegion {
    std::uint32_t startOffset = 0;
    std::uint32_t endOffset = 0;
    std::vector<std::uint8_t> rawBytes;
    std::string reason;
};

struct FlagAccessSummary {
    std::vector<std::uint32_t> flagsRead;
    std::vector<std::uint32_t> flagsWritten;
    std::vector<std::uint32_t> flagsTested;
};

struct SectionHeuristicEvidence {
    bool touchesFlags = false;
    bool branchesOnFlags = false;
    bool writesFlags = false;
    bool hasSwitch = false;
    bool hasLongLinearSequence = false;
    bool hasPlayerReposition = false;
    bool hasCameraOrTimingLikeOps = false;
    bool likelyTrigger = false;
    bool likelyCutscene = false;
    std::vector<std::string> notes;
};

struct SctSection {
    SctSectionId id;
    std::uint32_t startOffset = 0;
    std::uint32_t endOffset = 0;
    bool isStringSection = false;
    std::vector<SctInstruction> instructions;
    std::vector<SctBasicBlock> blocks;
    std::vector<SctUnknownRegion> unknownRegions;
    FlagAccessSummary flagSummary;
    SectionHeuristicEvidence heuristicEvidence;
};

struct SctFile {
    std::string sourcePath;
    std::vector<SctSection> sections;
};

struct SctDiagnostic {
    std::string message;
    std::uint32_t offset = 0;
    std::string section = {};
};

struct SctParseResult {
    SctFile file;
    std::vector<SctDiagnostic> diagnostics;
    bool parseOk = false;
};

} // namespace soasim::sct
