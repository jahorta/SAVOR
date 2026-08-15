#pragma once

#include <array>
#include <optional>
#include <string_view>

#include "EventTypeFormat.h"

namespace savor::db::events {

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

inline constexpr EventDispatchBinding kPayloadDispatchBindingsV1[] = {
    { { "Execution.JobSetCreated.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobSetMaterializing.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobPopulationSealed.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorksetPublicationCompleted.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobQueued.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobPendingWorkset.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobClaimed.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobClaimRequeued.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobStarted.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobLeaseRenewed.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobProgressed.v2", 2 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobExecutionFinished.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobResultProcessingStarted.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobResultParked.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobResultProcessed.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobCancellationRequested.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobCancellationDelivered.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobCancellationResolved.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobCompleted.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobEventArchived.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobRestored.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorksetPublished.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorksetClaimed.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorksetDispatched.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorksetReleased.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowInstanceCreated.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowStepReady.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowStepBlocked.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowStepMaterialized.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowStepCompleted.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowStepFailed.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowStepEmpty.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowStepCoordinatorFailure.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowStepDescriptorAvailable.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowStepInputRequested.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowStepInputFragmentReady.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowStepInputComplete.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowTransitionEvaluated.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowTransitionAdvanced.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowTransitionBlocked.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowInvariantViolation.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowRemediationReopened.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowRemediationRepairExecuted.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowRemediationTerminalFailed.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowInstanceCompleted.v1", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "AnalysisSpine.RunCreated.v1", 1 }, PayloadResolverContract::AnalysisSpineV1 },
    { { "AnalysisSpine.StateRefRegistered.v1", 1 }, PayloadResolverContract::AnalysisSpineV1 },
    { { "AnalysisSpine.LineageEdgeAdded.v1", 1 }, PayloadResolverContract::AnalysisSpineV1 },
    { { "AnalysisSpine.ArtifactLinked.v1", 1 }, PayloadResolverContract::AnalysisSpineV1 },
    { { "AnalysisSeedProbe.SetCreated.v1", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisSeedProbe.RunRequested.v1", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisSeedProbe.ObservationRecorded.v1", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisSeedProbe.EvidenceStateChanged.v1", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisSeedProbe.AcceptedInputFramesReplaced.v1", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisSeedProbe.RunStatusChanged.v1", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisSeedProbe.EncounterProjectionRecorded.v1", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisSeedProbe.RunCompleted.v1", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisSeedProbe.RunFailed.v1", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisBattle.BattleSetCreated.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.SeedCandidateAdded.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.TurnWaveCreated.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.TurnJobRecorded.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.TurnJobResultUpdated.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.TurnWaveStatusUpdated.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.BattleSetStatusUpdated.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.BattleAdvancementPoolCreated.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.BattleAdvancementDecisionRecorded.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.ManualFollowupUpdated.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.BattleCompletionCreated.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.BattleCompletionCompleted.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.BattleCompletionFailed.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.BattleRecordingCreated.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.BattleRecordingCompleted.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.BattleRecordingFailed.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.BattleReplayCreated.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.BattleReplayCompleted.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.BattleReplayFailed.v1", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "Authoring.SeedProbeSpecSaved.v1", 1 }, PayloadResolverContract::AuthoringV1 },
    { { "Authoring.TasSpecSaved.v1", 1 }, PayloadResolverContract::AuthoringV1 },
    { { "Authoring.BattleRunSpecSaved.v1", 1 }, PayloadResolverContract::AuthoringV1 },
    { { "Authoring.PlanSaved.v1", 1 }, PayloadResolverContract::AuthoringV1 },
    { { "Authoring.BattlePlanActionPresetSaved.v1", 1 }, PayloadResolverContract::AuthoringV1 },
    { { "Authoring.BattlePlanActionPresetRenamed.v1", 1 }, PayloadResolverContract::AuthoringV1 },
    { { "Authoring.SettingsSaved.v1", 1 }, PayloadResolverContract::AuthoringV1 },
    { { "Authoring.BattleChainSpecSaved.v1", 1 }, PayloadResolverContract::AuthoringV1 },
    { { "Authoring.WorkflowGraphSaved.v1", 1 }, PayloadResolverContract::AuthoringV1 },
};

inline constexpr std::array<EventDispatchBinding, 10> kStateArchiveDispatchBindingsV1{ {
    { { "State.ArtifactStored.v1", 1 }, PayloadResolverContract::StateArtifactV1 },
    { { "State.SavestateCreated.v1", 1 }, PayloadResolverContract::StateArtifactV1 },
    { { "State.SavestateDerived.v1", 1 }, PayloadResolverContract::StateArtifactV1 },
    { { "State.TasMovieRootCreated.v1", 1 }, PayloadResolverContract::StateArtifactV1 },
    { { "State.TasMovieTreeCreated.v1", 1 }, PayloadResolverContract::StateArtifactV1 },
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

} // namespace savor::db::events
