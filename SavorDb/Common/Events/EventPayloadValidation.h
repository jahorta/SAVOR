#pragma once

#include <string>
#include <string_view>

#include "EventEnvelope.h"
#include "EventTypeFormat.h"

namespace savor::db::events {

inline bool ValidateV1EnvelopeBasics(const EventEnvelope& envelope, std::string* error_out = nullptr) {
    if (envelope.event_version != 1) {
        if (error_out) *error_out = "event_version must be 1";
        return false;
    }
    if (envelope.event_type.empty()) {
        if (error_out) *error_out = "event_type is required";
        return false;
    }
    if (!ValidateEventTypeFormat(envelope.event_type, envelope.event_version, error_out)) {
        return false;
    }
    if (envelope.context_name.empty()) {
        if (error_out) *error_out = "context_name is required";
        return false;
    }
    if (envelope.aggregate_kind.empty()) {
        if (error_out) *error_out = "aggregate_kind is required";
        return false;
    }
    if (envelope.payload_ref_kind.empty()) {
        if (error_out) *error_out = "payload_ref_kind is required";
        return false;
    }
    if (envelope.payload_ref_id <= 0) {
        if (error_out) *error_out = "payload_ref_id must be > 0";
        return false;
    }
    return true;
}

inline bool ValidateV1PayloadRef(
    const EventEnvelope& envelope,
    std::string_view expected_context_name,
    std::string_view expected_payload_ref_kind,
    std::string* error_out = nullptr) {
    if (!ValidateV1EnvelopeBasics(envelope, error_out)) {
        return false;
    }
    if (envelope.context_name != expected_context_name) {
        if (error_out) *error_out = "context_name does not match expected payload family";
        return false;
    }
    if (envelope.payload_ref_kind != expected_payload_ref_kind) {
        if (error_out) *error_out = "payload_ref_kind does not match expected payload family";
        return false;
    }
    return true;
}

inline bool ValidateExecutionWorkflowJobPayloadV1(const EventEnvelope& envelope, std::string* error_out = nullptr) {
    if (!ValidateV1EnvelopeBasics(envelope, error_out)) {
        return false;
    }
    if (envelope.context_name != "Execution") {
        if (error_out) *error_out = "context_name does not match expected payload family";
        return false;
    }

    if (envelope.event_type == "Execution.JobSetCreated.v1"
        || envelope.event_type == "Execution.JobSetMaterializing.v1"
        || envelope.event_type == "Execution.JobPopulationSealed.v1"
        || envelope.event_type
            == "Execution.WorksetPublicationCompleted.v1") {
        if (envelope.payload_ref_kind != "job_set") {
            if (error_out) {
                *error_out =
                    "payload_ref_kind must be job_set for Execution "
                    "job-set events";
            }
            return false;
        }
        return true;
    }
    if (envelope.event_type == "Execution.JobQueued.v1"
        || envelope.event_type == "Execution.JobPendingWorkset.v1"
        || envelope.event_type == "Execution.JobClaimed.v1"
        || envelope.event_type == "Execution.JobClaimRequeued.v1"
        || envelope.event_type == "Execution.JobStarted.v1"
        || envelope.event_type == "Execution.JobLeaseRenewed.v1"
        || envelope.event_type == "Execution.JobExecutionFinished.v1"
        || envelope.event_type
            == "Execution.JobResultProcessingStarted.v1"
        || envelope.event_type == "Execution.JobResultParked.v1"
        || envelope.event_type == "Execution.JobResultProcessed.v1"
        || envelope.event_type
            == "Execution.JobCancellationRequested.v1"
        || envelope.event_type
            == "Execution.JobCancellationDelivered.v1"
        || envelope.event_type
            == "Execution.JobCancellationResolved.v1"
        || envelope.event_type == "Execution.JobCompleted.v1"
        || envelope.event_type == "Execution.JobEventArchived.v1"
        || envelope.event_type == "Execution.JobRestored.v1") {
        if (envelope.payload_ref_kind != "job") {
            if (error_out) *error_out = "payload_ref_kind must be job for Execution.Job* event";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "Execution.WorksetPublished.v1"
        || envelope.event_type == "Execution.WorksetClaimed.v1"
        || envelope.event_type == "Execution.WorksetDispatched.v1"
        || envelope.event_type == "Execution.WorksetReleased.v1") {
        if (envelope.payload_ref_kind != "workset") {
            if (error_out) {
                *error_out =
                    "payload_ref_kind must be workset for Execution "
                    "workset events";
            }
            return false;
        }
        return true;
    }

    if (envelope.event_type == "Execution.WorkflowInstanceCreated.v1"
        || envelope.event_type == "Execution.WorkflowStepReady.v1"
        || envelope.event_type == "Execution.WorkflowStepBlocked.v1"
        || envelope.event_type == "Execution.WorkflowStepMaterialized.v1"
        || envelope.event_type == "Execution.WorkflowStepCompleted.v1"
        || envelope.event_type == "Execution.WorkflowStepFailed.v1"
        || envelope.event_type == "Execution.WorkflowStepEmpty.v1"
        || envelope.event_type
            == "Execution.WorkflowStepCoordinatorFailure.v1"
        || envelope.event_type
            == "Execution.WorkflowStepDescriptorAvailable.v1"
        || envelope.event_type
            == "Execution.WorkflowStepInputRequested.v1"
        || envelope.event_type
            == "Execution.WorkflowStepInputFragmentReady.v1"
        || envelope.event_type
            == "Execution.WorkflowStepInputComplete.v1"
        || envelope.event_type
            == "Execution.WorkflowTransitionEvaluated.v1"
        || envelope.event_type
            == "Execution.WorkflowTransitionAdvanced.v1"
        || envelope.event_type
            == "Execution.WorkflowTransitionBlocked.v1"
        || envelope.event_type
            == "Execution.WorkflowInvariantViolation.v1"
        || envelope.event_type
            == "Execution.WorkflowRemediationReopened.v1"
        || envelope.event_type
            == "Execution.WorkflowRemediationRepairExecuted.v1"
        || envelope.event_type
            == "Execution.WorkflowRemediationTerminalFailed.v1"
        || envelope.event_type == "Execution.WorkflowInstanceCompleted.v1") {
        if (envelope.payload_ref_kind != "workflow_event") {
            if (error_out) *error_out = "payload_ref_kind must be workflow_event for Execution.Workflow* event";
            return false;
        }
        return true;
    }
    if (error_out) *error_out = "unsupported Execution event_type";
    return false;
}

inline bool ValidateExecutionCanonicalJobProgressPayloadV2(
    const EventEnvelope& envelope,
    std::string* error_out = nullptr) {
    if (envelope.event_version != 2
        || envelope.event_type != "Execution.JobProgressed.v2") {
        if (error_out != nullptr) {
            *error_out = "expected Execution.JobProgressed.v2";
        }
        return false;
    }
    if (!ValidateEventTypeFormat(
            envelope.event_type, envelope.event_version, error_out)) {
        return false;
    }
    if (envelope.context_name != "Execution"
        || envelope.payload_ref_kind != "job"
        || envelope.payload_ref_id <= 0) {
        if (error_out != nullptr) {
            *error_out =
                "canonical job progress must reference an Execution job";
        }
        return false;
    }
    return true;
}

inline bool ValidateAnalysisSeedProbePayloadV1(const EventEnvelope& envelope, std::string* error_out = nullptr) {
    if (!ValidateV1EnvelopeBasics(envelope, error_out)) {
        return false;
    }
    if (envelope.context_name != "AnalysisSeedProbe") {
        if (error_out) *error_out = "context_name does not match expected payload family";
        return false;
    }

    if (envelope.event_type == "AnalysisSeedProbe.SetCreated.v1") {
        if (envelope.payload_ref_kind != "probe_set") {
            if (error_out) *error_out = "payload_ref_kind must be probe_set for AnalysisSeedProbe.SetCreated.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisSeedProbe.RunRequested.v1"
        || envelope.event_type == "AnalysisSeedProbe.AcceptedInputFramesReplaced.v1"
        || envelope.event_type == "AnalysisSeedProbe.RunStatusChanged.v1"
        || envelope.event_type == "AnalysisSeedProbe.RunCompleted.v1"
        || envelope.event_type == "AnalysisSeedProbe.RunFailed.v1") {
        if (envelope.payload_ref_kind != "probe_run") {
            if (error_out) *error_out = "payload_ref_kind must be probe_run for SeedProbe run events";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisSeedProbe.ObservationRecorded.v1"
        || envelope.event_type == "AnalysisSeedProbe.EvidenceStateChanged.v1") {
        if (envelope.payload_ref_kind != "probe_result") {
            if (error_out) *error_out = "payload_ref_kind must be probe_result for SeedProbe result events";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisSeedProbe.EncounterProjectionRecorded.v1") {
        if (envelope.payload_ref_kind != "encounter_projection") {
            if (error_out) *error_out = "payload_ref_kind must be encounter_projection for AnalysisSeedProbe.EncounterProjectionRecorded.v1";
            return false;
        }
        return true;
    }
    if (error_out) *error_out = "unsupported AnalysisSeedProbe event_type";
    return false;
}

inline bool ValidateAnalysisSpinePayloadV1(const EventEnvelope& envelope, std::string* error_out = nullptr) {
    if (!ValidateV1EnvelopeBasics(envelope, error_out)) {
        return false;
    }
    if (envelope.context_name != "AnalysisSpine") {
        if (error_out) *error_out = "context_name does not match expected payload family";
        return false;
    }

    if (envelope.event_type == "AnalysisSpine.RunCreated.v1") {
        if (envelope.payload_ref_kind != "run") {
            if (error_out) *error_out = "payload_ref_kind must be run for AnalysisSpine.RunCreated.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisSpine.StateRefRegistered.v1") {
        if (envelope.payload_ref_kind != "state_ref") {
            if (error_out) *error_out = "payload_ref_kind must be state_ref for AnalysisSpine.StateRefRegistered.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisSpine.LineageEdgeAdded.v1") {
        if (envelope.payload_ref_kind != "lineage_edge") {
            if (error_out) *error_out = "payload_ref_kind must be lineage_edge for AnalysisSpine.LineageEdgeAdded.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisSpine.ArtifactLinked.v1") {
        if (envelope.payload_ref_kind != "artifact_ref") {
            if (error_out) *error_out = "payload_ref_kind must be artifact_ref for AnalysisSpine.ArtifactLinked.v1";
            return false;
        }
        return true;
    }

    if (error_out) *error_out = "unsupported AnalysisSpine event_type";
    return false;
}

inline bool ValidateAnalysisBattlePayloadV1(const EventEnvelope& envelope, std::string* error_out = nullptr) {
    if (!ValidateV1EnvelopeBasics(envelope, error_out)) {
        return false;
    }
    if (envelope.context_name != "AnalysisBattle") {
        if (error_out) *error_out = "context_name does not match expected payload family";
        return false;
    }

    if (envelope.event_type == "AnalysisBattle.BattleSetCreated.v1") {
        if (envelope.payload_ref_kind != "battle_set") {
            if (error_out) *error_out = "payload_ref_kind must be battle_set for AnalysisBattle.BattleSetCreated.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisBattle.SeedCandidateAdded.v1") {
        if (envelope.payload_ref_kind != "seed_candidate") {
            if (error_out) *error_out = "payload_ref_kind must be seed_candidate for AnalysisBattle.SeedCandidateAdded.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisBattle.TurnWaveCreated.v1") {
        if (envelope.payload_ref_kind != "turn_wave") {
            if (error_out) *error_out = "payload_ref_kind must be turn_wave for AnalysisBattle.TurnWaveCreated.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisBattle.TurnJobRecorded.v1") {
        if (envelope.payload_ref_kind != "turn_job") {
            if (error_out) *error_out = "payload_ref_kind must be turn_job for AnalysisBattle.TurnJobRecorded.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisBattle.TurnJobResultUpdated.v1") {
        if (envelope.payload_ref_kind != "turn_job") {
            if (error_out) *error_out = "payload_ref_kind must be turn_job for AnalysisBattle.TurnJobResultUpdated.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisBattle.TurnWaveStatusUpdated.v1") {
        if (envelope.payload_ref_kind != "turn_wave") {
            if (error_out) *error_out = "payload_ref_kind must be turn_wave for AnalysisBattle.TurnWaveStatusUpdated.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisBattle.BattleSetStatusUpdated.v1") {
        if (envelope.payload_ref_kind != "battle_set") {
            if (error_out) *error_out = "payload_ref_kind must be battle_set for AnalysisBattle.BattleSetStatusUpdated.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisBattle.BattleAdvancementPoolCreated.v1") {
        if (envelope.payload_ref_kind != "battle_advancement_pool") {
            if (error_out) *error_out = "payload_ref_kind must be battle_advancement_pool for AnalysisBattle.BattleAdvancementPoolCreated.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisBattle.BattleAdvancementDecisionRecorded.v1") {
        if (envelope.payload_ref_kind != "battle_advancement_decision") {
            if (error_out) *error_out = "payload_ref_kind must be battle_advancement_decision for AnalysisBattle.BattleAdvancementDecisionRecorded.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisBattle.ManualFollowupUpdated.v1") {
        if (envelope.payload_ref_kind != "manual_followup") {
            if (error_out) *error_out = "payload_ref_kind must be manual_followup for AnalysisBattle.ManualFollowupUpdated.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisBattle.BattleCompletionCreated.v1"
        || envelope.event_type == "AnalysisBattle.BattleCompletionCompleted.v1"
        || envelope.event_type == "AnalysisBattle.BattleCompletionFailed.v1") {
        if (envelope.payload_ref_kind != "battle_completion") {
            if (error_out) *error_out = "payload_ref_kind must be battle_completion for battle completion events";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisBattle.BattleResultsCreated.v1"
        || envelope.event_type == "AnalysisBattle.BattleResultsCompleted.v1"
        || envelope.event_type == "AnalysisBattle.BattleResultsFailed.v1") {
        if (envelope.payload_ref_kind != "battle_results") {
            if (error_out) *error_out = "payload_ref_kind must be battle_results for battle results events";
            return false;
        }
        return true;
    }

    if (error_out) *error_out = "unsupported AnalysisBattle event_type";
    return false;
}

inline bool ValidateStateArtifactPayloadV1(const EventEnvelope& envelope, std::string* error_out = nullptr) {
    if (!ValidateV1EnvelopeBasics(envelope, error_out)) {
        return false;
    }
    if (envelope.context_name != "State") {
        if (error_out) *error_out = "context_name does not match expected payload family";
        return false;
    }

    if (envelope.event_type == "State.ArtifactStored.v1") {
        if (envelope.payload_ref_kind != "artifact") {
            if (error_out) *error_out = "payload_ref_kind must be artifact for State.ArtifactStored.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "State.SavestateCreated.v1") {
        if (envelope.payload_ref_kind != "savestate") {
            if (error_out) *error_out = "payload_ref_kind must be savestate for State.SavestateCreated.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "State.SavestateDerived.v1") {
        if (envelope.payload_ref_kind != "savestate_derivation" && envelope.payload_ref_kind != "derivation") {
            if (error_out) *error_out = "payload_ref_kind must be savestate_derivation for State.SavestateDerived.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "State.TasMovieRootCreated.v1") {
        if (envelope.payload_ref_kind != "tas_movie_root") {
            if (error_out) *error_out = "payload_ref_kind must be tas_movie_root for State.TasMovieRootCreated.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "State.TasMovieTreeCreated.v1") {
        if (envelope.payload_ref_kind != "tas_movie_tree") {
            if (error_out) *error_out = "payload_ref_kind must be tas_movie_tree for State.TasMovieTreeCreated.v1";
            return false;
        }
        return true;
    }

    if (error_out) *error_out = "unsupported State event_type";
    return false;
}

inline bool ValidateArchivePackagePayloadV1(const EventEnvelope& envelope, std::string* error_out = nullptr) {
    return ValidateV1PayloadRef(envelope, "Archive", "archive_package", error_out);
}

inline bool ValidateEventPayloadRequiredFieldsV1(const EventEnvelope& envelope, std::string* error_out = nullptr) {
    if (envelope.context_name == "Execution") {
        return ValidateExecutionWorkflowJobPayloadV1(envelope, error_out);
    }
    if (envelope.context_name == "AnalysisSeedProbe") {
        return ValidateAnalysisSeedProbePayloadV1(envelope, error_out);
    }
    if (envelope.context_name == "AnalysisSpine") {
        return ValidateAnalysisSpinePayloadV1(envelope, error_out);
    }
    if (envelope.context_name == "AnalysisBattle") {
        return ValidateAnalysisBattlePayloadV1(envelope, error_out);
    }
    if (envelope.context_name == "State") {
        return ValidateStateArtifactPayloadV1(envelope, error_out);
    }
    if (envelope.context_name == "Archive") {
        return ValidateArchivePackagePayloadV1(envelope, error_out);
    }

    if (error_out) *error_out = "unsupported context_name";
    return false;
}

} // namespace savor::db::events
