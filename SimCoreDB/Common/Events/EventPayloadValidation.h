#pragma once

#include <string>
#include <string_view>

#include "EventEnvelope.h"
#include "EventTypeFormat.h"

namespace simcore::db::events {

inline bool ValidateV1PayloadRef(
    const EventEnvelope& envelope,
    std::string_view expected_context_name,
    std::string_view expected_payload_ref_kind,
    std::string* error_out = nullptr) {
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
    if (envelope.context_name != expected_context_name) {
        if (error_out) *error_out = "context_name does not match expected payload family";
        return false;
    }
    if (envelope.payload_ref_kind != expected_payload_ref_kind) {
        if (error_out) *error_out = "payload_ref_kind does not match expected payload family";
        return false;
    }
    if (envelope.payload_ref_id <= 0) {
        if (error_out) *error_out = "payload_ref_id must be > 0";
        return false;
    }
    return true;
}

inline bool ValidateExecutionWorkflowJobPayloadV1(const EventEnvelope& envelope, std::string* error_out = nullptr) {
    return ValidateV1PayloadRef(envelope, "Execution", "workflow_event", error_out);
}

inline bool ValidateAnalysisSeedProbePayloadV1(const EventEnvelope& envelope, std::string* error_out = nullptr) {
    return ValidateV1PayloadRef(envelope, "AnalysisSeedProbe", "seedprobe_event", error_out);
}

inline bool ValidateAnalysisBattlePayloadV1(const EventEnvelope& envelope, std::string* error_out = nullptr) {
    return ValidateV1PayloadRef(envelope, "AnalysisBattle", "battle_event", error_out);
}

inline bool ValidateStateArtifactPayloadV1(const EventEnvelope& envelope, std::string* error_out = nullptr) {
    return ValidateV1PayloadRef(envelope, "State", "artifact_event", error_out);
}

inline bool ValidateArchivePackagePayloadV1(const EventEnvelope& envelope, std::string* error_out = nullptr) {
    return ValidateV1PayloadRef(envelope, "Archive", "archive_event", error_out);
}

} // namespace simcore::db::events
