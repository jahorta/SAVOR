#include "SctGraphBuilder.h"

#include "ContentGraphIds.h"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>

namespace soasim::contentgraph {
namespace {

std::string decimalString(std::uint32_t value) {
    return std::to_string(value);
}

std::string hexString(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

ContentEvidence instructionEvidence(
    const std::string& sourcePath,
    const std::string& sectionName,
    const soasim::sct::SctInstruction& instruction,
    std::string detail) {
    ContentEvidence evidence{};
    evidence.sourcePath = sourcePath;
    evidence.sectionName = sectionName;
    evidence.instructionOffset = instruction.offset;
    evidence.opcode = instruction.opcode;
    evidence.detail = std::move(detail);
    return evidence;
}

const soasim::sct::SctInstruction* instructionAt(
    const soasim::sct::SctSection& section,
    std::uint32_t offset) {
    const auto it = std::find_if(section.instructions.begin(), section.instructions.end(), [offset](const auto& instruction) {
        return instruction.offset == offset;
    });
    return it == section.instructions.end() ? nullptr : &*it;
}

ContentEdgeType edgeTypeForSuccessor(
    const soasim::sct::SctInstruction* lastInstruction,
    std::size_t successorIndex,
    std::uint32_t successorOffset,
    std::uint32_t blockEndOffset) {
    if (lastInstruction == nullptr) {
        return successorOffset == blockEndOffset ? ContentEdgeType::Fallthrough : ContentEdgeType::Jump;
    }
    switch (lastInstruction->opcode) {
    case 0:
        return successorIndex == 0 ? ContentEdgeType::BranchFalse : ContentEdgeType::BranchTrue;
    case 3:
        return ContentEdgeType::SwitchCase;
    case 10:
        return ContentEdgeType::Jump;
    default:
        return successorOffset == blockEndOffset ? ContentEdgeType::Fallthrough : ContentEdgeType::Jump;
    }
}

bool isResourceLoadOpcode(std::uint16_t opcode) {
    return opcode == 23 || opcode == 43 || opcode == 210 || opcode == 238 || opcode == 257;
}

ContentEdgeType resourceEdgeType(std::uint16_t opcode) {
    return opcode == 23 ? ContentEdgeType::LoadsMld : ContentEdgeType::LoadsScript;
}

std::string resourceKind(std::uint16_t opcode) {
    return opcode == 23 ? "mld" : "script";
}

void addResourceEdge(
    ContentGraph& graph,
    const std::string& sourcePath,
    const std::string& sectionName,
    const soasim::sct::SctInstruction& instruction,
    const std::string& sourceNodeId) {
    if (!isResourceLoadOpcode(instruction.opcode)) {
        return;
    }

    const auto operand = instruction.operands.empty() ? 0u : instruction.operands.front();
    const auto kind = resourceKind(instruction.opcode);
    const auto targetId = resourceRefNodeId(kind, sourcePath + ":" + sectionName + ":" + decimalString(instruction.offset)
        + ":opcode:" + decimalString(instruction.opcode));
    ContentNode target{};
    target.id = targetId;
    target.type = ContentNodeType::ResourceRef;
    target.label = std::string("unresolved ") + kind + " load";
    target.sourcePath = sourcePath;
    target.attributes.emplace("resource_kind", kind);
    target.attributes.emplace("opcode", decimalString(instruction.opcode));
    target.attributes.emplace("operand0", decimalString(operand));
    graph.addNode(std::move(target));

    ContentEdge edge{};
    edge.from = sourceNodeId;
    edge.to = targetId;
    edge.type = resourceEdgeType(instruction.opcode);
    edge.confidence = ContentConfidence::Unresolved;
    edge.attributes.emplace("opcode", decimalString(instruction.opcode));
    edge.attributes.emplace("operand0", decimalString(operand));
    edge.evidence.push_back(instructionEvidence(sourcePath, sectionName, instruction, "SCT resource-load opcode."));
    graph.addEdge(std::move(edge));
}

void addFlagEdges(
    ContentGraph& graph,
    const std::string& sourcePath,
    const std::string& sectionName,
    const std::string& sectionNodeId,
    const soasim::sct::FlagAccessSummary& flags) {
    auto addFlag = [&](std::uint32_t flag, ContentEdgeType edgeType) {
        const auto flagId = flagRefNodeId(flag);
        ContentNode node{};
        node.id = flagId;
        node.type = ContentNodeType::FlagRef;
        node.label = "BitVar " + std::to_string(flag);
        node.attributes.emplace("flag", std::to_string(flag));
        graph.addNode(std::move(node));

        ContentEdge edge{};
        edge.from = sectionNodeId;
        edge.to = flagId;
        edge.type = edgeType;
        edge.confidence = ContentConfidence::Known;
        edge.evidence.push_back(ContentEvidence{sourcePath, sectionName, std::nullopt, std::nullopt, "SCT flag summary.", {}});
        graph.addEdge(std::move(edge));
    };

    for (const auto flag : flags.flagsRead) {
        addFlag(flag, ContentEdgeType::ReferencesFlag);
    }
    for (const auto flag : flags.flagsTested) {
        addFlag(flag, ContentEdgeType::ReferencesFlag);
    }
    for (const auto flag : flags.flagsWritten) {
        addFlag(flag, ContentEdgeType::WritesFlag);
    }
}

void addSubscriptCallEdge(
    ContentGraph& graph,
    const std::string& sourcePath,
    const std::string& sectionName,
    const soasim::sct::SctInstruction& instruction,
    const std::string& sourceNodeId) {
    if (instruction.opcode != 11) {
        return;
    }

    const auto operand = instruction.operands.empty() ? 0u : instruction.operands.front();
    const auto targetId = unknownTargetNodeId(sourcePath, sectionName, instruction.offset, instruction.opcode);
    ContentNode target{};
    target.id = targetId;
    target.type = ContentNodeType::UnknownTarget;
    target.label = "subscript target";
    target.sourcePath = sourcePath;
    target.attributes.emplace("opcode", decimalString(instruction.opcode));
    target.attributes.emplace("offset_operand", decimalString(operand));
    graph.addNode(std::move(target));

    ContentEdge edge{};
    edge.from = sourceNodeId;
    edge.to = targetId;
    edge.type = ContentEdgeType::CallSubscript;
    edge.confidence = ContentConfidence::Heuristic;
    edge.attributes.emplace("offset_operand", decimalString(operand));
    edge.evidence.push_back(instructionEvidence(sourcePath, sectionName, instruction, "Opcode 11 loads a subscript and pushes a return position."));
    graph.addEdge(std::move(edge));
}

void addReturnEdge(
    ContentGraph& graph,
    const std::string& sourcePath,
    const std::string& sectionName,
    const soasim::sct::SctInstruction& instruction,
    const std::string& sourceNodeId) {
    if (instruction.opcode != 12) {
        return;
    }
    const auto targetId = unknownTargetNodeId(sourcePath, sectionName, instruction.offset, instruction.opcode);
    ContentNode target{};
    target.id = targetId;
    target.type = ContentNodeType::UnknownTarget;
    target.label = "return target";
    target.sourcePath = sourcePath;
    target.attributes.emplace("opcode", decimalString(instruction.opcode));
    graph.addNode(std::move(target));

    ContentEdge edge{};
    edge.from = sourceNodeId;
    edge.to = targetId;
    edge.type = ContentEdgeType::Return;
    edge.confidence = ContentConfidence::Known;
    edge.evidence.push_back(instructionEvidence(sourcePath, sectionName, instruction, "Opcode 12 returns from the current subscript stack."));
    graph.addEdge(std::move(edge));
}

} // namespace

void SctGraphBuilder::addToGraph(ContentGraph& graph, const soasim::sct::SctParseResult& parseResult,
    const SctGraphBuildOptions& options) const {
    const auto sourcePath = parseResult.file.sourcePath;
    const auto scriptFileId = scriptFileNodeId(sourcePath);

    ContentNode scriptFile{};
    scriptFile.id = scriptFileId;
    scriptFile.type = ContentNodeType::ScriptFile;
    scriptFile.label = filenameLabel(sourcePath);
    scriptFile.sourcePath = sourcePath;
    scriptFile.attributes.emplace("section_count", std::to_string(parseResult.file.sections.size()));
    graph.addNode(std::move(scriptFile));

    for (const auto& section : parseResult.file.sections) {
        const auto sectionId = scriptSectionNodeId(sourcePath, section.id.name);
        ContentNode sectionNode{};
        sectionNode.id = sectionId;
        sectionNode.type = ContentNodeType::ScriptSection;
        sectionNode.label = section.id.name;
        sectionNode.sourcePath = sourcePath;
        sectionNode.offset = section.startOffset;
        sectionNode.size = section.endOffset > section.startOffset ? section.endOffset - section.startOffset : 0u;
        sectionNode.attributes.emplace("section_index", std::to_string(section.id.index));
        sectionNode.attributes.emplace("is_string_section", section.isStringSection ? "true" : "false");
        graph.addNode(std::move(sectionNode));

        ContentEdge containsSection{};
        containsSection.from = scriptFileId;
        containsSection.to = sectionId;
        containsSection.type = ContentEdgeType::Contains;
        graph.addEdge(std::move(containsSection));

        if (options.includeFlagEdges) {
            addFlagEdges(graph, sourcePath, section.id.name, sectionId, section.flagSummary);
        }

        std::unordered_map<std::uint32_t, std::string> blockIdsByOffset{};
        if (options.detailLevel != ContentGraphDetailLevel::Sections) {
            for (const auto& block : section.blocks) {
                const auto blockId = basicBlockNodeId(sourcePath, section.id.name, block.startOffset);
                blockIdsByOffset.emplace(block.startOffset, blockId);
                ContentNode blockNode{};
                blockNode.id = blockId;
                blockNode.type = ContentNodeType::BasicBlock;
                blockNode.label = section.id.name + " block " + hexString(block.startOffset);
                blockNode.sourcePath = sourcePath;
                blockNode.offset = block.startOffset;
                blockNode.size = block.endOffset > block.startOffset ? block.endOffset - block.startOffset : 0u;
                graph.addNode(std::move(blockNode));

                ContentEdge containsBlock{};
                containsBlock.from = sectionId;
                containsBlock.to = blockId;
                containsBlock.type = ContentEdgeType::Contains;
                graph.addEdge(std::move(containsBlock));
            }
        }

        for (const auto& instruction : section.instructions) {
            const auto sourceNodeId = options.detailLevel == ContentGraphDetailLevel::Instructions
                ? instructionNodeId(sourcePath, section.id.name, instruction.offset)
                : sectionId;

            if (options.detailLevel == ContentGraphDetailLevel::Instructions) {
                ContentNode instructionNode{};
                instructionNode.id = sourceNodeId;
                instructionNode.type = ContentNodeType::Instruction;
                instructionNode.label = section.id.name + " opcode " + std::to_string(instruction.opcode)
                    + " @" + hexString(instruction.offset);
                instructionNode.sourcePath = sourcePath;
                instructionNode.offset = instruction.offset;
                instructionNode.size = instruction.sizeBytes;
                instructionNode.attributes.emplace("opcode", std::to_string(instruction.opcode));
                instructionNode.attributes.emplace("decode_ok", instruction.decodeOk ? "true" : "false");
                graph.addNode(std::move(instructionNode));

                const auto containingBlock = std::find_if(section.blocks.begin(), section.blocks.end(), [&instruction](const auto& block) {
                    return instruction.offset >= block.startOffset && instruction.offset < block.endOffset;
                });
                if (containingBlock != section.blocks.end()) {
                    ContentEdge containsInstruction{};
                    containsInstruction.from = basicBlockNodeId(sourcePath, section.id.name, containingBlock->startOffset);
                    containsInstruction.to = sourceNodeId;
                    containsInstruction.type = ContentEdgeType::Contains;
                    graph.addEdge(std::move(containsInstruction));
                }
            }

            if (options.includeResourceEdges) {
                addResourceEdge(graph, sourcePath, section.id.name, instruction, sourceNodeId);
            }
            addSubscriptCallEdge(graph, sourcePath, section.id.name, instruction, sourceNodeId);
            addReturnEdge(graph, sourcePath, section.id.name, instruction, sourceNodeId);
        }

        if (options.detailLevel == ContentGraphDetailLevel::Sections) {
            continue;
        }

        for (const auto& block : section.blocks) {
            const auto blockId = basicBlockNodeId(sourcePath, section.id.name, block.startOffset);
            const auto lastInstruction = block.instructionOffsets.empty()
                ? nullptr
                : instructionAt(section, block.instructionOffsets.back());
            for (std::size_t i = 0; i < block.successorOffsets.size(); ++i) {
                const auto successorOffset = block.successorOffsets[i];
                const auto targetIt = blockIdsByOffset.find(successorOffset);
                if (targetIt == blockIdsByOffset.end()) {
                    continue;
                }
                ContentEdge edge{};
                edge.from = blockId;
                edge.to = targetIt->second;
                edge.type = edgeTypeForSuccessor(lastInstruction, i, successorOffset, block.endOffset);
                edge.confidence = ContentConfidence::Known;
                edge.attributes.emplace("target_offset", decimalString(successorOffset));
                if (lastInstruction != nullptr) {
                    edge.evidence.push_back(instructionEvidence(sourcePath, section.id.name, *lastInstruction, "SCT basic-block successor."));
                }
                graph.addEdge(std::move(edge));
            }
        }
    }
}

} // namespace soasim::contentgraph
