#include "../SoaSimContentGraph/SoaSimContentGraph.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>

namespace {

using soasim::contentgraph::ContentEdge;
using soasim::contentgraph::ContentEdgeType;
using soasim::contentgraph::ContentGraph;
using soasim::contentgraph::ContentGraphCorpusBuilder;
using soasim::contentgraph::ContentGraphCorpusBuildOptions;
using soasim::contentgraph::ContentGraphCorpusInput;
using soasim::contentgraph::ContentGraphDetailLevel;
using soasim::contentgraph::ContentGraphJsonExporter;
using soasim::contentgraph::ContentGraphProjection;
using soasim::contentgraph::ContentNodeType;
using soasim::contentgraph::mldEntryNodeId;
using soasim::contentgraph::mldFileNodeId;
using soasim::contentgraph::scriptFileNodeId;
using soasim::contentgraph::scriptSectionNodeId;

constexpr const char* kScriptPath = "C:/fixtures/me017b.sct";
constexpr const char* kMldPath = "C:/fixtures/a017b.mld";

soasim::sct::SctInstruction instruction(std::uint32_t offset,
    std::uint16_t opcode,
    std::initializer_list<std::uint32_t> operands,
    std::uint32_t sizeBytes)
{
    soasim::sct::SctInstruction result{};
    result.offset = offset;
    result.opcode = opcode;
    result.operands = operands;
    result.sizeBytes = sizeBytes;
    result.decodeOk = true;
    return result;
}

soasim::sct::SctParseResult makeScriptParseResult()
{
    soasim::sct::SctSection section{};
    section.id.index = 0;
    section.id.name = "M04999";
    section.startOffset = 0;
    section.endOffset = 96;
    section.instructions = {
        instruction(0, 9, {}, 16),
        instruction(16, 0, { 60 }, 16),
        instruction(32, 10, { 60 }, 8),
        instruction(40, 3, { 60, 72 }, 20),
        instruction(60, 11, { 4999 }, 8),
        instruction(72, 12, {}, 4),
        instruction(80, 23, { 17 }, 8),
    };

    section.blocks = {
        soasim::sct::SctBasicBlock{ .startOffset = 0, .endOffset = 32, .instructionOffsets = { 0, 16 }, .successorOffsets = { 60, 32 } },
        soasim::sct::SctBasicBlock{ .startOffset = 32, .endOffset = 40, .instructionOffsets = { 32 }, .successorOffsets = { 60 } },
        soasim::sct::SctBasicBlock{ .startOffset = 40, .endOffset = 60, .instructionOffsets = { 40 }, .successorOffsets = { 60, 72 } },
        soasim::sct::SctBasicBlock{ .startOffset = 60, .endOffset = 80, .instructionOffsets = { 60, 72 }, .successorOffsets = {} },
        soasim::sct::SctBasicBlock{ .startOffset = 80, .endOffset = 88, .instructionOffsets = { 80 }, .successorOffsets = {} },
    };

    section.flagSummary.flagsRead = { 2815 };
    section.flagSummary.flagsWritten = { 2816 };
    section.flagSummary.flagsTested = { 2815 };

    soasim::sct::SctParseResult result{};
    result.file.sourcePath = kScriptPath;
    result.file.sections.push_back(std::move(section));
    result.parseOk = true;
    return result;
}

soasim::mld::parsing::ParseResult makeMldParseResult()
{
    soasim::mld::parsing::ParsedEntryListItem entry{};
    entry.tableIndex = 7;
    entry.entryId = 42;
    entry.tblId = 4999;
    entry.fxnName = "esahaiti";
    entry.objectCount = 1;
    entry.textureCount = 1;
    entry.functionParameters = { 1, 2, 3, 4 };
    entry.paramList2 = { 5, 6 };
    entry.textureNames = { "example" };

    soasim::mld::parsing::ParseResult result{};
    result.entryList.push_back(std::move(entry));
    return result;
}

ContentGraph buildFixtureGraph()
{
    ContentGraphCorpusInput input{};
    input.sctFiles.push_back({ kScriptPath, makeScriptParseResult() });
    input.mldFiles.push_back({ kMldPath, makeMldParseResult() });

    ContentGraphCorpusBuildOptions options{};
    options.sctOptions.detailLevel = ContentGraphDetailLevel::Instructions;
    return ContentGraphCorpusBuilder{}.build(input, options);
}

bool hasEdge(const ContentGraph& graph, ContentEdgeType type)
{
    return std::any_of(graph.edges().begin(), graph.edges().end(),
        [type](const ContentEdge& edge) { return edge.type == type; });
}

bool hasEdge(const ContentGraph& graph,
    const std::string& from,
    const std::string& to,
    ContentEdgeType type)
{
    return std::any_of(graph.edges().begin(), graph.edges().end(),
        [&](const ContentEdge& edge) {
            return edge.from == from && edge.to == to && edge.type == type;
        });
}

} // namespace

TEST(ContentGraph, PairsMatchingSctAndMldFilesAndDispatchesTableSection)
{
    const ContentGraph graph = buildFixtureGraph();

    const std::string scriptFileId = scriptFileNodeId(kScriptPath);
    const std::string mldFileId = mldFileNodeId(kMldPath);
    const std::string sectionId = scriptSectionNodeId(kScriptPath, "M04999");
    const std::string entryId = mldEntryNodeId(kMldPath, 7, 42, 4999);

    EXPECT_TRUE(graph.hasNode(scriptFileId));
    EXPECT_TRUE(graph.hasNode(mldFileId));
    EXPECT_TRUE(graph.hasNode(sectionId));
    ASSERT_TRUE(graph.hasNode(entryId));

    const auto* entryNode = graph.findNode(entryId);
    ASSERT_NE(nullptr, entryNode);
    EXPECT_EQ(ContentNodeType::MldEntry, entryNode->type);
    EXPECT_EQ("esahaiti", entryNode->attributes.at("function"));
    EXPECT_EQ("4999", entryNode->attributes.at("tableID"));

    EXPECT_TRUE(hasEdge(graph, scriptFileId, mldFileId, ContentEdgeType::PairedWith));
    EXPECT_TRUE(hasEdge(graph, entryId, sectionId, ContentEdgeType::MldEntryDispatchesSection));
}

TEST(ContentGraph, ClassifiesSctControlFlowEdges)
{
    const ContentGraph graph = buildFixtureGraph();

    EXPECT_TRUE(hasEdge(graph, ContentEdgeType::BranchFalse));
    EXPECT_TRUE(hasEdge(graph, ContentEdgeType::BranchTrue));
    EXPECT_TRUE(hasEdge(graph, ContentEdgeType::SwitchCase));
    EXPECT_TRUE(hasEdge(graph, ContentEdgeType::Jump));
    EXPECT_TRUE(hasEdge(graph, ContentEdgeType::CallSubscript));
    EXPECT_TRUE(hasEdge(graph, ContentEdgeType::Return));
}

TEST(ContentGraph, EmitsUnresolvedResourceAndFlagEdges)
{
    const ContentGraph graph = buildFixtureGraph();

    EXPECT_TRUE(hasEdge(graph, ContentEdgeType::LoadsMld));
    EXPECT_TRUE(hasEdge(graph, ContentEdgeType::ReferencesFlag));
    EXPECT_TRUE(hasEdge(graph, ContentEdgeType::WritesFlag));

    EXPECT_TRUE(std::any_of(graph.nodes().begin(), graph.nodes().end(),
        [](const auto& node) { return node.type == ContentNodeType::ResourceRef; }));
}

TEST(ContentGraph, SectionsProjectionOmitsInstructionNodes)
{
    const ContentGraph graph = buildFixtureGraph();
    const std::string json = ContentGraphJsonExporter{}.toJson(graph, ContentGraphProjection::Sections);

    EXPECT_NE(std::string::npos, json.find("\"type\": \"ScriptSection\""));
    EXPECT_EQ(std::string::npos, json.find("\"type\": \"Instruction\""));
    EXPECT_EQ(std::string::npos, json.find("\"type\": \"BasicBlock\""));
}
