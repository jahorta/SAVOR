#include "ContentGraphCorpusBuilder.h"

#include "ContentGraphIds.h"

#include <unordered_map>

namespace soasim::contentgraph {
namespace {

ContentEvidence pairingEvidence(const std::string& scriptPath, const std::string& mldPath, std::string key) {
    ContentEvidence evidence{};
    evidence.sourcePath = scriptPath;
    evidence.detail = "SCT/MLD filename pairing.";
    evidence.attributes.emplace("script_path", scriptPath);
    evidence.attributes.emplace("mld_path", mldPath);
    evidence.attributes.emplace("pairing_key", std::move(key));
    return evidence;
}

} // namespace

ContentGraph ContentGraphCorpusBuilder::build(const ContentGraphCorpusInput& input,
    const ContentGraphCorpusBuildOptions& options) const {
    ContentGraph graph{};
    SctGraphBuilder sctBuilder{};
    MldGraphBuilder mldBuilder{};

    std::unordered_map<std::string, const ParsedSctGraphInput*> sctByKey{};
    std::unordered_map<std::string, const ParsedMldGraphInput*> mldByKey{};

    for (const auto& sct : input.sctFiles) {
        auto parseResult = sct.parseResult;
        if (parseResult.file.sourcePath.empty()) {
            parseResult.file.sourcePath = sct.sourcePath;
        }
        sctBuilder.addToGraph(graph, parseResult, options.sctOptions);
        sctByKey.try_emplace(scriptPairingKey(sct.sourcePath), &sct);
    }

    for (const auto& mld : input.mldFiles) {
        mldBuilder.addToGraph(graph, mld.sourcePath, mld.parseResult);
        mldByKey.try_emplace(mldPairingKey(mld.sourcePath), &mld);
    }

    for (const auto& [key, sct] : sctByKey) {
        const auto mldIt = mldByKey.find(key);
        if (mldIt == mldByKey.end()) {
            continue;
        }
        const auto* mld = mldIt->second;

        ContentEdge paired{};
        paired.from = scriptFileNodeId(sct->sourcePath);
        paired.to = mldFileNodeId(mld->sourcePath);
        paired.type = ContentEdgeType::PairedWith;
        paired.confidence = ContentConfidence::Known;
        paired.attributes.emplace("pairing_key", key);
        paired.evidence.push_back(pairingEvidence(sct->sourcePath, mld->sourcePath, key));
        graph.addEdge(std::move(paired));

        for (const auto& entry : mld->parseResult.entryList) {
            const auto sectionName = mSectionNameForTableId(entry.tblId);
            const auto sectionId = scriptSectionNodeId(sct->sourcePath, sectionName);
            if (!graph.hasNode(sectionId)) {
                continue;
            }

            ContentEdge dispatch{};
            dispatch.from = mldEntryNodeId(mld->sourcePath, entry.tableIndex, entry.entryId, entry.tblId);
            dispatch.to = sectionId;
            dispatch.type = ContentEdgeType::MldEntryDispatchesSection;
            dispatch.confidence = ContentConfidence::Known;
            dispatch.attributes.emplace("pairing_key", key);
            dispatch.attributes.emplace("tableID", std::to_string(entry.tblId));
            dispatch.attributes.emplace("section", sectionName);
            ContentEvidence evidence{};
            evidence.sourcePath = mld->sourcePath;
            evidence.sectionName = sectionName;
            evidence.detail = "Paired MLD entry tableID matches SCT section name.";
            evidence.attributes.emplace("entryID", std::to_string(entry.entryId));
            evidence.attributes.emplace("tableID", std::to_string(entry.tblId));
            evidence.attributes.emplace("function", entry.fxnName);
            dispatch.evidence.push_back(std::move(evidence));
            graph.addEdge(std::move(dispatch));
        }
    }

    return graph;
}

} // namespace soasim::contentgraph
