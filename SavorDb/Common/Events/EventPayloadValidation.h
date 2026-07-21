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

    if (envelope.event_type == "Execution.JobSetCreated.v1") {
        if (envelope.payload_ref_kind != "job_set") {
            if (error_out) *error_out = "payload_ref_kind must be job_set for Execution.JobSetCreated.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "Execution.JobQueued.v1"
        || envelope.event_type == "Execution.JobClaimed.v1"
        || envelope.event_type == "Execution.JobStarted.v1"
        || envelope.event_type == "Execution.JobLeaseRenewed.v1"
        || envelope.event_type == "Execution.JobProgressed.v1"
        || envelope.event_type == "Execution.JobCompleted.v1"
        || envelope.event_type == "Execution.JobEventArchived.v1"
        || envelope.event_type == "Execution.JobRestored.v1") {
        if (envelope.payload_ref_kind != "job") {
            if (error_out) *error_out = "payload_ref_kind must be job for Execution.Job* event";
            return false;
        }
        return true;
    }

    if (envelope.event_type == "Execution.WorkflowInstanceCreated.v1"
        || envelope.event_type == "Execution.WorkflowStepReady.v1"
        || envelope.event_type == "Execution.WorkflowStepMaterialized.v1"
        || envelope.event_type == "Execution.WorkflowStepCompleted.v1"
        || envelope.event_type == "Execution.WorkflowStepFailed.v1"
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
    if (envelope.event_type == "AnalysisSeedProbe.RunRequested.v1") {
        if (envelope.payload_ref_kind != "probe_run") {
            if (error_out) *error_out = "payload_ref_kind must be probe_run for AnalysisSeedProbe.RunRequested.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisSeedProbe.NeutralSeedRecorded.v1") {
        if (envelope.payload_ref_kind != "neutral_seed") {
            if (error_out) *error_out = "payload_ref_kind must be neutral_seed for AnalysisSeedProbe.NeutralSeedRecorded.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisSeedProbe.GridSeedRecorded.v1") {
        if (envelope.payload_ref_kind != "grid_seed") {
            if (error_out) *error_out = "payload_ref_kind must be grid_seed for AnalysisSeedProbe.GridSeedRecorded.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisSeedProbe.UniqueSeedRecorded.v1") {
        if (envelope.payload_ref_kind != "unique_seed") {
            if (error_out) *error_out = "payload_ref_kind must be unique_seed for AnalysisSeedProbe.UniqueSeedRecorded.v1";
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
    if (envelope.event_type == "AnalysisSeedProbe.RunCompleted.v1") {
        if (envelope.payload_ref_kind != "probe_result") {
            if (error_out) *error_out = "payload_ref_kind must be probe_result for AnalysisSeedProbe.RunCompleted.v1";
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
    if (envelope.event_type == "State.TasVariantCreated.v1") {
        if (envelope.payload_ref_kind != "tas_variant" && envelope.payload_ref_kind != "tas-variant") {
            if (error_out) *error_out = "payload_ref_kind must be tas_variant for State.TasVariantCreated.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "State.TasVariantProducedSavestateSet.v1") {
        if (envelope.payload_ref_kind != "tas_variant" && envelope.payload_ref_kind != "tas-variant") {
            if (error_out) *error_out = "payload_ref_kind must be tas_variant for State.TasVariantProducedSavestateSet.v1";
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
