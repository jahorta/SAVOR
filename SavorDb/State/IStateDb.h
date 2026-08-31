#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../Common/Events/EventEnvelope.h"
#include "../Common/Events/EventPayloadViews.h"
#include "../Common/Retention/OutboxRetention.h"
#include "../Common/Types/UtcTimestamp.h"

namespace savor::db {

using ArtifactPayloadRecord = events::StateArtifactPayloadView;

enum class SavestatePlaybackState {
    Unknown = 0,
    MovieInactive,
    MoviePaired,
};

inline std::string_view ToDbString(SavestatePlaybackState value) {
    switch (value) {
    case SavestatePlaybackState::MovieInactive: return "MOVIE_INACTIVE";
    case SavestatePlaybackState::MoviePaired: return "MOVIE_PAIRED";
    default: return "";
    }
}

inline SavestatePlaybackState ParseSavestatePlaybackState(
    std::string_view value) {
    if (value == "MOVIE_INACTIVE") return SavestatePlaybackState::MovieInactive;
    if (value == "MOVIE_PAIRED") return SavestatePlaybackState::MoviePaired;
    return SavestatePlaybackState::Unknown;
}

struct ArtifactStoreMetadata {
    std::string sha256;
    std::int64_t size_bytes = 0;
    int compression_kind = 0;
    std::string display_filename;
    std::string file_ext;
    std::string artifact_kind;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct CreateSavestateCommand {
    std::int64_t artifact_id = 0;
    SavestatePlaybackState playback_state =
        SavestatePlaybackState::MovieInactive;
    std::optional<std::int64_t> dtm_artifact_id;
    std::string savestate_type;
    std::string note;
    bool is_complete = false;
    types::UtcTimePoint created_at_utc{};
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
    std::string correlation_id;
    std::string causation_id;
};

struct ArtifactRecord {
    std::int64_t artifact_id = 0;
    std::string sha256;
    std::int64_t size_bytes = 0;
    int compression_kind = 0;
    // Resolved physical path for trusted backend consumers.
    std::string object_path;
    std::string object_relpath;
    std::string display_filename;
    std::string file_ext;
    std::string artifact_kind;
    types::UtcTimePoint created_at_utc{};
};

struct StoreWorkspaceArtifactCommand {
    std::filesystem::path workspace_relative_path;
    ArtifactStoreMetadata artifact;
};

struct ImportExternalArtifactCommand {
    std::filesystem::path absolute_source_path;
    ArtifactStoreMetadata artifact;
};

struct CreateOrGetSterilizedCheckpointCommand {
    std::int64_t from_savestate_id = 0;
    StoreWorkspaceArtifactCommand artifact;
    std::string savestate_type = "TAS_MOVIE_STERILIZED_CHECKPOINT";
    std::string note;
    std::string method_kind = "tasmovie.checkpoint_sterilize.v1";
    std::string source_context_kind;
    std::int64_t source_context_id = 0;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct CreateOrGetSterilizedCheckpointReceipt {
    std::int64_t savestate_id = 0;
    std::int64_t artifact_id = 0;
    std::int64_t derivation_id = 0;
    bool created = false;
};

struct CreateTasMovieRootCommand {
    std::int64_t source_dtm_artifact_id = 0;
    std::int64_t dtm_artifact_id = 0;
    std::int64_t rtc_value = 0;
    std::int64_t itinerary_artifact_id = 0;
    std::uint32_t required_final_breakpoint_pc = 0;
    std::int64_t checkpoint_savestate_id = 0;
    std::string source_context_kind;
    std::int64_t source_context_id = 0;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct TasMovieRootRecord {
    std::int64_t tas_movie_root_id = 0;
    std::int64_t source_dtm_artifact_id = 0;
    std::int64_t dtm_artifact_id = 0;
    std::int64_t rtc_value = 0;
    std::int64_t itinerary_artifact_id = 0;
    std::uint32_t required_final_breakpoint_pc = 0;
    std::int64_t checkpoint_savestate_id = 0;
    std::string source_context_kind;
    std::int64_t source_context_id = 0;
    types::UtcTimePoint created_at_utc{};
};

struct CreateTasMovieTreeCommand {
    std::int64_t tas_movie_root_id = 0;
    std::optional<std::int64_t> parent_tas_movie_tree_id;
    std::int64_t dtm_artifact_id = 0;
    std::int64_t itinerary_artifact_id = 0;
    std::uint32_t required_final_breakpoint_pc = 0;
    std::int64_t checkpoint_savestate_id = 0;
    std::string source_context_kind;
    std::int64_t source_context_id = 0;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct TasMovieTreeRecord {
    std::int64_t tas_movie_tree_id = 0;
    std::int64_t tas_movie_root_id = 0;
    std::optional<std::int64_t> parent_tas_movie_tree_id;
    std::int64_t dtm_artifact_id = 0;
    std::int64_t itinerary_artifact_id = 0;
    std::uint32_t required_final_breakpoint_pc = 0;
    std::int64_t checkpoint_savestate_id = 0;
    std::string source_context_kind;
    std::int64_t source_context_id = 0;
    types::UtcTimePoint created_at_utc{};
};

struct SavestateRecord {
    std::int64_t savestate_id = 0;
    std::int64_t artifact_id = 0;
    SavestatePlaybackState playback_state =
        SavestatePlaybackState::Unknown;
    std::optional<std::int64_t> dtm_artifact_id;
    std::string savestate_type;
    std::string note;
    bool is_complete = false;
    types::UtcTimePoint created_at_utc{};
    std::string artifact_sha256;
    std::int64_t artifact_size_bytes = 0;
    std::string artifact_filename;
    std::string artifact_file_ext;
    std::string artifact_kind;
    std::optional<std::string> dtm_sha256;
    std::optional<std::string> dtm_filename;
};

struct SavestateDerivationRecord {
    std::int64_t derivation_id = 0;
    std::int64_t from_savestate_id = 0;
    std::int64_t to_savestate_id = 0;
    std::string method_kind;
    std::string source_context_kind;
    std::int64_t source_context_id = 0;
    types::UtcTimePoint created_at_utc{};
};

struct IStateDb {
    virtual ~IStateDb() = default;

    [[nodiscard]] virtual std::filesystem::path ArtifactWorkspaceRoot() const = 0;

    virtual bool StoreWorkspaceArtifact(
        const StoreWorkspaceArtifactCommand& command,
        std::int64_t* artifact_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool ImportExternalArtifact(
        const ImportExternalArtifactCommand& command,
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

    virtual bool CreateOrGetSterilizedCheckpoint(
        const CreateOrGetSterilizedCheckpointCommand& command,
        CreateOrGetSterilizedCheckpointReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<ArtifactRecord> GetArtifact(
        std::int64_t artifact_id) const = 0;

    virtual std::optional<ArtifactRecord> GetArtifactBySha256(
        std::string_view sha256) const = 0;

    virtual std::optional<SavestateRecord> FindSavestateByArtifactId(
        std::int64_t artifact_id) const = 0;

    virtual std::optional<SavestateRecord> GetSavestate(
        std::int64_t savestate_id) const = 0;

    virtual std::vector<SavestateDerivationRecord> ListIncomingSavestateDerivations(
        std::int64_t to_savestate_id) const = 0;

    virtual std::vector<SavestateDerivationRecord> ListSavestateDerivationsBySourceContext(
        std::string_view source_context_kind,
        std::int64_t source_context_id) const = 0;

    virtual std::optional<SavestateDerivationRecord>
    FindSavestateDerivationBySourceAndMethod(
        std::int64_t from_savestate_id,
        std::string_view method_kind) const = 0;

    virtual bool CreateTasMovieRoot(
        const CreateTasMovieRootCommand& command,
        std::int64_t* tas_movie_root_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<TasMovieRootRecord> GetTasMovieRoot(
        std::int64_t tas_movie_root_id) const = 0;

    virtual std::optional<TasMovieRootRecord> FindTasMovieRootBySourceRtc(
        std::int64_t source_dtm_artifact_id,
        std::int64_t rtc_value) const = 0;

    virtual std::optional<TasMovieRootRecord> FindTasMovieRootByDtmArtifactId(
        std::int64_t dtm_artifact_id) const = 0;

    virtual bool CreateTasMovieTree(
        const CreateTasMovieTreeCommand& command,
        std::int64_t* tas_movie_tree_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<TasMovieTreeRecord> GetTasMovieTree(
        std::int64_t tas_movie_tree_id) const = 0;

    virtual std::optional<TasMovieTreeRecord> FindTasMovieTreeByDtmArtifactId(
        std::int64_t dtm_artifact_id) const = 0;

    virtual std::vector<TasMovieTreeRecord> ListTasMovieTreeLineage(
        std::int64_t tas_movie_tree_id) const = 0;

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

inline std::optional<StoreWorkspaceArtifactCommand>
MakeStoreWorkspaceArtifactCommand(
    IStateDb* state_db,
    const std::filesystem::path& source_path,
    ArtifactStoreMetadata artifact,
    std::string* error_out = nullptr) {
    if (state_db == nullptr || !source_path.is_absolute()) {
        if (error_out) {
            *error_out = "workspace artifact publisher requires an absolute source path";
        }
        return std::nullopt;
    }
    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(
        state_db->ArtifactWorkspaceRoot(), ec);
    if (ec || root.empty()) {
        if (error_out) *error_out = "artifact workspace root cannot be resolved";
        return std::nullopt;
    }
    const auto source = std::filesystem::weakly_canonical(source_path, ec);
    const auto relative = source.lexically_relative(root);
    if (ec || relative.empty() ||
        (!relative.empty() && relative.begin()->string() == "..")) {
        if (error_out) {
            *error_out = "artifact source is outside the configured workspace: " +
                source_path.string();
        }
        return std::nullopt;
    }
    return StoreWorkspaceArtifactCommand{
        .workspace_relative_path = relative,
        .artifact = std::move(artifact),
    };
}

inline bool StoreWorkspaceArtifactFile(
    IStateDb* state_db,
    const std::filesystem::path& source_path,
    ArtifactStoreMetadata artifact,
    std::int64_t* artifact_id_out = nullptr,
    std::string* error_out = nullptr) {
    auto command = MakeStoreWorkspaceArtifactCommand(
        state_db, source_path, std::move(artifact), error_out);
    return command && state_db->StoreWorkspaceArtifact(
        *command, artifact_id_out, error_out);
}

} // namespace savor::db
