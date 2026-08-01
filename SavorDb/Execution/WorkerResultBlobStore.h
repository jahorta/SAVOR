#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace savor::db::execution {

inline constexpr const char* kWorkerTerminalEnvelopeFormatV1 =
    "application/x-savor-durable-worker-terminal;version=1";

struct WorkerResultBlobStageRequest {
    std::int64_t workset_id = 0;
    std::int64_t dispatch_attempt_id = 0;
    std::int64_t job_id = 0;
    std::uint64_t terminal_id = 0;
    std::span<const std::uint8_t> envelope;
};

struct WorkerResultBlobReference {
    std::string relative_path;
    std::string sha256;
    std::int64_t size_bytes = 0;
    std::string format = kWorkerTerminalEnvelopeFormatV1;
};

using WorkerResultBlobOwnershipProbe = std::function<bool(
    std::string_view relative_path,
    bool* tracked_out,
    std::string* error_out)>;

// Files are private execution infrastructure. The relative reference is safe
// to persist, archive exclusion is explicit, and no caller receives a path
// outside object_store/worker_results.
class WorkerResultBlobStore {
public:
    explicit WorkerResultBlobStore(std::filesystem::path object_store_root);

    [[nodiscard]] const std::filesystem::path& Root() const noexcept;

    // Verifies that the configured store can complete the same durable
    // write/publish/read/delete cycle used for terminal envelopes. The probe is
    // idempotent and does not leave a durable blob behind.
    bool ValidateReady(std::string* error_out = nullptr) const;

    bool Stage(
        const WorkerResultBlobStageRequest& request,
        WorkerResultBlobReference* reference_out,
        std::string* error_out = nullptr) const;

    bool Read(
        const WorkerResultBlobReference& reference,
        std::vector<std::uint8_t>* envelope_out,
        std::string* error_out = nullptr) const;

    bool Remove(
        std::string_view relative_path,
        std::string* error_out = nullptr) const;

    // Stage keeps the published path pinned until its DB reference is durable.
    // The coordinator must unpin only after the DB accepts that reference.
    void Unpin(std::string_view relative_path) const;
    void UnpinAll() const;

    bool RemoveIfUnpinnedAndUntracked(
        std::string_view relative_path,
        const WorkerResultBlobOwnershipProbe& ownership_probe,
        bool* removed_out,
        std::string* error_out = nullptr) const;

    bool ListFilesOlderThan(
        std::chrono::milliseconds minimum_age,
        std::vector<std::string>* relative_paths_out,
        std::string* error_out = nullptr) const;

private:
    bool ResolvePrivatePath(
        std::string_view relative_path,
        std::filesystem::path* path_out,
        std::string* error_out) const;
    void PinPath(std::string relative_path) const;
    void UnpinPath(std::string_view relative_path) const;

    std::filesystem::path object_store_root_;
    std::filesystem::path worker_results_root_;
    mutable std::mutex stage_mutex_;
    mutable std::mutex pin_mutex_;
    mutable std::unordered_map<std::string, std::size_t>
        pinned_relative_paths_;
};

} // namespace savor::db::execution
