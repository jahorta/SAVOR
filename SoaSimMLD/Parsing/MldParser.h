#pragma once

#include "../Model/BlenderIrModel.h"
#include "../Model/NjcmModel.h"
#include "../Model/NjtlModel.h"
#include "../Model/MldTextureArchiveModel.h"
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

struct NjcmChunkSummary {
    std::size_t chunkOffset = 0;
    std::size_t chunkDataSize = 0;
    bool chunkSizeLittleEndian = true;
    bool payloadLittleEndian = true;
    std::uint32_t imageBase = 0;
    bool usedPof0Fixup = false;
    std::size_t objectCount = 0;
    std::size_t attachCount = 0;
    std::size_t vertexChunkCount = 0;
    std::size_t polyChunkCount = 0;
    std::size_t decodedVertexCount = 0;
    std::size_t decodedTriangleCount = 0;
    std::size_t score = 0;
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

struct ExtractedNjBlock {
    enum class Kind {
        Object,
        Motion,
    };

    Kind kind = Kind::Object;
    std::uint32_t offset = 0;
    std::size_t size = 0;
    bool includesNjtlPrefix = false;
    std::vector<std::uint8_t> bytes{};
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
    std::vector<ExtractedNjBlock> extractedNjBlocks{};
    std::optional<model::MldTextureArchive> textureArchive{};
    std::optional<model::BlenderIrScene> blenderIrScene{};
    std::vector<std::string> blenderIrDiagnostics{};
    std::vector<std::string> blenderIrArtifactPaths{};
};

class MldParser {
public:
    MldParser() = default;

    [[nodiscard]] ParseResult parse(std::span<const std::uint8_t> mldBytes,
        const ParseOptions& options = {}) const;

    [[nodiscard]] std::vector<ExtractedNjBlock> extractNjBlocks(
        std::span<const std::uint8_t> mldBytes,
        const ParseOptions& options = {}) const;
};

[[nodiscard]] std::string formatParseSummary(const ParseResult& parseResult);

} // namespace soasim::mld::parsing
