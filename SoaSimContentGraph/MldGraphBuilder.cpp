#include "MldGraphBuilder.h"

#include "ContentGraphIds.h"

#include <sstream>

namespace soasim::contentgraph {
namespace {

std::string joinU32(const std::vector<std::uint32_t>& values) {
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        out << values[i];
    }
    return out.str();
}

std::string joinStrings(const std::vector<std::string>& values) {
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        out << values[i];
    }
    return out.str();
}

} // namespace

void MldGraphBuilder::addToGraph(ContentGraph& graph, const std::string& sourcePath,
    const soasim::mld::parsing::ParseResult& parseResult) const {
    const auto fileId = mldFileNodeId(sourcePath);
    ContentNode file{};
    file.id = fileId;
    file.type = ContentNodeType::MldFile;
    file.label = filenameLabel(sourcePath);
    file.sourcePath = sourcePath;
    file.attributes.emplace("entry_count", std::to_string(parseResult.entryList.size()));
    graph.addNode(std::move(file));

    for (const auto& entry : parseResult.entryList) {
        const auto entryId = mldEntryNodeId(sourcePath, entry.tableIndex, entry.entryId, entry.tblId);
        ContentNode node{};
        node.id = entryId;
        node.type = ContentNodeType::MldEntry;
        node.label = entry.fxnName.empty()
            ? "entry " + std::to_string(entry.entryId)
            : entry.fxnName + " table " + std::to_string(entry.tblId);
        node.sourcePath = sourcePath;
        node.attributes.emplace("table_index", std::to_string(entry.tableIndex));
        node.attributes.emplace("entryID", std::to_string(entry.entryId));
        node.attributes.emplace("tableID", std::to_string(entry.tblId));
        node.attributes.emplace("function", entry.fxnName);
        node.attributes.emplace("object_count", std::to_string(entry.objectCount));
        node.attributes.emplace("ground_count", std::to_string(entry.groundCount));
        node.attributes.emplace("motion_count", std::to_string(entry.motionCount));
        node.attributes.emplace("texture_count", std::to_string(entry.textureCount));
        node.attributes.emplace("textures_pointer", std::to_string(entry.texturesPointer));
        node.attributes.emplace("ground_links", joinU32(entry.groundLinks));
        node.attributes.emplace("param_list2", joinU32(entry.paramList2));
        node.attributes.emplace("function_parameters", joinU32(entry.functionParameters));
        node.attributes.emplace("object_addresses", joinU32(entry.objectAddresses));
        node.attributes.emplace("ground_addresses", joinU32(entry.groundAddresses));
        node.attributes.emplace("motion_addresses", joinU32(entry.motionAddresses));
        node.attributes.emplace("texture_names", joinStrings(entry.textureNames));
        graph.addNode(std::move(node));

        ContentEdge contains{};
        contains.from = fileId;
        contains.to = entryId;
        contains.type = ContentEdgeType::Contains;
        graph.addEdge(std::move(contains));
    }
}

} // namespace soasim::contentgraph
