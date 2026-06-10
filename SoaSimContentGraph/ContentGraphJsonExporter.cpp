#include "ContentGraphJsonExporter.h"

#include <set>
#include <sstream>

namespace soasim::contentgraph {
namespace {

std::string jsonEscape(const std::string& value) {
    std::string escaped{};
    escaped.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped.push_back(c);
            break;
        }
    }
    return escaped;
}

bool includeNode(ContentNodeType type, ContentGraphProjection projection) {
    if (projection == ContentGraphProjection::Full) {
        return true;
    }
    if (projection == ContentGraphProjection::Sections) {
        return type != ContentNodeType::BasicBlock && type != ContentNodeType::Instruction;
    }
    return type == ContentNodeType::ScriptSection || type == ContentNodeType::MldEntry
        || type == ContentNodeType::ResourceRef || type == ContentNodeType::FlagRef
        || type == ContentNodeType::UnknownTarget;
}

void writeAttributes(std::ostream& out, const ContentAttributes& attributes, int indent) {
    const std::string pad(static_cast<std::size_t>(indent), ' ');
    out << "{";
    if (!attributes.empty()) {
        out << "\n";
    }
    std::size_t index = 0;
    for (const auto& [key, value] : attributes) {
        out << pad << "  \"" << jsonEscape(key) << "\": \"" << jsonEscape(value) << "\"";
        if (++index < attributes.size()) {
            out << ",";
        }
        out << "\n";
    }
    if (!attributes.empty()) {
        out << pad;
    }
    out << "}";
}

void writeEvidence(std::ostream& out, const std::vector<ContentEvidence>& evidence, int indent) {
    const std::string pad(static_cast<std::size_t>(indent), ' ');
    out << "[";
    if (!evidence.empty()) {
        out << "\n";
    }
    for (std::size_t i = 0; i < evidence.size(); ++i) {
        const auto& item = evidence[i];
        out << pad << "  {\n";
        out << pad << "    \"sourcePath\": \"" << jsonEscape(item.sourcePath) << "\",\n";
        out << pad << "    \"sectionName\": \"" << jsonEscape(item.sectionName) << "\",\n";
        out << pad << "    \"instructionOffset\": ";
        if (item.instructionOffset.has_value()) {
            out << *item.instructionOffset;
        } else {
            out << "null";
        }
        out << ",\n";
        out << pad << "    \"opcode\": ";
        if (item.opcode.has_value()) {
            out << *item.opcode;
        } else {
            out << "null";
        }
        out << ",\n";
        out << pad << "    \"detail\": \"" << jsonEscape(item.detail) << "\",\n";
        out << pad << "    \"attributes\": ";
        writeAttributes(out, item.attributes, indent + 4);
        out << "\n";
        out << pad << "  }";
        if (i + 1 < evidence.size()) {
            out << ",";
        }
        out << "\n";
    }
    if (!evidence.empty()) {
        out << pad;
    }
    out << "]";
}

} // namespace

std::string ContentGraphJsonExporter::toJson(const ContentGraph& graph, ContentGraphProjection projection) const {
    std::set<std::string> includedNodeIds{};
    for (const auto& node : graph.nodes()) {
        if (includeNode(node.type, projection)) {
            includedNodeIds.insert(node.id);
        }
    }

    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"soasim_content_graph_v1\",\n";
    out << "  \"projection\": \"" << toString(projection) << "\",\n";
    out << "  \"nodes\": [\n";
    std::size_t writtenNodes = 0;
    for (const auto& node : graph.nodes()) {
        if (!includedNodeIds.contains(node.id)) {
            continue;
        }
        if (writtenNodes++ > 0) {
            out << ",\n";
        }
        out << "    {\n";
        out << "      \"id\": \"" << jsonEscape(node.id) << "\",\n";
        out << "      \"type\": \"" << toString(node.type) << "\",\n";
        out << "      \"label\": \"" << jsonEscape(node.label) << "\",\n";
        out << "      \"sourcePath\": \"" << jsonEscape(node.sourcePath) << "\",\n";
        out << "      \"offset\": ";
        if (node.offset.has_value()) {
            out << *node.offset;
        } else {
            out << "null";
        }
        out << ",\n";
        out << "      \"size\": ";
        if (node.size.has_value()) {
            out << *node.size;
        } else {
            out << "null";
        }
        out << ",\n";
        out << "      \"attributes\": ";
        writeAttributes(out, node.attributes, 6);
        out << "\n";
        out << "    }";
    }
    out << "\n  ],\n";
    out << "  \"edges\": [\n";
    std::size_t writtenEdges = 0;
    for (const auto& edge : graph.edges()) {
        if (!includedNodeIds.contains(edge.from) || !includedNodeIds.contains(edge.to)) {
            continue;
        }
        if (writtenEdges++ > 0) {
            out << ",\n";
        }
        out << "    {\n";
        out << "      \"id\": \"" << jsonEscape(edge.id) << "\",\n";
        out << "      \"from\": \"" << jsonEscape(edge.from) << "\",\n";
        out << "      \"to\": \"" << jsonEscape(edge.to) << "\",\n";
        out << "      \"type\": \"" << toString(edge.type) << "\",\n";
        out << "      \"confidence\": \"" << toString(edge.confidence) << "\",\n";
        out << "      \"attributes\": ";
        writeAttributes(out, edge.attributes, 6);
        out << ",\n";
        out << "      \"evidence\": ";
        writeEvidence(out, edge.evidence, 6);
        out << "\n";
        out << "    }";
    }
    out << "\n  ]\n";
    out << "}\n";
    return out.str();
}

} // namespace soasim::contentgraph
