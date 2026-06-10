#include "ContentGraphTypes.h"

namespace soasim::contentgraph {

const char* toString(ContentNodeType type) {
    switch (type) {
    case ContentNodeType::ScriptFile:
        return "ScriptFile";
    case ContentNodeType::ScriptSection:
        return "ScriptSection";
    case ContentNodeType::BasicBlock:
        return "BasicBlock";
    case ContentNodeType::Instruction:
        return "Instruction";
    case ContentNodeType::MldFile:
        return "MldFile";
    case ContentNodeType::MldEntry:
        return "MldEntry";
    case ContentNodeType::ResourceRef:
        return "ResourceRef";
    case ContentNodeType::FlagRef:
        return "FlagRef";
    case ContentNodeType::UnknownTarget:
        return "UnknownTarget";
    default:
        return "UnknownTarget";
    }
}

const char* toString(ContentEdgeType type) {
    switch (type) {
    case ContentEdgeType::Contains:
        return "Contains";
    case ContentEdgeType::PairedWith:
        return "PairedWith";
    case ContentEdgeType::Fallthrough:
        return "Fallthrough";
    case ContentEdgeType::BranchTrue:
        return "BranchTrue";
    case ContentEdgeType::BranchFalse:
        return "BranchFalse";
    case ContentEdgeType::SwitchCase:
        return "SwitchCase";
    case ContentEdgeType::Jump:
        return "Jump";
    case ContentEdgeType::CallSubscript:
        return "CallSubscript";
    case ContentEdgeType::Return:
        return "Return";
    case ContentEdgeType::LoadsScript:
        return "LoadsScript";
    case ContentEdgeType::LoadsMld:
        return "LoadsMld";
    case ContentEdgeType::ReferencesFlag:
        return "ReferencesFlag";
    case ContentEdgeType::WritesFlag:
        return "WritesFlag";
    case ContentEdgeType::MldEntryDispatchesSection:
        return "MldEntryDispatchesSection";
    default:
        return "Contains";
    }
}

const char* toString(ContentConfidence confidence) {
    switch (confidence) {
    case ContentConfidence::Known:
        return "Known";
    case ContentConfidence::Inferred:
        return "Inferred";
    case ContentConfidence::Heuristic:
        return "Heuristic";
    case ContentConfidence::Unresolved:
        return "Unresolved";
    default:
        return "Unresolved";
    }
}

const char* toString(ContentGraphDetailLevel detailLevel) {
    switch (detailLevel) {
    case ContentGraphDetailLevel::Sections:
        return "Sections";
    case ContentGraphDetailLevel::Blocks:
        return "Blocks";
    case ContentGraphDetailLevel::Instructions:
        return "Instructions";
    default:
        return "Sections";
    }
}

const char* toString(ContentGraphProjection projection) {
    switch (projection) {
    case ContentGraphProjection::Full:
        return "Full";
    case ContentGraphProjection::Sections:
        return "Sections";
    case ContentGraphProjection::World:
        return "World";
    default:
        return "Full";
    }
}

} // namespace soasim::contentgraph
