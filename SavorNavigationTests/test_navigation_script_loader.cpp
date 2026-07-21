#include "Analysis/NavigationStartCatalogBuilder.h"
#include "Loading/NavigationScriptLoader.h"
#include "Loading/NavigationScenarioLoader.h"
#include "Model/NavigationAreaIdentity.h"

#include "SpiceSCT/SctModel.h"
#include "SpiceSCT/SctParser.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace savor::navigation {
namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("savor-navigation-script-" + std::to_string(nonce));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error{};
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_{};
};

[[nodiscard]] bool hasDiagnosticContaining(
    const NavigationScriptLoadResult& result,
    const std::string_view needle) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] spice::sct::SctScptAstNode numericAst(const float value) {
    return spice::sct::SctScptAstNode{
        .kind = spice::sct::SctScptAstNodeKind::FloatLiteral,
        .display = "float: " + std::to_string(value),
        .rawWords = { 0x04000000U, std::bit_cast<std::uint32_t>(value) },
    };
}

[[nodiscard]] spice::sct::SctScptAstNode variableAst(
    const spice::sct::SctScptAstNodeKind kind,
    const std::uint32_t id) {
    std::uint32_t prefix = 0;
    std::string label{};
    switch (kind) {
    case spice::sct::SctScptAstNodeKind::IntVariable:
        prefix = 0x50000000U;
        label = "IntVar: ";
        break;
    case spice::sct::SctScptAstNodeKind::BitVariable:
        prefix = 0x20000000U;
        label = "BitVar: ";
        break;
    default:
        label = "Variable: ";
        break;
    }
    return spice::sct::SctScptAstNode{
        .kind = kind,
        .display = label + std::to_string(id),
        .rawWords = { prefix | id },
    };
}

[[nodiscard]] spice::sct::SctScptAstNode comparisonAst(
    spice::sct::SctScptAstNode lhs,
    spice::sct::SctScptAstNode rhs) {
    const std::string display = "(" + lhs.display + " == " + rhs.display + ")";
    return spice::sct::SctScptAstNode{
        .kind = spice::sct::SctScptAstNodeKind::CompareOp,
        .display = display,
        .op = "==",
        .rawWords = { 4U },
        .children = { std::move(lhs), std::move(rhs) },
    };
}

[[nodiscard]] spice::sct::SctScptAstNode logicalAst(
    spice::sct::SctScptAstNode lhs,
    spice::sct::SctScptAstNode rhs,
    std::string operation) {
    const std::string display = "(" + lhs.display + " " + operation + " " + rhs.display + ")";
    return spice::sct::SctScptAstNode{
        .kind = spice::sct::SctScptAstNodeKind::CompareOp,
        .display = display,
        .op = std::move(operation),
        .children = { std::move(lhs), std::move(rhs) },
    };
}

[[nodiscard]] spice::sct::SctScptAstNode arithmeticAst(
    spice::sct::SctScptAstNode lhs,
    spice::sct::SctScptAstNode rhs,
    std::string operation) {
    const std::string display = "(" + lhs.display + " " + operation + " " + rhs.display + ")";
    return spice::sct::SctScptAstNode{
        .kind = spice::sct::SctScptAstNodeKind::ArithmeticOp,
        .display = display,
        .op = std::move(operation),
        .children = { std::move(lhs), std::move(rhs) },
    };
}

[[nodiscard]] spice::sct::SctParameter expressionParameter(
    const std::uint32_t index,
    spice::sct::SctScptAstNode ast,
    std::string role = {}) {
    const std::string display = ast.display;
    return spice::sct::SctParameter{
        .index = index,
        .role = std::move(role),
        .valueKind = spice::sct::SctParameterValueKind::Expression,
        .confidence = spice::sct::SctSemanticConfidence::Known,
        .displayValue = display,
        .expression = spice::sct::SctExpression{
            .display = display,
            .hitStopCode = true,
            .ast = std::move(ast),
        },
    };
}

[[nodiscard]] spice::sct::SctParameter integerParameter(
    const std::uint32_t index,
    const std::int32_t value,
    std::string role = {}) {
    return spice::sct::SctParameter{
        .index = index,
        .role = std::move(role),
        .valueKind = spice::sct::SctParameterValueKind::Integer,
        .confidence = spice::sct::SctSemanticConfidence::Known,
        .rawWords = { static_cast<std::uint32_t>(value) },
        .displayValue = std::to_string(value),
    };
}

[[nodiscard]] spice::sct::SctInstruction placementInstruction(
    const std::uint32_t offset,
    std::array<spice::sct::SctScptAstNode, 5> values) {
    spice::sct::SctInstruction instruction{};
    instruction.offset = offset;
    instruction.payloadOffset = offset;
    instruction.opcode = 77U;
    instruction.sizeBytes = 4U;
    instruction.decodeOk = true;
    for (std::uint32_t i = 0; i < values.size(); ++i) {
        instruction.parameters.push_back(expressionParameter(i, std::move(values[i])));
    }
    return instruction;
}

[[nodiscard]] spice::sct::SctInstruction simpleInstruction(
    const std::uint32_t offset,
    const std::uint16_t opcode) {
    return spice::sct::SctInstruction{
        .offset = offset,
        .payloadOffset = offset,
        .opcode = opcode,
        .sizeBytes = 4U,
        .decodeOk = true,
    };
}

[[nodiscard]] spice::sct::SctEdge controlEdge(
    const spice::sct::SctEdgeType type,
    const std::uint32_t from,
    const std::uint32_t to) {
    return spice::sct::SctEdge{
        .type = type,
        .confidence = spice::sct::SctSemanticConfidence::Known,
        .fromOffset = from,
        .toOffset = to,
        .opcode = type == spice::sct::SctEdgeType::SwitchCase ? 3U : 0U,
    };
}

[[nodiscard]] spice::sct::SctSection scriptSection(
    const std::uint32_t index,
    std::string name,
    std::vector<spice::sct::SctInstruction> instructions,
    std::vector<spice::sct::SctEdge> edges = {}) {
    return spice::sct::SctSection{
        .id = spice::sct::SctSectionId{ .index = index, .name = std::move(name) },
        .kind = spice::sct::SctSectionKind::Script,
        .instructions = std::move(instructions),
        .edges = std::move(edges),
    };
}

[[nodiscard]] spice::sct::SctParseResult parseResultWithSections(
    std::vector<spice::sct::SctSection> sections) {
    spice::sct::SctParseResult result{};
    result.file.sections = std::move(sections);
    result.parseOk = true;
    return result;
}

[[nodiscard]] const NavigationStartOption* optionAtPosition(
    const NavigationStartCatalog& catalog,
    const NavigationVec3 expected) {
    const auto found = std::ranges::find_if(catalog.options, [&](const auto& option) {
        return option.position.has_value() &&
            option.position->x == expected.x &&
            option.position->y == expected.y &&
            option.position->z == expected.z;
    });
    return found == catalog.options.end() ? nullptr : &*found;
}

[[nodiscard]] const NavigationStartPreviousContext* previousContext(
    const NavigationStartPredicate& predicate,
    const std::int32_t rawValue) {
    const auto found = std::ranges::find_if(predicate.previousContexts, [&](const auto& context) {
        return context.rawValue == rawValue;
    });
    return found == predicate.previousContexts.end() ? nullptr : &*found;
}

TEST(NavigationAreaIdentity, DerivesCaseNormalizedRelatedSctName) {
    const auto identity = resolveNavigationAreaIdentity(
        std::filesystem::path("C:/fixture/A101B.MLD"));

    ASSERT_TRUE(identity.recognized);
    EXPECT_EQ(identity.areaKey, "101b");
    EXPECT_EQ(identity.expectedSctFileName, "me101b.sct");
    EXPECT_EQ(identity.expectedSctPath(), std::filesystem::path("C:/fixture/me101b.sct"));
    EXPECT_TRUE(identity.matchesSctPath("D:/other/ME101B.SCT"));
    EXPECT_FALSE(identity.matchesSctPath("D:/other/me201a.sct"));
}

TEST(NavigationAreaIdentity, RejectsNonNavigationNamesAndExtensions) {
    EXPECT_FALSE(resolveNavigationAreaIdentity("field.mld").recognized);
    EXPECT_FALSE(resolveNavigationAreaIdentity("a10b.mld").recognized);
    EXPECT_FALSE(resolveNavigationAreaIdentity("a101b.bin").recognized);
    EXPECT_FALSE(navigationSctAreaKey("me10b.sct").has_value());
    EXPECT_FALSE(navigationSctAreaKey("me101b.mld").has_value());
}

TEST(NavigationStartCatalogBuilder, GroupsExactLoopArrivalsAndPreservesSwitchConditions) {
    auto initializationBranch = simpleInstruction(0U, 0U);
    initializationBranch.parameters.push_back(expressionParameter(
        0U,
        logicalAst(
            comparisonAst(
                variableAst(spice::sct::SctScptAstNodeKind::BitVariable, 1910U),
                numericAst(0.0F)),
            comparisonAst(
                variableAst(spice::sct::SctScptAstNodeKind::BitVariable, 5U),
                numericAst(1.0F)),
            "&&"),
        "condition"));

    auto contextSwitch = simpleInstruction(100U, 3U);
    contextSwitch.parameters = {
        expressionParameter(0U, variableAst(spice::sct::SctScptAstNodeKind::IntVariable, 15U), "choice"),
        integerParameter(1U, 2, "caseCount"),
        integerParameter(2U, 1010, "caseValue"),
        integerParameter(3U, 0, "caseOffset"),
        integerParameter(4U, 20000, "caseValue"),
        integerParameter(5U, 0, "caseOffset"),
    };

    const auto placement = std::array{
        numericAst(5.0F), numericAst(23.5F), numericAst(64.0F), numericAst(146.5F), numericAst(-70.0F),
    };
    auto firstPlacement = placementInstruction(200U, placement);
    auto secondPlacement = placementInstruction(300U, placement);
    auto firstJump = simpleInstruction(250U, 10U);
    auto secondJump = simpleInstruction(350U, 10U);

    auto loop = scriptSection(0U, "loop", {
        std::move(initializationBranch),
        std::move(contextSwitch),
        std::move(firstPlacement),
        std::move(firstJump),
        std::move(secondPlacement),
        std::move(secondJump),
        simpleInstruction(400U, 12U),
    }, {
        controlEdge(spice::sct::SctEdgeType::BranchTrue, 0U, 100U),
        controlEdge(spice::sct::SctEdgeType::BranchFalse, 0U, 400U),
        controlEdge(spice::sct::SctEdgeType::SwitchCase, 100U, 200U),
        controlEdge(spice::sct::SctEdgeType::SwitchCase, 100U, 300U),
        controlEdge(spice::sct::SctEdgeType::Jump, 250U, 400U),
        controlEdge(spice::sct::SctEdgeType::Jump, 350U, 400U),
    });
    auto ignoredRestore = scriptSection(1U, "battle_return", {
        simpleInstruction(0U, 156U),
        simpleInstruction(4U, 12U),
    });
    const auto parsed = parseResultWithSections({ std::move(loop), std::move(ignoredRestore) });

    const auto catalog = NavigationStartCatalogBuilder{}.build(
        parsed, resolveNavigationAreaIdentity("C:/fixture/a101b.mld"));

    ASSERT_EQ(catalog.analyzedOpcode77Count, 2U);
    ASSERT_EQ(catalog.options.size(), 1U);
    const auto& option = catalog.options.front();
    EXPECT_EQ(option.kind, NavigationStartKind::AuthoredArrival);
    EXPECT_EQ(option.availability, NavigationStartAvailability::Resolvable);
    ASSERT_EQ(option.variants.size(), 2U);
    EXPECT_TRUE(std::ranges::all_of(option.variants, [](const auto& variant) {
        return variant.source.opcode == 77U && variant.conditionAnalysisComplete;
    }));
    EXPECT_TRUE(std::ranges::any_of(option.variants, [](const auto& variant) {
        return variant.conditionSummary.find("Save Load") != std::string::npos &&
            std::ranges::any_of(variant.predicates, [](const auto& predicate) {
                const auto* context = previousContext(predicate, 20000);
                return context != nullptr && context->label == "Save Load";
            });
    }));
}

TEST(NavigationStartCatalogBuilder, DoesNotTreatNonImplyingLoopOrConditionAsInitializationGate) {
    auto branch = simpleInstruction(0U, 0U);
    branch.parameters.push_back(expressionParameter(
        0U,
        logicalAst(
            comparisonAst(
                variableAst(spice::sct::SctScptAstNodeKind::BitVariable, 1910U),
                numericAst(0.0F)),
            comparisonAst(
                variableAst(spice::sct::SctScptAstNodeKind::BitVariable, 9U),
                numericAst(1.0F)),
            "||"),
        "condition"));
    auto loop = scriptSection(0U, "loop", {
        std::move(branch),
        placementInstruction(100U, {
            numericAst(0.0F), numericAst(1.0F), numericAst(2.0F), numericAst(3.0F), numericAst(4.0F),
        }),
        simpleInstruction(200U, 12U),
    }, {
        controlEdge(spice::sct::SctEdgeType::BranchTrue, 0U, 100U),
        controlEdge(spice::sct::SctEdgeType::BranchFalse, 0U, 200U),
    });
    const auto parsed = parseResultWithSections({ std::move(loop) });

    const auto catalog = NavigationStartCatalogBuilder{}.build(
        parsed, resolveNavigationAreaIdentity("C:/fixture/a101b.mld"));

    ASSERT_EQ(catalog.options.size(), 1U);
    EXPECT_EQ(catalog.options.front().kind, NavigationStartKind::ScriptedReposition);
}

TEST(NavigationStartCatalogBuilder, PreservesBranchPolarityAndUsesOpaqueMalformedFallbacks) {
    auto contextBranch = simpleInstruction(0U, 0U);
    contextBranch.parameters.push_back(expressionParameter(
        0U,
        logicalAst(
            comparisonAst(
                variableAst(spice::sct::SctScptAstNodeKind::IntVariable, 15U),
                numericAst(20000.0F)),
            comparisonAst(
                variableAst(spice::sct::SctScptAstNodeKind::BitVariable, 9U),
                numericAst(1.0F)),
            "&&"),
        "condition"));
    const auto sharedPlacement = std::array{
        numericAst(0.0F), numericAst(8.0F), numericAst(9.0F), numericAst(10.0F), numericAst(45.0F),
    };
    auto polarity = scriptSection(0U, "init", {
        std::move(contextBranch),
        placementInstruction(100U, sharedPlacement),
        simpleInstruction(150U, 10U),
        placementInstruction(200U, sharedPlacement),
        simpleInstruction(250U, 10U),
        simpleInstruction(300U, 12U),
    }, {
        controlEdge(spice::sct::SctEdgeType::BranchTrue, 0U, 100U),
        controlEdge(spice::sct::SctEdgeType::BranchFalse, 0U, 200U),
        controlEdge(spice::sct::SctEdgeType::Jump, 150U, 300U),
        controlEdge(spice::sct::SctEdgeType::Jump, 250U, 300U),
    });

    auto opaqueBranch = simpleInstruction(0U, 0U);
    opaqueBranch.parameters.push_back(expressionParameter(
        0U,
        spice::sct::SctScptAstNode{
            .kind = spice::sct::SctScptAstNodeKind::Unknown,
            .display = "unsupported predicate",
        },
        "condition"));
    auto opaque = scriptSection(1U, "opaque_event", {
        std::move(opaqueBranch),
        placementInstruction(100U, {
            numericAst(1.0F), numericAst(20.0F), numericAst(21.0F), numericAst(22.0F), numericAst(0.0F),
        }),
        simpleInstruction(200U, 12U),
    }, {
        controlEdge(spice::sct::SctEdgeType::BranchTrue, 0U, 100U),
        controlEdge(spice::sct::SctEdgeType::BranchFalse, 0U, 200U),
    });

    auto missingCall = simpleInstruction(0U, 11U);
    auto missingCallEdge = controlEdge(spice::sct::SctEdgeType::CallSubscript, 0U, 0U);
    missingCallEdge.attributes.emplace("target_section_index", "99");
    auto malformedCaller = scriptSection(2U, "malformed_event", {
        std::move(missingCall),
        placementInstruction(4U, {
            numericAst(2.0F), numericAst(30.0F), numericAst(31.0F), numericAst(32.0F), numericAst(0.0F),
        }),
        simpleInstruction(8U, 12U),
    }, { std::move(missingCallEdge) });
    const auto parsed = parseResultWithSections({
        std::move(polarity), std::move(opaque), std::move(malformedCaller),
    });

    const auto catalog = NavigationStartCatalogBuilder{}.build(
        parsed, resolveNavigationAreaIdentity("C:/fixture/a101b.mld"));

    const auto* polarityOption = optionAtPosition(catalog, NavigationVec3{ 8.0F, 9.0F, 10.0F });
    ASSERT_NE(polarityOption, nullptr);
    ASSERT_EQ(polarityOption->variants.size(), 2U);
    EXPECT_TRUE(std::ranges::any_of(polarityOption->variants, [](const auto& variant) {
        return std::ranges::any_of(variant.predicates, [](const auto& predicate) {
            return !predicate.negated && previousContext(predicate, 20000) != nullptr;
        });
    }));
    EXPECT_TRUE(std::ranges::any_of(polarityOption->variants, [](const auto& variant) {
        return std::ranges::any_of(variant.predicates, [](const auto& predicate) {
            return predicate.negated && previousContext(predicate, 20000) != nullptr &&
                predicate.summary.starts_with("NOT (");
        });
    }));

    const auto* opaqueOption = optionAtPosition(catalog, NavigationVec3{ 20.0F, 21.0F, 22.0F });
    ASSERT_NE(opaqueOption, nullptr);
    ASSERT_EQ(opaqueOption->variants.size(), 1U);
    EXPECT_FALSE(opaqueOption->variants.front().conditionAnalysisComplete);
    ASSERT_FALSE(opaqueOption->variants.front().predicates.empty());
    EXPECT_TRUE(opaqueOption->variants.front().predicates.front().opaque);
    EXPECT_TRUE(std::ranges::any_of(catalog.diagnostics, [](const auto& diagnostic) {
        return diagnostic.message.find("missing CallSubscript section target") != std::string::npos;
    }));
}

TEST(NavigationStartCatalogBuilder, PreservesEveryNestedPreviousContextValueAndSpecialLabel) {
    auto branch = simpleInstruction(0U, 0U);
    branch.parameters.push_back(expressionParameter(
        0U,
        logicalAst(
            comparisonAst(
                variableAst(spice::sct::SctScptAstNodeKind::IntVariable, 15U),
                numericAst(10000.0F)),
            comparisonAst(
                variableAst(spice::sct::SctScptAstNodeKind::IntVariable, 15U),
                numericAst(20000.0F)),
            "||"),
        "condition"));
    auto init = scriptSection(0U, "init", {
        std::move(branch),
        placementInstruction(100U, {
            numericAst(0.0F), numericAst(1.0F), numericAst(2.0F), numericAst(3.0F), numericAst(4.0F),
        }),
        simpleInstruction(200U, 12U),
    }, {
        controlEdge(spice::sct::SctEdgeType::BranchTrue, 0U, 100U),
        controlEdge(spice::sct::SctEdgeType::BranchFalse, 0U, 200U),
    });
    const auto catalog = NavigationStartCatalogBuilder{}.build(
        parseResultWithSections({ std::move(init) }),
        resolveNavigationAreaIdentity("C:/fixture/a101b.mld"));

    ASSERT_EQ(catalog.options.size(), 1U);
    ASSERT_EQ(catalog.options.front().variants.size(), 1U);
    const auto& predicate = catalog.options.front().variants.front().predicates.front();
    ASSERT_EQ(predicate.previousContexts.size(), 2U);
    const auto* battleReturn = previousContext(predicate, 10000);
    const auto* saveLoad = previousContext(predicate, 20000);
    ASSERT_NE(battleReturn, nullptr);
    ASSERT_NE(saveLoad, nullptr);
    EXPECT_EQ(battleReturn->label, "Battle Return");
    EXPECT_EQ(saveLoad->label, "Save Load");
    EXPECT_NE(predicate.summary.find("Battle Return"), std::string::npos);
    EXPECT_NE(predicate.summary.find("Save Load"), std::string::npos);
}

TEST(NavigationStartCatalogBuilder, TreatsNamedAndHeuristicEventCalleesAsExternalRoots) {
    auto namedCall = simpleInstruction(0U, 11U);
    auto namedCallEdge = controlEdge(spice::sct::SctEdgeType::CallSubscript, 0U, 0U);
    namedCallEdge.attributes.emplace("target_section_index", "1");
    auto heuristicCall = simpleInstruction(4U, 11U);
    auto heuristicCallEdge = controlEdge(spice::sct::SctEdgeType::CallSubscript, 4U, 0U);
    heuristicCallEdge.attributes.emplace("target_section_index", "2");
    auto init = scriptSection(0U, "init", {
        std::move(namedCall), std::move(heuristicCall), simpleInstruction(8U, 12U),
    }, { std::move(namedCallEdge), std::move(heuristicCallEdge) });
    auto namedEvent = scriptSection(1U, "ARRIVAL_EVENT", {
        placementInstruction(0U, {
            numericAst(0.0F), numericAst(10.0F), numericAst(11.0F), numericAst(12.0F), numericAst(0.0F),
        }),
        simpleInstruction(64U, 12U),
    });
    auto heuristicEvent = scriptSection(2U, "arrival_helper", {
        placementInstruction(0U, {
            numericAst(0.0F), numericAst(20.0F), numericAst(21.0F), numericAst(22.0F), numericAst(0.0F),
        }),
        simpleInstruction(64U, 12U),
    });
    heuristicEvent.heuristicEvidence.likelyCutscene = true;
    const auto catalog = NavigationStartCatalogBuilder{}.build(
        parseResultWithSections({ std::move(init), std::move(namedEvent), std::move(heuristicEvent) }),
        resolveNavigationAreaIdentity("C:/fixture/a101b.mld"));

    ASSERT_EQ(catalog.options.size(), 4U);
    for (const auto expectedPosition : {
             NavigationVec3{ 10.0F, 11.0F, 12.0F },
             NavigationVec3{ 20.0F, 21.0F, 22.0F },
         }) {
        EXPECT_EQ(std::ranges::count_if(catalog.options, [&](const auto& option) {
            return option.position.has_value() &&
                option.position->x == expectedPosition.x &&
                option.position->y == expectedPosition.y &&
                option.position->z == expectedPosition.z;
        }), 2U);
        EXPECT_TRUE(std::ranges::any_of(catalog.options, [&](const auto& option) {
            return option.kind == NavigationStartKind::ScriptedReposition &&
                option.position.has_value() &&
                option.position->x == expectedPosition.x &&
                std::ranges::any_of(option.variants, [](const auto& variant) {
                    return variant.callPath.empty();
                });
        }));
    }
}

TEST(NavigationStartCatalogBuilder, PropagatesInitCallsAndRetainsCycleDiagnostics) {
    auto call = simpleInstruction(0U, 11U);
    auto callEdge = controlEdge(spice::sct::SctEdgeType::CallSubscript, 0U, 0U);
    callEdge.attributes.emplace("target_section_index", "1");
    auto init = scriptSection(0U, "init", {
        std::move(call),
        simpleInstruction(4U, 12U),
    }, { std::move(callEdge) });

    auto recursiveCall = simpleInstruction(10U, 11U);
    auto recursiveEdge = controlEdge(spice::sct::SctEdgeType::CallSubscript, 10U, 0U);
    recursiveEdge.attributes.emplace("target_section_index", "1");
    auto callee = scriptSection(1U, "arrival_helper", {
        placementInstruction(0U, {
            numericAst(0.0F), numericAst(1.0F), numericAst(2.0F), numericAst(3.0F), numericAst(90.0F),
        }),
        std::move(recursiveCall),
        simpleInstruction(20U, 12U),
    }, { std::move(recursiveEdge) });
    auto event = scriptSection(2U, "NYUJO_EVENT", {
        placementInstruction(0U, {
            numericAst(0.0F), numericAst(-32.0F), numericAst(16.0F), numericAst(-104.0F), numericAst(180.0F),
        }),
        simpleInstruction(64U, 12U),
    });
    auto eventCall = simpleInstruction(0U, 11U);
    auto eventCallEdge = controlEdge(spice::sct::SctEdgeType::CallSubscript, 0U, 0U);
    eventCallEdge.attributes.emplace("target_section_index", "1");
    auto eventCaller = scriptSection(3U, "alternate_event", {
        std::move(eventCall),
        simpleInstruction(4U, 12U),
    }, { std::move(eventCallEdge) });
    const auto parsed = parseResultWithSections({
        std::move(init), std::move(callee), std::move(event), std::move(eventCaller),
    });

    const auto catalog = NavigationStartCatalogBuilder{}.build(
        parsed, resolveNavigationAreaIdentity("C:/fixture/a101b.mld"));

    ASSERT_EQ(catalog.analyzedOpcode77Count, 2U);
    ASSERT_EQ(catalog.options.size(), 3U);
    const auto* arrival = optionAtPosition(catalog, NavigationVec3{ 1.0F, 2.0F, 3.0F });
    ASSERT_NE(arrival, nullptr);
    EXPECT_EQ(arrival->kind, NavigationStartKind::AuthoredArrival);
    EXPECT_TRUE(std::ranges::any_of(arrival->variants, [](const auto& variant) {
        return !variant.callPath.empty() && variant.callPath.front().calleeSectionName == "arrival_helper";
    }));
    const auto* eventOption = optionAtPosition(catalog, NavigationVec3{ -32.0F, 16.0F, -104.0F });
    ASSERT_NE(eventOption, nullptr);
    EXPECT_EQ(eventOption->kind, NavigationStartKind::ScriptedReposition);
    EXPECT_EQ(std::ranges::count_if(catalog.options, [](const auto& option) {
        return option.position.has_value() &&
            option.position->x == 1.0F && option.position->y == 2.0F && option.position->z == 3.0F;
    }), 2U);
    EXPECT_TRUE(std::ranges::any_of(catalog.options, [](const auto& option) {
        return option.kind == NavigationStartKind::ScriptedReposition &&
            std::ranges::any_of(option.variants, [](const auto& variant) {
                return !variant.callPath.empty() &&
                    variant.callPath.front().callerSectionName == "alternate_event";
            });
    }));
    EXPECT_TRUE(std::ranges::any_of(catalog.diagnostics, [](const auto& diagnostic) {
        return diagnostic.message.find("recursive") != std::string::npos;
    }));
}

TEST(NavigationStartCatalogBuilder, ConstantFoldsPlacementValuesAndKeepsIncompleteOccurrences) {
    auto folded = placementInstruction(0U, {
        arithmeticAst(numericAst(2.0F), numericAst(3.0F), "+"),
        arithmeticAst(numericAst(10.0F), numericAst(2.0F), "+"),
        numericAst(16.0F),
        numericAst(-86.0F),
        variableAst(spice::sct::SctScptAstNodeKind::IntVariable, 9U),
    });
    folded.decodeOk = false;
    auto incomplete = placementInstruction(100U, {
        numericAst(0.0F),
        variableAst(spice::sct::SctScptAstNodeKind::IntVariable, 3U),
        numericAst(0.0F),
        numericAst(0.0F),
        numericAst(0.0F),
    });
    incomplete.parameters[0] = spice::sct::SctParameter{
        .index = 0U,
        .valueKind = spice::sct::SctParameterValueKind::Raw,
        .rawWords = { 0x7fffffffU },
        .displayValue = "0x7fffffff",
    };
    auto event = scriptSection(0U, "event", {
        std::move(folded),
        std::move(incomplete),
        simpleInstruction(200U, 12U),
    });
    const auto parsed = parseResultWithSections({ std::move(event) });

    const auto catalog = NavigationStartCatalogBuilder{}.build(
        parsed, resolveNavigationAreaIdentity("C:/fixture/a101b.mld"));

    ASSERT_EQ(catalog.analyzedOpcode77Count, 2U);
    ASSERT_EQ(catalog.options.size(), 2U);
    EXPECT_EQ(catalog.incompleteOpcode77Count, 1U);
    const auto* foldedOption = optionAtPosition(catalog, NavigationVec3{ 12.0F, 16.0F, -86.0F });
    ASSERT_NE(foldedOption, nullptr);
    EXPECT_EQ(foldedOption->groundTblId, 5);
    EXPECT_FALSE(foldedOption->yawDegrees.has_value());
    EXPECT_EQ(foldedOption->availability, NavigationStartAvailability::Resolvable);
    EXPECT_TRUE(std::ranges::any_of(catalog.diagnostics, [](const auto& diagnostic) {
        return diagnostic.message.find("incomplete instruction decoding") != std::string::npos &&
            diagnostic.message.find("remain selectable") != std::string::npos;
    }));
    const auto incompleteOption = std::ranges::find_if(catalog.options, [](const auto& option) {
        return option.availability == NavigationStartAvailability::Incomplete;
    });
    ASSERT_NE(incompleteOption, catalog.options.end());
    EXPECT_FALSE(incompleteOption->groundTblId.has_value());
    EXPECT_FALSE(incompleteOption->position.has_value());
    EXPECT_NE(incompleteOption->unavailableReason.find("sentinel"), std::string::npos);
}

TEST(NavigationStartCatalogBuilder, CorroboratesNormalPreviousAreaLabelsAgainstSiblingFiles) {
    TemporaryDirectory directory{};
    std::ofstream(directory.path() / "a099a.mld", std::ios::binary).put('\0');
    const auto identity = resolveNavigationAreaIdentity(directory.path() / "a101b.mld");

    auto branch = simpleInstruction(0U, 0U);
    branch.parameters.push_back(expressionParameter(
        0U,
        comparisonAst(
            variableAst(spice::sct::SctScptAstNodeKind::IntVariable, 15U),
            numericAst(990.0F)),
        "condition"));
    auto init = scriptSection(0U, "init", {
        std::move(branch),
        placementInstruction(100U, {
            numericAst(0.0F), numericAst(1.0F), numericAst(2.0F), numericAst(3.0F), numericAst(4.0F),
        }),
        simpleInstruction(200U, 12U),
    }, {
        controlEdge(spice::sct::SctEdgeType::BranchTrue, 0U, 100U),
        controlEdge(spice::sct::SctEdgeType::BranchFalse, 0U, 200U),
    });
    const auto parsed = parseResultWithSections({ std::move(init) });

    const auto corroborated = NavigationStartCatalogBuilder{}.build(parsed, identity);
    ASSERT_EQ(corroborated.options.size(), 1U);
    ASSERT_EQ(corroborated.options.front().variants.size(), 1U);
    ASSERT_EQ(corroborated.options.front().variants.front().predicates.size(), 1U);
    const auto* corroboratedContext = previousContext(
        corroborated.options.front().variants.front().predicates.front(), 990);
    ASSERT_NE(corroboratedContext, nullptr);
    EXPECT_EQ(corroboratedContext->label, "Previous area a099a");

    const auto uncorroborated = NavigationStartCatalogBuilder{}.build(
        parsed, resolveNavigationAreaIdentity(directory.path() / "other" / "a101b.mld"));
    ASSERT_EQ(uncorroborated.options.size(), 1U);
    const auto* uncorroboratedContext = previousContext(
        uncorroborated.options.front().variants.front().predicates.front(), 990);
    ASSERT_NE(uncorroboratedContext, nullptr);
    EXPECT_TRUE(uncorroboratedContext->label.empty());
}

TEST(NavigationScriptLoader, MissingSiblingIsRecoverableAndNamesExpectedFile) {
    TemporaryDirectory directory{};
    const auto identity = resolveNavigationAreaIdentity(directory.path() / "a101b.mld");

    const auto result = NavigationScriptLoader{}.discoverAndLoad(identity);

    EXPECT_EQ(result.model.associationStatus, NavigationScriptAssociationStatus::Missing);
    EXPECT_EQ(result.model.loadStatus, NavigationScriptLoadStatus::NotAttempted);
    EXPECT_EQ(result.model.expectedPath, directory.path() / "me101b.sct");
    EXPECT_FALSE(result.hasDocument());
    EXPECT_TRUE(hasDiagnosticContaining(result, "Related SCT was not found"));
}

TEST(NavigationScriptLoader, MismatchedManualSelectionIsRejectedBeforeParsing) {
    TemporaryDirectory directory{};
    const auto identity = resolveNavigationAreaIdentity(directory.path() / "a101b.mld");
    const auto mismatched = directory.path() / "me201a.sct";
    std::ofstream(mismatched, std::ios::binary).put('\0');

    const auto result = NavigationScriptLoader{}.loadMatchingFile(identity, mismatched);

    EXPECT_EQ(result.model.associationStatus, NavigationScriptAssociationStatus::Rejected);
    EXPECT_EQ(result.model.loadStatus, NavigationScriptLoadStatus::NotAttempted);
    EXPECT_FALSE(result.hasDocument());
    EXPECT_TRUE(hasDiagnosticContaining(result, "expected me101b.sct"));
}

TEST(NavigationScriptLoader, MatchingMalformedFileReportsParseFailure) {
    TemporaryDirectory directory{};
    const auto identity = resolveNavigationAreaIdentity(directory.path() / "a101b.mld");
    const auto malformed = directory.path() / "me101b.sct";
    std::ofstream(malformed, std::ios::binary).put('\0');

    const auto result = NavigationScriptLoader{}.loadMatchingFile(identity, malformed);

    EXPECT_EQ(result.model.associationStatus, NavigationScriptAssociationStatus::Matched);
    EXPECT_EQ(result.model.loadStatus, NavigationScriptLoadStatus::Failed);
    EXPECT_FALSE(result.hasDocument());
}

TEST(NavigationScriptLoader, MatchingUnreadablePathReportsInspectionFailure) {
    TemporaryDirectory directory{};
    const auto identity = resolveNavigationAreaIdentity(directory.path() / "a101b.mld");
    const auto unreadable = directory.path() / "me101b.sct";
    std::filesystem::create_directory(unreadable);

    const auto result = NavigationScriptLoader{}.loadMatchingFile(identity, unreadable);

    EXPECT_EQ(result.model.associationStatus, NavigationScriptAssociationStatus::Matched);
    EXPECT_EQ(result.model.loadStatus, NavigationScriptLoadStatus::Failed);
    EXPECT_FALSE(result.hasDocument());
    EXPECT_TRUE(hasDiagnosticContaining(result, "Could not inspect SCT file"));
}

TEST(NavigationScriptLoader, TruncatedAklzReportsParseFailure) {
    TemporaryDirectory directory{};
    const auto identity = resolveNavigationAreaIdentity(directory.path() / "a101b.mld");
    const auto truncated = directory.path() / "me101b.sct";
    constexpr std::array<unsigned char, 16> bytes{
        'A', 'K', 'L', 'Z', '~', '?', 'Q', 'd', '=', 0xCC, 0xCC, 0xCD,
        0x00, 0x00, 0x00, 0x04,
    };
    {
        std::ofstream output(truncated, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    const auto result = NavigationScriptLoader{}.loadMatchingFile(identity, truncated);

    EXPECT_EQ(result.model.associationStatus, NavigationScriptAssociationStatus::Matched);
    EXPECT_EQ(result.model.loadStatus, NavigationScriptLoadStatus::Failed);
    EXPECT_FALSE(result.hasDocument());
}

TEST(NavigationScriptLoader, Me101bFixtureRetainsParsedDocumentAndLoopSummary) {
    const char* fixturePath = std::getenv("SAVOR_NAV_ME101B_SCT");
    if (fixturePath == nullptr || *fixturePath == '\0' || !std::filesystem::is_regular_file(fixturePath)) {
        GTEST_SKIP() << "Set SAVOR_NAV_ME101B_SCT to the extracted me101b.sct fixture.";
    }

    // A valid manual replacement may live outside the MLD directory as long as
    // its prefix-stripped area key matches.
    const auto identity = resolveNavigationAreaIdentity(
        std::filesystem::temp_directory_path() / "manual-area" / "a101b.mld");
    const auto result = NavigationScriptLoader{}.loadMatchingFile(identity, fixturePath);

    ASSERT_TRUE(result.hasDocument());
    EXPECT_NE(result.model.loadStatus, NavigationScriptLoadStatus::Failed);
    EXPECT_TRUE(result.model.originalCompressedAklz);
    EXPECT_EQ(result.model.sections.size(), 51U);
    EXPECT_EQ(result.model.exactLoopSectionCount, 1U);
    const auto loop = std::find_if(result.model.sections.begin(), result.model.sections.end(), [](const auto& section) {
        return section.name == "loop";
    });
    ASSERT_NE(loop, result.model.sections.end());
    EXPECT_EQ(loop->instructionCount, 20U);

    const auto& catalog = result.model.startCatalog;
    EXPECT_EQ(catalog.analyzedOpcode77Count, 3U);
    EXPECT_EQ(catalog.incompleteOpcode77Count, 0U);
    ASSERT_EQ(catalog.options.size(), 3U);

    const auto* saveLoad = optionAtPosition(catalog, NavigationVec3{ 23.5F, 64.0F, 146.5F });
    ASSERT_NE(saveLoad, nullptr);
    EXPECT_EQ(saveLoad->kind, NavigationStartKind::AuthoredArrival);
    EXPECT_EQ(saveLoad->groundTblId, 5);
    EXPECT_EQ(saveLoad->yawDegrees, -70.0F);
    EXPECT_TRUE(std::ranges::any_of(saveLoad->variants, [](const auto& variant) {
        return variant.conditionSummary.find("Save Load") != std::string::npos;
    }));

    const auto* normalFallback = optionAtPosition(catalog, NavigationVec3{ -64.0F, 16.0F, -86.0F });
    ASSERT_NE(normalFallback, nullptr);
    EXPECT_EQ(normalFallback->kind, NavigationStartKind::AuthoredArrival);
    EXPECT_EQ(normalFallback->groundTblId, 0);
    EXPECT_EQ(normalFallback->yawDegrees, 180.0F);

    const auto* eventPlacement = optionAtPosition(catalog, NavigationVec3{ -32.0F, 16.0F, -104.0F });
    ASSERT_NE(eventPlacement, nullptr);
    EXPECT_EQ(eventPlacement->kind, NavigationStartKind::ScriptedReposition);
    EXPECT_EQ(eventPlacement->groundTblId, 0);
    EXPECT_EQ(eventPlacement->yawDegrees, 180.0F);
    EXPECT_TRUE(std::ranges::all_of(catalog.options, [](const auto& option) {
        return std::ranges::all_of(option.variants, [](const auto& variant) {
            return variant.source.opcode == 77U;
        });
    }));
}

TEST(NavigationScriptLoader, Me201aFixtureRetainsParsedDocumentAndLoopSummary) {
    const char* fixturePath = std::getenv("SAVOR_NAV_ME201A_SCT");
    if (fixturePath == nullptr || *fixturePath == '\0' || !std::filesystem::is_regular_file(fixturePath)) {
        GTEST_SKIP() << "Set SAVOR_NAV_ME201A_SCT to the extracted me201a.sct fixture.";
    }

    const auto identity = resolveNavigationAreaIdentity(
        std::filesystem::path(fixturePath).parent_path() / "a201a.mld");
    const auto result = NavigationScriptLoader{}.loadMatchingFile(identity, fixturePath);

    ASSERT_TRUE(result.hasDocument());
    EXPECT_NE(result.model.loadStatus, NavigationScriptLoadStatus::Failed);
    EXPECT_TRUE(result.model.originalCompressedAklz);
    EXPECT_EQ(result.model.sections.size(), 115U);
    EXPECT_EQ(result.model.exactLoopSectionCount, 1U);
    const auto loop = std::find_if(result.model.sections.begin(), result.model.sections.end(), [](const auto& section) {
        return section.name == "loop";
    });
    ASSERT_NE(loop, result.model.sections.end());
    EXPECT_EQ(loop->instructionCount, 20U);
}

TEST(NavigationStartCatalogBuilder, Me004aFixtureRetainsInitializationAndContextStructure) {
    const char* fixturePath = std::getenv("SAVOR_NAV_ME004A_SCT");
    if (fixturePath == nullptr || *fixturePath == '\0' || !std::filesystem::is_regular_file(fixturePath)) {
        GTEST_SKIP() << "Set SAVOR_NAV_ME004A_SCT to the extracted me004a.sct fixture.";
    }

    const auto parsed = spice::sct::SctParser{}.parseFile(fixturePath);
    ASSERT_TRUE(parsed.parseOk);
    const auto identity = resolveNavigationAreaIdentity(
        std::filesystem::path(fixturePath).parent_path() / "a004a.mld");
    const auto catalog = NavigationStartCatalogBuilder{}.build(parsed, identity);

    std::size_t rawOpcode77Count = 0;
    std::size_t branchEdgeCount = 0;
    for (const auto& section : parsed.file.sections) {
        rawOpcode77Count += std::ranges::count_if(section.instructions, [](const auto& instruction) {
            return instruction.opcode == 77U;
        });
        branchEdgeCount += std::ranges::count_if(section.edges, [](const auto& edge) {
            return edge.type == spice::sct::SctEdgeType::BranchTrue ||
                edge.type == spice::sct::SctEdgeType::BranchFalse ||
                edge.type == spice::sct::SctEdgeType::SwitchCase;
        });
    }

    ASSERT_GT(rawOpcode77Count, 0U);
    EXPECT_EQ(catalog.analyzedOpcode77Count, rawOpcode77Count);
    EXPECT_GT(branchEdgeCount, rawOpcode77Count);
    EXPECT_TRUE(std::ranges::all_of(catalog.options, [](const auto& option) {
        return std::ranges::all_of(option.variants, [](const auto& variant) {
            return variant.source.opcode == 77U;
        });
    }));
    EXPECT_TRUE(std::ranges::any_of(catalog.options, [](const auto& option) {
        return option.kind == NavigationStartKind::AuthoredArrival &&
            std::ranges::any_of(option.variants, [](const auto& variant) {
                return variant.conditionSummary.find("BitVar: 1910") != std::string::npos;
            });
    }));
    EXPECT_TRUE(std::ranges::any_of(catalog.options, [](const auto& option) {
        return std::ranges::any_of(option.variants, [](const auto& variant) {
            return std::ranges::any_of(variant.predicates, [](const auto& predicate) {
                return !predicate.previousContexts.empty();
            });
        });
    }));
    EXPECT_TRUE(std::ranges::any_of(catalog.options, [](const auto& option) {
        return std::ranges::any_of(option.variants, [](const auto& variant) {
            return variant.predicates.size() >= 2U;
        });
    }));
    EXPECT_TRUE(std::ranges::any_of(catalog.options, [](const auto& option) {
        return option.variants.size() >= 2U;
    }));
}

TEST(NavigationScenarioLoader, A101bAutomaticallyLoadsMatchingSiblingScriptAndBuildsGraph) {
    const char* fixturePath = std::getenv("SAVOR_NAV_A101B_MLD");
    if (fixturePath == nullptr || *fixturePath == '\0' || !std::filesystem::is_regular_file(fixturePath)) {
        GTEST_SKIP() << "Set SAVOR_NAV_A101B_MLD to the extracted a101b.mld fixture.";
    }
    const auto expectedSct = std::filesystem::path(fixturePath).parent_path() / "me101b.sct";
    if (!std::filesystem::is_regular_file(expectedSct)) {
        GTEST_SKIP() << "The a101b.mld fixture directory does not contain me101b.sct.";
    }

    const auto result = NavigationScenarioLoader{}.loadFile(fixturePath);

    ASSERT_TRUE(result.hasModel());
    EXPECT_TRUE(result.model->identity.recognized);
    EXPECT_EQ(result.model->identity.areaKey, "101b");
    EXPECT_EQ(result.model->script.associationStatus, NavigationScriptAssociationStatus::Matched);
    EXPECT_TRUE(result.model->script.hasDocument());
    EXPECT_EQ(result.model->script.source.path.filename(), "me101b.sct");
    EXPECT_EQ(result.model->script.sections.size(), 51U);
    EXPECT_EQ(result.model->script.exactLoopSectionCount, 1U);
    EXPECT_FALSE(result.model->traversalGraph.nodes.empty());
    EXPECT_TRUE(result.model->isPathfindingReady());
}

TEST(NavigationScenarioLoader, MissingRelatedScriptDoesNotDisableManualPathfinding) {
    const char* fixturePath = std::getenv("SAVOR_NAV_A101B_MLD");
    if (fixturePath == nullptr || *fixturePath == '\0' || !std::filesystem::is_regular_file(fixturePath)) {
        GTEST_SKIP() << "Set SAVOR_NAV_A101B_MLD to the extracted a101b.mld fixture.";
    }

    TemporaryDirectory directory{};
    const auto isolatedMld = directory.path() / "a101b.mld";
    std::filesystem::copy_file(fixturePath, isolatedMld);

    const auto result = NavigationScenarioLoader{}.loadFile(isolatedMld);

    ASSERT_TRUE(result.hasModel());
    EXPECT_EQ(result.status, NavigationScenarioLoadStatus::Partial);
    EXPECT_EQ(result.model->script.associationStatus, NavigationScriptAssociationStatus::Missing);
    EXPECT_FALSE(result.model->script.hasDocument());
    EXPECT_TRUE(result.model->isPathfindingReady());
}

} // namespace
} // namespace savor::navigation
