#pragma once

#include "ContentGraphTypes.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace soasim::contentgraph {

class ContentGraph {
public:
    ContentNode& addNode(ContentNode node);
    ContentEdge& addEdge(ContentEdge edge);

    [[nodiscard]] const ContentNode* findNode(const std::string& id) const;
    [[nodiscard]] ContentNode* findNode(const std::string& id);
    [[nodiscard]] bool hasNode(const std::string& id) const;

    [[nodiscard]] const std::vector<ContentNode>& nodes() const { return nodes_; }
    [[nodiscard]] const std::vector<ContentEdge>& edges() const { return edges_; }

private:
    std::vector<ContentNode> nodes_{};
    std::vector<ContentEdge> edges_{};
    std::unordered_map<std::string, std::size_t> nodeIndexById_{};
};

} // namespace soasim::contentgraph
