#include "ContentGraph.h"

#include <algorithm>

namespace soasim::contentgraph {

ContentNode& ContentGraph::addNode(ContentNode node) {
    if (auto existing = findNode(node.id)) {
        if (existing->label.empty()) {
            existing->label = std::move(node.label);
        }
        if (existing->sourcePath.empty()) {
            existing->sourcePath = std::move(node.sourcePath);
        }
        if (!existing->offset.has_value()) {
            existing->offset = node.offset;
        }
        if (!existing->size.has_value()) {
            existing->size = node.size;
        }
        for (const auto& [key, value] : node.attributes) {
            existing->attributes.try_emplace(key, value);
        }
        return *existing;
    }

    const auto index = nodes_.size();
    nodeIndexById_.emplace(node.id, index);
    nodes_.push_back(std::move(node));
    return nodes_.back();
}

ContentEdge& ContentGraph::addEdge(ContentEdge edge) {
    if (edge.id.empty()) {
        edge.id = edge.from + "|" + toString(edge.type) + "|" + edge.to + "|" + std::to_string(edges_.size());
    }
    edges_.push_back(std::move(edge));
    return edges_.back();
}

const ContentNode* ContentGraph::findNode(const std::string& id) const {
    const auto it = nodeIndexById_.find(id);
    if (it == nodeIndexById_.end()) {
        return nullptr;
    }
    return &nodes_[it->second];
}

ContentNode* ContentGraph::findNode(const std::string& id) {
    const auto it = nodeIndexById_.find(id);
    if (it == nodeIndexById_.end()) {
        return nullptr;
    }
    return &nodes_[it->second];
}

bool ContentGraph::hasNode(const std::string& id) const {
    return nodeIndexById_.contains(id);
}

} // namespace soasim::contentgraph
