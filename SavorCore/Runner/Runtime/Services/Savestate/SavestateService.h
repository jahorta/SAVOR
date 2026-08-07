#pragma once

#include "Runner/Runtime/Services/Savestate/ISavestateBackendPort.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace savor::runtime {

struct SavestateServiceLimits
{
    std::size_t maximum_memory_handles = 16;
    std::size_t maximum_handle_bytes = 256ull * 1024ull * 1024ull;
    std::size_t maximum_total_memory_bytes = 512ull * 1024ull * 1024ull;
};

// Owns savestate bytes and immutable-artifact records for exactly one active
// workset. It does not open Dolphin, restart the guest core, own the workset
// epoch, or coordinate other session services.
class SavestateService final
{
public:
    SavestateService(
        ISavestateBackendPort& backend,
        WorksetEpoch workset_epoch,
        ArtifactCompatibilityToken compatibility,
        SavestateServiceLimits limits = {});

    SavestateService(const SavestateService&) = delete;
    SavestateService& operator=(const SavestateService&) = delete;

    [[nodiscard]] SavestateHandleReceipt CaptureMemoryHandle(
        const SavestateHandleCaptureRequest& request = {});
    [[nodiscard]] SavestateRestoreReceipt RestoreMemoryHandle(
        SavestateHandleId handle);
    [[nodiscard]] SavestateServiceResult ReleaseMemoryHandle(
        SavestateHandleId handle) noexcept;
    [[nodiscard]] std::optional<SavestateHandleReceipt> DescribeMemoryHandle(
        SavestateHandleId handle) const;

    [[nodiscard]] ImmutableSavestateArtifactCaptureReceipt
        CaptureImmutableArtifact(const SavestateCaptureRequest& request);
    [[nodiscard]] SavestateFileArtifactReceipt CommitImmutableArtifact(
        const ImmutableSavestateArtifactPublicationReceipt& publication);
    [[nodiscard]] SavestateServiceResult AbandonImmutableArtifact(
        SavestateArtifactId artifact) noexcept;
    [[nodiscard]] SavestateFileArtifactReceipt ImportFileArtifact(
        const SavestateImportRequest& request);
    [[nodiscard]] SavestateRestoreReceipt RestoreFileArtifact(
        SavestateArtifactId artifact);
    [[nodiscard]] SavestateServiceResult ReleaseFileArtifact(
        SavestateArtifactId artifact) noexcept;
    [[nodiscard]] std::optional<SavestateFileArtifactReceipt>
        DescribeFileArtifact(SavestateArtifactId artifact) const;

    [[nodiscard]] WorksetEpoch workset_epoch() const noexcept
    {
        return workset_epoch_;
    }
    [[nodiscard]] const ArtifactCompatibilityToken& compatibility() const noexcept
    {
        return compatibility_;
    }
    [[nodiscard]] std::size_t memory_handle_count() const noexcept
    {
        return memory_handles_.size();
    }
    [[nodiscard]] std::size_t memory_bytes_in_use() const noexcept
    {
        return memory_bytes_in_use_;
    }

private:
    struct MemoryRecord
    {
        SavestateHandleReceipt receipt;
        std::vector<std::uint8_t> bytes;
    };

    struct FileRecord
    {
        SavestateFileArtifactReceipt receipt;
    };

    struct PendingFileRecord
    {
        SavestateArtifactId artifact;
        WorksetEpoch captured_epoch;
        std::filesystem::path path;
        std::size_t state_size_bytes = 0;
        std::size_t movie_size_bytes = 0;
        ArtifactCompatibilityToken compatibility;
        ArtifactLineage lineage;
        std::optional<MovieCheckpointMetadata> movie;
    };

    [[nodiscard]] SavestateServiceResult ValidateReady() const;
    [[nodiscard]] SavestateServiceResult ValidateCompatibility(
        const ArtifactCompatibilityToken& token) const;
    [[nodiscard]] SavestateServiceResult NormalizeMovieMetadata(
        std::optional<MovieCheckpointMetadata>& movie,
        bool external,
        ExternalMovieImportMode import_mode =
            ExternalMovieImportMode::Unspecified) const;
    [[nodiscard]] bool OnOwnerThread() const noexcept;
    [[nodiscard]] static SavestateServiceResult WrongThread();

    ISavestateBackendPort& backend_;
    std::thread::id owner_thread_;
    WorksetEpoch workset_epoch_;
    ArtifactCompatibilityToken compatibility_;
    SavestateServiceLimits limits_;
    std::unordered_map<std::uint64_t, MemoryRecord> memory_handles_;
    std::unordered_map<std::uint64_t, FileRecord> file_artifacts_;
    std::unordered_map<std::uint64_t, PendingFileRecord>
        pending_file_artifacts_;
    std::uint64_t next_handle_id_ = 1;
    std::uint64_t next_artifact_id_ = 1;
    std::size_t memory_bytes_in_use_ = 0;
};

} // namespace savor::runtime
