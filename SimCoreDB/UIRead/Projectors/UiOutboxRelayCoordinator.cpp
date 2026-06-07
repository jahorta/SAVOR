#include "UiOutboxRelayCoordinator.h"

#include "ArchiveCatalogProjector.h"
#include "ArtifactProjector.h"
#include "BattleProjector.h"
#include "JobProjector.h"
#include "SeedProbeProjector.h"
#include "ProjectorContract.h"

namespace simcore::db::uiread::projectors {

UiOutboxRelayCoordinator::UiOutboxRelayCoordinator(sqlite3* db)
    : db_(db) {
}

bool UiOutboxRelayCoordinator::RelayWithConfig(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table,
    const events::OutboxRelayConfig& config,
    const std::vector<events::OutboxRelayDispatchBinding>& bindings,
    int max_batch_size,
    std::string* error_out) const {
    return RunProjectorRelay(
        db_,
        projector_name,
        source_context,
        source_outbox_table,
        config,
        bindings,
        max_batch_size,
        error_out);
}

bool UiOutboxRelayCoordinator::RelayExecutionOutbox(
    const std::string& projector_name,
    int max_batch_size,
    std::string* error_out,
    int max_attempts) {
    if (!ValidateProjectorContractInputs(projector_name, max_batch_size, max_attempts, error_out)) {
        return false;
    }

    JobProjector job_projector(db_);
    BattleProjector battle_projector(db_);

    const auto project_jobs = [&job_projector](const events::EventEnvelope&, std::string* handler_error) {
        return job_projector.ProjectAll(handler_error);
    };
    const auto project_battles = [&battle_projector](const events::EventEnvelope&, std::string* handler_error) {
        return battle_projector.ProjectAll(handler_error);
    };

    const std::vector<events::OutboxRelayDispatchBinding> bindings{
        { { "Execution.JobSetCreated.v1", 1 }, project_jobs },
        { { "Execution.JobQueued.v1", 1 }, project_jobs },
        { { "Execution.JobClaimed.v1", 1 }, project_battles },
        { { "Execution.JobLeaseRenewed.v1", 1 }, project_battles },
        { { "Execution.JobProgressed.v1", 1 }, project_battles },
        { { "Execution.JobCompleted.v1", 1 }, project_battles },
        { { "Execution.JobEventArchived.v1", 1 }, project_battles },
        { { "Execution.JobRestored.v1", 1 }, project_battles },
        { { "Execution.WorkflowInstanceCreated.v1", 1 }, project_jobs },
        { { "Execution.WorkflowStepReady.v1", 1 }, project_jobs },
        { { "Execution.WorkflowStepMaterialized.v1", 1 }, project_jobs },
        { { "Execution.WorkflowStepCompleted.v1", 1 }, project_jobs },
        { { "Execution.WorkflowStepFailed.v1", 1 }, project_jobs },
        { { "Execution.WorkflowInstanceCompleted.v1", 1 }, project_jobs },
    };

    return RelayWithConfig(
        projector_name,
        "Execution",
        "exec_outbox_message",
        {
            .db = db_,
            .outbox_table = "exec_outbox_message",
            .context_name = "Execution",
            .aggregate_kind = "",
            .payload_ref_kind = "",
            .max_attempts = max_attempts,
        },
        bindings,
        max_batch_size,
        error_out);
}

bool UiOutboxRelayCoordinator::RelayStateOutbox(
    const std::string& projector_name,
    int max_batch_size,
    std::string* error_out,
    int max_attempts) {
    if (!ValidateProjectorContractInputs(projector_name, max_batch_size, max_attempts, error_out)) {
        return false;
    }

    ArtifactProjector artifact_projector(db_);
    if (!artifact_projector.EnsureSqlFunctions(error_out)) {
        return false;
    }
    const auto project_artifacts = [&artifact_projector](const events::EventEnvelope&, std::string* handler_error) {
        return artifact_projector.ProjectAll(handler_error);
    };

    const std::vector<events::OutboxRelayDispatchBinding> bindings{
        { { "State.ArtifactStored.v1", 1 }, project_artifacts },
    };

    return RelayWithConfig(
        projector_name,
        "State",
        "state_outbox_message",
        {
            .db = db_,
            .outbox_table = "state_outbox_message",
            .context_name = "State",
            .aggregate_kind = "artifact",
            .payload_ref_kind = "artifact",
            .max_attempts = max_attempts,
        },
        bindings,
        max_batch_size,
        error_out);
}

bool UiOutboxRelayCoordinator::RelaySeedProbeOutbox(
    const std::string& projector_name,
    int max_batch_size,
    std::string* error_out,
    int max_attempts) {
    if (!ValidateProjectorContractInputs(projector_name, max_batch_size, max_attempts, error_out)) {
        return false;
    }

    SeedProbeProjector seed_probe_projector(db_);
    const auto project_seed_probe = [&seed_probe_projector](const events::EventEnvelope&, std::string* handler_error) {
        return seed_probe_projector.ProjectAll(handler_error);
    };

    const std::vector<events::OutboxRelayDispatchBinding> bindings{
        { { "AnalysisSeedProbe.SetCreated.v1", 1 }, project_seed_probe },
        { { "AnalysisSeedProbe.RunRequested.v1", 1 }, project_seed_probe },
        { { "AnalysisSeedProbe.NeutralSeedRecorded.v1", 1 }, project_seed_probe },
        { { "AnalysisSeedProbe.GridSeedRecorded.v1", 1 }, project_seed_probe },
        { { "AnalysisSeedProbe.UniqueSeedRecorded.v1", 1 }, project_seed_probe },
        { { "AnalysisSeedProbe.EncounterProjectionRecorded.v1", 1 }, project_seed_probe },
        { { "AnalysisSeedProbe.RunCompleted.v1", 1 }, project_seed_probe },
    };

    return RelayWithConfig(
        projector_name,
        "AnalysisSeedProbe",
        "sp_outbox_message",
        {
            .db = db_,
            .outbox_table = "sp_outbox_message",
            .context_name = "AnalysisSeedProbe",
            .aggregate_kind = "probe_run",
            .payload_ref_kind = "",
            .max_attempts = max_attempts,
        },
        bindings,
        max_batch_size,
        error_out);
}

bool UiOutboxRelayCoordinator::RelayAnalysisBattleOutbox(
    const std::string& projector_name,
    int max_batch_size,
    std::string* error_out,
    int max_attempts) {
    if (!ValidateProjectorContractInputs(projector_name, max_batch_size, max_attempts, error_out)) {
        return false;
    }

    BattleProjector battle_projector(db_);
    const auto project_battles = [&battle_projector](const events::EventEnvelope&, std::string* handler_error) {
        return battle_projector.ProjectAll(handler_error);
    };

    const std::vector<events::OutboxRelayDispatchBinding> bindings{
        { { "AnalysisBattle.BattleSetCreated.v1", 1 }, project_battles },
        { { "AnalysisBattle.SeedCandidateAdded.v1", 1 }, project_battles },
        { { "AnalysisBattle.TurnWaveCreated.v1", 1 }, project_battles },
        { { "AnalysisBattle.TurnJobRecorded.v1", 1 }, project_battles },
        { { "AnalysisBattle.SelectionPoolCreated.v1", 1 }, project_battles },
        { { "AnalysisBattle.SelectionDecisionRecorded.v1", 1 }, project_battles },
        { { "AnalysisBattle.TerminalFollowupUpdated.v1", 1 }, project_battles },
    };

    return RelayWithConfig(
        projector_name,
        "AnalysisBattle",
        "ab_outbox_message",
        {
            .db = db_,
            .outbox_table = "ab_outbox_message",
            .context_name = "AnalysisBattle",
            .aggregate_kind = "battle_set",
            .payload_ref_kind = "",
            .max_attempts = max_attempts,
        },
        bindings,
        max_batch_size,
        error_out);
}

bool UiOutboxRelayCoordinator::RelayAnalysisSpineOutbox(
    const std::string& projector_name,
    int max_batch_size,
    std::string* error_out,
    int max_attempts) {
    if (!ValidateProjectorContractInputs(projector_name, max_batch_size, max_attempts, error_out)) {
        return false;
    }

    const auto no_op = [](const events::EventEnvelope&, std::string*) {
        return true;
    };

    const std::vector<events::OutboxRelayDispatchBinding> bindings{
        { { "AnalysisSpine.RunCreated.v1", 1 }, no_op },
        { { "AnalysisSpine.StateRefRegistered.v1", 1 }, no_op },
        { { "AnalysisSpine.LineageEdgeAdded.v1", 1 }, no_op },
        { { "AnalysisSpine.ArtifactLinked.v1", 1 }, no_op },
    };

    return RelayWithConfig(
        projector_name,
        "AnalysisSpine",
        "asp_outbox_message",
        {
            .db = db_,
            .outbox_table = "asp_outbox_message",
            .context_name = "AnalysisSpine",
            .aggregate_kind = "run",
            .payload_ref_kind = "analysis_spine_event",
            .max_attempts = max_attempts,
        },
        bindings,
        max_batch_size,
        error_out);
}

bool UiOutboxRelayCoordinator::RelayAuthoringOutbox(
    const std::string& projector_name,
    int max_batch_size,
    std::string* error_out,
    int max_attempts) {
    if (!ValidateProjectorContractInputs(projector_name, max_batch_size, max_attempts, error_out)) {
        return false;
    }

    const auto no_op = [](const events::EventEnvelope&, std::string*) {
        return true;
    };

    const std::vector<events::OutboxRelayDispatchBinding> bindings{
        { { "Authoring.SeedProbeSpecSaved.v1", 1 }, no_op },
        { { "Authoring.TasSpecSaved.v1", 1 }, no_op },
        { { "Authoring.BattleRunSpecSaved.v1", 1 }, no_op },
        { { "Authoring.PlanSaved.v1", 1 }, no_op },
        { { "Authoring.PredicateSpecSaved.v1", 1 }, no_op },
        { { "Authoring.PredicateSpecUpdated.v1", 1 }, no_op },
        { { "Authoring.PredicateSpecDeleted.v1", 1 }, no_op },
        { { "Authoring.SettingsSaved.v1", 1 }, no_op },
        { { "Authoring.TemplateSaved.v1", 1 }, no_op },
        { { "Authoring.WorkflowGraphSaved.v1", 1 }, no_op },
    };

    return RelayWithConfig(
        projector_name,
        "Authoring",
        "au_outbox_message",
        {
            .db = db_,
            .outbox_table = "au_outbox_message",
            .context_name = "Authoring",
            .aggregate_kind = "template",
            .payload_ref_kind = "authoring_event",
            .max_attempts = max_attempts,
        },
        bindings,
        max_batch_size,
        error_out);
}

bool UiOutboxRelayCoordinator::RelayArchiveOutbox(
    const std::string& projector_name,
    int max_batch_size,
    std::string* error_out,
    int max_attempts) {
    if (!ValidateProjectorContractInputs(projector_name, max_batch_size, max_attempts, error_out)) {
        return false;
    }

    ArchiveCatalogProjector archive_catalog_projector(db_);
    const auto project_archive = [&archive_catalog_projector](const events::EventEnvelope&, std::string* handler_error) {
        return archive_catalog_projector.ProjectAll(handler_error);
    };

    const std::vector<events::OutboxRelayDispatchBinding> bindings{
        { { "Archive.PackageCreated.v1", 1 }, project_archive },
        { { "Archive.PackageIndexed.v1", 1 }, project_archive },
        { { "Archive.RehydrateRequested.v1", 1 }, project_archive },
        { { "Archive.RehydrateCompleted.v1", 1 }, project_archive },
        { { "Archive.RehydrateFailed.v1", 1 }, project_archive },
    };

    return RelayWithConfig(
        projector_name,
        "Archive",
        "ar_outbox_message",
        {
            .db = db_,
            .outbox_table = "ar_outbox_message",
            .context_name = "Archive",
            .aggregate_kind = "archive_package",
            .payload_ref_kind = "archive_package",
            .max_attempts = max_attempts,
        },
        bindings,
        max_batch_size,
        error_out);
}

bool UiOutboxRelayCoordinator::RelayAll(
    const std::string& projector_name,
    int max_batch_size,
    std::string* error_out,
    int max_attempts,
    bool include_archive) {
    if (!RelayExecutionOutbox(projector_name, max_batch_size, error_out, max_attempts)) return false;
    if (!RelayStateOutbox(projector_name, max_batch_size, error_out, max_attempts)) return false;
    if (!RelaySeedProbeOutbox(projector_name, max_batch_size, error_out, max_attempts)) return false;
    if (!RelayAnalysisBattleOutbox(projector_name, max_batch_size, error_out, max_attempts)) return false;
    if (!RelayAnalysisSpineOutbox(projector_name, max_batch_size, error_out, max_attempts)) return false;
    if (!RelayAuthoringOutbox(projector_name, max_batch_size, error_out, max_attempts)) return false;
    if (include_archive && !RelayArchiveOutbox(projector_name, max_batch_size, error_out, max_attempts)) return false;

    return true;
}

} // namespace simcore::db::uiread::projectors
