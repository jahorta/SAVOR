#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../Common/Events/EventEnvelope.h"
#include "../Common/Events/EventPayloadViews.h"
#include "../Common/Types/UtcTimestamp.h"

namespace simcore::db {

using SeedProbePayloadRecord = events::AnalysisSeedProbePayloadView;
using BattlePayloadRecord = events::AnalysisBattlePayloadView;
using SpinePayloadRecord = events::AnalysisSpinePayloadView;

struct RequestSeedProbeRunCommand {
    std::int64_t probe_set_id = 0;
    std::int64_t entry_savestate_id = 0;
    std::int64_t seed_probe_spec_id = 0;
    int codec_version = 1;
    std::string status;
    types::UtcTimePoint requested_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct RecordSeedProbeNeutralSeedCommand {
    std::int64_t probe_result_id = 0;
    std::int64_t neutral_seed_value = 0;
    std::string source_kind;
    types::UtcTimePoint recorded_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct RecordSeedProbeGridSeedCommand {
    std::int64_t probe_result_id = 0;
    std::string source_family;
    std::int64_t axis_xy_id = 0;
    std::int64_t seed_value = 0;
    std::int64_t seed_delta = 0;
    types::UtcTimePoint recorded_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct RecordSeedProbeUniqueSeedCommand {
    std::int64_t probe_result_id = 0;
    std::int64_t input_frame_id = 0;
    std::int64_t seed_value = 0;
    std::int64_t seed_delta = 0;
    types::UtcTimePoint recorded_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct RecordSeedProbeEncounterProjectionCommand {
    std::int64_t probe_run_id = 0;
    std::int64_t seed_value = 0;
    int option_ordinal = 0;
    std::string encounter_id;
    std::int64_t encounter_frame = 0;
    std::optional<std::int64_t> stutter_step_at;
    bool movement_required = false;
    types::UtcTimePoint recorded_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct CompleteSeedProbeRunCommand {
    std::int64_t probe_run_id = 0;
    std::optional<std::int64_t> neutral_seed_value;
    int grid_count = 0;
    int unique_count = 0;
    std::string result_status;
    std::string run_status;
    types::UtcTimePoint recorded_at_utc{};
    types::UtcTimePoint completed_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct IAnalysisDb {
    virtual ~IAnalysisDb() = default;

    virtual bool RequestSeedProbeRun(
        const RequestSeedProbeRunCommand& command,
        std::int64_t* probe_run_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool RecordSeedProbeNeutralSeed(
        const RecordSeedProbeNeutralSeedCommand& command,
        std::int64_t* neutral_seed_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool RecordSeedProbeGridSeed(
        const RecordSeedProbeGridSeedCommand& command,
        std::int64_t* grid_seed_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool RecordSeedProbeUniqueSeed(
        const RecordSeedProbeUniqueSeedCommand& command,
        std::int64_t* unique_seed_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool RecordSeedProbeEncounterProjection(
        const RecordSeedProbeEncounterProjectionCommand& command,
        std::int64_t* encounter_projection_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool CompleteSeedProbeRun(
        const CompleteSeedProbeRunCommand& command,
        std::int64_t* probe_result_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::vector<events::EventEnvelope> ReadUnpublishedOutboxBatch(
        std::int64_t after_outbox_id,
        int max_batch_size) = 0;

    virtual bool MarkOutboxPublished(
        std::int64_t outbox_id,
        types::UtcTimePoint published_at_utc) = 0;

    virtual bool MarkOutboxPublishFailure(
        std::int64_t outbox_id,
        std::string_view last_error) = 0;

    // Seed-probe family payload resolver.
    virtual std::optional<SeedProbePayloadRecord> ResolveSeedProbePayload(
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<SeedProbePayloadRecord> ResolveSeedProbePayload(
        const events::EventEnvelope& envelope) const = 0;

    // Battle-analysis family payload resolver.
    virtual std::optional<BattlePayloadRecord> ResolveBattlePayload(
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<BattlePayloadRecord> ResolveBattlePayload(
        const events::EventEnvelope& envelope) const = 0;

    // Analysis-spine family payload resolver.
    virtual std::optional<SpinePayloadRecord> ResolveSpinePayload(
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<SpinePayloadRecord> ResolveSpinePayload(
        const events::EventEnvelope& envelope) const = 0;
};

} // namespace simcore::db
