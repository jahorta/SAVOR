#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace simcore::db::events {

enum class PayloadResolverContract {
    Unknown = 0,
    ExecutionWorkflowJobV1,
    AnalysisSeedProbeV1,
    AnalysisBattleV1,
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

inline constexpr std::array<EventDispatchBinding, 28> kPayloadDispatchBindingsV1{ {
    { { "Execution.JobSetCreated", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobQueued", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobClaimed", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobLeaseRenewed", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobProgressed", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobCompleted", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobEventArchived", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.JobRestored", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowInstanceCreated", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowStepReady", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowStepMaterialized", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowStepCompleted", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowStepFailed", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "Execution.WorkflowInstanceCompleted", 1 }, PayloadResolverContract::ExecutionWorkflowJobV1 },
    { { "AnalysisSeedProbe.SetCreated", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisSeedProbe.RunRequested", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisSeedProbe.NeutralSeedRecorded", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisSeedProbe.GridSeedRecorded", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisSeedProbe.UniqueSeedRecorded", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisSeedProbe.EncounterProjectionRecorded", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisSeedProbe.RunCompleted", 1 }, PayloadResolverContract::AnalysisSeedProbeV1 },
    { { "AnalysisBattle.BattleSetCreated", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.SeedCandidateAdded", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.TurnWaveCreated", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.TurnJobRecorded", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.SelectionPoolCreated", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.SelectionDecisionRecorded", 1 }, PayloadResolverContract::AnalysisBattleV1 },
    { { "AnalysisBattle.TerminalFollowupUpdated", 1 }, PayloadResolverContract::AnalysisBattleV1 },
} };

inline constexpr std::array<EventDispatchBinding, 9> kStateArchiveDispatchBindingsV1{ {
    { { "State.ArtifactStored", 1 }, PayloadResolverContract::StateArtifactV1 },
    { { "State.SavestateCreated", 1 }, PayloadResolverContract::StateArtifactV1 },
    { { "State.SavestateDerived", 1 }, PayloadResolverContract::StateArtifactV1 },
    { { "State.TasVariantCreated", 1 }, PayloadResolverContract::StateArtifactV1 },
    { { "Archive.PackageCreated", 1 }, PayloadResolverContract::ArchivePackageV1 },
    { { "Archive.PackageIndexed", 1 }, PayloadResolverContract::ArchivePackageV1 },
    { { "Archive.RehydrateRequested", 1 }, PayloadResolverContract::ArchivePackageV1 },
    { { "Archive.RehydrateCompleted", 1 }, PayloadResolverContract::ArchivePackageV1 },
    { { "Archive.RehydrateFailed", 1 }, PayloadResolverContract::ArchivePackageV1 },
} };

inline std::optional<PayloadResolverContract> ResolvePayloadResolverContract(
    std::string_view event_type,
    int event_version) {
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
