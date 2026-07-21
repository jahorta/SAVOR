#include "NavigationScriptLoader.h"

#include "../Analysis/NavigationStartCatalogBuilder.h"

#include "SpiceSCT/SctParser.h"

#include <algorithm>
#include <exception>
#include <system_error>
#include <utility>

namespace savor::navigation {

class NavigationScriptDocument final {
public:
    explicit NavigationScriptDocument(spice::sct::SctParseResult result)
        : parseResult(std::move(result)) {
    }

    spice::sct::SctParseResult parseResult{};
};

namespace {

void appendDiagnostic(std::vector<NavigationDiagnostic>& diagnostics,
    const NavigationDiagnosticSeverity severity,
    std::string message) {
    diagnostics.push_back(NavigationDiagnostic{
        .severity = severity,
        .message = std::move(message),
    });
}

[[nodiscard]] std::string displayPath(const std::filesystem::path& path) {
    return path.u8string().empty()
        ? std::string("(empty path)")
        : path.string();
}

} // namespace

NavigationScriptLoadResult NavigationScriptLoader::discoverAndLoad(
    const NavigationAreaIdentity& identity) const {
    NavigationScriptLoadResult result{};
    result.model.expectedPath = identity.expectedSctPath();

    if (!identity.recognized) {
        result.model.associationStatus = NavigationScriptAssociationStatus::Unavailable;
        appendDiagnostic(result.diagnostics, NavigationDiagnosticSeverity::Warning,
            "The MLD filename does not match aNNNC.mld; a related SCT cannot be identified.");
        result.model.diagnostics = result.diagnostics;
        return result;
    }

    std::error_code error{};
    const bool exists = std::filesystem::is_regular_file(result.model.expectedPath, error);
    if (!exists) {
        result.model.associationStatus = NavigationScriptAssociationStatus::Missing;
        result.model.loadStatus = NavigationScriptLoadStatus::NotAttempted;
        const std::string suffix = error
            ? " (filesystem error: " + error.message() + ")"
            : std::string{};
        appendDiagnostic(result.diagnostics, NavigationDiagnosticSeverity::Warning,
            "Related SCT was not found beside the MLD. Expected: " +
                displayPath(result.model.expectedPath) + suffix);
        result.model.diagnostics = result.diagnostics;
        return result;
    }

    return loadMatchingFile(identity, result.model.expectedPath);
}

NavigationScriptLoadResult NavigationScriptLoader::loadMatchingFile(
    const NavigationAreaIdentity& identity,
    const std::filesystem::path& sctPath) const {
    NavigationScriptLoadResult result{};
    result.model.source.path = sctPath;
    result.model.expectedPath = identity.expectedSctPath();

    if (!identity.recognized || !identity.matchesSctPath(sctPath)) {
        result.model.associationStatus = NavigationScriptAssociationStatus::Rejected;
        result.model.loadStatus = NavigationScriptLoadStatus::NotAttempted;
        const std::string expected = identity.recognized
            ? identity.expectedSctFileName
            : std::string("an SCT matching a recognized aNNNC.mld");
        appendDiagnostic(result.diagnostics, NavigationDiagnosticSeverity::Error,
            "Rejected SCT '" + sctPath.filename().string() + "'; expected " + expected + ".");
        result.model.diagnostics = result.diagnostics;
        return result;
    }

    result.model.associationStatus = NavigationScriptAssociationStatus::Matched;
    std::error_code regularFileError{};
    const bool isRegularFile = std::filesystem::is_regular_file(sctPath, regularFileError);
    if (!isRegularFile) {
        result.model.loadStatus = NavigationScriptLoadStatus::Failed;
        const std::string detail = regularFileError
            ? regularFileError.message()
            : std::string("the path is not a regular file");
        appendDiagnostic(result.diagnostics, NavigationDiagnosticSeverity::Error,
            "Could not inspect SCT file '" + displayPath(sctPath) + "': " + detail);
        result.model.diagnostics = result.diagnostics;
        return result;
    }

    std::error_code sizeError{};
    result.model.source.byteSize = std::filesystem::file_size(sctPath, sizeError);
    if (sizeError) {
        result.model.loadStatus = NavigationScriptLoadStatus::Failed;
        appendDiagnostic(result.diagnostics, NavigationDiagnosticSeverity::Error,
            "Could not inspect SCT file '" + displayPath(sctPath) + "': " + sizeError.message());
        result.model.diagnostics = result.diagnostics;
        return result;
    }

    try {
        spice::sct::SctParser parser{};
        auto parseResult = parser.parseFile(sctPath.string());
        result.model.originalCompressedAklz = parseResult.file.originalCompressedAklz;
        result.model.sections.reserve(parseResult.file.sections.size());
        for (const auto& section : parseResult.file.sections) {
            result.model.totalInstructionCount += section.instructions.size();
            if (section.id.name == "loop") {
                ++result.model.exactLoopSectionCount;
            }
            result.model.sections.push_back(NavigationScriptSectionSummary{
                .index = section.id.index,
                .name = section.id.name,
                .startOffset = section.startOffset,
                .endOffset = section.endOffset,
                .instructionCount = section.instructions.size(),
            });
        }

        if (parseResult.parseOk) {
            result.model.startCatalog = NavigationStartCatalogBuilder{}.build(parseResult, identity);
            result.diagnostics.insert(
                result.diagnostics.end(),
                result.model.startCatalog.diagnostics.begin(),
                result.model.startCatalog.diagnostics.end());
        }

        for (const auto& diagnostic : parseResult.diagnostics) {
            std::string message = diagnostic.message;
            if (!diagnostic.section.empty()) {
                message += " [section=" + diagnostic.section + "]";
            }
            message += " [offset=" + std::to_string(diagnostic.offset) + "]";
            appendDiagnostic(result.diagnostics,
                parseResult.parseOk ? NavigationDiagnosticSeverity::Warning : NavigationDiagnosticSeverity::Error,
                std::move(message));
        }

        result.model.loadStatus = parseResult.parseOk
            ? (parseResult.diagnostics.empty()
                ? NavigationScriptLoadStatus::Complete
                : NavigationScriptLoadStatus::Partial)
            : NavigationScriptLoadStatus::Failed;
        if (parseResult.parseOk) {
            result.model.document = std::make_shared<NavigationScriptDocument>(std::move(parseResult));
            appendDiagnostic(result.diagnostics, NavigationDiagnosticSeverity::Info,
                "Loaded related SCT '" + sctPath.filename().string() + "' with " +
                    std::to_string(result.model.sections.size()) + " section(s) and " +
                    std::to_string(result.model.totalInstructionCount) + " instruction(s).");
        } else {
            appendDiagnostic(result.diagnostics, NavigationDiagnosticSeverity::Error,
                "SpiceSCT could not parse related SCT '" + displayPath(sctPath) + "'.");
        }
    } catch (const std::exception& exception) {
        result.model.loadStatus = NavigationScriptLoadStatus::Failed;
        appendDiagnostic(result.diagnostics, NavigationDiagnosticSeverity::Error,
            "Could not load related SCT '" + displayPath(sctPath) + "': " + exception.what());
    }

    result.model.diagnostics = result.diagnostics;
    return result;
}

} // namespace savor::navigation
