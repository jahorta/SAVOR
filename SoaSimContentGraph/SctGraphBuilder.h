#pragma once

#include "ContentGraph.h"

#include "../SoaSimSCT/SctModel.h"

namespace soasim::contentgraph {

struct SctGraphBuildOptions {
    ContentGraphDetailLevel detailLevel = ContentGraphDetailLevel::Instructions;
    bool includeResourceEdges = true;
    bool includeFlagEdges = true;
};

class SctGraphBuilder {
public:
    void addToGraph(ContentGraph& graph, const soasim::sct::SctParseResult& parseResult,
        const SctGraphBuildOptions& options = {}) const;
};

} // namespace soasim::contentgraph
