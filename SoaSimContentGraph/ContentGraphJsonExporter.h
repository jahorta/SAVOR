#pragma once

#include "ContentGraph.h"

#include <string>

namespace soasim::contentgraph {

class ContentGraphJsonExporter {
public:
    [[nodiscard]] std::string toJson(const ContentGraph& graph,
        ContentGraphProjection projection = ContentGraphProjection::Full) const;
};

} // namespace soasim::contentgraph
