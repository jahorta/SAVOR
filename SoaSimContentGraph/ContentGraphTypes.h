#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace soasim::contentgraph {

enum class ContentNodeType {
    ScriptFile,
    ScriptSection,
    BasicBlock,
    Instruction,
    MldFile,
    MldEntry,
    ResourceRef,
    FlagRef,
    UnknownTarget,
};

enum class ContentEdgeType {
    Contains,
    PairedWith,
    Fallthrough,
    BranchTrue,
    BranchFalse,
    SwitchCase,
    Jump,
    CallSubscript,
    Return,
    LoadsScript,
    LoadsMld,
    ReferencesFlag,
    WritesFlag,
    MldEntryDispatchesSection,
};

enum class ContentConfidence {
    Known,
    Inferred,
    Heuristic,
    Unresolved,
};

enum class ContentGraphDetailLevel {
    Sections,
    Blocks,
    Instructions,
};

enum class ContentGraphProjection {
    Full,
    Sections,
    World,
};

using ContentAttributes = std::map<std::string, std::string>;

struct ContentEvidence {
    std::string sourcePath{};
    std::string sectionName{};
    std::optional<std::uint32_t> instructionOffset{};
    std::optional<std::uint16_t> opcode{};
    std::string detail{};
    ContentAttributes attributes{};
};

struct ContentNode {
    std::string id{};
    ContentNodeType type = ContentNodeType::UnknownTarget;
    std::string label{};
    std::string sourcePath{};
    std::optional<std::uint32_t> offset{};
    std::optional<std::uint32_t> size{};
    ContentAttributes attributes{};
};

struct ContentEdge {
    std::string id{};
    std::string from{};
    std::string to{};
    ContentEdgeType type = ContentEdgeType::Contains;
    ContentConfidence confidence = ContentConfidence::Known;
    ContentAttributes attributes{};
    std::vector<ContentEvidence> evidence{};
};

[[nodiscard]] const char* toString(ContentNodeType type);
[[nodiscard]] const char* toString(ContentEdgeType type);
[[nodiscard]] const char* toString(ContentConfidence confidence);
[[nodiscard]] const char* toString(ContentGraphDetailLevel detailLevel);
[[nodiscard]] const char* toString(ContentGraphProjection projection);

} // namespace soasim::contentgraph
