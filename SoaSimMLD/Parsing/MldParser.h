#pragma once

#include "NJCMParser.h"
#include "../Model/SearchWorldModel.h"
#include "../Model/WorldModel.h"

#include <cstddef>
#include <cstdint>
#include <span>
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
};

struct ParseResult {
    model::WorldModel world{};
    model::SearchWorldModel searchWorld{};
    std::vector<ParseDiagnostic> diagnostics{};
    std::vector<std::pair<std::string, std::size_t>> fxnHistogram{};
    std::vector<std::pair<std::string, std::size_t>> chunkTypeHistogram{};
    std::vector<NjcmChunkSummary> njcmChunks{};
};

class MldParser {
public:
    MldParser() = default;

    [[nodiscard]] ParseResult parse(std::span<const std::uint8_t> mldBytes,
        const ParseOptions& options = {}) const;
};

[[nodiscard]] std::string formatParseSummary(const ParseResult& parseResult);

} // namespace soasim::mld::parsing
