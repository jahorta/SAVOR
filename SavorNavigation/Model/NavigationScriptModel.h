#pragma once

#include "NavigationAreaModel.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace savor::navigation {

class NavigationScriptDocument;

enum class NavigationScriptAssociationStatus {
    Unavailable,
    Missing,
    Matched,
    Rejected,
};

enum class NavigationScriptLoadStatus {
    NotAttempted,
    Complete,
    Partial,
    Failed,
};

struct NavigationScriptSourceIdentity {
    std::filesystem::path path{};
    std::uintmax_t byteSize = 0;
};

struct NavigationScriptSectionSummary {
    std::uint32_t index = 0;
    std::string name{};
    std::uint32_t startOffset = 0;
    std::uint32_t endOffset = 0;
    std::size_t instructionCount = 0;
};

enum class NavigationStartKind {
    AuthoredArrival,
    ScriptedReposition,
};

enum class NavigationStartAvailability {
    Resolvable,
    Incomplete,
};

struct NavigationStartPreviousContext {
    std::int32_t rawValue = 0;
    std::string label{};
};

struct NavigationStartPredicate {
    std::string summary{};
    bool negated = false;
    bool opaque = false;
    std::vector<NavigationStartPreviousContext> previousContexts{};
};

struct NavigationStartCallFrame {
    std::uint32_t callerSectionIndex = 0;
    std::string callerSectionName{};
    std::uint32_t instructionOffset = 0;
    std::uint32_t calleeSectionIndex = 0;
    std::string calleeSectionName{};
};

struct NavigationStartSourceProvenance {
    std::uint32_t sectionIndex = 0;
    std::string sectionName{};
    std::uint32_t instructionOffset = 0;
    std::uint32_t instructionPayloadOffset = 0;
    std::uint16_t opcode = 77;
};

struct NavigationStartVariant {
    NavigationStartSourceProvenance source{};
    std::vector<NavigationStartPredicate> predicates{};
    std::vector<NavigationStartCallFrame> callPath{};
    std::string conditionSummary{};
    std::string callPathSummary{};
    bool conditionAnalysisComplete = true;
};

struct NavigationStartOption {
    std::string id{};
    std::string label{};
    NavigationStartKind kind = NavigationStartKind::ScriptedReposition;
    NavigationStartAvailability availability = NavigationStartAvailability::Incomplete;
    std::optional<std::int32_t> groundTblId{};
    std::optional<NavigationVec3> position{};
    std::optional<float> yawDegrees{};
    std::string unavailableReason{};
    std::vector<NavigationStartVariant> variants{};
};

struct NavigationStartCatalog {
    std::vector<NavigationStartOption> options{};
    std::vector<NavigationDiagnostic> diagnostics{};
    std::size_t analyzedOpcode77Count = 0;
    std::size_t incompleteOpcode77Count = 0;
};

struct NavigationScriptModel {
    NavigationScriptSourceIdentity source{};
    std::filesystem::path expectedPath{};
    NavigationScriptAssociationStatus associationStatus = NavigationScriptAssociationStatus::Unavailable;
    NavigationScriptLoadStatus loadStatus = NavigationScriptLoadStatus::NotAttempted;
    std::vector<NavigationScriptSectionSummary> sections{};
    std::vector<NavigationDiagnostic> diagnostics{};
    std::size_t totalInstructionCount = 0;
    std::size_t exactLoopSectionCount = 0;
    bool originalCompressedAklz = false;
    NavigationStartCatalog startCatalog{};
    std::shared_ptr<const NavigationScriptDocument> document{};

    [[nodiscard]] bool hasDocument() const noexcept {
        return document != nullptr;
    }
};

} // namespace savor::navigation
