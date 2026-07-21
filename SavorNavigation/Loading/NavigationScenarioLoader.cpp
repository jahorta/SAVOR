#include "NavigationScenarioLoader.h"

#include "../Graph/NavigationTraversalGraph.h"

#include <iterator>
#include <utility>

namespace savor::navigation {
namespace {

void appendDiagnostics(std::vector<NavigationDiagnostic>& destination,
    const std::vector<NavigationDiagnostic>& source) {
    destination.insert(destination.end(), source.begin(), source.end());
}

} // namespace

NavigationScenarioLoadResult NavigationScenarioLoader::loadFile(
    const std::filesystem::path& mldPath) const {
    NavigationScenarioLoadResult result{};

    NavigationAreaLoader areaLoader{};
    auto areaResult = areaLoader.loadFile(mldPath);
    appendDiagnostics(result.diagnostics, areaResult.diagnostics);
    if (!areaResult.hasModel()) {
        result.status = NavigationScenarioLoadStatus::Failed;
        return result;
    }

    NavigationScenarioModel scenario{};
    scenario.identity = resolveNavigationAreaIdentity(mldPath);
    scenario.area = std::move(*areaResult.model);

    NavigationScriptLoader scriptLoader{};
    auto scriptResult = scriptLoader.discoverAndLoad(scenario.identity);
    appendDiagnostics(result.diagnostics, scriptResult.diagnostics);
    scenario.script = std::move(scriptResult.model);

    NavigationGraphBuilder graphBuilder{};
    scenario.traversalGraph = graphBuilder.build(scenario.area);
    appendDiagnostics(result.diagnostics, scenario.traversalGraph.diagnostics);
    scenario.diagnostics = result.diagnostics;

    const bool scriptComplete =
        scenario.script.associationStatus == NavigationScriptAssociationStatus::Matched &&
        scenario.script.loadStatus == NavigationScriptLoadStatus::Complete;
    const bool areaComplete = areaResult.status == NavigationAreaLoadStatus::Complete;
    result.status = areaComplete && scriptComplete && scenario.isPathfindingReady()
        ? NavigationScenarioLoadStatus::Complete
        : NavigationScenarioLoadStatus::Partial;
    result.model = std::move(scenario);
    return result;
}

NavigationScriptLoadResult NavigationScenarioLoader::loadRelatedScript(
    const NavigationAreaIdentity& identity,
    const std::filesystem::path& sctPath) const {
    return NavigationScriptLoader{}.loadMatchingFile(identity, sctPath);
}

} // namespace savor::navigation
