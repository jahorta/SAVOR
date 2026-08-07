#include "Runner/Runtime/Services/Savestate/SavestateService.h"

#include "Tas/DtmFile.h"
#include "Utils/Hash.h"

#include <algorithm>
#include <array>
#include <exception>
#include <fstream>
#include <string_view>
#include <utility>

namespace savor::runtime {
namespace {

[[nodiscard]] bool CompleteSha256(std::string_view value) noexcept
{
    return value.size() == 64 &&
        std::ranges::all_of(value, [](char ch) {
            return (ch >= '0' && ch <= '9') ||
                (ch >= 'a' && ch <= 'f');
        });
}

[[nodiscard]] bool HasDtmMagic(
    const std::vector<std::uint8_t>& bytes) noexcept
{
    constexpr std::array<std::uint8_t, 4> magic{'D', 'T', 'M', 0x1a};
    return bytes.size() >= savor::tas::DtmFile::kMinHeader &&
        std::equal(magic.begin(), magic.end(), bytes.begin());
}

[[nodiscard]] SavestateServiceResult ReadFile(
    const std::filesystem::path& path,
    std::vector<std::uint8_t>& bytes)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
    {
        return SavestateServiceResult::Failure(
            SavestateServiceErrorCode::ArtifactFailure,
            "Unable to open artifact " + path.string());
    }
    const std::streamsize size = input.tellg();
    if (size < 0)
    {
        return SavestateServiceResult::Failure(
            SavestateServiceErrorCode::ArtifactFailure,
            "Unable to determine artifact size for " + path.string());
    }
    bytes.resize(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    if (size > 0 &&
        !input.read(reinterpret_cast<char*>(bytes.data()), size))
    {
        bytes.clear();
        return SavestateServiceResult::Failure(
            SavestateServiceErrorCode::ArtifactFailure,
            "Unable to read artifact " + path.string());
    }
    return SavestateServiceResult::Success();
}

[[nodiscard]] std::string HashFile(
    const std::filesystem::path& path,
    SavestateServiceResult& result)
{
    try
    {
        result = SavestateServiceResult::Success();
        return hash::sha256_of_file(path.string());
    }
    catch (const std::exception& ex)
    {
        result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::ArtifactFailure,
            ex.what());
        return {};
    }
}

[[nodiscard]] SavestateServiceResult FromBackend(
    SavestateBackendResult result,
    std::string_view fallback)
{
    if (result.ok)
        return SavestateServiceResult::Success();
    return SavestateServiceResult::Failure(
        SavestateServiceErrorCode::BackendFailure,
        result.message.empty() ? std::string(fallback) : std::move(result.message),
        result.integrity);
}

template <typename Operation>
[[nodiscard]] SavestateBackendResult CallRestoreBackend(
    std::string_view name,
    Operation&& operation) noexcept
{
    try
    {
        return operation();
    }
    catch (const std::exception& ex)
    {
        return SavestateBackendResult::Failure(
            std::string(name) + " threw: " + ex.what(),
            GuestIntegrity::Unknown);
    }
    catch (...)
    {
        return SavestateBackendResult::Failure(
            std::string(name) + " threw",
            GuestIntegrity::Unknown);
    }
}

[[nodiscard]] SavestateBackendBufferResult CallSaveBufferBackend(
    ISavestateBackendPort& backend) noexcept
{
    try
    {
        return backend.SaveStateBuffer();
    }
    catch (const std::exception& ex)
    {
        return {
            SavestateBackendResult::Failure(
                std::string("Savestate buffer capture threw: ") + ex.what(),
                GuestIntegrity::Unknown),
            {}};
    }
    catch (...)
    {
        return {
            SavestateBackendResult::Failure(
                "Savestate buffer capture threw",
                GuestIntegrity::Unknown),
            {}};
    }
}

[[nodiscard]] SavestateBackendBufferResult CallSaveFileBytesBackend(
    ISavestateBackendPort& backend) noexcept
{
    try
    {
        return backend.SaveStateFileBytes();
    }
    catch (const std::exception& ex)
    {
        return {
            SavestateBackendResult::Failure(
                std::string("Savestate file capture threw: ") + ex.what(),
                GuestIntegrity::Unknown),
            {}};
    }
    catch (...)
    {
        return {
            SavestateBackendResult::Failure(
                "Savestate file capture threw",
                GuestIntegrity::Unknown),
            {}};
    }
}

} // namespace

SavestateService::SavestateService(
    ISavestateBackendPort& backend,
    WorksetEpoch workset_epoch,
    ArtifactCompatibilityToken compatibility,
    SavestateServiceLimits limits)
    : backend_(backend),
      owner_thread_(std::this_thread::get_id()),
      workset_epoch_(workset_epoch),
      compatibility_(std::move(compatibility)),
      limits_(limits)
{
}

SavestateHandleReceipt SavestateService::CaptureMemoryHandle(
    const SavestateHandleCaptureRequest& request)
{
    SavestateHandleReceipt receipt;
    receipt.captured_epoch = workset_epoch_;
    if (SavestateServiceResult ready = ValidateReady(); !ready.ok)
    {
        receipt.result = std::move(ready);
        return receipt;
    }
    if (memory_handles_.size() >= limits_.maximum_memory_handles)
    {
        receipt.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::CapacityExceeded,
            "The in-memory savestate handle limit was reached");
        return receipt;
    }

    std::optional<MovieCheckpointMetadata> movie = request.movie;
    if (SavestateServiceResult valid = NormalizeMovieMetadata(movie, false);
        !valid.ok)
    {
        receipt.result = std::move(valid);
        return receipt;
    }
    if (movie && movie->mode == MovieCheckpointMode::Recording)
        movie->recording_workset_epoch = workset_epoch_;

    SavestateBackendBufferResult saved = CallSaveBufferBackend(backend_);
    if (!saved.result.ok)
    {
        receipt.result = FromBackend(
            std::move(saved.result), "Savestate buffer capture failed");
        return receipt;
    }
    if (saved.bytes.empty())
    {
        receipt.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::ArtifactFailure,
            "Savestate backend returned an empty buffer");
        return receipt;
    }
    const std::size_t movie_bytes = movie ? movie->dtm_bytes.size() : 0;
    const std::size_t total = saved.bytes.size() + movie_bytes;
    if (total > limits_.maximum_handle_bytes ||
        total > limits_.maximum_total_memory_bytes -
            std::min(memory_bytes_in_use_, limits_.maximum_total_memory_bytes))
    {
        receipt.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::CapacityExceeded,
            "The in-memory savestate byte limit was reached");
        return receipt;
    }
    if (next_handle_id_ == 0)
    {
        receipt.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::IntegrityFailure,
            "SavestateHandleId is exhausted",
            GuestIntegrity::Unknown);
        return receipt;
    }

    receipt.result = SavestateServiceResult::Success();
    receipt.handle = SavestateHandleId(next_handle_id_++);
    receipt.size_bytes = total;
    receipt.sha256 = hash::sha256(saved.bytes.data(), saved.bytes.size());
    receipt.compatibility = compatibility_;
    receipt.lineage = request.lineage;
    receipt.movie = std::move(movie);
    memory_bytes_in_use_ += total;
    memory_handles_.emplace(
        receipt.handle.value(), MemoryRecord{receipt, std::move(saved.bytes)});
    return receipt;
}

SavestateRestoreReceipt SavestateService::RestoreMemoryHandle(
    SavestateHandleId handle)
{
    SavestateRestoreReceipt receipt{
        .workset_epoch = workset_epoch_,
        .compatibility = compatibility_};
    if (SavestateServiceResult ready = ValidateReady(); !ready.ok)
    {
        receipt.result = std::move(ready);
        return receipt;
    }
    const auto found = memory_handles_.find(handle.value());
    if (!handle || found == memory_handles_.end())
    {
        receipt.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::NotFound,
            "Savestate memory handle was not found");
        return receipt;
    }
    const MemoryRecord& record = found->second;
    if (SavestateServiceResult compatible =
            ValidateCompatibility(record.receipt.compatibility);
        !compatible.ok)
    {
        receipt.result = std::move(compatible);
        return receipt;
    }
    if (record.receipt.captured_epoch != workset_epoch_ ||
        (record.receipt.movie &&
         record.receipt.movie->mode == MovieCheckpointMode::Recording &&
         record.receipt.movie->recording_workset_epoch != workset_epoch_))
    {
        receipt.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::WorksetMismatch,
            "Savestate handle belongs to another workset");
        return receipt;
    }
    receipt.result = FromBackend(
        CallRestoreBackend(
            "Savestate buffer restore",
            [&] { return backend_.RestoreStateBuffer(record.bytes); }),
        "Savestate buffer restore failed");
    if (receipt.result.ok)
        receipt.movie = record.receipt.movie;
    return receipt;
}

SavestateServiceResult SavestateService::ReleaseMemoryHandle(
    SavestateHandleId handle) noexcept
{
    if (!OnOwnerThread())
        return WrongThread();
    const auto found = memory_handles_.find(handle.value());
    if (!handle || found == memory_handles_.end())
    {
        return SavestateServiceResult::Failure(
            SavestateServiceErrorCode::NotFound,
            "Savestate memory handle was not found");
    }
    memory_bytes_in_use_ -= found->second.receipt.size_bytes;
    memory_handles_.erase(found);
    return SavestateServiceResult::Success();
}

std::optional<SavestateHandleReceipt> SavestateService::DescribeMemoryHandle(
    SavestateHandleId handle) const
{
    if (!OnOwnerThread())
        return std::nullopt;
    const auto found = memory_handles_.find(handle.value());
    return !handle || found == memory_handles_.end()
        ? std::nullopt
        : std::optional<SavestateHandleReceipt>(found->second.receipt);
}

ImmutableSavestateArtifactCaptureReceipt
SavestateService::CaptureImmutableArtifact(
    const SavestateCaptureRequest& request)
{
    ImmutableSavestateArtifactCaptureReceipt receipt;
    receipt.final_path = request.path;
    receipt.captured_epoch = workset_epoch_;
    if (SavestateServiceResult ready = ValidateReady(); !ready.ok)
    {
        receipt.result = std::move(ready);
        return receipt;
    }
    if (request.path.empty() || request.path.filename().empty())
    {
        receipt.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::InvalidArgument,
            "Immutable savestate capture requires a final path");
        return receipt;
    }
    const auto path_owned = [&](const auto& records) {
        return std::ranges::any_of(records, [&](const auto& item) {
            if constexpr (requires { item.second.path; })
                return item.second.path.lexically_normal() == request.path.lexically_normal();
            else
                return item.second.receipt.path.lexically_normal() == request.path.lexically_normal();
        });
    };
    if (path_owned(pending_file_artifacts_) || path_owned(file_artifacts_))
    {
        receipt.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::ArtifactFailure,
            "Immutable savestate path is already owned");
        return receipt;
    }

    std::optional<MovieCheckpointMetadata> movie = request.movie;
    if (movie && movie->mode == MovieCheckpointMode::Recording)
    {
        receipt.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::Unsupported,
            "Publishing an in-progress recording checkpoint is unsupported");
        return receipt;
    }
    if (SavestateServiceResult valid = NormalizeMovieMetadata(movie, false);
        !valid.ok)
    {
        receipt.result = std::move(valid);
        return receipt;
    }

    // Immutable artifacts must contain Dolphin's portable on-disk savestate
    // representation. Raw SaveToBuffer bytes are reserved for process-local
    // workset handles and cannot be published as a .sav file.
    SavestateBackendBufferResult saved =
        CallSaveFileBytesBackend(backend_);
    if (!saved.result.ok)
    {
        receipt.result = FromBackend(
            std::move(saved.result), "Immutable savestate capture failed");
        return receipt;
    }
    if (saved.bytes.empty() || next_artifact_id_ == 0)
    {
        receipt.result = SavestateServiceResult::Failure(
            next_artifact_id_ == 0
                ? SavestateServiceErrorCode::IntegrityFailure
                : SavestateServiceErrorCode::ArtifactFailure,
            next_artifact_id_ == 0
                ? "SavestateArtifactId is exhausted"
                : "Savestate backend returned an empty buffer",
            next_artifact_id_ == 0
                ? GuestIntegrity::Unknown
                : GuestIntegrity::Preserved);
        return receipt;
    }

    receipt.result = SavestateServiceResult::Success();
    receipt.artifact = SavestateArtifactId(next_artifact_id_++);
    receipt.state_bytes = ImmutableSavestateBytes::Capture(std::move(saved.bytes));
    if (movie)
        receipt.movie_bytes = ImmutableSavestateBytes::Capture(movie->dtm_bytes);
    receipt.compatibility = compatibility_;
    receipt.lineage = request.lineage;
    receipt.movie = movie;
    pending_file_artifacts_.emplace(
        receipt.artifact.value(),
        PendingFileRecord{
            .artifact = receipt.artifact,
            .captured_epoch = workset_epoch_,
            .path = request.path,
            .state_size_bytes = receipt.state_bytes.size(),
            .movie_size_bytes = receipt.movie_bytes ? receipt.movie_bytes->size() : 0,
            .compatibility = compatibility_,
            .lineage = request.lineage,
            .movie = std::move(movie)});
    return receipt;
}

SavestateFileArtifactReceipt SavestateService::CommitImmutableArtifact(
    const ImmutableSavestateArtifactPublicationReceipt& publication)
{
    SavestateFileArtifactReceipt receipt;
    if (SavestateServiceResult ready = ValidateReady(); !ready.ok)
    {
        receipt.result = std::move(ready);
        return receipt;
    }
    const auto found = pending_file_artifacts_.find(publication.artifact.value());
    if (!publication.artifact || found == pending_file_artifacts_.end())
    {
        receipt.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::NotFound,
            "Pending immutable savestate capture was not found");
        return receipt;
    }
    const PendingFileRecord& pending = found->second;
    const bool expects_movie = pending.movie.has_value();
    const std::filesystem::path expected_movie =
        SavestateDtmSidecarPath(pending.path);
    if (publication.state_path.lexically_normal() != pending.path.lexically_normal() ||
        publication.state_size_bytes != pending.state_size_bytes ||
        !CompleteSha256(publication.state_sha256) ||
        expects_movie != publication.movie_path.has_value() ||
        (expects_movie &&
         (publication.movie_path->lexically_normal() != expected_movie.lexically_normal() ||
          publication.movie_size_bytes != pending.movie_size_bytes ||
          !CompleteSha256(publication.movie_sha256))))
    {
        receipt.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::ArtifactFailure,
            "Immutable savestate publication does not match its pending capture");
        return receipt;
    }
    SavestateServiceResult hash_result;
    const std::string state_hash = HashFile(publication.state_path, hash_result);
    if (!hash_result.ok || state_hash != publication.state_sha256)
    {
        receipt.result = hash_result.ok
            ? SavestateServiceResult::Failure(
                  SavestateServiceErrorCode::ArtifactFailure,
                  "Published savestate SHA-256 does not match")
            : std::move(hash_result);
        return receipt;
    }
    if (expects_movie)
    {
        const std::string movie_hash = HashFile(*publication.movie_path, hash_result);
        if (!hash_result.ok || movie_hash != publication.movie_sha256 ||
            movie_hash != pending.movie->dtm_sha256)
        {
            receipt.result = hash_result.ok
                ? SavestateServiceResult::Failure(
                      SavestateServiceErrorCode::ArtifactFailure,
                      "Published DTM SHA-256 does not match")
                : std::move(hash_result);
            return receipt;
        }
    }

    receipt.result = SavestateServiceResult::Success();
    receipt.artifact = publication.artifact;
    receipt.captured_epoch = pending.captured_epoch;
    receipt.path = publication.state_path;
    receipt.size_bytes = publication.state_size_bytes + publication.movie_size_bytes;
    receipt.sha256 = publication.state_sha256;
    receipt.compatibility = pending.compatibility;
    receipt.lineage = pending.lineage;
    receipt.movie = pending.movie;
    if (receipt.movie)
        receipt.movie->dtm_path = *publication.movie_path;
    file_artifacts_.emplace(receipt.artifact.value(), FileRecord{receipt});
    pending_file_artifacts_.erase(found);
    return receipt;
}

SavestateServiceResult SavestateService::AbandonImmutableArtifact(
    SavestateArtifactId artifact) noexcept
{
    if (!OnOwnerThread())
        return WrongThread();
    if (!artifact || pending_file_artifacts_.erase(artifact.value()) == 0)
    {
        return SavestateServiceResult::Failure(
            SavestateServiceErrorCode::NotFound,
            "Pending immutable savestate capture was not found");
    }
    return SavestateServiceResult::Success();
}

SavestateFileArtifactReceipt SavestateService::ImportFileArtifact(
    const SavestateImportRequest& request)
{
    SavestateFileArtifactReceipt receipt;
    receipt.path = request.path;
    receipt.external = true;
    if (SavestateServiceResult ready = ValidateReady(); !ready.ok)
    {
        receipt.result = std::move(ready);
        return receipt;
    }
    if (request.movie_mode == ExternalMovieImportMode::Unspecified ||
        request.movie_mode == ExternalMovieImportMode::Recording ||
        !std::filesystem::is_regular_file(request.path) ||
        !CompleteSha256(request.expected_sha256))
    {
        receipt.result = SavestateServiceResult::Failure(
            request.movie_mode == ExternalMovieImportMode::Recording
                ? SavestateServiceErrorCode::Unsupported
                : SavestateServiceErrorCode::InvalidArgument,
            "Savestate import requires an exact file, hash, and supported movie mode");
        return receipt;
    }
    if (SavestateServiceResult compatible = ValidateCompatibility(request.compatibility);
        !compatible.ok)
    {
        receipt.result = std::move(compatible);
        return receipt;
    }
    SavestateServiceResult hash_result;
    const std::string state_hash = HashFile(request.path, hash_result);
    if (!hash_result.ok || state_hash != request.expected_sha256)
    {
        receipt.result = hash_result.ok
            ? SavestateServiceResult::Failure(
                  SavestateServiceErrorCode::ArtifactFailure,
                  "Imported savestate SHA-256 does not match")
            : std::move(hash_result);
        return receipt;
    }

    std::optional<MovieCheckpointMetadata> movie;
    const std::filesystem::path sidecar =
        SavestateDtmSidecarPath(request.path);
    if (request.movie_mode == ExternalMovieImportMode::NoMovie)
    {
        if (request.dtm_path || std::filesystem::exists(sidecar))
        {
            receipt.result = SavestateServiceResult::Failure(
                SavestateServiceErrorCode::InvalidArgument,
                "NoMovie import conflicts with a DTM sidecar");
            return receipt;
        }
    }
    else
    {
        if (request.dtm_path.value_or(sidecar).lexically_normal() != sidecar.lexically_normal() ||
            !CompleteSha256(request.expected_dtm_sha256))
        {
            receipt.result = SavestateServiceResult::Failure(
                SavestateServiceErrorCode::InvalidArgument,
                "Read-only import requires the exact <savestate>.dtm sidecar and hash");
            return receipt;
        }
        movie = MovieCheckpointMetadata{
            .mode = MovieCheckpointMode::ReadOnlyPlayback,
            .dtm_sha256 = request.expected_dtm_sha256,
            .dtm_path = sidecar};
        if (SavestateServiceResult valid = NormalizeMovieMetadata(
                movie, true, request.movie_mode);
            !valid.ok)
        {
            receipt.result = std::move(valid);
            return receipt;
        }
    }
    if (next_artifact_id_ == 0)
    {
        receipt.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::IntegrityFailure,
            "SavestateArtifactId is exhausted",
            GuestIntegrity::Unknown);
        return receipt;
    }
    receipt.result = SavestateServiceResult::Success();
    receipt.artifact = SavestateArtifactId(next_artifact_id_++);
    receipt.captured_epoch = workset_epoch_;
    receipt.size_bytes = static_cast<std::size_t>(std::filesystem::file_size(request.path));
    receipt.sha256 = state_hash;
    receipt.compatibility = request.compatibility;
    receipt.lineage = request.lineage;
    receipt.movie = std::move(movie);
    file_artifacts_.emplace(receipt.artifact.value(), FileRecord{receipt});
    return receipt;
}

SavestateRestoreReceipt SavestateService::RestoreFileArtifact(
    SavestateArtifactId artifact)
{
    SavestateRestoreReceipt receipt{
        .workset_epoch = workset_epoch_,
        .compatibility = compatibility_};
    if (SavestateServiceResult ready = ValidateReady(); !ready.ok)
    {
        receipt.result = std::move(ready);
        return receipt;
    }
    const auto found = file_artifacts_.find(artifact.value());
    if (!artifact || found == file_artifacts_.end())
    {
        receipt.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::NotFound,
            "Savestate file artifact was not found");
        return receipt;
    }
    const SavestateFileArtifactReceipt& record = found->second.receipt;
    SavestateServiceResult hash_result;
    if (ValidateCompatibility(record.compatibility).ok)
    {
        const std::string state_hash = HashFile(record.path, hash_result);
        if (!hash_result.ok || state_hash != record.sha256)
        {
            receipt.result = hash_result.ok
                ? SavestateServiceResult::Failure(
                      SavestateServiceErrorCode::ArtifactFailure,
                      "Savestate artifact changed after import")
                : std::move(hash_result);
            return receipt;
        }
    }
    else
    {
        receipt.result = ValidateCompatibility(record.compatibility);
        return receipt;
    }
    if (record.movie)
    {
        const std::string dtm_hash = HashFile(
            SavestateDtmSidecarPath(record.path), hash_result);
        if (!hash_result.ok || dtm_hash != record.movie->dtm_sha256)
        {
            receipt.result = hash_result.ok
                ? SavestateServiceResult::Failure(
                      SavestateServiceErrorCode::ArtifactFailure,
                      "DTM sidecar changed after import")
                : std::move(hash_result);
            return receipt;
        }
    }
    receipt.result = FromBackend(
        CallRestoreBackend(
            "Savestate file restore",
            [&] { return backend_.RestoreStateFile(record.path); }),
        "Savestate file restore failed");
    if (receipt.result.ok)
        receipt.movie = record.movie;
    return receipt;
}

SavestateServiceResult SavestateService::ReleaseFileArtifact(
    SavestateArtifactId artifact) noexcept
{
    if (!OnOwnerThread())
        return WrongThread();
    if (!artifact || file_artifacts_.erase(artifact.value()) == 0)
    {
        return SavestateServiceResult::Failure(
            SavestateServiceErrorCode::NotFound,
            "Savestate file artifact was not found");
    }
    return SavestateServiceResult::Success();
}

std::optional<SavestateFileArtifactReceipt>
SavestateService::DescribeFileArtifact(SavestateArtifactId artifact) const
{
    if (!OnOwnerThread())
        return std::nullopt;
    const auto found = file_artifacts_.find(artifact.value());
    return !artifact || found == file_artifacts_.end()
        ? std::nullopt
        : std::optional<SavestateFileArtifactReceipt>(found->second.receipt);
}

SavestateServiceResult SavestateService::ValidateReady() const
{
    if (!OnOwnerThread())
        return WrongThread();
    if (!workset_epoch_)
    {
        return SavestateServiceResult::Failure(
            SavestateServiceErrorCode::InvalidState,
            "SavestateService requires an active workset");
    }
    if (!compatibility_.Complete())
    {
        return SavestateServiceResult::Failure(
            SavestateServiceErrorCode::CompatibilityMismatch,
            "SavestateService has incomplete compatibility evidence");
    }
    return SavestateServiceResult::Success();
}

SavestateServiceResult SavestateService::ValidateCompatibility(
    const ArtifactCompatibilityToken& token) const
{
    if (!token.Complete())
    {
        return SavestateServiceResult::Failure(
            SavestateServiceErrorCode::InvalidArgument,
            "Artifact compatibility token is incomplete");
    }
    if (token != compatibility_)
    {
        return SavestateServiceResult::Failure(
            SavestateServiceErrorCode::CompatibilityMismatch,
            "Savestate artifact is incompatible with the open session");
    }
    return SavestateServiceResult::Success();
}

SavestateServiceResult SavestateService::NormalizeMovieMetadata(
    std::optional<MovieCheckpointMetadata>& movie,
    bool external,
    ExternalMovieImportMode import_mode) const
{
    if (!movie)
        return SavestateServiceResult::Success();
    if (!movie->HasMovie())
    {
        return SavestateServiceResult::Failure(
            SavestateServiceErrorCode::InvalidArgument,
            "Movie checkpoint metadata must declare a movie mode");
    }
    if (external &&
        (import_mode != ExternalMovieImportMode::ReadOnlyPlayback ||
         movie->mode != MovieCheckpointMode::ReadOnlyPlayback))
    {
        return SavestateServiceResult::Failure(
            SavestateServiceErrorCode::Unsupported,
            "External movie continuation must be read-only playback");
    }
    if (movie->mode == MovieCheckpointMode::Recording && external)
    {
        return SavestateServiceResult::Failure(
            SavestateServiceErrorCode::Unsupported,
            "Cold restoration of an in-progress recording is unsupported");
    }
    if (movie->dtm_bytes.empty())
    {
        if (movie->dtm_path.empty())
        {
            return SavestateServiceResult::Failure(
                SavestateServiceErrorCode::InvalidArgument,
                "Movie continuation requires DTM bytes or a path");
        }
        if (SavestateServiceResult read = ReadFile(movie->dtm_path, movie->dtm_bytes);
            !read.ok)
            return read;
    }
    if (!HasDtmMagic(movie->dtm_bytes))
    {
        return SavestateServiceResult::Failure(
            SavestateServiceErrorCode::ArtifactFailure,
            "Movie continuation is not a valid DTM");
    }
    const std::string actual = hash::sha256(
        movie->dtm_bytes.data(), movie->dtm_bytes.size());
    if (!movie->dtm_sha256.empty() && movie->dtm_sha256 != actual)
    {
        return SavestateServiceResult::Failure(
            SavestateServiceErrorCode::ArtifactFailure,
            "Movie continuation SHA-256 does not match");
    }
    movie->dtm_sha256 = actual;
    const std::string game_id(
        reinterpret_cast<const char*>(movie->dtm_bytes.data() + 4), 6);
    if (!movie->game_id.empty() && movie->game_id != game_id)
    {
        return SavestateServiceResult::Failure(
            SavestateServiceErrorCode::ArtifactFailure,
            "Movie continuation game ID does not match its DTM");
    }
    movie->game_id = game_id;
    movie->starts_from_savestate = movie->dtm_bytes[12] != 0;
    if (movie->game_id != compatibility_.game_id)
    {
        return SavestateServiceResult::Failure(
            SavestateServiceErrorCode::CompatibilityMismatch,
            "Movie continuation belongs to another game");
    }
    return SavestateServiceResult::Success();
}

bool SavestateService::OnOwnerThread() const noexcept
{
    return owner_thread_ == std::this_thread::get_id();
}

SavestateServiceResult SavestateService::WrongThread()
{
    return SavestateServiceResult::Failure(
        SavestateServiceErrorCode::InvalidState,
        "SavestateService operation used the wrong actor thread");
}

} // namespace savor::runtime
