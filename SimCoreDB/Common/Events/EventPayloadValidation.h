#pragma once

#include <string>
#include <string_view>

#include "EventEnvelope.h"
#include "EventTypeFormat.h"

namespace simcore::db::events {

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
    return ValidateV1PayloadRef(envelope, "Execution", "workflow_event", error_out);
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
    if (envelope.event_type == "AnalysisBattle.SelectionPoolCreated.v1") {
        if (envelope.payload_ref_kind != "selection_pool") {
            if (error_out) *error_out = "payload_ref_kind must be selection_pool for AnalysisBattle.SelectionPoolCreated.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisBattle.SelectionDecisionRecorded.v1") {
        if (envelope.payload_ref_kind != "selection_decision") {
            if (error_out) *error_out = "payload_ref_kind must be selection_decision for AnalysisBattle.SelectionDecisionRecorded.v1";
            return false;
        }
        return true;
    }
    if (envelope.event_type == "AnalysisBattle.TerminalFollowupUpdated.v1") {
        if (envelope.payload_ref_kind != "terminal_followup") {
            if (error_out) *error_out = "payload_ref_kind must be terminal_followup for AnalysisBattle.TerminalFollowupUpdated.v1";
            return false;
        }
        return true;
    }

    if (error_out) *error_out = "unsupported AnalysisBattle event_type";
    return false;
}

inline bool ValidateStateArtifactPayloadV1(const EventEnvelope& envelope, std::string* error_out = nullptr) {
    return ValidateV1PayloadRef(envelope, "State", "artifact", error_out);
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

} // namespace simcore::db::events
