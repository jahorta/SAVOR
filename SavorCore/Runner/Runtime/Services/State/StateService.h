#pragma once

#include "Runner/Runtime/Services/State/IStateBackendPort.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace savor::runtime {

struct StateServiceLimits
{
    std::size_t maximum_memory_handles = 16;
    std::size_t maximum_handle_bytes = 256ull * 1024ull * 1024ull;
    std::size_t maximum_total_memory_bytes = 512ull * 1024ull * 1024ull;
};

class StateService final
{
public:
    explicit StateService(
        IStateBackendPort& backend,
        StateServiceLimits limits = {});

    StateService(const StateService&) = delete;
    StateService& operator=(const StateService&) = delete;

    [[nodiscard]] StateServiceResult RegisterParticipant(
        IStateReplacementParticipant& participant);

    [[nodiscard]] StateOperationReceipt Boot(
        const StateBootRequest& request = {});
    [[nodiscard]] StateOperationReceipt Reboot(
        const StateBootRequest& request = {});
    [[nodiscard]] StateServiceResult Shutdown() noexcept;

    [[nodiscard]] StateHandleReceipt CaptureMemoryHandle(
        const StateHandleCaptureRequest& request = {});
    [[nodiscard]] StateOperationReceipt RestoreMemoryHandle(
        StateHandleId handle);
    [[nodiscard]] StateServiceResult ReleaseMemoryHandle(
        StateHandleId handle) noexcept;
    [[nodiscard]] std::optional<StateHandleReceipt> DescribeMemoryHandle(
        StateHandleId handle) const;

    [[nodiscard]] StateFileArtifactReceipt CaptureFileArtifact(
        const StateFileCaptureRequest& request);
    [[nodiscard]] ImmutableStateArtifactCaptureReceipt
        CaptureImmutableArtifact(
            const StateFileCaptureRequest& request);
    [[nodiscard]] StateFileArtifactReceipt CommitImmutableArtifact(
        const ImmutableStateArtifactPublicationReceipt& publication);
    [[nodiscard]] StateServiceResult AbandonImmutableArtifact(
        StateArtifactId artifact) noexcept;
    [[nodiscard]] StateFileArtifactReceipt ImportFileArtifact(
        const StateFileImportRequest& request);
    [[nodiscard]] StateOperationReceipt RestoreFileArtifact(
        StateArtifactId artifact);
    [[nodiscard]] StateServiceResult ReleaseFileArtifact(
        StateArtifactId artifact) noexcept;
    [[nodiscard]] std::optional<StateFileArtifactReceipt> DescribeFileArtifact(
        StateArtifactId artifact) const;

    [[nodiscard]] StateEpoch current_epoch() const noexcept
    {
        return current_epoch_;
    }
    [[nodiscard]] const StateCompatibilityToken& compatibility() const noexcept
    {
        return compatibility_;
    }
    [[nodiscard]] bool is_open() const noexcept { return open_; }
    [[nodiscard]] bool is_tainted() const noexcept { return tainted_; }
    [[nodiscard]] std::size_t memory_handle_count() const noexcept
    {
        return memory_handles_.size();
    }
    [[nodiscard]] std::size_t memory_bytes_in_use() const noexcept
    {
        return memory_bytes_in_use_;
    }
    [[nodiscard]] std::uint64_t session_generation() const noexcept
    {
        return session_generation_;
    }

private:
    struct MemoryRecord
    {
        StateHandleReceipt receipt;
        std::vector<std::uint8_t> bytes;
    };

    struct FileRecord
    {
        StateFileArtifactReceipt receipt;
    };

    struct PendingFileRecord
    {
        StateArtifactId artifact;
        StateEpoch captured_epoch;
        std::filesystem::path path;
        std::size_t state_size_bytes = 0;
        std::size_t movie_size_bytes = 0;
        StateCompatibilityToken compatibility;
        StateLineage lineage;
        std::optional<MovieCheckpointMetadata> movie;
    };

    [[nodiscard]] StateOperationReceipt ReplaceState(
        StateReplacementContext context,
        const std::function<StateBackendResult()>& operation);
    [[nodiscard]] StateServiceResult ValidateReady() const;
    [[nodiscard]] StateServiceResult ValidateCompatibility(
        const StateCompatibilityToken& token) const;
    [[nodiscard]] StateServiceResult NormalizeMovieMetadata(
        std::optional<MovieCheckpointMetadata>& movie,
        bool external,
        ExternalMovieImportMode import_mode =
            ExternalMovieImportMode::Unspecified) const;
    [[nodiscard]] StateServiceResult RollbackPrepared(
        const StateReplacementContext& context,
        std::size_t prepared_count) noexcept;
    [[nodiscard]] std::optional<StateEpoch> NextEpoch() const noexcept;
    [[nodiscard]] bool OnOwnerThread() const noexcept;
    [[nodiscard]] static StateServiceResult WrongThread();

    IStateBackendPort& backend_;
    std::thread::id owner_thread_;
    StateServiceLimits limits_;
    std::vector<IStateReplacementParticipant*> participants_;
    std::unordered_map<std::uint64_t, MemoryRecord> memory_handles_;
    std::unordered_map<std::uint64_t, FileRecord> file_artifacts_;
    std::unordered_map<std::uint64_t, PendingFileRecord>
        pending_file_artifacts_;
    StateCompatibilityToken compatibility_;
    StateEpoch current_epoch_;
    std::uint64_t next_handle_id_ = 1;
    std::uint64_t next_artifact_id_ = 1;
    std::uint64_t session_generation_ = 0;
    std::size_t memory_bytes_in_use_ = 0;
    bool open_ = false;
    bool replacing_ = false;
    bool tainted_ = false;
    bool stopped_ = false;
    std::optional<StateServiceResult> shutdown_result_;
};

} // namespace savor::runtime
