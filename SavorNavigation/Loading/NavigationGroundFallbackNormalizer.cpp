#include "NavigationGroundFallbackNormalizer.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>

namespace savor::navigation::detail {
namespace {

[[nodiscard]] const NavigationGroundFallbackEntryEvidence* resolveTargetEntry(
    const std::span<const NavigationGroundFallbackEntryEvidence> entries,
    const std::uint32_t targetEntryId) noexcept {
    if (targetEntryId >= entries.size()) {
        return nullptr;
    }
    if (entries[targetEntryId].entryId == targetEntryId) {
        return &entries[targetEntryId];
    }
    const auto found = std::find_if(entries.begin(), entries.end(), [targetEntryId](const auto& entry) {
        return entry.entryId == targetEntryId;
    });
    return found == entries.end() ? nullptr : &*found;
}

void appendDiagnostic(
    NavigationGroundFallbackNormalizationResult& result,
    const NavigationDiagnosticSeverity severity,
    std::string message) {
    result.diagnostics.push_back(NavigationDiagnostic{
        .severity = severity,
        .message = std::move(message),
    });
}

} // namespace

NavigationGroundFallbackNormalizationResult NavigationGroundFallbackNormalizer::normalize(
    const std::span<const NavigationGroundFallbackEntryEvidence> entries,
    const std::span<const NavigationSurface> surfaces) const {
    NavigationGroundFallbackNormalizationResult result{};
    std::unordered_map<std::size_t, std::vector<NavigationSurfaceSourceKey>> surfacesByTableIndex{};
    for (const NavigationSurface& surface : surfaces) {
        surfacesByTableIndex[surface.sourceTableIndex].push_back(surface.sourceKey);
    }

    for (const NavigationGroundFallbackEntryEvidence& entry : entries) {
        if (entry.targetEntryIds.empty()) {
            continue;
        }

        NavigationAuthoredGroundFallbackChain chain{
            .sourceTableIndex = entry.tableIndex,
            .sourceEntryId = entry.entryId,
        };
        if (const auto found = surfacesByTableIndex.find(entry.tableIndex);
            found != surfacesByTableIndex.end()) {
            chain.sourceSurfaces = found->second;
        }
        if (chain.sourceSurfaces.empty()) {
            appendDiagnostic(result, NavigationDiagnosticSeverity::Warning,
                "Ground fallback source table=" + std::to_string(entry.tableIndex) +
                " entry=" + std::to_string(entry.entryId) +
                " has authored targets but no usable source ground geometry.");
        }

        chain.targets.reserve(entry.targetEntryIds.size());
        bool missingEntryTruncatedChain = false;
        for (std::size_t ordinal = 0; ordinal < entry.targetEntryIds.size(); ++ordinal) {
            const std::uint32_t targetEntryId = entry.targetEntryIds[ordinal];
            NavigationAuthoredGroundFallbackTarget target{
                .targetEntryId = targetEntryId,
                .authoredOrdinal = ordinal,
            };

            if (missingEntryTruncatedChain) {
                target.status = NavigationAuthoredGroundFallbackTargetStatus::SuppressedAfterMissingEntry;
                chain.targets.push_back(std::move(target));
                continue;
            }

            const NavigationGroundFallbackEntryEvidence* resolvedEntry =
                resolveTargetEntry(entries, targetEntryId);
            if (resolvedEntry == nullptr) {
                target.status = NavigationAuthoredGroundFallbackTargetStatus::MissingEntry;
                missingEntryTruncatedChain = true;
                appendDiagnostic(result, NavigationDiagnosticSeverity::Warning,
                    "Ground fallback source table=" + std::to_string(entry.tableIndex) +
                    " entry=" + std::to_string(entry.entryId) +
                    " target ordinal=" + std::to_string(ordinal) +
                    " EntryID=" + std::to_string(targetEntryId) +
                    " does not resolve; the effective runtime fallback chain stops here.");
                chain.targets.push_back(std::move(target));
                continue;
            }

            target.targetTableIndex = resolvedEntry->tableIndex;
            if (const auto found = surfacesByTableIndex.find(resolvedEntry->tableIndex);
                found != surfacesByTableIndex.end()) {
                target.targetSurfaces = found->second;
            }
            if (target.targetSurfaces.empty()) {
                target.status = NavigationAuthoredGroundFallbackTargetStatus::MissingGeometry;
                appendDiagnostic(result, NavigationDiagnosticSeverity::Warning,
                    "Ground fallback source table=" + std::to_string(entry.tableIndex) +
                    " entry=" + std::to_string(entry.entryId) +
                    " target ordinal=" + std::to_string(ordinal) +
                    " EntryID=" + std::to_string(targetEntryId) +
                    " resolves to table=" + std::to_string(resolvedEntry->tableIndex) +
                    " but has no usable ground geometry; later authored targets remain eligible.");
            } else {
                target.status = NavigationAuthoredGroundFallbackTargetStatus::Resolved;
            }
            chain.targets.push_back(std::move(target));
        }
        result.chains.push_back(std::move(chain));
    }

    return result;
}

} // namespace savor::navigation::detail
