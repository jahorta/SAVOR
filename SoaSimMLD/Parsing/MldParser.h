#pragma once

#include "NJCMParser.h"
#include "../Model/BlenderIrModel.h"
#include "../Model/NjcmModel.h"
#include "../Model/NjtlModel.h"
#include "../Model/SearchWorldModel.h"
#include "../Model/WorldModel.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace soasim::mld::parsing {

struct ParseDiagnostic {
    enum class Severity {
        Info,
        Warning,
        Error,
    };

    Severity severity = Severity::Info;
    std::string message{};
};

struct CoordinatePolicy {
    bool swapYZ = false;
    bool negateX = false;
    bool negateY = false;
    bool negateZ = false;
    float uniformScale = 1.0f;
    bool reverseTriangleWinding = false;
    bool transposeMatrices = false;
};

struct ParseOptions {
    CoordinatePolicy coordinates{};
    bool preserveUnknownEntries = true;
    bool emitFxnHistogram = true;
    NjcmParsePolicy njcmPolicy{ .useSaToolsParityPath = true };
    std::string filterFxnName{};
    std::vector<std::uint32_t> filterEntryIdList{};
    bool buildBlenderIntermediateIr = true;
    bool exportBlenderIrJson = false;
    std::string blenderIrOutputDir{};
};

struct DecodedObjectChunkRange {
    std::uint32_t objectAddress = 0;
    std::size_t decodedChunkBegin = 0;
    std::size_t decodedChunkEnd = 0;
};

struct ParsedRawEntry {
    std::uint32_t sourceEntryId = 0;
    std::string fxnName{};
    std::uint32_t tblId = 0;
    model::Transform transform{};
    std::vector<std::uint32_t> objectAddresses{};
    std::vector<std::uint8_t> payload{};
};

struct ParseResult {
    model::WorldModel world{};
    model::SearchWorldModel searchWorld{};
    std::vector<ParsedRawEntry> rawEntries{};
    std::vector<ParseDiagnostic> diagnostics{};
    std::vector<std::pair<std::string, std::size_t>> fxnHistogram{};
    std::vector<std::pair<std::string, std::size_t>> chunkTypeHistogram{};
    std::vector<NjcmChunkSummary> njcmChunks{};
    std::vector<model::NjcmDecodedChunk> decodedNjcmChunks{};
    std::vector<model::NjObjectBlockModel> decodedNjObjectBlocks{};
    std::vector<DecodedObjectChunkRange> decodedObjectChunkRanges{};
    std::optional<model::BlenderIrScene> blenderIrScene{};
    std::vector<std::string> blenderIrDiagnostics{};
    std::vector<std::string> blenderIrArtifactPaths{};
};

class MldParser {
public:
    MldParser() = default;

    [[nodiscard]] ParseResult parse(std::span<const std::uint8_t> mldBytes,
        const ParseOptions& options = {}) const;
};

[[nodiscard]] std::string formatParseSummary(const ParseResult& parseResult);

} // namespace soasim::mld::parsing
