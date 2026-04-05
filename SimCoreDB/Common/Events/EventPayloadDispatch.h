#pragma once

#include <array>
#include <optional>
#include <string_view>

#include "EventTypeFormat.h"

namespace simcore::db::events {

enum class PayloadResolverContract {
    Unknown = 0,
    ExecutionWorkflowJobV1,
    AnalysisSpineV1,
    AnalysisSeedProbeV1,
    AnalysisBattleV1,
    AuthoringV1,
    StateArtifactV1,
    ArchivePackageV1,
};

struct EventDispatchKey {
    std::string_view event_type;
    int event_version = 1;
};

struct EventDispatchBinding {
    EventDispatchKey key;
    PayloadResolverContract contract = PayloadResolverContract::Unknown;
};

inline constexpr std::array<EventDispatchBinding, 39> kPayloadDispatchBindingsV1{ {
    { { "Execution.JobSetCreated.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobQueued.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobClaimed.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobLeaseRenewed.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobProgressed.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobCompleted.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobEventArchived.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobRestored.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowInstanceCreated.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowStepReady.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowStepMaterialized.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowStepCompleted.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowStepFailed.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowInstanceCompleted.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "AnalysisSpine.RunCreated.v1", 1 }, PayloadResolverContract::AnalysisSpineV1 },
    { { "AnalysisSpine.StateRefRegistered.v1", 1 }, PayloadResolverContract::AnalysisSpineV1 },
    { { "AnalysisSpine.LineageEdgeAdded.v1", 1 }, PayloadResolverContract::AnalysisSpineV1 },
    { { "AnalysisSpine.ArtifactLinked.v1", 1 }, PayloadResolverContract::AnalysisSpineV1 },
    { { "AnalysisSeedProbe.SetCreated.v1", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisSeedProbe.RunRequested.v1", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisSeedProbe.NeutralSeedRecorded.v1", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisSeedProbe.GridSeedRecorded.v1", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisSeedProbe.UniqueSeedRecorded.v1", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisSeedProbe.EncounterProjectionRecorded.v1", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisSeedProbe.RunCompleted.v1", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisBattle.BattleSetCreated.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.SeedCandidateAdded.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.TurnWaveCreated.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.TurnJobRecorded.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.SelectionPoolCreated.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.SelectionDecisionRecorded.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.TerminalFollowupUpdated.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "Authoring.SeedProbeSpecSaved.v1", 1 }, PayloadResolverContract::AuthoringV1 },
    { { "Authoring.TasSpecSaved.v1", 1 }, PayloadResolverContract::AuthoringV1 },
    { { "Authoring.BattleRunSpecSaved.v1", 1 }, PayloadResolverContract::AuthoringV1 },
    { { "Authoring.PlanSaved.v1", 1 }, PayloadResolverContract::AuthoringV1 },
    { { "Authoring.PredicateSpecSaved.v1", 1 }, PayloadResolverContract::AuthoringV1 },
    { { "Authoring.SettingsSaved.v1", 1 }, PayloadResolverContract::AuthoringV1 },
    { { "Authoring.TemplateSaved.v1", 1 }, PayloadResolverContract::AuthoringV1 },
} };

inline constexpr std::array<EventDispatchBinding, 9> kStateArchiveDispatchBindingsV1{ {
    { { "State.ArtifactStored.v1", 1 }, PayloadResolverContract::StateArtifactV1 },
    { { "State.SavestateCreated.v1", 1 }, PayloadResolverContract::StateArtifactV1 },
    { { "State.SavestateDerived.v1", 1 }, PayloadResolverContract::StateArtifactV1 },
    { { "State.TasVariantCreated.v1", 1 }, PayloadResolverContract::StateArtifactV1 },
    { { "Archive.PackageCreated.v1", 1 }, PayloadResolverContract::ArchivePackageV1 },
    { { "Archive.PackageIndexed.v1", 1 }, PayloadResolverContract::ArchivePackageV1 },
    { { "Archive.RehydrateRequested.v1", 1 }, PayloadResolverContract::ArchivePackageV1 },
    { { "Archive.RehydrateCompleted.v1", 1 }, PayloadResolverContract::ArchivePackageV1 },
    { { "Archive.RehydrateFailed.v1", 1 }, PayloadResolverContract::ArchivePackageV1 },
} };

inline std::optional<PayloadResolverContract> ResolvePayloadResolverContract(
    std::string_view event_type,
    int event_version) {
    if (!ValidateEventTypeFormat(event_type, event_version)) {
        return std::nullopt;
    }

    for (const auto& binding : kPayloadDispatchBindingsV1) {
        if (binding.key.event_version == event_version && binding.key.event_type == event_type) {
            return binding.contract;
        }
    }

    for (const auto& binding : kStateArchiveDispatchBindingsV1) {
        if (binding.key.event_version == event_version && binding.key.event_type == event_type) {
            return binding.contract;
        }
    }

    return std::nullopt;
}

} // namespace simcore::db::events
