#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>

#include "IStateDb.h"

namespace savor::db::state {

class SqliteStateDb final : public savor::db::IStateDb {
public:
    explicit SqliteStateDb(sqlite3* db);

    bool StoreArtifact(
        const StoreArtifactCommand& command,
        std::int64_t* artifact_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool CreateSavestate(
        const CreateSavestateCommand& command,
        std::int64_t* savestate_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool DeriveSavestate(
        const DeriveSavestateCommand& command,
        std::int64_t* derivation_id_out = nullptr,
        std::string* error_out = nullptr) override;

    std::optional<ArtifactRecord> GetArtifact(std::int64_t artifact_id) const override;
    std::optional<ArtifactRecord> GetArtifactBySha256(std::string_view sha256) const override;
    std::optional<SavestateRecord> FindSavestateByArtifactId(std::int64_t artifact_id) const override;
    std::optional<SavestateRecord> GetSavestate(
        std::int64_t savestate_id) const override;
    std::vector<SavestateDerivationRecord> ListIncomingSavestateDerivations(
        std::int64_t to_savestate_id) const override;
    std::vector<SavestateDerivationRecord> ListSavestateDerivationsBySourceContext(
        std::string_view source_context_kind,
        std::int64_t source_context_id) const override;

    bool CreateTasMovieRoot(const CreateTasMovieRootCommand& command, std::int64_t* tas_movie_root_id_out = nullptr, std::string* error_out = nullptr) override;
    std::optional<TasMovieRootRecord> GetTasMovieRoot(std::int64_t tas_movie_root_id) const override;
    std::optional<TasMovieRootRecord> FindTasMovieRootBySourceRtc(std::int64_t source_dtm_artifact_id, std::int64_t rtc_value) const override;
    std::optional<TasMovieRootRecord> FindTasMovieRootByDtmArtifactId(std::int64_t dtm_artifact_id) const override;
    bool CreateTasMovieTree(const CreateTasMovieTreeCommand& command, std::int64_t* tas_movie_tree_id_out = nullptr, std::string* error_out = nullptr) override;
    std::optional<TasMovieTreeRecord> GetTasMovieTree(std::int64_t tas_movie_tree_id) const override;
    std::optional<TasMovieTreeRecord> FindTasMovieTreeByDtmArtifactId(std::int64_t dtm_artifact_id) const override;
    std::vector<TasMovieTreeRecord> ListTasMovieTreeLineage(std::int64_t tas_movie_tree_id) const override;

    std::optional<std::string> MaterializeArtifactToDirectory(
        std::int64_t artifact_id,
        std::string_view output_directory,
        std::string* error_out = nullptr) const override;

    std::optional<std::string> MaterializeArtifactToPath(
        std::int64_t artifact_id,
        std::string_view output_path,
        std::string* error_out = nullptr) const override;

    std::optional<std::string> MaterializeSavestateToPath(
        std::int64_t savestate_id,
        std::string_view output_path,
        std::string* error_out = nullptr) const override;

    std::vector<events::EventEnvelope> ReadUnpublishedOutboxBatch(
        std::int64_t after_outbox_id,
        int max_batch_size) override;

    bool MarkOutboxPublished(
        std::int64_t outbox_id,
        types::UtcTimePoint published_at_utc) override;

    bool MarkOutboxPublishFailure(
        std::int64_t outbox_id,
        std::string_view last_error) override;
    retention::OutboxRetentionPreview PreviewOutboxRetention(
        const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
        types::UtcTimePoint now_utc,
        const retention::OutboxRetentionPolicy& policy) const override;
    bool PurgeOutboxThroughRetentionFloor(
        const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
        types::UtcTimePoint now_utc,
        const retention::OutboxRetentionPolicy& policy,
        int max_rows,
        int* rows_deleted_out = nullptr,
        std::string* error_out = nullptr) override;

    std::optional<ArtifactPayloadRecord> ResolveArtifactPayload(
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;

    std::optional<ArtifactPayloadRecord> ResolveArtifactPayload(
        const events::EventEnvelope& envelope) const override;

private:
    std::optional<ArtifactPayloadRecord> ResolveArtifactPayloadForEvent(
        std::string_view event_type,
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const;

    sqlite3* db_ = nullptr;
};

} // namespace savor::db::state
