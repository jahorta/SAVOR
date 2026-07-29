#include "Runner/Runtime/Services/State/StateService.h"

#include "Tas/DtmFile.h"
#include "Utils/Hash.h"

#include <algorithm>
#include <array>
#include <exception>
#include <fstream>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace savor::runtime {
namespace {

[[nodiscard]] std::size_t MovieByteSize(
    const std::optional<MovieCheckpointMetadata>& movie) noexcept
{
    return movie.has_value() ? movie->dtm_bytes.size() : 0;
}

[[nodiscard]] bool HasDtmMagic(
    const std::vector<std::uint8_t>& bytes) noexcept
{
    constexpr std::array<std::uint8_t, 4> magic{'D', 'T', 'M', 0x1a};
    return bytes.size() >= savor::tas::DtmFile::kMinHeader &&
        std::equal(magic.begin(), magic.end(), bytes.begin());
}

[[nodiscard]] StateServiceResult ReadFile(
    const std::filesystem::path& path,
    std::vector<std::uint8_t>& out)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::ArtifactFailure,
            "Unable to open artifact " + path.string());
    }
    const std::streamsize size = input.tellg();
    if (size < 0)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::ArtifactFailure,
            "Unable to determine artifact size for " + path.string());
    }
    out.resize(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    if (size > 0 && !input.read(
            reinterpret_cast<char*>(out.data()),
            size))
    {
        out.clear();
        return StateServiceResult::Failure(
            StateServiceErrorCode::ArtifactFailure,
            "Unable to read artifact " + path.string());
    }
    return StateServiceResult::Success();
}

[[nodiscard]] StateServiceResult WriteExclusive(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes)
{
    if (std::filesystem::exists(path))
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::ArtifactFailure,
            "Immutable artifact already exists: " + path.string());
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::ArtifactFailure,
            "Unable to create artifact " + path.string());
    }
    if (!bytes.empty())
    {
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    output.flush();
    const bool good = output.good();
    output.close();
    if (!good)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::ArtifactFailure,
            "Unable to finalize artifact " + path.string());
    }
    return StateServiceResult::Success();
}

[[nodiscard]] std::string HashFile(
    const std::filesystem::path& path,
    StateServiceResult& result)
{
    try
    {
        result = StateServiceResult::Success();
        return hash::sha256_of_file(path.string());
    }
    catch (const std::exception& ex)
    {
        result = StateServiceResult::Failure(
            StateServiceErrorCode::ArtifactFailure,
            ex.what());
        return {};
    }
}

[[nodiscard]] std::filesystem::path DtmSidecar(
    const std::filesystem::path& state_path)
{
    return std::filesystem::path(state_path.string() + ".dtm");
}

[[nodiscard]] std::filesystem::path StagingPath(
    const std::filesystem::path& final_path,
    std::uint64_t id)
{
    return std::filesystem::path(
        final_path.string() + ".savor-stage-" + std::to_string(id));
}

void RemoveOwnedFile(const std::filesystem::path& path) noexcept
{
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

[[nodiscard]] bool CompleteSha256(std::string_view value) noexcept
{
    return value.size() == 64 &&
        std::all_of(
            value.begin(),
            value.end(),
            [](char ch) {
                return (ch >= '0' && ch <= '9') ||
                    (ch >= 'a' && ch <= 'f');
            });
}

} // namespace

StateService::StateService(
    IStateBackendPort& backend,
    StateServiceLimits limits)
    : backend_(backend),
      owner_thread_(std::this_thread::get_id()),
      limits_(limits)
{
}

StateServiceResult StateService::RegisterParticipant(
    IStateReplacementParticipant& participant)
{
    if (!OnOwnerThread())
        return WrongThread();
    if (open_ || replacing_ || stopped_)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::InvalidState,
            "State replacement participants must be registered before boot");
    }
    if (std::find(
            participants_.begin(),
            participants_.end(),
            &participant) != participants_.end())
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::InvalidArgument,
            "State replacement participant is already registered");
    }
    participants_.push_back(&participant);
    return StateServiceResult::Success();
}

StateOperationReceipt StateService::Boot(const StateBootRequest& request)
{
    StateOperationReceipt receipt;
    receipt.operation = StateReplacementKind::Boot;
    if (!OnOwnerThread())
    {
        receipt.result = WrongThread();
        return receipt;
    }
    receipt.origin_epoch = current_epoch_;
    receipt.resulting_epoch = current_epoch_;
    if (open_ || stopped_)
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::InvalidState,
            stopped_ ? "StateService is stopped" : "StateService is already open");
        return receipt;
    }
    if (!request.movie.has_value() &&
        request.startup_savestate.has_value())
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::InvalidArgument,
            "A startup savestate requires explicit movie continuation metadata");
        return receipt;
    }

    std::optional<MovieCheckpointMetadata> movie = request.movie;
    if (StateServiceResult validation =
            NormalizeMovieMetadata(movie, false);
        !validation.ok)
    {
        receipt.result = std::move(validation);
        return receipt;
    }

    StateReplacementContext context{
        .kind = StateReplacementKind::Boot,
        .origin_epoch = current_epoch_,
        .candidate_epoch = StateEpoch(1),
        .compatibility = {},
        .movie = std::move(movie),
        .external_artifact = false,
    };
    StateBootRequest normalized = request;
    normalized.movie = context.movie;
    receipt = ReplaceState(
        std::move(context),
        [&] { return backend_.Boot(normalized); });
    return receipt;
}

StateOperationReceipt StateService::Reboot(const StateBootRequest& request)
{
    StateOperationReceipt receipt;
    receipt.operation = StateReplacementKind::Reboot;
    if (!OnOwnerThread())
    {
        receipt.result = WrongThread();
        return receipt;
    }
    receipt.origin_epoch = current_epoch_;
    receipt.resulting_epoch = current_epoch_;
    if (StateServiceResult ready = ValidateReady(); !ready.ok)
    {
        receipt.result = std::move(ready);
        return receipt;
    }
    if (!request.movie.has_value() &&
        request.startup_savestate.has_value())
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::InvalidArgument,
            "A startup savestate requires explicit movie continuation metadata");
        return receipt;
    }

    std::optional<MovieCheckpointMetadata> movie = request.movie;
    if (StateServiceResult validation =
            NormalizeMovieMetadata(movie, false);
        !validation.ok)
    {
        receipt.result = std::move(validation);
        return receipt;
    }
    const std::optional<StateEpoch> candidate = NextEpoch();
    if (!candidate.has_value())
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::IntegrityFailure,
            "StateEpoch is exhausted",
            StateIntegrity::Unknown);
        tainted_ = true;
        return receipt;
    }
    StateReplacementContext context{
        .kind = StateReplacementKind::Reboot,
        .origin_epoch = current_epoch_,
        .candidate_epoch = *candidate,
        .compatibility = compatibility_,
        .movie = std::move(movie),
        .external_artifact = false,
    };
    StateBootRequest normalized = request;
    normalized.movie = context.movie;
    receipt = ReplaceState(
        std::move(context),
        [&] { return backend_.Reboot(normalized); });
    if (receipt.result.ok)
        ++session_generation_;
    return receipt;
}

StateServiceResult StateService::Shutdown() noexcept
{
    if (!OnOwnerThread())
        return WrongThread();
    if (shutdown_result_.has_value())
        return *shutdown_result_;
    stopped_ = true;
    replacing_ = false;
    pending_file_artifacts_.clear();
    StateBackendResult backend = backend_.Shutdown();
    open_ = false;
    if (!backend.ok)
    {
        if (backend.integrity == StateIntegrity::Unknown)
            tainted_ = true;
        shutdown_result_ = StateServiceResult::Failure(
            StateServiceErrorCode::BackendFailure,
            backend.message.empty() ? "State backend shutdown failed" :
                                      std::move(backend.message),
            backend.integrity);
        return *shutdown_result_;
    }
    shutdown_result_ = StateServiceResult::Success();
    return *shutdown_result_;
}

StateHandleReceipt StateService::CaptureMemoryHandle(
    const StateHandleCaptureRequest& request)
{
    StateHandleReceipt receipt;
    if (!OnOwnerThread())
    {
        receipt.result = WrongThread();
        return receipt;
    }
    receipt.captured_epoch = current_epoch_;
    if (StateServiceResult ready = ValidateReady(); !ready.ok)
    {
        receipt.result = std::move(ready);
        return receipt;
    }
    if (memory_handles_.size() >= limits_.maximum_memory_handles)
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::CapacityExceeded,
            "The in-memory state handle count limit was reached");
        return receipt;
    }

    std::optional<MovieCheckpointMetadata> movie = request.movie;
    if (StateServiceResult validation =
            NormalizeMovieMetadata(movie, false);
        !validation.ok)
    {
        receipt.result = std::move(validation);
        return receipt;
    }
    if (movie.has_value() &&
        movie->mode == MovieCheckpointMode::Recording)
    {
        movie->recording_session_generation = session_generation_;
    }

    StateBackendBufferResult saved = backend_.SaveStateBuffer();
    if (!saved.result.ok)
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::BackendFailure,
            saved.result.message.empty() ? "State buffer capture failed" :
                                           std::move(saved.result.message),
            saved.result.integrity);
        if (saved.result.integrity == StateIntegrity::Unknown)
            tainted_ = true;
        return receipt;
    }
    if (saved.bytes.empty())
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::ArtifactFailure,
            "State backend returned an empty state buffer");
        return receipt;
    }
    const std::size_t total_bytes =
        saved.bytes.size() + MovieByteSize(movie);
    if (total_bytes > limits_.maximum_handle_bytes ||
        total_bytes > limits_.maximum_total_memory_bytes -
            std::min(
                memory_bytes_in_use_,
                limits_.maximum_total_memory_bytes))
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::CapacityExceeded,
            "The in-memory state byte limit was reached");
        return receipt;
    }
    if (next_handle_id_ == 0)
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::IntegrityFailure,
            "StateHandleId is exhausted",
            StateIntegrity::Unknown);
        tainted_ = true;
        return receipt;
    }

    receipt.result = StateServiceResult::Success();
    receipt.handle = StateHandleId(next_handle_id_++);
    receipt.captured_epoch = current_epoch_;
    receipt.size_bytes = total_bytes;
    receipt.sha256 = hash::sha256(
        saved.bytes.data(),
        saved.bytes.size());
    receipt.compatibility = compatibility_;
    receipt.lineage = request.lineage;
    receipt.movie = std::move(movie);

    memory_bytes_in_use_ += total_bytes;
    memory_handles_.emplace(
        receipt.handle.value(),
        MemoryRecord{receipt, std::move(saved.bytes)});
    return receipt;
}

StateOperationReceipt StateService::RestoreMemoryHandle(
    StateHandleId handle)
{
    StateOperationReceipt receipt;
    receipt.operation = StateReplacementKind::RestoreMemoryHandle;
    if (!OnOwnerThread())
    {
        receipt.result = WrongThread();
        return receipt;
    }
    receipt.origin_epoch = current_epoch_;
    receipt.resulting_epoch = current_epoch_;
    if (StateServiceResult ready = ValidateReady(); !ready.ok)
    {
        receipt.result = std::move(ready);
        return receipt;
    }
    const auto found = memory_handles_.find(handle.value());
    if (!handle || found == memory_handles_.end())
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::NotFound,
            "State memory handle was not found");
        return receipt;
    }
    const MemoryRecord& record = found->second;
    if (StateServiceResult compatible =
            ValidateCompatibility(record.receipt.compatibility);
        !compatible.ok)
    {
        receipt.result = std::move(compatible);
        return receipt;
    }
    if (record.receipt.movie.has_value() &&
        record.receipt.movie->mode == MovieCheckpointMode::Recording &&
        record.receipt.movie->recording_session_generation !=
            session_generation_)
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::Unsupported,
            "Cold restoration of an in-progress recording is unsupported");
        return receipt;
    }

    const std::optional<StateEpoch> candidate = NextEpoch();
    if (!candidate.has_value())
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::IntegrityFailure,
            "StateEpoch is exhausted",
            StateIntegrity::Unknown);
        tainted_ = true;
        return receipt;
    }
    StateReplacementContext context{
        .kind = StateReplacementKind::RestoreMemoryHandle,
        .origin_epoch = current_epoch_,
        .candidate_epoch = *candidate,
        .compatibility = compatibility_,
        .movie = record.receipt.movie,
        .external_artifact = false,
    };
    return ReplaceState(
        std::move(context),
        [&] { return backend_.RestoreStateBuffer(record.bytes); });
}

StateServiceResult StateService::ReleaseMemoryHandle(
    StateHandleId handle) noexcept
{
    if (!OnOwnerThread())
        return WrongThread();
    const auto found = memory_handles_.find(handle.value());
    if (!handle || found == memory_handles_.end())
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::NotFound,
            "State memory handle was not found");
    }
    memory_bytes_in_use_ -= found->second.receipt.size_bytes;
    memory_handles_.erase(found);
    return StateServiceResult::Success();
}

std::optional<StateHandleReceipt> StateService::DescribeMemoryHandle(
    StateHandleId handle) const
{
    if (!OnOwnerThread())
        return std::nullopt;
    const auto found = memory_handles_.find(handle.value());
    if (!handle || found == memory_handles_.end())
        return std::nullopt;
    return found->second.receipt;
}

StateFileArtifactReceipt StateService::CaptureFileArtifact(
    const StateFileCaptureRequest& request)
{
    StateFileArtifactReceipt receipt;
    receipt.path = request.path;
    if (!OnOwnerThread())
    {
        receipt.result = WrongThread();
        return receipt;
    }
    receipt.captured_epoch = current_epoch_;
    if (StateServiceResult ready = ValidateReady(); !ready.ok)
    {
        receipt.result = std::move(ready);
        return receipt;
    }
    if (request.path.empty() ||
        request.path.parent_path().empty() ||
        !std::filesystem::is_directory(request.path.parent_path()))
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::InvalidArgument,
            "A caller-declared path in an existing directory is required");
        return receipt;
    }
    const std::filesystem::path final_dtm = DtmSidecar(request.path);
    if (std::filesystem::exists(request.path) ||
        std::filesystem::exists(final_dtm))
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::ArtifactFailure,
            "Caller-declared immutable state artifact already exists");
        return receipt;
    }

    std::optional<MovieCheckpointMetadata> movie = request.movie;
    if (StateServiceResult validation =
            NormalizeMovieMetadata(movie, false);
        !validation.ok)
    {
        receipt.result = std::move(validation);
        return receipt;
    }
    if (movie.has_value() &&
        movie->mode == MovieCheckpointMode::Recording)
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::Unsupported,
            "In-progress movie recording checkpoints are limited to same-session memory handles");
        return receipt;
    }
    if (next_artifact_id_ == 0)
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::IntegrityFailure,
            "StateArtifactId is exhausted",
            StateIntegrity::Unknown);
        tainted_ = true;
        return receipt;
    }

    const std::uint64_t id = next_artifact_id_++;
    const std::filesystem::path staging =
        StagingPath(request.path, id);
    const std::filesystem::path staging_dtm = DtmSidecar(staging);
    if (std::filesystem::exists(staging) ||
        std::filesystem::exists(staging_dtm))
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::ArtifactFailure,
            "Caller path collides with an existing staging artifact");
        return receipt;
    }

    StateBackendResult saved = backend_.SaveStateFile(staging);
    if (!saved.ok)
    {
        RemoveOwnedFile(staging);
        RemoveOwnedFile(staging_dtm);
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::BackendFailure,
            saved.message.empty() ? "State file capture failed" :
                                    std::move(saved.message),
            saved.integrity);
        if (saved.integrity == StateIntegrity::Unknown)
            tainted_ = true;
        return receipt;
    }
    if (!std::filesystem::is_regular_file(staging))
    {
        RemoveOwnedFile(staging_dtm);
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::ArtifactFailure,
            "State backend did not create the staged artifact");
        return receipt;
    }

    if (movie.has_value())
    {
        if (std::filesystem::exists(staging_dtm))
        {
            StateServiceResult hash_result;
            const std::string actual =
                HashFile(staging_dtm, hash_result);
            if (!hash_result.ok || actual != movie->dtm_sha256)
            {
                RemoveOwnedFile(staging);
                RemoveOwnedFile(staging_dtm);
                receipt.result = hash_result.ok ?
                    StateServiceResult::Failure(
                        StateServiceErrorCode::ArtifactFailure,
                        "Backend movie sidecar does not match the exact DTM") :
                    std::move(hash_result);
                return receipt;
            }
        }
        else if (StateServiceResult written =
                     WriteExclusive(staging_dtm, movie->dtm_bytes);
                 !written.ok)
        {
            RemoveOwnedFile(staging);
            RemoveOwnedFile(staging_dtm);
            receipt.result = std::move(written);
            return receipt;
        }
    }
    else
    {
        RemoveOwnedFile(staging_dtm);
    }

    StateServiceResult hash_result;
    const std::string state_hash = HashFile(staging, hash_result);
    if (!hash_result.ok)
    {
        RemoveOwnedFile(staging);
        RemoveOwnedFile(staging_dtm);
        receipt.result = std::move(hash_result);
        return receipt;
    }

    std::error_code ec;
    if (movie.has_value())
    {
        std::filesystem::rename(staging_dtm, final_dtm, ec);
        if (ec)
        {
            RemoveOwnedFile(staging);
            RemoveOwnedFile(staging_dtm);
            receipt.result = StateServiceResult::Failure(
                StateServiceErrorCode::ArtifactFailure,
                "Unable to publish immutable movie sidecar: " + ec.message());
            return receipt;
        }
    }
    std::filesystem::rename(staging, request.path, ec);
    if (ec)
    {
        RemoveOwnedFile(staging);
        if (movie.has_value())
            RemoveOwnedFile(final_dtm);
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::ArtifactFailure,
            "Unable to publish immutable state artifact: " + ec.message());
        return receipt;
    }

    receipt.result = StateServiceResult::Success();
    receipt.artifact = StateArtifactId(id);
    receipt.captured_epoch = current_epoch_;
    receipt.path = request.path;
    receipt.size_bytes =
        static_cast<std::size_t>(std::filesystem::file_size(request.path));
    if (movie.has_value())
    {
        receipt.size_bytes +=
            static_cast<std::size_t>(std::filesystem::file_size(final_dtm));
    }
    receipt.sha256 = state_hash;
    receipt.compatibility = compatibility_;
    receipt.lineage = request.lineage;
    if (movie.has_value())
        movie->dtm_path = final_dtm;
    receipt.movie = std::move(movie);
    receipt.external = false;
    file_artifacts_.emplace(id, FileRecord{receipt});
    return receipt;
}

ImmutableStateArtifactCaptureReceipt
StateService::CaptureImmutableArtifact(
    const StateFileCaptureRequest& request)
{
    ImmutableStateArtifactCaptureReceipt receipt;
    receipt.final_path = request.path;
    receipt.captured_epoch = current_epoch_;
    if (!OnOwnerThread())
    {
        receipt.result = WrongThread();
        return receipt;
    }
    if (StateServiceResult ready = ValidateReady(); !ready.ok)
    {
        receipt.result = std::move(ready);
        return receipt;
    }
    if (request.path.empty() || request.path.filename().empty())
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::InvalidArgument,
            "Immutable state capture requires a caller-declared final path");
        return receipt;
    }
    const std::filesystem::path normalized =
        request.path.lexically_normal();
    const auto path_pending =
        std::ranges::any_of(
            pending_file_artifacts_,
            [&](const auto& entry) {
                return entry.second.path.lexically_normal() ==
                    normalized;
            });
    const auto path_committed =
        std::ranges::any_of(
            file_artifacts_,
            [&](const auto& entry) {
                return entry.second.receipt.path.lexically_normal() ==
                    normalized;
            });
    if (path_pending || path_committed)
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::ArtifactFailure,
            "Immutable state artifact path is already owned");
        return receipt;
    }

    std::optional<MovieCheckpointMetadata> movie = request.movie;
    if (movie)
    {
        if (!movie->HasMovie() ||
            movie->mode == MovieCheckpointMode::Recording)
        {
            receipt.result = StateServiceResult::Failure(
                movie->mode == MovieCheckpointMode::Recording
                    ? StateServiceErrorCode::Unsupported
                    : StateServiceErrorCode::InvalidArgument,
                movie->mode == MovieCheckpointMode::Recording
                    ? "Asynchronous file publication does not support an "
                      "in-progress movie recording"
                    : "Movie checkpoint metadata must declare a movie mode");
            return receipt;
        }
        // Async capture never performs file reads or hashes on the actor.
        // MovieService supplies the exact bytes while paused; the finalizer
        // validates their optional expected digest off-thread.
        if (!HasDtmMagic(movie->dtm_bytes))
        {
            receipt.result = StateServiceResult::Failure(
                StateServiceErrorCode::ArtifactFailure,
                "Asynchronous movie continuation requires exact DTM bytes");
            return receipt;
        }
        const std::string dtm_game_id(
            reinterpret_cast<const char*>(
                movie->dtm_bytes.data() + 4),
            6);
        if ((!movie->game_id.empty() &&
             movie->game_id != dtm_game_id) ||
            (compatibility_.Complete() &&
             compatibility_.game_id != dtm_game_id))
        {
            receipt.result = StateServiceResult::Failure(
                StateServiceErrorCode::CompatibilityMismatch,
                "Movie continuation belongs to another game");
            return receipt;
        }
        if (!movie->dtm_sha256.empty() &&
            !CompleteSha256(movie->dtm_sha256))
        {
            receipt.result = StateServiceResult::Failure(
                StateServiceErrorCode::InvalidArgument,
                "Movie continuation expected SHA-256 is malformed");
            return receipt;
        }
        movie->game_id = dtm_game_id;
        movie->starts_from_savestate =
            movie->dtm_bytes[12] != 0;
    }
    if (next_artifact_id_ == 0)
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::IntegrityFailure,
            "StateArtifactId is exhausted",
            StateIntegrity::Unknown);
        tainted_ = true;
        return receipt;
    }

    StateBackendBufferResult saved = backend_.SaveStateBuffer();
    if (!saved.result.ok)
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::BackendFailure,
            saved.result.message.empty()
                ? "Immutable state buffer capture failed"
                : std::move(saved.result.message),
            saved.result.integrity);
        if (saved.result.integrity == StateIntegrity::Unknown)
            tainted_ = true;
        return receipt;
    }
    if (saved.bytes.empty())
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::ArtifactFailure,
            "State backend returned an empty state buffer");
        return receipt;
    }

    const StateArtifactId artifact(next_artifact_id_++);
    const std::size_t state_size = saved.bytes.size();
    std::size_t movie_size = 0;
    receipt.result = StateServiceResult::Success();
    receipt.artifact = artifact;
    receipt.captured_epoch = current_epoch_;
    receipt.final_path = request.path;
    receipt.state_bytes =
        ImmutableStateBytes::Capture(std::move(saved.bytes));
    receipt.compatibility = compatibility_;
    receipt.lineage = request.lineage;
    if (movie)
    {
        movie_size = movie->dtm_bytes.size();
        receipt.movie_bytes = ImmutableStateBytes::Capture(
            std::move(movie->dtm_bytes));
        movie->dtm_bytes.clear();
        movie->dtm_path = DtmSidecar(request.path);
        receipt.movie = movie;
    }

    pending_file_artifacts_.emplace(
        artifact.value(),
        PendingFileRecord{
            artifact,
            current_epoch_,
            request.path,
            state_size,
            movie_size,
            compatibility_,
            request.lineage,
            std::move(movie)});
    return receipt;
}

StateFileArtifactReceipt StateService::CommitImmutableArtifact(
    const ImmutableStateArtifactPublicationReceipt& publication)
{
    StateFileArtifactReceipt receipt;
    receipt.artifact = publication.artifact;
    receipt.path = publication.state_path;
    if (!OnOwnerThread())
    {
        receipt.result = WrongThread();
        return receipt;
    }
    const auto found =
        pending_file_artifacts_.find(publication.artifact.value());
    if (!publication.artifact ||
        found == pending_file_artifacts_.end())
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::NotFound,
            "Pending immutable state capture was not found");
        return receipt;
    }
    const PendingFileRecord pending = found->second;
    const bool movie_expected = pending.movie.has_value();
    if (publication.state_path.lexically_normal() !=
            pending.path.lexically_normal() ||
        publication.state_size_bytes != pending.state_size_bytes ||
        !CompleteSha256(publication.state_sha256) ||
        movie_expected != publication.movie_path.has_value() ||
        (movie_expected &&
         (publication.movie_path->lexically_normal() !=
              DtmSidecar(pending.path).lexically_normal() ||
          publication.movie_size_bytes != pending.movie_size_bytes ||
          !CompleteSha256(publication.movie_sha256))))
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::IntegrityFailure,
            "Finalized state artifact does not match its exact pending "
            "capture");
        return receipt;
    }
    if (file_artifacts_.contains(publication.artifact.value()))
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::IntegrityFailure,
            "Finalized state artifact identity is already committed");
        return receipt;
    }

    receipt.result = StateServiceResult::Success();
    receipt.artifact = pending.artifact;
    receipt.captured_epoch = pending.captured_epoch;
    receipt.path = pending.path;
    receipt.size_bytes = publication.state_size_bytes +
        publication.movie_size_bytes;
    receipt.sha256 = publication.state_sha256;
    receipt.compatibility = pending.compatibility;
    receipt.lineage = pending.lineage;
    receipt.movie = pending.movie;
    if (receipt.movie)
    {
        receipt.movie->dtm_path = *publication.movie_path;
        receipt.movie->dtm_sha256 = publication.movie_sha256;
    }
    receipt.external = false;
    file_artifacts_.emplace(
        receipt.artifact.value(),
        FileRecord{receipt});
    pending_file_artifacts_.erase(found);
    return receipt;
}

StateServiceResult StateService::AbandonImmutableArtifact(
    StateArtifactId artifact) noexcept
{
    if (!OnOwnerThread())
        return WrongThread();
    if (!artifact ||
        pending_file_artifacts_.erase(artifact.value()) == 0)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::NotFound,
            "Pending immutable state capture was not found");
    }
    return StateServiceResult::Success();
}

StateFileArtifactReceipt StateService::ImportFileArtifact(
    const StateFileImportRequest& request)
{
    StateFileArtifactReceipt receipt;
    receipt.path = request.path;
    receipt.external = true;
    if (!OnOwnerThread())
    {
        receipt.result = WrongThread();
        return receipt;
    }
    if (StateServiceResult ready = ValidateReady(); !ready.ok)
    {
        receipt.result = std::move(ready);
        return receipt;
    }
    if (request.movie_mode == ExternalMovieImportMode::Unspecified)
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::InvalidArgument,
            "External state import requires an explicit movie mode");
        return receipt;
    }
    if (request.movie_mode == ExternalMovieImportMode::Recording)
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::Unsupported,
            "Cold import of an in-progress recording is unsupported");
        return receipt;
    }
    if (!std::filesystem::is_regular_file(request.path) ||
        request.expected_sha256.empty())
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::InvalidArgument,
            "External state path and expected SHA-256 are required");
        return receipt;
    }
    if (StateServiceResult compatible =
            ValidateCompatibility(request.compatibility);
        !compatible.ok)
    {
        receipt.result = std::move(compatible);
        return receipt;
    }

    StateServiceResult hash_result;
    const std::string actual_hash = HashFile(request.path, hash_result);
    if (!hash_result.ok || actual_hash != request.expected_sha256)
    {
        receipt.result = hash_result.ok ?
            StateServiceResult::Failure(
                StateServiceErrorCode::ArtifactFailure,
                "External state SHA-256 does not match") :
            std::move(hash_result);
        return receipt;
    }

    std::optional<MovieCheckpointMetadata> movie;
    const std::filesystem::path expected_sidecar =
        DtmSidecar(request.path);
    if (request.movie_mode == ExternalMovieImportMode::NoMovie)
    {
        if (std::filesystem::exists(expected_sidecar) ||
            request.dtm_path.has_value())
        {
            receipt.result = StateServiceResult::Failure(
                StateServiceErrorCode::InvalidArgument,
                "NoMovie import conflicts with a DTM sidecar");
            return receipt;
        }
    }
    else
    {
        const std::filesystem::path dtm =
            request.dtm_path.value_or(expected_sidecar);
        if (dtm != expected_sidecar ||
            request.expected_dtm_sha256.empty())
        {
            receipt.result = StateServiceResult::Failure(
                StateServiceErrorCode::InvalidArgument,
                "Read-only movie import requires the exact <state>.dtm sidecar and SHA-256");
            return receipt;
        }
        movie = MovieCheckpointMetadata{
            .mode = MovieCheckpointMode::ReadOnlyPlayback,
            .dtm_sha256 = request.expected_dtm_sha256,
            .dtm_path = dtm,
        };
        if (StateServiceResult validation =
                NormalizeMovieMetadata(
                    movie,
                    true,
                    request.movie_mode);
            !validation.ok)
        {
            receipt.result = std::move(validation);
            return receipt;
        }
    }
    if (next_artifact_id_ == 0)
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::IntegrityFailure,
            "StateArtifactId is exhausted",
            StateIntegrity::Unknown);
        tainted_ = true;
        return receipt;
    }

    receipt.result = StateServiceResult::Success();
    receipt.artifact = StateArtifactId(next_artifact_id_++);
    receipt.captured_epoch = current_epoch_;
    receipt.path = request.path;
    receipt.size_bytes =
        static_cast<std::size_t>(std::filesystem::file_size(request.path));
    receipt.sha256 = actual_hash;
    receipt.compatibility = request.compatibility;
    receipt.lineage = request.lineage;
    receipt.movie = std::move(movie);
    receipt.external = true;
    file_artifacts_.emplace(
        receipt.artifact.value(),
        FileRecord{receipt});
    return receipt;
}

StateOperationReceipt StateService::RestoreFileArtifact(
    StateArtifactId artifact)
{
    StateOperationReceipt receipt;
    receipt.operation = StateReplacementKind::RestoreFileArtifact;
    if (!OnOwnerThread())
    {
        receipt.result = WrongThread();
        return receipt;
    }
    receipt.origin_epoch = current_epoch_;
    receipt.resulting_epoch = current_epoch_;
    if (StateServiceResult ready = ValidateReady(); !ready.ok)
    {
        receipt.result = std::move(ready);
        return receipt;
    }
    const auto found = file_artifacts_.find(artifact.value());
    if (!artifact || found == file_artifacts_.end())
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::NotFound,
            "State file artifact was not found");
        return receipt;
    }
    const StateFileArtifactReceipt& record = found->second.receipt;
    if (StateServiceResult compatible =
            ValidateCompatibility(record.compatibility);
        !compatible.ok)
    {
        receipt.result = std::move(compatible);
        return receipt;
    }
    StateServiceResult hash_result;
    const std::string state_hash = HashFile(record.path, hash_result);
    if (!hash_result.ok || state_hash != record.sha256)
    {
        receipt.result = hash_result.ok ?
            StateServiceResult::Failure(
                StateServiceErrorCode::ArtifactFailure,
                "Immutable state artifact changed after publication") :
            std::move(hash_result);
        return receipt;
    }
    if (record.movie.has_value())
    {
        if (record.movie->mode == MovieCheckpointMode::Recording)
        {
            receipt.result = StateServiceResult::Failure(
                StateServiceErrorCode::Unsupported,
                "In-progress movie recording checkpoints may be restored only from same-session memory handles");
            return receipt;
        }
        StateServiceResult dtm_hash_result;
        const std::string dtm_hash =
            HashFile(DtmSidecar(record.path), dtm_hash_result);
        if (!dtm_hash_result.ok ||
            dtm_hash != record.movie->dtm_sha256)
        {
            receipt.result = dtm_hash_result.ok ?
                StateServiceResult::Failure(
                    StateServiceErrorCode::ArtifactFailure,
                    "Immutable DTM continuation changed after publication") :
                std::move(dtm_hash_result);
            return receipt;
        }
    }

    const std::optional<StateEpoch> candidate = NextEpoch();
    if (!candidate.has_value())
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::IntegrityFailure,
            "StateEpoch is exhausted",
            StateIntegrity::Unknown);
        tainted_ = true;
        return receipt;
    }
    StateReplacementContext context{
        .kind = StateReplacementKind::RestoreFileArtifact,
        .origin_epoch = current_epoch_,
        .candidate_epoch = *candidate,
        .compatibility = compatibility_,
        .movie = record.movie,
        .external_artifact = record.external,
    };
    return ReplaceState(
        std::move(context),
        [&] { return backend_.RestoreStateFile(record.path); });
}

StateServiceResult StateService::ReleaseFileArtifact(
    StateArtifactId artifact) noexcept
{
    if (!OnOwnerThread())
        return WrongThread();
    const auto found = file_artifacts_.find(artifact.value());
    if (!artifact || found == file_artifacts_.end())
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::NotFound,
            "State file artifact was not found");
    }
    // Releasing the service record never deletes the caller-owned immutable
    // artifact.
    file_artifacts_.erase(found);
    return StateServiceResult::Success();
}

std::optional<StateFileArtifactReceipt> StateService::DescribeFileArtifact(
    StateArtifactId artifact) const
{
    if (!OnOwnerThread())
        return std::nullopt;
    const auto found = file_artifacts_.find(artifact.value());
    if (!artifact || found == file_artifacts_.end())
        return std::nullopt;
    return found->second.receipt;
}

StateOperationReceipt StateService::ReplaceState(
    StateReplacementContext context,
    const std::function<StateBackendResult()>& operation)
{
    StateOperationReceipt receipt{
        .result = StateServiceResult::Failure(
            StateServiceErrorCode::BackendFailure,
            "State replacement did not run"),
        .operation = context.kind,
        .origin_epoch = context.origin_epoch,
        .resulting_epoch = context.origin_epoch,
        .compatibility = compatibility_,
    };
    if (replacing_)
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::InvalidState,
            "Another state replacement is active");
        return receipt;
    }
    replacing_ = true;

    std::size_t prepared = 0;
    for (IStateReplacementParticipant* participant : participants_)
    {
        StateServiceResult result =
            participant->PrepareStateReplacement(context);
        if (!result.ok)
        {
            const StateServiceResult rollback =
                RollbackPrepared(context, prepared);
            replacing_ = false;
            if (!rollback.ok)
            {
                tainted_ = true;
                receipt.result = rollback;
            }
            else
            {
                receipt.result = StateServiceResult::Failure(
                    StateServiceErrorCode::ParticipantFailure,
                    result.message.empty() ?
                        "State replacement participant rejected prepare" :
                        std::move(result.message),
                    result.integrity);
            }
            return receipt;
        }
        ++prepared;
    }

    StateBackendResult backend = operation();
    if (!backend.ok)
    {
        if (backend.integrity == StateIntegrity::Preserved)
        {
            const StateServiceResult rollback =
                RollbackPrepared(context, prepared);
            if (!rollback.ok)
            {
                tainted_ = true;
                receipt.result = rollback;
            }
            else
            {
                receipt.result = StateServiceResult::Failure(
                    StateServiceErrorCode::BackendFailure,
                    backend.message.empty() ? "State backend replacement failed" :
                                              std::move(backend.message));
            }
        }
        else
        {
            tainted_ = true;
            receipt.result = StateServiceResult::Failure(
                StateServiceErrorCode::IntegrityFailure,
                backend.message.empty() ?
                    "State backend integrity is uncertain" :
                    std::move(backend.message),
                StateIntegrity::Unknown);
        }
        replacing_ = false;
        return receipt;
    }

    current_epoch_ = context.candidate_epoch;
    if (context.kind == StateReplacementKind::Boot)
    {
        open_ = true;
        ++session_generation_;
        compatibility_ = backend_.CurrentCompatibility();
        context.compatibility = compatibility_;
        if (!compatibility_.Complete())
        {
            (void)RollbackPrepared(context, prepared);
            tainted_ = true;
            replacing_ = false;
            receipt.result = StateServiceResult::Failure(
                StateServiceErrorCode::IntegrityFailure,
                "State backend returned an incomplete compatibility token",
                StateIntegrity::Unknown);
            receipt.resulting_epoch = current_epoch_;
            receipt.compatibility = compatibility_;
            return receipt;
        }
        if (context.movie.has_value() &&
            context.movie->game_id != compatibility_.game_id)
        {
            (void)RollbackPrepared(context, prepared);
            tainted_ = true;
            replacing_ = false;
            receipt.result = StateServiceResult::Failure(
                StateServiceErrorCode::CompatibilityMismatch,
                "Booted game does not match the prepared movie",
                StateIntegrity::Unknown);
            receipt.resulting_epoch = current_epoch_;
            receipt.compatibility = compatibility_;
            return receipt;
        }
    }

    StateServiceResult first_commit_failure =
        StateServiceResult::Success();
    for (IStateReplacementParticipant* participant : participants_)
    {
        StateServiceResult result =
            participant->CommitStateReplacement(context);
        if (!result.ok && first_commit_failure.ok)
            first_commit_failure = std::move(result);
    }
    replacing_ = false;
    receipt.resulting_epoch = current_epoch_;
    receipt.compatibility = compatibility_;
    if (!first_commit_failure.ok)
    {
        tainted_ = true;
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::ParticipantFailure,
            first_commit_failure.message.empty() ?
                "State replacement participant failed commit" :
                std::move(first_commit_failure.message),
            StateIntegrity::Unknown);
        return receipt;
    }
    receipt.result = StateServiceResult::Success();
    return receipt;
}

StateServiceResult StateService::ValidateReady() const
{
    if (!OnOwnerThread())
        return WrongThread();
    if (stopped_)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::InvalidState,
            "StateService is stopped");
    }
    if (tainted_)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::IntegrityFailure,
            "StateService is tainted",
            StateIntegrity::Unknown);
    }
    if (!open_)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::InvalidState,
            "StateService has no open session");
    }
    if (replacing_)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::InvalidState,
            "A state replacement is already active");
    }
    return StateServiceResult::Success();
}

StateServiceResult StateService::ValidateCompatibility(
    const StateCompatibilityToken& token) const
{
    if (!token.Complete())
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::InvalidArgument,
            "State compatibility token is incomplete");
    }
    if (token != compatibility_)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::CompatibilityMismatch,
            "State artifact is incompatible with the open session");
    }
    return StateServiceResult::Success();
}

StateServiceResult StateService::NormalizeMovieMetadata(
    std::optional<MovieCheckpointMetadata>& movie,
    bool external,
    ExternalMovieImportMode import_mode) const
{
    if (!movie.has_value())
        return StateServiceResult::Success();
    if (!movie->HasMovie())
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::InvalidArgument,
            "Movie checkpoint metadata must declare a movie mode");
    }
    if (external &&
        (import_mode != ExternalMovieImportMode::ReadOnlyPlayback ||
         movie->mode != MovieCheckpointMode::ReadOnlyPlayback))
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::Unsupported,
            "External movie continuation must be read-only playback");
    }
    if (movie->mode == MovieCheckpointMode::Recording && external)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::Unsupported,
            "Cold restoration of an in-progress recording is unsupported");
    }

    if (movie->dtm_bytes.empty())
    {
        if (movie->dtm_path.empty())
        {
            return StateServiceResult::Failure(
                StateServiceErrorCode::InvalidArgument,
                "Movie continuation requires exact DTM bytes or a DTM path");
        }
        if (StateServiceResult read =
                ReadFile(movie->dtm_path, movie->dtm_bytes);
            !read.ok)
        {
            return read;
        }
    }
    if (!HasDtmMagic(movie->dtm_bytes))
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::ArtifactFailure,
            "Movie continuation is not a valid DTM");
    }
    const std::string actual = hash::sha256(
        movie->dtm_bytes.data(),
        movie->dtm_bytes.size());
    if (!movie->dtm_sha256.empty() && movie->dtm_sha256 != actual)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::ArtifactFailure,
            "Movie continuation SHA-256 does not match");
    }
    movie->dtm_sha256 = actual;
    const std::string dtm_game_id(
        reinterpret_cast<const char*>(movie->dtm_bytes.data() + 4),
        6);
    if (!movie->game_id.empty() && movie->game_id != dtm_game_id)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::ArtifactFailure,
            "Movie continuation game ID does not match its DTM");
    }
    movie->game_id = dtm_game_id;
    movie->starts_from_savestate = movie->dtm_bytes[12] != 0;
    if (compatibility_.Complete() &&
        movie->game_id != compatibility_.game_id)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::CompatibilityMismatch,
            "Movie continuation belongs to another game");
    }
    return StateServiceResult::Success();
}

StateServiceResult StateService::RollbackPrepared(
    const StateReplacementContext& context,
    std::size_t prepared_count) noexcept
{
    StateServiceResult first_failure = StateServiceResult::Success();
    while (prepared_count > 0)
    {
        --prepared_count;
        StateServiceResult result =
            participants_[prepared_count]->RollbackStateReplacement(context);
        if (!result.ok && first_failure.ok)
            first_failure = std::move(result);
    }
    if (first_failure.ok)
        return first_failure;
    return StateServiceResult::Failure(
        StateServiceErrorCode::IntegrityFailure,
        first_failure.message.empty() ?
            "State replacement participant rollback failed" :
            std::move(first_failure.message),
        StateIntegrity::Unknown);
}

std::optional<StateEpoch> StateService::NextEpoch() const noexcept
{
    if (current_epoch_.value() ==
        std::numeric_limits<StateEpoch::value_type>::max())
    {
        return std::nullopt;
    }
    return StateEpoch(current_epoch_.value() + 1);
}

bool StateService::OnOwnerThread() const noexcept
{
    return owner_thread_ == std::this_thread::get_id();
}

StateServiceResult StateService::WrongThread()
{
    return StateServiceResult::Failure(
        StateServiceErrorCode::InvalidState,
        "StateService operation used the wrong actor thread");
}

} // namespace savor::runtime
