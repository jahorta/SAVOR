#include "NavigationStartCatalogBuilder.h"

#include "SpiceSCT/SctModel.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace savor::navigation {
namespace {

constexpr std::uint16_t kPlacementOpcode = 77;
constexpr std::size_t kMaximumVisitedStates = 200000;
constexpr std::size_t kMaximumCallDepth = 32;
constexpr std::size_t kMaximumPredicatesPerPath = 48;

enum class AnalysisRootKind {
    Init,
    Loop,
    Other,
};

struct PathContext {
    AnalysisRootKind rootKind = AnalysisRootKind::Other;
    std::vector<NavigationStartPredicate> predicates{};
    std::vector<NavigationStartCallFrame> callPath{};
    std::unordered_set<std::uint64_t> visitedNodes{};
    std::vector<std::size_t> activeSections{};
    bool initializationGate = false;
    bool complete = true;
};

struct PlacementValues {
    NavigationStartAvailability availability = NavigationStartAvailability::Incomplete;
    std::optional<std::int32_t> groundTblId{};
    std::optional<NavigationVec3> position{};
    std::optional<float> yawDegrees{};
    std::string unavailableReason{};
};

[[nodiscard]] std::uint64_t nodeKey(const std::size_t sectionIndex, const std::uint32_t offset) noexcept {
    return (static_cast<std::uint64_t>(sectionIndex) << 32U) | offset;
}

[[nodiscard]] std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

[[nodiscard]] bool containsUnknownNode(const spice::sct::SctScptAstNode& node) {
    if (node.kind == spice::sct::SctScptAstNodeKind::Unknown ||
        node.kind == spice::sct::SctScptAstNodeKind::NoLoopValue ||
        node.kind == spice::sct::SctScptAstNodeKind::Stop) {
        return true;
    }
    return std::ranges::any_of(node.children, containsUnknownNode);
}

[[nodiscard]] std::optional<double> evaluateNumeric(const spice::sct::SctScptAstNode& node) {
    if (const auto literal = node.numericLiteral(); literal.has_value()) {
        return std::isfinite(literal->value) ? std::optional<double>(literal->value) : std::nullopt;
    }
    if (node.kind != spice::sct::SctScptAstNodeKind::ArithmeticOp || node.children.size() != 2U) {
        return std::nullopt;
    }

    const auto lhs = evaluateNumeric(node.children[0]);
    const auto rhs = evaluateNumeric(node.children[1]);
    if (!lhs.has_value() || !rhs.has_value()) {
        return std::nullopt;
    }

    double value = 0.0;
    if (node.op == "+") {
        value = *lhs + *rhs;
    } else if (node.op == "-") {
        value = *lhs - *rhs;
    } else if (node.op == "*") {
        value = *lhs * *rhs;
    } else if (node.op == "/") {
        if (*rhs == 0.0) {
            return std::nullopt;
        }
        value = *lhs / *rhs;
    } else if (node.op == "%") {
        if (*rhs == 0.0) {
            return std::nullopt;
        }
        value = std::fmod(*lhs, *rhs);
    } else {
        return std::nullopt;
    }
    return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
}

[[nodiscard]] const spice::sct::SctParameter* findParameter(
    const spice::sct::SctInstruction& instruction,
    const std::uint32_t parameterIndex) {
    const auto found = std::ranges::find_if(instruction.parameters, [&](const auto& parameter) {
        return parameter.index == parameterIndex;
    });
    return found == instruction.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] const spice::sct::SctScptAstNode* parameterAst(
    const spice::sct::SctInstruction& instruction,
    const std::uint32_t parameterIndex) {
    if (const auto* parameter = findParameter(instruction, parameterIndex);
        parameter != nullptr && parameter->expression.has_value() && parameter->expression->ast.has_value()) {
        return &*parameter->expression->ast;
    }
    const auto found = std::ranges::find_if(instruction.scptParameterValueRecords, [&](const auto& record) {
        return record.parameterIndex == parameterIndex && record.ast.has_value();
    });
    return found == instruction.scptParameterValueRecords.end() ? nullptr : &*found->ast;
}

[[nodiscard]] std::optional<double> numericParameter(
    const spice::sct::SctInstruction& instruction,
    const std::uint32_t parameterIndex) {
    if (const auto* ast = parameterAst(instruction, parameterIndex); ast != nullptr) {
        return evaluateNumeric(*ast);
    }
    const auto* parameter = findParameter(instruction, parameterIndex);
    if (parameter != nullptr &&
        parameter->valueKind == spice::sct::SctParameterValueKind::Integer &&
        parameter->rawWords.size() == 1U) {
        return static_cast<double>(static_cast<std::int32_t>(parameter->rawWords.front()));
    }
    return std::nullopt;
}

[[nodiscard]] bool isSentinelParameter(
    const spice::sct::SctInstruction& instruction,
    const std::uint32_t parameterIndex) {
    constexpr std::uint32_t sentinel = 0x7fffffffU;
    if (const auto* parameter = findParameter(instruction, parameterIndex);
        parameter != nullptr && parameter->rawWords.size() == 1U && parameter->rawWords.front() == sentinel) {
        return true;
    }
    if (const auto* ast = parameterAst(instruction, parameterIndex); ast != nullptr) {
        return ast->rawWords.size() == 1U && ast->rawWords.front() == sentinel;
    }
    return false;
}

[[nodiscard]] std::optional<std::int32_t> exactInteger(const double value) {
    if (!std::isfinite(value) ||
        value < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
        value > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
        return std::nullopt;
    }
    const double rounded = std::round(value);
    if (std::abs(value - rounded) > 0.000001) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(rounded);
}

[[nodiscard]] std::optional<float> finiteFloat(const std::optional<double>& value) {
    if (!value.has_value() || !std::isfinite(*value) ||
        std::abs(*value) > static_cast<double>(std::numeric_limits<float>::max())) {
        return std::nullopt;
    }
    return static_cast<float>(*value);
}

[[nodiscard]] std::string joinReasons(const std::vector<std::string>& reasons) {
    std::ostringstream out;
    for (std::size_t i = 0; i < reasons.size(); ++i) {
        if (i != 0U) {
            out << "; ";
        }
        out << reasons[i];
    }
    return out.str();
}

[[nodiscard]] PlacementValues decodePlacement(const spice::sct::SctInstruction& instruction) {
    PlacementValues result{};
    std::vector<std::string> reasons{};

    if (isSentinelParameter(instruction, 0U)) {
        reasons.emplace_back("ground selector is the suppression sentinel");
    } else if (const auto ground = numericParameter(instruction, 0U); ground.has_value()) {
        result.groundTblId = exactInteger(*ground);
        if (!result.groundTblId.has_value()) {
            reasons.emplace_back("ground selector is not a finite integer constant");
        }
    } else {
        reasons.emplace_back("ground selector is not a constant");
    }

    std::array<std::optional<float>, 3> coordinates{};
    constexpr std::array<const char*, 3> coordinateNames{ "X", "Y", "Z" };
    for (std::size_t i = 0; i < coordinates.size(); ++i) {
        const auto parameterIndex = static_cast<std::uint32_t>(i + 1U);
        if (isSentinelParameter(instruction, parameterIndex)) {
            reasons.emplace_back(std::string(coordinateNames[i]) + " position is the suppression sentinel");
            continue;
        }
        coordinates[i] = finiteFloat(numericParameter(instruction, parameterIndex));
        if (!coordinates[i].has_value()) {
            reasons.emplace_back(std::string(coordinateNames[i]) + " position is not a finite constant");
        }
    }
    if (std::ranges::all_of(coordinates, [](const auto& value) { return value.has_value(); })) {
        result.position = NavigationVec3{ *coordinates[0], *coordinates[1], *coordinates[2] };
    }

    if (!isSentinelParameter(instruction, 4U)) {
        result.yawDegrees = finiteFloat(numericParameter(instruction, 4U));
    }

    if (result.groundTblId.has_value() && result.position.has_value() && reasons.empty()) {
        result.availability = NavigationStartAvailability::Resolvable;
    } else {
        result.unavailableReason = joinReasons(reasons);
        if (result.unavailableReason.empty()) {
            result.unavailableReason = "required placement values are incomplete";
        }
    }
    return result;
}

[[nodiscard]] std::string formatFloat(const float value) {
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
    return out.str();
}

[[nodiscard]] std::string placementLabel(
    const NavigationStartKind kind,
    const PlacementValues& placement,
    const NavigationStartSourceProvenance& source) {
    std::ostringstream out;
    out << (kind == NavigationStartKind::AuthoredArrival ? "Arrival" : "Reposition");
    if (placement.groundTblId.has_value()) {
        out << " - ground " << *placement.groundTblId;
    }
    if (placement.position.has_value()) {
        out << " - (" << formatFloat(placement.position->x) << ", "
            << formatFloat(placement.position->y) << ", "
            << formatFloat(placement.position->z) << ")";
    }
    if (placement.yawDegrees.has_value()) {
        out << " - yaw " << formatFloat(*placement.yawDegrees);
    } else {
        out << " - yaw unknown";
    }
    if (placement.availability == NavigationStartAvailability::Incomplete) {
        out << " - incomplete @" << source.sectionName << ":" << source.instructionOffset;
    }
    return out.str();
}

[[nodiscard]] std::string predicateSummary(const std::vector<NavigationStartPredicate>& predicates) {
    if (predicates.empty()) {
        return "Unconditional within the analyzed entry path";
    }
    std::ostringstream out;
    for (std::size_t i = 0; i < predicates.size(); ++i) {
        if (i != 0U) {
            out << " AND ";
        }
        out << predicates[i].summary;
    }
    return out.str();
}

[[nodiscard]] std::string callPathSummary(const std::vector<NavigationStartCallFrame>& callPath) {
    if (callPath.empty()) {
        return {};
    }
    std::ostringstream out;
    for (std::size_t i = 0; i < callPath.size(); ++i) {
        if (i != 0U) {
            out << " -> ";
        }
        out << callPath[i].callerSectionName << "@" << callPath[i].instructionOffset
            << " -> " << callPath[i].calleeSectionName;
    }
    return out.str();
}

[[nodiscard]] bool variableIdEquals(
    const spice::sct::SctScptAstNode& node,
    const spice::sct::SctScptAstNodeKind kind,
    const std::uint32_t id) {
    return node.kind == kind && !node.rawWords.empty() && (node.rawWords.front() & 0x00ffffffU) == id;
}

[[nodiscard]] std::optional<std::int32_t> comparisonValueForVariable(
    const spice::sct::SctScptAstNode& node,
    const spice::sct::SctScptAstNodeKind variableKind,
    const std::uint32_t variableId) {
    if (node.kind != spice::sct::SctScptAstNodeKind::CompareOp || node.op != "==" || node.children.size() != 2U) {
        return std::nullopt;
    }
    if (variableIdEquals(node.children[0], variableKind, variableId)) {
        if (const auto value = evaluateNumeric(node.children[1]); value.has_value()) {
            return exactInteger(*value);
        }
    }
    if (variableIdEquals(node.children[1], variableKind, variableId)) {
        if (const auto value = evaluateNumeric(node.children[0]); value.has_value()) {
            return exactInteger(*value);
        }
    }
    return std::nullopt;
}

void collectComparisonValuesForVariable(
    const spice::sct::SctScptAstNode& node,
    const spice::sct::SctScptAstNodeKind variableKind,
    const std::uint32_t variableId,
    std::set<std::int32_t>& values) {
    if (const auto value = comparisonValueForVariable(node, variableKind, variableId); value.has_value()) {
        values.insert(*value);
    }
    for (const auto& child : node.children) {
        collectComparisonValuesForVariable(child, variableKind, variableId, values);
    }
}

[[nodiscard]] std::set<std::int32_t> comparisonValuesForVariable(
    const spice::sct::SctScptAstNode& node,
    const spice::sct::SctScptAstNodeKind variableKind,
    const std::uint32_t variableId) {
    std::set<std::int32_t> values{};
    collectComparisonValuesForVariable(node, variableKind, variableId, values);
    return values;
}

[[nodiscard]] bool trueExpressionImpliesVariableComparison(
    const spice::sct::SctScptAstNode& node,
    const spice::sct::SctScptAstNodeKind variableKind,
    const std::uint32_t variableId,
    const std::int32_t expectedValue) {
    if (comparisonValueForVariable(node, variableKind, variableId) == expectedValue) {
        return true;
    }
    if (node.children.size() != 2U) {
        return false;
    }
    if (node.op == "&&") {
        // If an AND is true, every operand is true; either operand can imply
        // the initialization gate.
        return trueExpressionImpliesVariableComparison(
                   node.children[0], variableKind, variableId, expectedValue) ||
            trueExpressionImpliesVariableComparison(
                   node.children[1], variableKind, variableId, expectedValue);
    }
    if (node.op == "||") {
        // A true OR implies the gate only when every alternative implies it.
        return trueExpressionImpliesVariableComparison(
                   node.children[0], variableKind, variableId, expectedValue) &&
            trueExpressionImpliesVariableComparison(
                   node.children[1], variableKind, variableId, expectedValue);
    }
    return false;
}

[[nodiscard]] std::optional<std::int32_t> parseSignedInteger(const std::string& text) {
    std::int32_t value = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto [pointer, error] = std::from_chars(begin, end, value);
    return error == std::errc{} && pointer == end ? std::optional<std::int32_t>(value) : std::nullopt;
}

class CatalogBuildState final {
public:
    CatalogBuildState(
        const spice::sct::SctParseResult& parseResult,
        const NavigationAreaIdentity& areaIdentity)
        : parseResult_(parseResult), areaIdentity_(areaIdentity) {
        collectSiblingAreaKeys();
    }

    [[nodiscard]] NavigationStartCatalog build() {
        for (std::size_t sectionIndex = 0; sectionIndex < parseResult_.file.sections.size(); ++sectionIndex) {
            for (const auto& instruction : parseResult_.file.sections[sectionIndex].instructions) {
                if (instruction.opcode == kPlacementOpcode) {
                    ++catalog_.analyzedOpcode77Count;
                }
            }
        }

        analyzeNamedRoots("init", AnalysisRootKind::Init);
        analyzeNamedRoots("loop", AnalysisRootKind::Loop);

        // Sections with no incoming CallSubscript edge are potential engine-
        // invoked roots. Analyze every such root whose call graph can reach an
        // opcode 77, even when that opcode was already seen through init/loop.
        // This preserves distinct authored-arrival and event-reposition paths
        // into a shared helper.
        const auto candidateRoots = externalRootsReachingPlacements();
        for (const auto sectionIndex : candidateRoots) {
            if (!analyzedRootSections_.contains(sectionIndex)) {
                analyzeRoot(sectionIndex, AnalysisRootKind::Other);
            }
        }

        std::set<std::size_t> remainingSections{};
        for (std::size_t sectionIndex = 0; sectionIndex < parseResult_.file.sections.size(); ++sectionIndex) {
            for (const auto& instruction : parseResult_.file.sections[sectionIndex].instructions) {
                if (instruction.opcode == kPlacementOpcode &&
                    !coveredOccurrences_.contains(nodeKey(sectionIndex, instruction.offset))) {
                    remainingSections.insert(sectionIndex);
                }
            }
        }
        for (const auto sectionIndex : remainingSections) {
            analyzeRoot(sectionIndex, AnalysisRootKind::Other);
        }

        // Resource limits and malformed control flow must never hide an opcode
        // 77 occurrence. Anything still uncovered is retained as an advisory
        // direct-section fallback with explicitly incomplete path analysis.
        for (std::size_t sectionIndex = 0; sectionIndex < parseResult_.file.sections.size(); ++sectionIndex) {
            const auto& section = parseResult_.file.sections[sectionIndex];
            for (const auto& instruction : section.instructions) {
                if (instruction.opcode != kPlacementOpcode ||
                    coveredOccurrences_.contains(nodeKey(sectionIndex, instruction.offset))) {
                    continue;
                }
                PathContext context{};
                context.rootKind = section.id.name == "init" ? AnalysisRootKind::Init : AnalysisRootKind::Other;
                context.complete = false;
                context.predicates.push_back(NavigationStartPredicate{
                    .summary = "Control-flow conditions could not be reconstructed from the section entry",
                    .opaque = true,
                });
                recordPlacement(sectionIndex, instruction, context);
                addDiagnosticOnce(NavigationDiagnosticSeverity::Warning,
                    "Opcode 77 at section '" + section.id.name + "' offset " +
                        std::to_string(instruction.offset) +
                        " was retained with opaque conditions because its control-flow path was incomplete.");
            }
        }

        catalog_.incompleteOpcode77Count = incompleteOccurrences_.size();
        std::ranges::sort(catalog_.options, [](const auto& lhs, const auto& rhs) {
            if (lhs.kind != rhs.kind) {
                return lhs.kind < rhs.kind;
            }
            if (lhs.availability != rhs.availability) {
                return lhs.availability < rhs.availability;
            }
            return lhs.id < rhs.id;
        });
        return std::move(catalog_);
    }

private:
    void collectSiblingAreaKeys() {
        if (areaIdentity_.mldPath.empty()) {
            return;
        }
        std::error_code error{};
        std::filesystem::directory_iterator iterator(areaIdentity_.mldPath.parent_path(), error);
        const std::filesystem::directory_iterator end{};
        for (; !error && iterator != end; iterator.increment(error)) {
            if (!iterator->is_regular_file(error)) {
                error.clear();
                continue;
            }
            const auto& path = iterator->path();
            const auto mldIdentity = resolveNavigationAreaIdentity(path);
            if (mldIdentity.recognized) {
                siblingAreaKeys_.insert(mldIdentity.areaKey);
            }
            if (const auto sctKey = navigationSctAreaKey(path); sctKey.has_value()) {
                siblingAreaKeys_.insert(*sctKey);
            }
        }
    }

    void analyzeNamedRoots(const std::string_view name, const AnalysisRootKind rootKind) {
        for (std::size_t i = 0; i < parseResult_.file.sections.size(); ++i) {
            if (parseResult_.file.sections[i].id.name == name) {
                analyzeRoot(i, rootKind);
            }
        }
    }

    void analyzeRoot(const std::size_t sectionIndex, const AnalysisRootKind rootKind) {
        const auto& section = parseResult_.file.sections[sectionIndex];
        if (section.instructions.empty()) {
            return;
        }
        analyzedRootSections_.insert(sectionIndex);
        PathContext context{};
        context.rootKind = rootKind;
        context.activeSections.push_back(sectionIndex);
        walk(sectionIndex, section.instructions.front().offset, std::move(context));
    }

    [[nodiscard]] std::set<std::size_t> externalRootsReachingPlacements() const {
        const std::size_t sectionCount = parseResult_.file.sections.size();
        std::vector<std::vector<std::size_t>> calls(sectionCount);
        std::unordered_set<std::size_t> callTargets{};
        std::vector<bool> reachesPlacement(sectionCount, false);

        for (std::size_t sectionIndex = 0; sectionIndex < sectionCount; ++sectionIndex) {
            const auto& section = parseResult_.file.sections[sectionIndex];
            reachesPlacement[sectionIndex] = std::ranges::any_of(section.instructions, [](const auto& instruction) {
                return instruction.opcode == kPlacementOpcode;
            });
            for (const auto& edge : section.edges) {
                if (edge.type != spice::sct::SctEdgeType::CallSubscript) {
                    continue;
                }
                if (const auto target = callTargetSection(edge); target.has_value()) {
                    calls[sectionIndex].push_back(*target);
                    callTargets.insert(*target);
                }
            }
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (std::size_t caller = 0; caller < sectionCount; ++caller) {
                if (reachesPlacement[caller]) {
                    continue;
                }
                if (std::ranges::any_of(calls[caller], [&](const auto target) {
                        return reachesPlacement[target];
                    })) {
                    reachesPlacement[caller] = true;
                    changed = true;
                }
            }
        }

        std::set<std::size_t> result{};
        for (std::size_t sectionIndex = 0; sectionIndex < sectionCount; ++sectionIndex) {
            if (reachesPlacement[sectionIndex] &&
                (!callTargets.contains(sectionIndex) || isPotentialExternalEventRoot(sectionIndex))) {
                result.insert(sectionIndex);
            }
        }
        return result;
    }

    [[nodiscard]] bool isPotentialExternalEventRoot(const std::size_t sectionIndex) const {
        const auto& section = parseResult_.file.sections[sectionIndex];
        const std::string name = lowerAscii(section.id.name);
        return name.find("event") != std::string::npos ||
            name.find("cutscene") != std::string::npos ||
            section.heuristicEvidence.likelyCutscene ||
            section.heuristicEvidence.likelyTrigger;
    }

    [[nodiscard]] const spice::sct::SctInstruction* instructionAt(
        const std::size_t sectionIndex,
        const std::uint32_t offset) const {
        const auto& instructions = parseResult_.file.sections[sectionIndex].instructions;
        const auto found = std::ranges::find_if(instructions, [&](const auto& instruction) {
            return instruction.offset == offset;
        });
        return found == instructions.end() ? nullptr : &*found;
    }

    [[nodiscard]] std::optional<std::uint32_t> nextInstructionOffset(
        const std::size_t sectionIndex,
        const std::uint32_t offset) const {
        const auto& instructions = parseResult_.file.sections[sectionIndex].instructions;
        for (std::size_t i = 0; i < instructions.size(); ++i) {
            if (instructions[i].offset == offset && i + 1U < instructions.size()) {
                return instructions[i + 1U].offset;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::vector<const spice::sct::SctEdge*> edgesFrom(
        const std::size_t sectionIndex,
        const std::uint32_t offset,
        const spice::sct::SctEdgeType type) const {
        std::vector<const spice::sct::SctEdge*> result{};
        for (const auto& edge : parseResult_.file.sections[sectionIndex].edges) {
            if (edge.fromOffset == offset && edge.type == type) {
                result.push_back(&edge);
            }
        }
        return result;
    }

    void walk(std::size_t sectionIndex, std::uint32_t offset, PathContext context) {
        if (++visitedStateCount_ > kMaximumVisitedStates) {
            addDiagnosticOnce(NavigationDiagnosticSeverity::Warning,
                "Opcode-77 control-flow analysis reached its conservative state limit; remaining placements use opaque fallback paths.");
            return;
        }
        const auto currentNode = nodeKey(sectionIndex, offset);
        if (!context.visitedNodes.insert(currentNode).second) {
            addDiagnosticOnce(NavigationDiagnosticSeverity::Warning,
                "Opcode-77 control-flow analysis encountered a cycle in section '" +
                    parseResult_.file.sections[sectionIndex].id.name + "'.");
            return;
        }

        const auto* instruction = instructionAt(sectionIndex, offset);
        if (instruction == nullptr) {
            addDiagnosticOnce(NavigationDiagnosticSeverity::Warning,
                "Opcode-77 control-flow analysis encountered a missing instruction target in section '" +
                    parseResult_.file.sections[sectionIndex].id.name + "' at offset " + std::to_string(offset) + ".");
            return;
        }

        if (instruction->opcode == kPlacementOpcode) {
            recordPlacement(sectionIndex, *instruction, context);
        }

        if (instruction->opcode == 0U) {
            walkBranch(sectionIndex, *instruction, std::move(context));
            return;
        }
        if (instruction->opcode == 3U) {
            walkSwitch(sectionIndex, *instruction, std::move(context));
            return;
        }
        if (instruction->opcode == 10U) {
            walkEdges(sectionIndex, *instruction, spice::sct::SctEdgeType::Jump, std::move(context));
            return;
        }
        if (instruction->opcode == 12U) {
            return;
        }
        if (instruction->opcode == 11U) {
            walkCall(sectionIndex, *instruction, context);
        }

        if (const auto next = nextInstructionOffset(sectionIndex, offset); next.has_value()) {
            walk(sectionIndex, *next, std::move(context));
        }
    }

    void walkEdges(
        const std::size_t sectionIndex,
        const spice::sct::SctInstruction& instruction,
        const spice::sct::SctEdgeType type,
        PathContext context) {
        const auto edges = edgesFrom(sectionIndex, instruction.offset, type);
        if (edges.empty()) {
            addDiagnosticOnce(NavigationDiagnosticSeverity::Warning,
                "Opcode-77 control-flow analysis could not resolve a target for opcode " +
                    std::to_string(instruction.opcode) + " in section '" +
                    parseResult_.file.sections[sectionIndex].id.name + "'.");
            return;
        }
        for (const auto* edge : edges) {
            if (edge->toOffset.has_value()) {
                walk(sectionIndex, *edge->toOffset, context);
            }
        }
    }

    [[nodiscard]] NavigationStartPredicate branchPredicate(
        const spice::sct::SctInstruction& instruction,
        const bool negated) const {
        NavigationStartPredicate result{};
        const auto* parameter = findParameter(instruction, 0U);
        const auto* ast = parameterAst(instruction, 0U);
        std::string base = parameter != nullptr ? parameter->displayValue : std::string{};
        if (base.empty() && ast != nullptr) {
            base = ast->display;
        }
        if (base.empty()) {
            base = "opaque branch condition @" + std::to_string(instruction.offset);
        }
        result.negated = negated;
        result.opaque = ast == nullptr || containsUnknownNode(*ast);
        result.summary = negated ? "NOT (" + base + ")" : base;
        if (ast != nullptr) {
            appendPreviousContexts(result, comparisonValuesForVariable(
                *ast, spice::sct::SctScptAstNodeKind::IntVariable, 15U));
        }
        return result;
    }

    void appendPredicate(PathContext& context, NavigationStartPredicate predicate) {
        if (context.predicates.size() >= kMaximumPredicatesPerPath) {
            context.complete = false;
            if (context.predicates.empty() || context.predicates.back().summary != "Additional nested predicates omitted") {
                context.predicates.push_back(NavigationStartPredicate{
                    .summary = "Additional nested predicates omitted",
                    .opaque = true,
                });
            }
            return;
        }
        if (predicate.opaque) {
            context.complete = false;
        }
        context.predicates.push_back(std::move(predicate));
    }

    void walkBranch(
        const std::size_t sectionIndex,
        const spice::sct::SctInstruction& instruction,
        PathContext context) {
        const auto trueEdges = edgesFrom(sectionIndex, instruction.offset, spice::sct::SctEdgeType::BranchTrue);
        const auto falseEdges = edgesFrom(sectionIndex, instruction.offset, spice::sct::SctEdgeType::BranchFalse);
        const auto* ast = parameterAst(instruction, 0U);

        for (const auto* edge : trueEdges) {
            if (!edge->toOffset.has_value()) {
                continue;
            }
            auto branchContext = context;
            appendPredicate(branchContext, branchPredicate(instruction, false));
            if (ast != nullptr && trueExpressionImpliesVariableComparison(
                    *ast, spice::sct::SctScptAstNodeKind::BitVariable, 1910U, 0)) {
                branchContext.initializationGate = true;
            }
            walk(sectionIndex, *edge->toOffset, std::move(branchContext));
        }
        for (const auto* edge : falseEdges) {
            if (!edge->toOffset.has_value()) {
                continue;
            }
            auto branchContext = context;
            appendPredicate(branchContext, branchPredicate(instruction, true));
            walk(sectionIndex, *edge->toOffset, std::move(branchContext));
        }
        if (trueEdges.empty() || falseEdges.empty()) {
            addDiagnosticOnce(NavigationDiagnosticSeverity::Warning,
                "Opcode-77 control-flow analysis found an incomplete branch edge set in section '" +
                    parseResult_.file.sections[sectionIndex].id.name + "'.");
        }
    }

    [[nodiscard]] NavigationStartPredicate switchPredicate(
        const spice::sct::SctInstruction& instruction,
        const std::size_t caseOrdinal) const {
        NavigationStartPredicate result{};
        const auto* choiceParameter = findParameter(instruction, 0U);
        const auto* choiceAst = parameterAst(instruction, 0U);
        const auto* caseParameter = findParameter(instruction, static_cast<std::uint32_t>(2U + caseOrdinal * 2U));
        const std::string choice = choiceParameter != nullptr && !choiceParameter->displayValue.empty()
            ? choiceParameter->displayValue
            : (choiceAst != nullptr && !choiceAst->display.empty() ? choiceAst->display : "opaque switch choice");
        const std::string caseValue = caseParameter != nullptr && !caseParameter->displayValue.empty()
            ? caseParameter->displayValue
            : "opaque case " + std::to_string(caseOrdinal);
        result.summary = choice + " == " + caseValue;
        result.opaque = choiceAst == nullptr || caseParameter == nullptr || containsUnknownNode(*choiceAst);

        if (choiceAst != nullptr && variableIdEquals(
                *choiceAst, spice::sct::SctScptAstNodeKind::IntVariable, 15U) &&
            caseParameter != nullptr && caseParameter->rawWords.size() == 1U) {
            appendPreviousContexts(result, {
                static_cast<std::int32_t>(caseParameter->rawWords.front()),
            });
        }
        return result;
    }

    void walkSwitch(
        const std::size_t sectionIndex,
        const spice::sct::SctInstruction& instruction,
        PathContext context) {
        const auto edges = edgesFrom(sectionIndex, instruction.offset, spice::sct::SctEdgeType::SwitchCase);
        if (edges.empty()) {
            addDiagnosticOnce(NavigationDiagnosticSeverity::Warning,
                "Opcode-77 control-flow analysis found a switch without resolvable cases in section '" +
                    parseResult_.file.sections[sectionIndex].id.name + "'.");
            return;
        }
        for (std::size_t i = 0; i < edges.size(); ++i) {
            if (!edges[i]->toOffset.has_value()) {
                continue;
            }
            auto caseContext = context;
            appendPredicate(caseContext, switchPredicate(instruction, i));
            walk(sectionIndex, *edges[i]->toOffset, std::move(caseContext));
        }
        // Deliberately no implicit fallthrough/default path: SPICE exposes
        // only the decoded case edges, and inventing a default would attach
        // placements to conditions not present in the script.
    }

    [[nodiscard]] std::optional<std::size_t> callTargetSection(
        const spice::sct::SctEdge& edge) const {
        const auto attribute = edge.attributes.find("target_section_index");
        if (attribute != edge.attributes.end()) {
            if (const auto parsed = parseSignedInteger(attribute->second); parsed.has_value() && *parsed >= 0) {
                const auto found = std::ranges::find_if(parseResult_.file.sections, [&](const auto& section) {
                    return section.id.index == static_cast<std::uint32_t>(*parsed);
                });
                if (found != parseResult_.file.sections.end()) {
                    return static_cast<std::size_t>(std::distance(parseResult_.file.sections.begin(), found));
                }
            }
        }
        if (edge.toPayloadOffset.has_value()) {
            for (std::size_t i = 0; i < parseResult_.file.sections.size(); ++i) {
                const auto& section = parseResult_.file.sections[i];
                if (std::ranges::any_of(section.instructions, [&](const auto& instruction) {
                        return instruction.payloadOffset == *edge.toPayloadOffset;
                    })) {
                    return i;
                }
            }
        }
        return std::nullopt;
    }

    void walkCall(
        const std::size_t sectionIndex,
        const spice::sct::SctInstruction& instruction,
        const PathContext& context) {
        const auto edges = edgesFrom(sectionIndex, instruction.offset, spice::sct::SctEdgeType::CallSubscript);
        if (edges.empty()) {
            addDiagnosticOnce(NavigationDiagnosticSeverity::Warning,
                "Opcode-77 analysis could not resolve a CallSubscript target in section '" +
                    parseResult_.file.sections[sectionIndex].id.name + "'.");
            return;
        }
        for (const auto* edge : edges) {
            const auto targetSection = callTargetSection(*edge);
            if (!targetSection.has_value() || parseResult_.file.sections[*targetSection].instructions.empty()) {
                addDiagnosticOnce(NavigationDiagnosticSeverity::Warning,
                    "Opcode-77 analysis encountered a missing CallSubscript section target from '" +
                        parseResult_.file.sections[sectionIndex].id.name + "'.");
                continue;
            }

            auto callContext = context;
            const auto& caller = parseResult_.file.sections[sectionIndex];
            const auto& callee = parseResult_.file.sections[*targetSection];
            callContext.callPath.push_back(NavigationStartCallFrame{
                .callerSectionIndex = caller.id.index,
                .callerSectionName = caller.id.name,
                .instructionOffset = instruction.offset,
                .calleeSectionIndex = callee.id.index,
                .calleeSectionName = callee.id.name,
            });
            if (callContext.activeSections.size() >= kMaximumCallDepth ||
                std::ranges::find(callContext.activeSections, *targetSection) != callContext.activeSections.end()) {
                callContext.complete = false;
                appendPredicate(callContext, NavigationStartPredicate{
                    .summary = "Recursive or cyclic CallSubscript path",
                    .opaque = true,
                });
                recordOpaqueSectionPlacements(*targetSection, callContext);
                addDiagnosticOnce(NavigationDiagnosticSeverity::Warning,
                    "Opcode-77 analysis stopped a recursive or over-depth CallSubscript path at section '" +
                        callee.id.name + "'.");
                continue;
            }
            callContext.activeSections.push_back(*targetSection);
            const auto targetOffset = edge->toOffset.value_or(callee.instructions.front().offset);
            walk(*targetSection, targetOffset, std::move(callContext));
        }
    }

    void recordOpaqueSectionPlacements(const std::size_t sectionIndex, const PathContext& context) {
        for (const auto& instruction : parseResult_.file.sections[sectionIndex].instructions) {
            if (instruction.opcode == kPlacementOpcode) {
                recordPlacement(sectionIndex, instruction, context);
            }
        }
    }

    [[nodiscard]] NavigationStartKind startKind(const PathContext& context) const noexcept {
        if (context.rootKind == AnalysisRootKind::Init ||
            (context.rootKind == AnalysisRootKind::Loop && context.initializationGate)) {
            return NavigationStartKind::AuthoredArrival;
        }
        return NavigationStartKind::ScriptedReposition;
    }

    [[nodiscard]] std::string groupingKey(
        const NavigationStartKind kind,
        const PlacementValues& placement,
        const std::size_t sectionIndex,
        const std::uint32_t instructionOffset) const {
        std::ostringstream out;
        out << (kind == NavigationStartKind::AuthoredArrival ? "arrival" : "reposition");
        if (placement.availability == NavigationStartAvailability::Incomplete ||
            !placement.groundTblId.has_value() || !placement.position.has_value()) {
            out << ":incomplete:" << sectionIndex << ":" << instructionOffset;
            return out.str();
        }
        out << ":g" << *placement.groundTblId
            << ":x" << std::hex << std::bit_cast<std::uint32_t>(placement.position->x)
            << ":y" << std::bit_cast<std::uint32_t>(placement.position->y)
            << ":z" << std::bit_cast<std::uint32_t>(placement.position->z)
            << ":r";
        if (placement.yawDegrees.has_value()) {
            out << std::bit_cast<std::uint32_t>(*placement.yawDegrees);
        } else {
            out << "unknown";
        }
        return out.str();
    }

    void recordPlacement(
        const std::size_t sectionIndex,
        const spice::sct::SctInstruction& instruction,
        const PathContext& context) {
        const auto& section = parseResult_.file.sections[sectionIndex];
        const auto occurrence = nodeKey(sectionIndex, instruction.offset);
        coveredOccurrences_.insert(occurrence);
        const auto placement = decodePlacement(instruction);
        if (!instruction.decodeOk &&
            placement.availability == NavigationStartAvailability::Resolvable) {
            addDiagnosticOnce(NavigationDiagnosticSeverity::Warning,
                "Opcode 77 at section '" + section.id.name + "' offset " +
                    std::to_string(instruction.offset) +
                    " has incomplete instruction decoding, but its finite constant ground and XYZ values remain selectable.");
        }
        if (placement.availability == NavigationStartAvailability::Incomplete) {
            incompleteOccurrences_.insert(occurrence);
            addDiagnosticOnce(NavigationDiagnosticSeverity::Warning,
                "Opcode 77 at section '" + section.id.name + "' offset " +
                    std::to_string(instruction.offset) + " is not directly selectable: " +
                    placement.unavailableReason + ".");
        }

        NavigationStartVariant variant{};
        variant.source = NavigationStartSourceProvenance{
            .sectionIndex = section.id.index,
            .sectionName = section.id.name,
            .instructionOffset = instruction.offset,
            .instructionPayloadOffset = instruction.payloadOffset,
            .opcode = kPlacementOpcode,
        };
        variant.predicates = context.predicates;
        variant.callPath = context.callPath;
        variant.conditionSummary = predicateSummary(variant.predicates);
        variant.callPathSummary = callPathSummary(variant.callPath);
        variant.conditionAnalysisComplete = context.complete &&
            std::ranges::none_of(variant.predicates, [](const auto& predicate) { return predicate.opaque; });

        const auto kind = startKind(context);
        const auto key = groupingKey(kind, placement, sectionIndex, instruction.offset);
        std::ostringstream variantKey;
        variantKey << key << ":source:" << section.id.index << ":" << instruction.offset
            << ":conditions:" << variant.conditionSummary << ":calls:" << variant.callPathSummary;
        if (!recordedVariantKeys_.insert(variantKey.str()).second) {
            return;
        }

        auto optionFound = optionIndexes_.find(key);
        if (optionFound == optionIndexes_.end()) {
            const auto newIndex = catalog_.options.size();
            optionIndexes_.emplace(key, newIndex);
            catalog_.options.push_back(NavigationStartOption{
                .id = "opcode77:" + key,
                .label = placementLabel(kind, placement, variant.source),
                .kind = kind,
                .availability = placement.availability,
                .groundTblId = placement.groundTblId,
                .position = placement.position,
                .yawDegrees = placement.yawDegrees,
                .unavailableReason = placement.unavailableReason,
                .variants = { std::move(variant) },
            });
        } else {
            catalog_.options[optionFound->second].variants.push_back(std::move(variant));
        }
    }

    [[nodiscard]] std::string contextLabel(const std::int32_t value) const {
        if (value == 10000) {
            return "Battle Return";
        }
        if (value == 20000) {
            return "Save Load";
        }
        if (value < 0) {
            return {};
        }
        const auto suffix = value % 10;
        const auto areaNumber = value / 10;
        if (suffix < 0 || suffix > 25 || areaNumber < 0 || areaNumber > 999) {
            return {};
        }
        std::ostringstream key;
        key << std::setw(3) << std::setfill('0') << areaNumber
            << static_cast<char>('a' + suffix);
        if (!siblingAreaKeys_.contains(lowerAscii(key.str()))) {
            return {};
        }
        return "Previous area a" + lowerAscii(key.str());
    }

    void appendPreviousContexts(
        NavigationStartPredicate& predicate,
        const std::set<std::int32_t>& rawValues) const {
        std::vector<std::string> labels{};
        for (const auto rawValue : rawValues) {
            const auto label = contextLabel(rawValue);
            predicate.previousContexts.push_back(NavigationStartPreviousContext{
                .rawValue = rawValue,
                .label = label,
            });
            if (!label.empty()) {
                labels.push_back(label);
            }
        }
        if (!labels.empty()) {
            predicate.summary += " [";
            for (std::size_t i = 0; i < labels.size(); ++i) {
                if (i != 0U) {
                    predicate.summary += "; ";
                }
                predicate.summary += labels[i];
            }
            predicate.summary += "]";
        }
    }

    void addDiagnosticOnce(const NavigationDiagnosticSeverity severity, std::string message) {
        if (!diagnosticMessages_.insert(message).second) {
            return;
        }
        catalog_.diagnostics.push_back(NavigationDiagnostic{
            .severity = severity,
            .message = std::move(message),
        });
    }

    const spice::sct::SctParseResult& parseResult_;
    const NavigationAreaIdentity& areaIdentity_;
    NavigationStartCatalog catalog_{};
    std::unordered_set<std::string> siblingAreaKeys_{};
    std::unordered_map<std::string, std::size_t> optionIndexes_{};
    std::unordered_set<std::string> recordedVariantKeys_{};
    std::unordered_set<std::uint64_t> coveredOccurrences_{};
    std::unordered_set<std::uint64_t> incompleteOccurrences_{};
    std::unordered_set<std::size_t> analyzedRootSections_{};
    std::unordered_set<std::string> diagnosticMessages_{};
    std::size_t visitedStateCount_ = 0;
};

} // namespace

NavigationStartCatalog NavigationStartCatalogBuilder::build(
    const spice::sct::SctParseResult& parseResult,
    const NavigationAreaIdentity& areaIdentity) const {
    return CatalogBuildState(parseResult, areaIdentity).build();
}

} // namespace savor::navigation
