#pragma once

#include "../Model/BlenderIrModel.h"
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
    bool entryListOnly = false;
    bool buildBlenderIntermediateIr = true;
    bool exportBlenderIrJson = false;
    bool extractGrndGobjBlocks = false;
    std::string blenderIrOutputDir{};
};

struct ParsedEntryListItem {
    std::size_t tableIndex = 0;
    std::uint32_t entryId = 0;
    std::uint32_t tblId = 0;
    std::string fxnName{};
    std::size_t objectCount = 0;
    std::size_t groundCount = 0;
    std::size_t motionCount = 0;
    std::size_t textureCount = 0;
    std::uint32_t texturesPointer = 0;
    std::vector<std::uint32_t> groundLinks{};
    std::vector<std::uint32_t> paramList2{};
    std::vector<std::uint32_t> functionParameters{};
    std::vector<std::uint32_t> objectAddresses{};
    std::vector<std::uint32_t> groundAddresses{};
    std::vector<std::uint32_t> motionAddresses{};
    std::vector<std::string> textureNames{};
};

struct ParsedRawEntry {
    std::uint32_t sourceEntryId = 0;
    std::string fxnName{};
    std::uint32_t tblId = 0;
    model::Transform transform{};
    std::vector<std::uint32_t> objectAddresses{};
    std::vector<std::uint32_t> groundAddresses{};
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

struct BlockOwnerRef {
    std::uint32_t sourceEntryId = 0;
    std::size_t tableIndex = 0;
    std::string fxnName{};
    std::string role{};
};

struct ExtractedMldSpatialBlock {
    enum class Kind {
        Grnd,
        Gobj,
        UnknownGround,
        UnknownObject,
    };

    Kind kind = Kind::UnknownObject;
    std::uint32_t offset = 0;
    std::size_t size = 0;
    std::string tag{};
    std::string sizeSource{};
    std::vector<BlockOwnerRef> owners{};
    std::vector<std::pair<std::string, std::string>> headerProbe{};
    std::vector<std::uint8_t> bytes{};
};

struct ParseResult {
    model::WorldModel world{};
    model::SearchWorldModel searchWorld{};
    std::vector<ParsedEntryListItem> entryList{};
    std::vector<ParsedRawEntry> rawEntries{};
    std::vector<ParseDiagnostic> diagnostics{};
    std::vector<std::pair<std::string, std::size_t>> fxnHistogram{};
    std::vector<std::pair<std::string, std::size_t>> chunkTypeHistogram{};
    std::vector<ExtractedNjBlock> extractedNjBlocks{};
    std::vector<ExtractedMldSpatialBlock> extractedSpatialBlocks{};
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

    [[nodiscard]] std::vector<ExtractedMldSpatialBlock> extractGrndGobjBlocks(
        std::span<const std::uint8_t> mldBytes,
        const ParseOptions& options = {}) const;
};

[[nodiscard]] std::string formatParseSummary(const ParseResult& parseResult);

} // namespace soasim::mld::parsing
