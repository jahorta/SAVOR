#pragma once

#include "Runner/Runtime/RuntimeTypes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace savor::runtime {

struct StateHandleIdTag;
struct StateArtifactIdTag;

using StateHandleId = StrongId<StateHandleIdTag>;
using StateArtifactId = StrongId<StateArtifactIdTag>;

enum class StateServiceErrorCode : std::uint16_t
{
    None,
    InvalidArgument,
    InvalidState,
    Unsupported,
    StaleEpoch,
    NotFound,
    CompatibilityMismatch,
    CapacityExceeded,
    ParticipantFailure,
    BackendFailure,
    ArtifactFailure,
    IntegrityFailure,
};

enum class StateIntegrity : std::uint8_t
{
    Preserved,
    Unknown,
};

enum class StateReplacementKind : std::uint8_t
{
    Boot,
    Reboot,
    RestoreMemoryHandle,
    RestoreFileArtifact,
};

enum class MovieCheckpointMode : std::uint8_t
{
    None,
    ReadOnlyPlayback,
    Recording,
};

enum class ExternalMovieImportMode : std::uint8_t
{
    Unspecified,
    NoMovie,
    ReadOnlyPlayback,
    Recording,
};

struct StateCompatibilityToken
{
    std::string game_id;
    std::string iso_sha256;
    std::string emulator_build;
    std::string runtime_revision;

    auto operator<=>(const StateCompatibilityToken&) const = default;

    [[nodiscard]] bool Complete() const noexcept
    {
        if (game_id.empty() || iso_sha256.size() != 64 ||
            emulator_build.empty())
        {
            return false;
        }
        for (const char value : iso_sha256)
        {
            if (!((value >= '0' && value <= '9') ||
                  (value >= 'a' && value <= 'f')))
            {
                return false;
            }
        }
        return true;
    }
};

struct MovieCheckpointMetadata
{
    MovieCheckpointMode mode = MovieCheckpointMode::None;
    std::string dtm_sha256;
    std::string game_id;
    std::vector<std::uint8_t> dtm_bytes;
    std::filesystem::path dtm_path;
    std::uint64_t current_frame = 0;
    std::uint64_t current_input_count = 0;
    // Captured checkpoints carry an exact cursor. External state/movie pairs
    // intentionally begin without one: Dolphin restores the cursor from the
    // savestate, after which MovieService records the observed position.
    bool cursor_known = false;
    bool starts_from_savestate = false;
    // Recording continuations are intentionally process-local. This value is
    // assigned by StateService when a handle is captured and is never accepted
    // from an external artifact.
    std::uint64_t recording_session_generation = 0;

    [[nodiscard]] bool HasMovie() const noexcept
    {
        return mode != MovieCheckpointMode::None;
    }
};

struct StateLineage
{
    std::optional<StateHandleId> parent_handle;
    std::optional<StateArtifactId> parent_artifact;
    std::string edge;
    std::string producer;
};

struct StateServiceResult
{
    bool ok = false;
    StateServiceErrorCode code = StateServiceErrorCode::BackendFailure;
    StateIntegrity integrity = StateIntegrity::Preserved;
    std::string message;

    [[nodiscard]] static StateServiceResult Success()
    {
        return {true, StateServiceErrorCode::None, StateIntegrity::Preserved, {}};
    }

    [[nodiscard]] static StateServiceResult Failure(
        StateServiceErrorCode code,
        std::string message,
        StateIntegrity integrity = StateIntegrity::Preserved)
    {
        return {false, code, integrity, std::move(message)};
    }
};

struct StateOperationReceipt
{
    StateServiceResult result;
    StateReplacementKind operation = StateReplacementKind::Boot;
    StateEpoch origin_epoch;
    StateEpoch resulting_epoch;
    StateCompatibilityToken compatibility;
};

struct StateHandleReceipt
{
    StateServiceResult result;
    StateHandleId handle;
    StateEpoch captured_epoch;
    std::size_t size_bytes = 0;
    std::string sha256;
    StateCompatibilityToken compatibility;
    StateLineage lineage;
    std::optional<MovieCheckpointMetadata> movie;
};

struct StateFileArtifactReceipt
{
    StateServiceResult result;
    StateArtifactId artifact;
    StateEpoch captured_epoch;
    std::filesystem::path path;
    std::size_t size_bytes = 0;
    std::string sha256;
    StateCompatibilityToken compatibility;
    StateLineage lineage;
    std::optional<MovieCheckpointMetadata> movie;
    bool external = false;
};

struct StateBootRequest
{
    std::optional<std::filesystem::path> startup_savestate;
    std::optional<MovieCheckpointMetadata> movie;
    std::string diagnostic_label;
};

struct StateHandleCaptureRequest
{
    std::optional<MovieCheckpointMetadata> movie;
    StateLineage lineage;
};

struct StateFileCaptureRequest
{
    std::filesystem::path path;
    std::optional<MovieCheckpointMetadata> movie;
    StateLineage lineage;
};

struct StateFileImportRequest
{
    std::filesystem::path path;
    std::string expected_sha256;
    StateCompatibilityToken compatibility;
    ExternalMovieImportMode movie_mode = ExternalMovieImportMode::Unspecified;
    std::optional<std::filesystem::path> dtm_path;
    std::string expected_dtm_sha256;
    StateLineage lineage;
};

struct StateReplacementContext
{
    StateReplacementKind kind = StateReplacementKind::Boot;
    StateEpoch origin_epoch;
    StateEpoch candidate_epoch;
    StateCompatibilityToken compatibility;
    std::optional<MovieCheckpointMetadata> movie;
    bool external_artifact = false;
};

} // namespace savor::runtime
