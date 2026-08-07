#pragma once

#include "WorksetTypes.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace savor::runtime {

struct SavestateArtifactFinalizationIdTag;
using SavestateArtifactFinalizationId =
    StrongId<SavestateArtifactFinalizationIdTag>;

enum class SavestateArtifactFinalizerErrorCode : std::uint16_t
{
    None,
    InvalidArgument,
    NotAccepting,
    CapacityExceeded,
    HashMismatch,
    FilesystemFailure,
    IntegrityFailure,
    SequenceExhausted,
};

struct SavestateArtifactFinalizerResult
{
    bool ok = false;
    SavestateArtifactFinalizerErrorCode code =
        SavestateArtifactFinalizerErrorCode::FilesystemFailure;
    std::string message;

    [[nodiscard]] static SavestateArtifactFinalizerResult Success()
    {
        return {true, SavestateArtifactFinalizerErrorCode::None, {}};
    }

    [[nodiscard]] static SavestateArtifactFinalizerResult Failure(
        SavestateArtifactFinalizerErrorCode code,
        std::string message)
    {
        return {false, code, std::move(message)};
    }
};

using ImmutableArtifactBytes = ImmutableSavestateBytes;

struct ImmutableArtifactFile
{
    std::filesystem::path final_path;
    ImmutableArtifactBytes bytes;
    // Empty means compute the authoritative digest from the immutable bytes.
    std::string expected_sha256;
};

struct SavestateArtifactFinalizationRequest
{
    // Preferred pre-terminal identity. It is sufficient to finalize output
    // while ordinary invocation unwind is still establishing its terminal.
    WorkerItemExecutionCorrelation item;
    // Optional later binding retained in the request/completion so the
    // completion ledger can correlate a reservation made before Submit.
    WorkerItemTerminalCorrelation terminal;
    SavestateArtifactId state_artifact_id;
    std::string logical_artifact_id;
    ImmutableArtifactFile state;
    // Sidecars publish before the state file. Every file is immutable: an
    // existing exact file is reused and a conflicting file is never replaced.
    std::vector<ImmutableArtifactFile> sidecars;
};

struct FinalizedArtifactFile
{
    std::filesystem::path path;
    std::size_t size_bytes = 0;
    std::string sha256;
    bool reused_existing = false;
};

struct SavestateArtifactFinalizationCompletion
{
    SavestateArtifactFinalizationId finalization_id;
    WorkerItemExecutionCorrelation item;
    WorkerItemTerminalCorrelation terminal;
    SavestateArtifactId state_artifact_id;
    std::string logical_artifact_id;
    SavestateArtifactFinalizerResult result;
    FinalizedArtifactFile state;
    std::vector<FinalizedArtifactFile> sidecars;
};

struct SavestateArtifactFinalizerSubmission
{
    SavestateArtifactFinalizerResult result;
    SavestateArtifactFinalizationId finalization_id;
};

struct SavestateArtifactFinalizerSnapshot
{
    bool accepting = false;
    bool shutdown = false;
    std::size_t queued = 0;
    std::size_t running = 0;
    std::size_t completed_undrained = 0;
    std::size_t outstanding_jobs = 0;
    std::size_t resident_payload_bytes = 0;
};

// The notifier is deliberately weaker than a completion callback: its only
// permitted role is waking the WorkerRuntime actor. Finalizer threads place the
// complete value in the internal queue before invoking this noexcept seam.
class ISavestateArtifactFinalizerNotifier
{
public:
    virtual ~ISavestateArtifactFinalizerNotifier() = default;
    virtual void NotifySavestateArtifactFinalizerCompletion() noexcept = 0;
};

// Host-only bounded publication of already-captured immutable bytes. Jobs do
// not contain EmulationSession, Dolphin, SavestateService, or arbitrary callbacks,
// so finalizer threads cannot mutate guest/session state.
class SavestateArtifactFinalizer final
{
public:
    explicit SavestateArtifactFinalizer(
        const WorkerWorksetLimits& limits = {},
        std::shared_ptr<ISavestateArtifactFinalizerNotifier> notifier = {},
        std::function<void()> before_process_for_testing = {});
    ~SavestateArtifactFinalizer();

    SavestateArtifactFinalizer(const SavestateArtifactFinalizer&) = delete;
    SavestateArtifactFinalizer& operator=(const SavestateArtifactFinalizer&) = delete;

    [[nodiscard]] SavestateArtifactFinalizerSubmission Submit(
        SavestateArtifactFinalizationRequest request);

    // May be called by the actor after a notifier wake. Draining frees the
    // corresponding count credit; payload byte credit is freed as soon as a
    // worker finishes because completions retain receipts, not captured bytes.
    [[nodiscard]] std::vector<SavestateArtifactFinalizationCompletion>
        DrainResults();

    // Stops admission, lets every accepted job reach a completion, and joins
    // all finalizer threads. It is idempotent and does not discard completions.
    void Shutdown() noexcept;

    [[nodiscard]] SavestateArtifactFinalizerSnapshot snapshot() const noexcept;

private:
    struct Job
    {
        SavestateArtifactFinalizationId id;
        SavestateArtifactFinalizationRequest request;
        std::size_t resident_bytes = 0;
    };

    void WorkerMain() noexcept;
    [[nodiscard]] SavestateArtifactFinalizationCompletion Process(
        const Job& job) const noexcept;

    WorkerWorksetLimits limits_;
    std::shared_ptr<ISavestateArtifactFinalizerNotifier> notifier_;
    std::function<void()> before_process_for_testing_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable stopped_;
    bool accepting_ = true;
    bool shutdown_ = false;
    bool joining_ = false;
    bool joined_ = false;
    std::uint64_t next_finalization_id_ = 1;
    std::size_t running_ = 0;
    std::size_t outstanding_jobs_ = 0;
    std::size_t resident_payload_bytes_ = 0;
    std::deque<Job> jobs_;
    std::deque<SavestateArtifactFinalizationCompletion> completions_;
    std::unordered_set<std::uint64_t> outstanding_ids_;
    // SavestateArtifactId is session-owned and unique for each captured output.
    // Multiple independent captures from one invocation/item are allowed.
    std::unordered_set<std::uint64_t> outstanding_artifact_ids_;
    std::vector<std::thread> threads_;
};

} // namespace savor::runtime
