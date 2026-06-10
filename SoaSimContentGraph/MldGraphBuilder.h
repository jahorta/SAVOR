#pragma once

#include "ContentGraph.h"

#include "../SoaSimMLD/Parsing/MldParser.h"

#include <string>

namespace soasim::contentgraph {

class MldGraphBuilder {
public:
    void addToGraph(ContentGraph& graph, const std::string& sourcePath,
        const soasim::mld::parsing::ParseResult& parseResult) const;
};

} // namespace soasim::contentgraph
