#pragma once

#include "ContentGraph.h"
#include "MldGraphBuilder.h"
#include "SctGraphBuilder.h"

#include <string>
#include <vector>

namespace soasim::contentgraph {

struct ParsedSctGraphInput {
    std::string sourcePath{};
    soasim::sct::SctParseResult parseResult{};
};

struct ParsedMldGraphInput {
    std::string sourcePath{};
    soasim::mld::parsing::ParseResult parseResult{};
};

struct ContentGraphCorpusBuildOptions {
    SctGraphBuildOptions sctOptions{};
};

struct ContentGraphCorpusInput {
    std::vector<ParsedSctGraphInput> sctFiles{};
    std::vector<ParsedMldGraphInput> mldFiles{};
};

class ContentGraphCorpusBuilder {
public:
    [[nodiscard]] ContentGraph build(const ContentGraphCorpusInput& input,
        const ContentGraphCorpusBuildOptions& options = {}) const;
};

} // namespace soasim::contentgraph
