#pragma once

#include "Runner/Runtime/RuntimeTypes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace savor::runtime {

struct SavestateHandleIdTag;
struct SavestateArtifactIdTag;

using SavestateHandleId = StrongId<SavestateHandleIdTag>;
using SavestateArtifactId = StrongId<SavestateArtifactIdTag>;

// Dolphin discovers a savestate's movie history at the exact companion path
// formed by appending ".dtm" to the complete savestate path. Keep this
// identity in the shared artifact contract so capture, finalization, restore,
// and baseline validation cannot independently choose different paths.
[[nodiscard]] inline std::filesystem::path SavestateDtmSidecarPath(
    const std::filesystem::path& savestate)
{
    return std::filesystem::path(savestate.string() + ".dtm");
}

// Immutable ownership transfer for state/movie bytes captured while Dolphin
// is authoritatively paused. The bytes may be copied between host-only
// pipeline records, but cannot be mutated after capture.
class ImmutableSavestateBytes final
{
public:
    ImmutableSavestateBytes() = default;

    [[nodiscard]] static ImmutableSavestateBytes Capture(
        std::vector<std::uint8_t> bytes)
    {
        ImmutableSavestateBytes captured;
        captured.bytes_ =
            std::make_shared<const std::vector<std::uint8_t>>(
                std::move(bytes));
        return captured;
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return static_cast<bool>(bytes_);
    }
    [[nodiscard]] const std::uint8_t* data() const noexcept
    {
        return bytes_ ? bytes_->data() : nullptr;
    }
    [[nodiscard]] std::size_t size() const noexcept
    {
        return bytes_ ? bytes_->size() : 0;
    }

private:
    std::shared_ptr<const std::vector<std::uint8_t>> bytes_;
};

enum class SavestateServiceErrorCode : std::uint16_t
{
    None,
    InvalidArgument,
    InvalidState,
    Unsupported,
    WorksetMismatch,
    NotFound,
    CompatibilityMismatch,
    CapacityExceeded,
    BackendFailure,
    ArtifactFailure,
    IntegrityFailure,
};

enum class GuestIntegrity : std::uint8_t
{
    Preserved,
    Unknown,
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

struct ArtifactCompatibilityToken
{
    std::string game_id;
    std::string iso_sha256;
    std::string emulator_build;
    std::string runtime_revision;

    auto operator<=>(const ArtifactCompatibilityToken&) const = default;

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
    // Recording continuations are intentionally workset-local and are never
    // accepted from an external artifact.
    WorksetEpoch recording_workset_epoch;

    [[nodiscard]] bool HasMovie() const noexcept
    {
        return mode != MovieCheckpointMode::None;
    }
};

struct ArtifactLineage
{
    std::optional<SavestateHandleId> parent_handle;
    std::optional<SavestateArtifactId> parent_artifact;
    std::string edge;
    std::string producer;

    auto operator<=>(const ArtifactLineage&) const = default;
};

struct SavestateServiceResult
{
    bool ok = false;
    SavestateServiceErrorCode code = SavestateServiceErrorCode::BackendFailure;
    GuestIntegrity integrity = GuestIntegrity::Preserved;
    std::string message;

    [[nodiscard]] static SavestateServiceResult Success()
    {
        return {true, SavestateServiceErrorCode::None, GuestIntegrity::Preserved, {}};
    }

    [[nodiscard]] static SavestateServiceResult Failure(
        SavestateServiceErrorCode code,
        std::string message,
        GuestIntegrity integrity = GuestIntegrity::Preserved)
    {
        return {false, code, integrity, std::move(message)};
    }
};

struct SavestateRestoreReceipt
{
    SavestateServiceResult result;
    WorksetEpoch workset_epoch;
    ArtifactCompatibilityToken compatibility;
    std::optional<MovieCheckpointMetadata> movie;
};

struct SavestateHandleReceipt
{
    SavestateServiceResult result;
    SavestateHandleId handle;
    WorksetEpoch captured_epoch;
    std::size_t size_bytes = 0;
    std::string sha256;
    ArtifactCompatibilityToken compatibility;
    ArtifactLineage lineage;
    std::optional<MovieCheckpointMetadata> movie;
};

struct SavestateFileArtifactReceipt
{
    SavestateServiceResult result;
    SavestateArtifactId artifact;
    WorksetEpoch captured_epoch;
    std::filesystem::path path;
    std::size_t size_bytes = 0;
    std::string sha256;
    ArtifactCompatibilityToken compatibility;
    ArtifactLineage lineage;
    std::optional<MovieCheckpointMetadata> movie;
    bool external = false;
};

// First half of asynchronous immutable artifact publication. The actor
// captures these bytes synchronously while the session is idle-paused, then
// transfers the receipt to a host-only finalizer. No path has been published
// and no authoritative artifact reference exists yet.
struct ImmutableSavestateArtifactCaptureReceipt
{
    SavestateServiceResult result;
    SavestateArtifactId artifact;
    WorksetEpoch captured_epoch;
    std::filesystem::path final_path;
    ImmutableSavestateBytes state_bytes;
    std::optional<ImmutableSavestateBytes> movie_bytes;
    ArtifactCompatibilityToken compatibility;
    ArtifactLineage lineage;
    std::optional<MovieCheckpointMetadata> movie;

    [[nodiscard]] std::size_t resident_bytes() const noexcept
    {
        return state_bytes.size() +
            (movie_bytes ? movie_bytes->size() : 0);
    }
};

// Host finalization evidence returned to the actor. SavestateService accepts it
// only for the exact pending capture and then records the ordinary immutable
// file artifact. Hashing and filesystem publication have already completed.
struct ImmutableSavestateArtifactPublicationReceipt
{
    SavestateArtifactId artifact;
    std::filesystem::path state_path;
    std::size_t state_size_bytes = 0;
    std::string state_sha256;
    std::optional<std::filesystem::path> movie_path;
    std::size_t movie_size_bytes = 0;
    std::string movie_sha256;
};

struct SavestateHandleCaptureRequest
{
    std::optional<MovieCheckpointMetadata> movie;
    ArtifactLineage lineage;
};

// Controls only the movie bytes published beside an immutable SAV. The
// ordinary mode publishes the exact checkpoint-time movie sidecar. A complete
// DTM producer may instead retain that checkpoint internally as a prefix
// witness and return the SAV for later pairing with the finalized recording.
enum class SavestateMovieArtifactMode : std::uint8_t
{
    ExactCheckpointSidecar = 0,
    DeferredFinalRecordingPair = 1,
};

struct SavestateCaptureRequest
{
    std::filesystem::path path;
    std::optional<MovieCheckpointMetadata> movie;
    ArtifactLineage lineage;
    SavestateMovieArtifactMode movie_artifact_mode =
        SavestateMovieArtifactMode::ExactCheckpointSidecar;
};

struct SavestateImportRequest
{
    std::filesystem::path path;
    std::string expected_sha256;
    ArtifactCompatibilityToken compatibility;
    ExternalMovieImportMode movie_mode = ExternalMovieImportMode::Unspecified;
    std::optional<std::filesystem::path> dtm_path;
    std::string expected_dtm_sha256;
    ArtifactLineage lineage;
};

} // namespace savor::runtime
