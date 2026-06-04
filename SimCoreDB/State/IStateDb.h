#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../Common/Events/EventEnvelope.h"
#include "../Common/Events/EventPayloadViews.h"
#include "../Common/Retention/OutboxRetention.h"
#include "../Common/Types/UtcTimestamp.h"

namespace simcore::db {

using ArtifactPayloadRecord = events::StateArtifactPayloadView;

struct StoreArtifactCommand {
    std::string sha256;
    std::int64_t size_bytes = 0;
    int compression_kind = 0;
    std::string filename;
    std::string file_ext;
    std::string artifact_kind;
    types::UtcTimePoint created_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct CreateSavestateCommand {
    std::int64_t artifact_id = 0;
    std::string savestate_type;
    std::string note;
    bool is_complete = false;
    types::UtcTimePoint created_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct DeriveSavestateCommand {
    std::int64_t from_savestate_id = 0;
    std::int64_t to_savestate_id = 0;
    std::string method_kind;
    std::string source_context_kind;
    std::int64_t source_context_id = 0;
    types::UtcTimePoint created_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct CreateTasVariantCommand {
    std::string name;
    std::int64_t base_dtm_artifact_id = 0;
    std::optional<std::int64_t> dtmini_artifact_id;
    std::string mutation_mode;
    std::optional<std::int64_t> rtc_value;
    std::optional<std::string> bookmark_name;
    std::optional<std::int64_t> insert_frame_count;
    std::optional<std::int64_t> parent_tas_variant_id;
    std::optional<std::int64_t> produced_savestate_id;
    types::UtcTimePoint created_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct IStateDb {
    virtual ~IStateDb() = default;

    virtual bool StoreArtifact(
        const StoreArtifactCommand& command,
        std::int64_t* artifact_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool CreateSavestate(
        const CreateSavestateCommand& command,
        std::int64_t* savestate_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool DeriveSavestate(
        const DeriveSavestateCommand& command,
        std::int64_t* derivation_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool CreateTasVariant(
        const CreateTasVariantCommand& command,
        std::int64_t* tas_variant_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    // Materializes an artifact to a directory using "<sha256><file_ext>" from state_artifact.
    virtual std::optional<std::string> MaterializeArtifactToDirectory(
        std::int64_t artifact_id,
        std::string_view output_directory,
        std::string* error_out = nullptr) const = 0;

    // Materializes an artifact to an explicit destination path.
    virtual std::optional<std::string> MaterializeArtifactToPath(
        std::int64_t artifact_id,
        std::string_view output_path,
        std::string* error_out = nullptr) const = 0;

    // Materializes the artifact backing a savestate to an explicit destination path.
    virtual std::optional<std::string> MaterializeSavestateToPath(
        std::int64_t savestate_id,
        std::string_view output_path,
        std::string* error_out = nullptr) const = 0;

    // Reads unpublished outbox rows in ascending outbox cursor order.
    virtual std::vector<events::EventEnvelope> ReadUnpublishedOutboxBatch(
        std::int64_t after_outbox_id,
        int max_batch_size) = 0;

    // Marks an outbox row as published.
    virtual bool MarkOutboxPublished(
        std::int64_t outbox_id,
        types::UtcTimePoint published_at_utc) = 0;

    // Increments delivery attempt count and stores the most recent publish error.
    virtual bool MarkOutboxPublishFailure(
        std::int64_t outbox_id,
        std::string_view last_error) = 0;

    virtual retention::OutboxRetentionPreview PreviewOutboxRetention(
        const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
        types::UtcTimePoint now_utc,
        const retention::OutboxRetentionPolicy& policy) const = 0;

    virtual bool PurgeOutboxThroughRetentionFloor(
        const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
        types::UtcTimePoint now_utc,
        const retention::OutboxRetentionPolicy& policy,
        int max_rows,
        int* rows_deleted_out = nullptr,
        std::string* error_out = nullptr) = 0;

    // Resolves state/artifact event payload references for a specific event version.
    virtual std::optional<ArtifactPayloadRecord> ResolveArtifactPayload(
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<ArtifactPayloadRecord> ResolveArtifactPayload(
        const events::EventEnvelope& envelope) const = 0;
};

} // namespace simcore::db
